// WASAPI output: shared mode, event-driven, 48 kHz float stereo (the audio engine converts to the device mix format
// with AUTOCONVERTPCM). The mixer thread runs with the "Pro Audio" MMCSS profile and reopens the device after a
// device change (headphones plugged in, default device switched).

#include "Audio/AudioEngine.h"
#include "Core/Log/Log.h"
#include "Core/String/Utf.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>

namespace zx::audio {

ZX_DEFINE_LOG_CATEGORY_STATIC(LogWasapi, Info);

namespace {

struct WasapiDevice
{
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE event = nullptr;
    UINT32 bufferFrames = 0;
};

template <typename T>
void SafeRelease(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// PKEY_Device_FriendlyName (declared here to avoid the propkey headers' GUID-definition macros).
const PROPERTYKEY DeviceFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

const CLSID ClsidEnumerator = __uuidof(MMDeviceEnumerator);
const IID IidEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IidAudioClient = __uuidof(IAudioClient);
const IID IidRenderClient = __uuidof(IAudioRenderClient);

} // namespace

bool AudioEngine::OpenDevice()
{
    WasapiDevice* d = New<WasapiDevice>();
    m_device = d;
    auto fail = [&](const char* what, HRESULT hr) {
        ZX_LOG_WARN(LogWasapi, "Audio device: {} failed (0x{:08X})", what, u32(hr));
        CloseDevice();
        return false;
    };
    HRESULT hr = CoCreateInstance(ClsidEnumerator, nullptr, CLSCTX_ALL, IidEnumerator, reinterpret_cast<void**>(&d->enumerator));
    if (FAILED(hr))
        return fail("MMDeviceEnumerator", hr);
    hr = d->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &d->device);
    if (FAILED(hr))
        return fail("default output device", hr);
    // Friendly name for the stats / log.
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(d->device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(props->GetValue(DeviceFriendlyName, &name)) && name.vt == VT_LPWSTR)
            m_deviceName = Utf16ToUtf8(name.pwszVal);
        PropVariantClear(&name);
        props->Release();
    }
    hr = d->device->Activate(IidAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&d->client));
    if (FAILED(hr))
        return fail("IAudioClient", hr);
    WAVEFORMATEXTENSIBLE fmt = {};
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = 2;
    fmt.Format.nSamplesPerSec = MixRate;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = 8;
    fmt.Format.nAvgBytesPerSec = MixRate * 8;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    const REFERENCE_TIME duration = REFERENCE_TIME(Max(m_settings.bufferMs, 5.0f) * 10000.0f);
    hr = d->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                               duration, 0, &fmt.Format, nullptr);
    if (FAILED(hr))
        return fail("IAudioClient::Initialize", hr);
    d->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d->event || FAILED(d->client->SetEventHandle(d->event)))
        return fail("event", E_FAIL);
    hr = d->client->GetBufferSize(&d->bufferFrames);
    if (FAILED(hr))
        return fail("GetBufferSize", hr);
    hr = d->client->GetService(IidRenderClient, reinterpret_cast<void**>(&d->render));
    if (FAILED(hr))
        return fail("IAudioRenderClient", hr);
    // Prime with silence, then start.
    BYTE* data = nullptr;
    if (SUCCEEDED(d->render->GetBuffer(d->bufferFrames, &data)))
        d->render->ReleaseBuffer(d->bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    hr = d->client->Start();
    if (FAILED(hr))
        return fail("Start", hr);
    m_deviceOpen = true;
    ZX_LOG_INFO(LogWasapi, "Audio output: {} ({} frames buffer, {:.1f} ms)", m_deviceName, d->bufferFrames,
                f32(d->bufferFrames) * 1000.0f / f32(MixRate));
    return true;
}

void AudioEngine::CloseDevice()
{
    WasapiDevice* d = static_cast<WasapiDevice*>(m_device);
    if (!d)
        return;
    if (d->client)
        d->client->Stop();
    SafeRelease(d->render);
    SafeRelease(d->client);
    SafeRelease(d->device);
    SafeRelease(d->enumerator);
    if (d->event)
        CloseHandle(d->event);
    Delete(d);
    m_device = nullptr;
    m_deviceOpen = false;
}

void AudioEngine::MixerThread()
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    Array<f32> fallback;
    fallback.Resize(MixBlock * 2);
    f64 retryAt = 0.0;
    while (!m_quit.load(std::memory_order_relaxed)) {
        WasapiDevice* d = static_cast<WasapiDevice*>(m_device);
        if (!d) {
            // No device (yet): keep the mixer running in real time so voices still progress and end.
            const f64 now = f64(GetTickCount64()) / 1000.0;
            if (now >= retryAt) {
                if (!OpenDevice())
                    retryAt = now + 2.0;
                continue;
            }
            Mix(fallback.Data(), MixBlock);
            Thread::SleepMs(MixBlock * 1000 / MixRate);
            continue;
        }
        if (WaitForSingleObject(d->event, 200) != WAIT_OBJECT_0)
            continue;
        UINT32 padding = 0;
        HRESULT hr = d->client->GetCurrentPadding(&padding);
        if (SUCCEEDED(hr)) {
            if (padding == 0)
                m_underruns.fetch_add(1, std::memory_order_relaxed);
            const UINT32 frames = d->bufferFrames - padding;
            BYTE* data = nullptr;
            if (frames > 0 && SUCCEEDED(hr = d->render->GetBuffer(frames, &data))) {
                Mix(reinterpret_cast<f32*>(data), frames);
                hr = d->render->ReleaseBuffer(frames, 0);
            }
        }
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
            ZX_LOG_WARN(LogWasapi, "Audio device lost - reopening");
            CloseDevice();
            retryAt = 0.0;
        }
    }
    CloseDevice();
    if (mmcss)
        AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(coInit))
        CoUninitialize();
}

} // namespace zx::audio
