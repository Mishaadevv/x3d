#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/AudioSystem.h"
#include "Engine/PhysicsSystem.h"
#include "Engine/Project.h"
#include "Engine/Scripting.h"
#include "Scene/Hierarchy.h"

#include <cmath>

using namespace zx;

namespace {

String SoundRoot(StringView name)
{
    const String dir = Path::Join(Path::Normalize(platform::GetTempDirectory()), String::Format("zx_audio_test/{}", name));
    platform::DeleteDirectoryTree(dir);
    platform::CreateDirectoryTree(dir + "/Sounds");
    audio::PcmData tone;
    tone.sampleRate = 48000;
    tone.channels = 1;
    tone.samples.Resize(48000);
    for (u32 i = 0; i < 48000; ++i)
        tone.samples[i] = 0.5f * std::sin(TwoPi * 440.0f * f32(i) / 48000.0f);
    fs::WriteFile(dir + "/Sounds/Tone.wav", audio::EncodeWav(tone).View());
    return dir;
}

struct Fixture
{
    audio::AudioEngine engine;
    UniquePtr<AudioSystem> system;
    ecs::World world;
    Fixture(StringView root)
    {
        audio::AudioSettings s;
        s.backend = audio::AudioSettings::Backend::Null;
        engine.Initialize(s);
        system = MakeUnique<AudioSystem>(engine);
        system->SetContentRoot(root);
        scene::InstallHierarchyHooks(world);
    }
    void Step(u32 frames = 1)
    {
        f32 buffer[480 * 2];
        for (u32 i = 0; i < frames; ++i) {
            scene::UpdateWorldTransforms(world);
            system->Tick(world, 0.01f, ListenerPose{});
            engine.RenderOffline(buffer, 480);
        }
    }
};

} // namespace

ZX_TEST(AudioSystem, SourcesFollowEntities)
{
    const String root = SoundRoot("sources");
    Fixture f(root);
    AudioSource src;
    src.clip = "Sounds/Tone.wav";
    const ecs::Entity a = scene::CreateNode(f.world, "A", Transform(Vec3(5, 0, 0)));
    f.world.Add(a, src);
    src.playOnStart = false;
    const ecs::Entity b = scene::CreateNode(f.world, "B", Transform(Vec3(0, 0, 5)));
    f.world.Add(b, src);
    f.system->Begin(f.world);
    f.Step(3);
    ZX_EXPECT_EQ(f.system->PlayingSources(), 1u); // only playOnStart
    ZX_EXPECT(f.system->PlayEntity(f.world, b));
    f.Step(3);
    ZX_EXPECT_EQ(f.system->PlayingSources(), 2u);
    ZX_EXPECT_EQ(f.engine.GetStats().voices, 2u);

    // Removing an entity stops its sound; End stops everything.
    scene::DestroyHierarchy(f.world, a);
    f.Step(10);
    ZX_EXPECT_EQ(f.system->PlayingSources(), 1u);
    f.system->End();
    f.Step(10);
    ZX_EXPECT_EQ(f.engine.GetStats().voices, 0u);

    // Missing files fail cleanly; one-shots play.
    String error;
    ZX_EXPECT(!f.system->LoadClip("Sounds/Nope.wav", &error));
    ZX_EXPECT(error.Contains("not found"));
    ZX_EXPECT(f.system->PlayOneShot("Sounds/Tone.wav") != 0u);
    platform::DeleteDirectoryTree(root);
}

ZX_TEST(AudioSystem, StarterSoundsAndScripts)
{
    // Starter Content sounds decode, and loops are seamless (no jump at the loop point).
    const String root = SoundRoot("starter");
    ZX_REQUIRE(WriteStarterContent(root) > 10u);
    for (StringView name : {StringView("UI_Click.wav"), StringView("Chime.wav"), StringView("Hum_Loop.wav"), StringView("Wind_Loop.ogg"),
                            StringView("Waves_Loop.ogg"), StringView("Fire_Loop.ogg"), StringView("Rain_Loop.ogg"),
                            StringView("Props/Engine_Loop.ogg"), StringView("Footsteps/Footstep_Grass_2.ogg")}) {
        Result<RefPtr<audio::AudioClip>> clip = audio::LoadAudioClipFile(String::Format("{}/StarterContent/Sounds/{}", root, name));
        ZX_REQUIRE(clip.IsOk());
        Array<f32> s = (*clip)->samples;
        u32 channels = (*clip)->channels;
        if (s.Empty()) { // long files stream: decode the whole thing for the check
            Array<u8> bytes;
            ZX_REQUIRE(fs::ReadFile(String::Format("{}/StarterContent/Sounds/{}", root, name), bytes));
            Result<audio::PcmData> pcm = audio::DecodeVorbis(bytes.View());
            ZX_REQUIRE(pcm.IsOk());
            channels = pcm.Value().channels;
            s = move(pcm.Value().samples);
        }
        ZX_REQUIRE(s.Size() > usize(1000) && channels > 0);
        // Seamless: the last frame continues into the first (left channel of both).
        const f32 last = s[s.Size() - channels], first = s[0];
        if (name.Contains("Loop") && Abs(last - first) >= 0.08f)
            test::ReportFailure(__FILE__, __LINE__, String::Format("{}: loop seam {} -> {}", name, last, first).View());
    }

    // ZeqouScript: audio.play and entity.playSound.
    Fixture f(root);
    ScriptSystem scripts;
    scripts.SetContentRoot(root);
    scripts.audio = f.system.Get();
    u32 errors = 0;
    scripts.onLog = [&](StringView, bool isError) { errors += isError; };
    fs::WriteTextFile(root + "/Sounds/Player.zs", R"zs(
        script Player {
            fn start() {
                audio.play("Sounds/Tone.wav", 0.5)
                entity.playSound()
            }
        }
    )zs");
    const ecs::Entity e = scene::CreateNode(f.world, "E", Transform::Identity());
    AudioSource src;
    src.clip = "Sounds/Tone.wav";
    src.playOnStart = false;
    f.world.Add(e, src);
    f.world.Add(e, Script{String("Sounds/Player.zs"), String()});
    f.system->Begin(f.world);
    scripts.Begin(f.world);
    scripts.Tick(f.world, 0.01f, nullptr);
    f.Step(2);
    ZX_EXPECT_EQ(errors, 0u);
    ZX_EXPECT_EQ(f.engine.GetStats().voices, 2u);
    scripts.End();
    f.system->End();
    platform::DeleteDirectoryTree(root);
}

ZX_TEST(AudioSystem, VariantsAndImpactSounds)
{
    const String root = SoundRoot("impacts");
    for (u32 i = 0; i < 3; ++i)
        platform::CopyFileAt(root + "/Sounds/Tone.wav", String::Format("{}/Sounds/Hit_{}.wav", root, i));
    Fixture f(root);
    // A set of variants: every pick is one of the existing files.
    for (u32 i = 0; i < 20; ++i) {
        const String pick = f.system->PickVariant("Sounds/Hit");
        ZX_EXPECT(pick == "Sounds/Hit_0.wav" || pick == "Sounds/Hit_1.wav" || pick == "Sounds/Hit_2.wav");
    }
    ZX_EXPECT(f.system->PickVariant("Sounds/Tone.wav") == "Sounds/Tone.wav");
    ZX_EXPECT(f.system->PickVariant("Sounds/Missing").Empty());

    const ecs::Entity crate = f.world.Create(Transform::Identity());
    ImpactSound s;
    s.sound = "Sounds/Hit";
    f.world.Add(crate, s);
    f.system->Begin(f.world);
    PhysicsEvent soft;
    soft.a = crate;
    soft.speed = 0.2f; // below Min Speed: silent
    f.system->PlayImpacts(f.world, Span<const PhysicsEvent>(&soft, 1));
    ZX_EXPECT_EQ(f.engine.GetStats().voices, 0u);
    PhysicsEvent hard = soft;
    hard.speed = 5.0f;
    f.system->PlayImpacts(f.world, Span<const PhysicsEvent>(&hard, 1));
    f.system->PlayImpacts(f.world, Span<const PhysicsEvent>(&hard, 1)); // same instant: one sound per body
    f.Step();
    ZX_EXPECT_EQ(f.engine.GetStats().voices, 1u);
    ZX_EXPECT(f.system->PlayOneShot("Sounds/Hit") != 0); // variant sets work for one-shots (scripts) too
    f.system->End();
}
