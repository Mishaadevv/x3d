// Audio in the editor: the Audio Mixer window (bus faders, mute, meters, device stats) and the Audio Source clip slot.

#include "Editor.h"
#include "Core/String/Path.h"

namespace zx::editor {

namespace {

/// Horizontal level meter (peak, with a dB scale feel).
void Meter(f32 peak, f32 width)
{
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    DrawList& dl = ui::GetWindowDrawList();
    const Vec2 p = ui::GetCursorPos();
    const f32 h = Round(8.0f * s);
    const Rect r(p + Vec2(0.0f, Round((ui::GetFrameHeight() - h) * 0.5f)), p + Vec2(Round(width * s), Round((ui::GetFrameHeight() + h) * 0.5f)));
    dl.AddRect(r, Color(0x1A, 0x1A, 0x1A), ui::CornerRadii(2.0f * s), 1.0f, ctx.GetStyle().colors.border);
    // -60 dB .. 0 dB across the bar.
    const f32 db = audio::GainToDb(peak);
    const f32 t = Clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
    if (t > 0.0f) {
        const Color c = db > -1.0f ? Color(0xE5, 0x48, 0x4D) : db > -9.0f ? Color(0xE8, 0xB8, 0x4A) : Color(0x4C, 0xC3, 0x7A);
        dl.AddRect(Rect(r.min, Vec2(r.min.x + (r.Width() * t), r.max.y)), c, ui::CornerRadii(2.0f * s));
    }
    ui::Dummy(Vec2(width, ui::GetFrameHeight() / s));
}

} // namespace

void DrawAudioMixerPanel(Editor& ed)
{
    if (!ed.showMixer || !ed.audioEngine)
        return;
    ui::SetNextWindowSize(Vec2(420.0f, 360.0f), ui::Cond::FirstUse);
    if (!ui::Begin(AudioMixerTitle, &ed.showMixer)) {
        ui::End();
        return;
    }
    audio::AudioEngine& a = *ed.audioEngine;
    const audio::AudioStats st = a.GetStats();
    ui::TextDisabled(st.deviceOpen ? String::Format(ZX_ICON_SPEAKER "  {}", st.deviceName) : String(ZX_ICON_SPEAKER "  No output device"));
    ui::Spacing();
    for (u32 b = 0; b < u32(audio::Bus::Count); ++b) {
        const audio::Bus bus = audio::Bus(b);
        ui::PushID(i32(b));
        ui::AlignTextToFramePadding();
        ui::PushFont(b == 0 ? ui::FontStyle::SemiBold : ui::FontStyle::Regular);
        ui::Text(audio::BusName(bus));
        ui::PopFont();
        ui::SameLine(70.0f);
        bool muted = a.IsBusMuted(bus);
        if (ui::IconButton(muted ? ZX_ICON_VOLUME_X : ZX_ICON_VOLUME_2, muted ? "Unmute" : "Mute", muted))
            a.SetBusMuted(bus, !muted);
        ui::SameLine(0.0f, 6.0f);
        f32 db = audio::GainToDb(a.GetBusVolume(bus));
        db = Clamp(db, -60.0f, 12.0f);
        ui::SetNextItemWidth(150.0f);
        if (ui::SliderFloat("##vol", &db, -60.0f, 12.0f, "{:.1f} dB"))
            a.SetBusVolume(bus, db <= -59.9f ? 0.0f : audio::DbToGain(db));
        ui::SameLine(0.0f, 8.0f);
        Meter(a.GetBusPeak(bus), Max(60.0f, ui::GetContentRegionAvail().x / ui::GetCurrentContext()->GetScale() - 4.0f));
        ui::PopID();
    }
    ui::Spacing();
    ui::Separator();
    ui::LabelText("Voices", String::Format("{} playing, {} audible, {} virtual, {} streaming", st.voices, st.audible, st.virtualVoices, st.streams));
    ui::LabelText("Mixer", String::Format("{:.3f} ms per {}-frame block", st.mixCpuMs, audio::MixBlock));
    ui::LabelText("Underruns", String::Format("{}", st.underruns));
    if (ed.audio && ed.audio->IsRunning())
        ui::LabelText("Sources", String::Format("{} audio sources playing", ed.audio->PlayingSources()));
    ui::End();
}

bool EditAudioSourceClip(Editor& ed, AudioSource& src)
{
    // Clip slot: the file name, drop a .wav / .ogg, preview button.
    bool changed = false;
    const f32 s = ui::GetCurrentContext()->GetScale();
    const f32 fh = ui::GetFrameHeight() / s;
    ui::AlignTextToFramePadding();
    ui::TextColored(ui::GetCurrentContext()->GetStyle().colors.textDim, "Clip");
    ui::ItemTooltip("Sound file (WAV or Ogg Vorbis) under Content. Drop one from the Content Browser.");
    ui::SameLine(Round(ui::GetContentRegionAvail().x / s * 0.38f));
    ui::SetNextItemWidth(-(fh + 4.0f));
    String shown = src.clip.Empty() ? String("None") : String::Format(ZX_ICON_AUDIO_WAVEFORM "  {}", Path::FileName(src.clip));
    ui::InputText("##clip", shown, ui::InputTextFlags::ReadOnly);
    ui::ItemTooltip(src.clip.Empty() ? StringView("Drop a sound (.wav, .ogg) from the Content Browser here") : src.clip.View());
    if (const void* payload = ui::AcceptDragDropPayload("asset")) {
        const StringView path(static_cast<const char*>(payload));
        if (Path::HasExtension(path, ".wav") || Path::HasExtension(path, ".ogg")) {
            src.clip = Path::MakeRelative(path, ed.contentRoot);
            changed = true;
        }
    }
    ui::SameLine(0.0f, 4.0f);
    const bool playing = ed.audio && !src.clip.Empty() && ed.audio->PreviewFile() == src.clip;
    ui::BeginDisabled(src.clip.Empty() || !ed.audio);
    if (ui::IconButton(playing ? ZX_ICON_SQUARE : ZX_ICON_PLAY, playing ? "Stop preview" : "Preview", playing))
        ed.audio->TogglePreview(src.clip);
    ui::EndDisabled();
    if (!src.clip.Empty() && ed.audio) {
        String error;
        if (RefPtr<audio::AudioClip> clip = ed.audio->LoadClip(src.clip, &error))
            ui::TextDisabled(String::Format("{:.2f} s, {} Hz, {}{}", clip->Duration(), clip->sampleRate, clip->channels == 1 ? "mono" : "stereo",
                                            clip->IsStreamed() ? ", streamed" : ""));
        else
            ui::TextColored(Color(0xF0, 0x60, 0x60), error);
    }
    return changed;
}

} // namespace zx::editor
