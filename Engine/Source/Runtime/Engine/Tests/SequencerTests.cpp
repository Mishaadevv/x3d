// Sequencer: the .zseq form round-trips and rejects broken files, Transform tracks interpolate (Linear / Step / ends),
// Apply poses the scene and Restore puts it back (transforms and camera cuts), targets resolve by name and "@self/Child",
// Sequence Players start with the game, fire their Event keys once, finish and restore. The component is registered.

#include "Core/HAL/PlatformFile.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/String/Path.h"
#include "Core/Testing/Test.h"
#include "Engine/ComponentRegistry.h"
#include "Engine/Gameplay.h"
#include "Engine/Sequencer.h"
#include "Scene/Hierarchy.h"

using namespace zx;

namespace {

struct Fixture
{
    ecs::World world;
    SequenceSystem seq;
    String dir;

    Fixture()
    {
        RegisterEngineComponents();
        dir = Path::Join(Path::Normalize(platform::GetTempDirectory()), "zx_seq_test");
        platform::CreateDirectoryTree(dir);
        seq.contentRoot = dir;
    }
    ~Fixture() { platform::DeleteDirectoryTree(dir); }

    void Write(StringView name, const SequenceAsset& s) { fs::WriteTextFile(Path::Join(dir, name), WriteSequence(s)); }
    Vec3 Pos(ecs::Entity e) const { return world.Get<const Transform>(e)->position; }
};

SeqTrack MoveTrack(StringView target, Vec3 a, Vec3 b, f32 t0, f32 t1, SeqInterp interp)
{
    SeqTrack t;
    t.type = SeqTrackType::Transform;
    t.target = String(target);
    SeqKey k0;
    k0.time = t0;
    k0.position = a;
    k0.interp = interp;
    SeqKey k1 = k0;
    k1.time = t1;
    k1.position = b;
    t.keys.PushBack(k0);
    t.keys.PushBack(k1);
    return t;
}

SeqTrack EventTrack(StringView target, f32 time, StringView name)
{
    SeqTrack t;
    t.type = SeqTrackType::Event;
    t.target = String(target);
    SeqKey k;
    k.time = time;
    k.text = String(name);
    t.keys.PushBack(k);
    return t;
}

} // namespace

ZX_TEST(Sequencer, FileRoundTripAndValidation)
{
    SequenceAsset s = MakeCameraFlyover("Cine", Vec3(0, 2, 0), Vec3(10, 2, 0), 90.0f, 10.0f, 6.0f);
    s.tracks.PushBack(EventTrack("@self", 2.5f, "boom"));
    SequenceAsset back;
    String error;
    ZX_REQUIRE(ParseSequence(WriteSequence(s).View(), back, &error));
    ZX_EXPECT_NEAR(back.duration, 6.0f, 1e-4f);
    ZX_REQUIRE(back.tracks.Size() == s.tracks.Size());
    for (usize i = 0; i < s.tracks.Size(); ++i) {
        ZX_EXPECT(back.tracks[i].type == s.tracks[i].type);
        ZX_EXPECT(back.tracks[i].target == s.tracks[i].target);
        ZX_REQUIRE(back.tracks[i].keys.Size() == s.tracks[i].keys.Size());
        for (usize k = 0; k < s.tracks[i].keys.Size(); ++k) {
            ZX_EXPECT_NEAR(back.tracks[i].keys[k].time, s.tracks[i].keys[k].time, 1e-4f);
            ZX_EXPECT(back.tracks[i].keys[k].text == s.tracks[i].keys[k].text);
            ZX_EXPECT_NEAR(back.tracks[i].keys[k].position.x, s.tracks[i].keys[k].position.x, 1e-4f);
        }
    }
    ZX_EXPECT(!ParseSequence("{\"format\":\"zbt\"}", back, &error));
    ZX_EXPECT(!ParseSequence("not json", back, &error));

    Array<String> problems;
    CheckSequence(s, problems);
    ZX_EXPECT(problems.Empty());
    SequenceAsset broken;
    SeqTrack empty;
    empty.type = SeqTrackType::Transform;
    broken.tracks.PushBack(empty);
    CheckSequence(broken, problems);
    ZX_EXPECT(!problems.Empty());
}

ZX_TEST(Sequencer, TransformInterpolation)
{
    const SeqTrack lin = MoveTrack("x", Vec3(0, 0, 0), Vec3(10, 0, 0), 1.0f, 3.0f, SeqInterp::Linear);
    ZX_EXPECT_NEAR(SampleTransformTrack(lin, 0.0f).position.x, 0.0f, 1e-4f); // before the first key: that key
    ZX_EXPECT_NEAR(SampleTransformTrack(lin, 2.0f).position.x, 5.0f, 1e-3f);
    ZX_EXPECT_NEAR(SampleTransformTrack(lin, 9.0f).position.x, 10.0f, 1e-4f); // after the last key: that key
    const SeqTrack step = MoveTrack("x", Vec3(0, 0, 0), Vec3(10, 0, 0), 1.0f, 3.0f, SeqInterp::Step);
    ZX_EXPECT_NEAR(SampleTransformTrack(step, 2.9f).position.x, 0.0f, 1e-4f);
    ZX_EXPECT_NEAR(SampleTransformTrack(step, 3.0f).position.x, 10.0f, 1e-4f);
    const SeqTrack smooth = MoveTrack("x", Vec3(0, 0, 0), Vec3(10, 0, 0), 0.0f, 2.0f, SeqInterp::Smooth);
    ZX_EXPECT_NEAR(SampleTransformTrack(smooth, 1.0f).position.x, 5.0f, 1e-3f); // symmetric ease
    ZX_EXPECT_LT(SampleTransformTrack(smooth, 0.4f).position.x, 2.0f);           // slower than linear at the start
}

ZX_TEST(Sequencer, ApplyAndRestore)
{
    Fixture f;
    const ecs::Entity owner = scene::CreateNode(f.world, "Owner", Transform(Vec3(0, 0, 0)));
    const ecs::Entity child = scene::CreateNode(f.world, "Prop", Transform(Vec3(1, 1, 1)), owner);
    const ecs::Entity box = scene::CreateNode(f.world, "Box", Transform(Vec3(-5, 0, 0)));
    const ecs::Entity game = scene::CreateNode(f.world, "GameCam");
    f.world.Add(game, Camera{});
    const ecs::Entity cine = scene::CreateNode(f.world, "Cine");
    Camera off;
    off.active = false;
    f.world.Add(cine, off);

    SequenceSession session;
    ZX_EXPECT(f.seq.Resolve(f.world, session, "@self/Prop", owner) == child);
    ZX_EXPECT(f.seq.Resolve(f.world, session, "Owner/Prop", owner) == child);
    ZX_EXPECT(f.seq.Resolve(f.world, session, "@self", owner) == owner);
    ZX_EXPECT(!f.seq.Resolve(f.world, session, "Nobody", owner));

    SequenceAsset s;
    s.duration = 4.0f;
    s.tracks.PushBack(MoveTrack("Box", Vec3(0, 0, 0), Vec3(4, 0, 0), 0.0f, 4.0f, SeqInterp::Linear));
    SeqTrack cuts;
    cuts.type = SeqTrackType::Camera;
    SeqKey c0;
    c0.time = 1.0f;
    c0.text = "Cine";
    c0.fov = 30.0f;
    SeqKey c1;
    c1.time = 3.0f; // empty: back to the game camera
    cuts.keys.PushBack(c0);
    cuts.keys.PushBack(c1);
    s.tracks.PushBack(cuts);

    f.seq.Apply(f.world, s, owner, 2.0f, session);
    ZX_EXPECT_NEAR(f.Pos(box).x, 2.0f, 1e-3f);
    ZX_EXPECT(f.world.Get<const Camera>(cine)->active);
    ZX_EXPECT(!f.world.Get<const Camera>(game)->active);
    ZX_EXPECT_NEAR(f.world.Get<const Camera>(cine)->fieldOfView, 30.0f, 1e-4f);

    f.seq.Apply(f.world, s, owner, 3.5f, session); // released
    ZX_EXPECT(!f.world.Get<const Camera>(cine)->active);
    ZX_EXPECT(f.world.Get<const Camera>(game)->active);

    f.seq.Apply(f.world, s, owner, 2.0f, session);
    f.seq.Restore(f.world, session);
    ZX_EXPECT_NEAR(f.Pos(box).x, -5.0f, 1e-4f);
    ZX_EXPECT(f.world.Get<const Camera>(game)->active);
    ZX_EXPECT(!f.world.Get<const Camera>(cine)->active);
    ZX_EXPECT_NEAR(f.world.Get<const Camera>(cine)->fieldOfView, 70.0f, 1e-4f);
}

ZX_TEST(Sequencer, PlayerRunsFiresAndRestores)
{
    Fixture f;
    SequenceAsset s;
    s.duration = 1.0f;
    s.tracks.PushBack(MoveTrack("@self", Vec3(0, 0, 0), Vec3(0, 3, 0), 0.0f, 1.0f, SeqInterp::Linear));
    s.tracks.PushBack(EventTrack("@self", 0.5f, "half"));
    f.Write("Intro.zseq", s);

    const ecs::Entity e = scene::CreateNode(f.world, "Door", Transform(Vec3(7, 0, 0)));
    SequencePlayer p;
    p.sequence = "Intro.zseq";
    p.restoreWhenDone = true;
    f.world.Add(e, p);

    f.seq.Begin(f.world); // Play On Start
    ZX_REQUIRE(f.seq.IsPlaying(e));
    u32 fired = 0;
    bool sawMiddle = false;
    for (u32 i = 0; i < 40; ++i) { // 40 * 1/30 s > 1 s
        f.seq.Tick(f.world, 1.0f / 30.0f);
        for (const SequenceSystem::Event& ev : f.seq.Events()) {
            ZX_EXPECT(ev.entity == e);
            ZX_EXPECT(ev.name == "half");
            ++fired;
        }
        if (f.seq.IsPlaying(e) && Abs(f.seq.Time(e) - 0.5f) < 0.02f) {
            ZX_EXPECT_NEAR(f.Pos(e).y, 1.5f, 0.1f);
            sawMiddle = true;
        }
    }
    ZX_EXPECT_EQ(fired, 1u);
    ZX_EXPECT(sawMiddle);
    ZX_EXPECT(!f.seq.IsPlaying(e));
    ZX_EXPECT_NEAR(f.Pos(e).x, 7.0f, 1e-4f); // restored when done
    ZX_EXPECT_NEAR(f.Pos(e).y, 0.0f, 1e-4f);

    // Started by hand (scripts), stopped midway: put back too.
    ZX_REQUIRE(f.seq.Play(f.world, e, 0.25f));
    f.seq.Tick(f.world, 0.25f);
    ZX_EXPECT_GT(f.Pos(e).y, 1.0f);
    f.seq.Stop(f.world, e);
    ZX_EXPECT_NEAR(f.Pos(e).y, 0.0f, 1e-4f);
    ZX_EXPECT(!f.seq.Play(f.world, scene::CreateNode(f.world, "NoPlayer"), 0.0f));
}

ZX_TEST(Sequencer, ComponentRegistered)
{
    RegisterEngineComponents();
    ZX_EXPECT(FindComponentType("SequencePlayer") != nullptr);
}
