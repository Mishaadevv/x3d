#pragma once

// Sequencer window: the .zseq timeline. Tracks (Transform, Animation, Event, Camera, Audio) are rows, keys sit on a
// time ruler; the playhead scrubs the scene (the targets are posed live while editing and put back when the window
// closes, before Play or before the scene is saved). Space plays, K adds a key at the playhead, Del removes it.

#include "Engine/Sequencer.h"
#include "Core/Math/Geometry.h"
#include "Scene/ECS/World.h"

namespace zx::editor {

class Editor;

struct SeqEditorState
{
    bool open = false;
    String path; // physical
    SequenceAsset seq;
    Array<String> problems; // structural (CheckSequence)
    Array<String> missing;  // tracks whose target / camera is not in the scene
    bool dirty = false;
    bool capturesKeys = false;

    // Playhead / preview
    f32 time = 0.0f;
    f32 prev = -1e-4f; // time of the last preview step (Audio keys fire for keys after it)
    bool playing = false;
    bool loop = false;
    bool preview = true;      // pose the scene while editing
    bool snap = true;         // keys and the playhead snap to frames (1/30 s)
    bool cameraView = false;  // the viewport looks through the camera of the current cut
    bool applyNeeded = true;
    bool wasPlayingGame = false;
    ecs::Entity owner;        // the Sequence Player that plays this file ("@self" targets)
    SequenceSession session;
    bool hasSavedCamera = false;
    Vec3 savedCameraPosition;
    f32 savedYaw = 0.0f, savedPitch = 0.0f, savedFov = 1.0f;

    // View
    f32 scrollT = 0.0f; // seconds at the left edge of the timeline
    f32 pps = 100.0f;   // logical pixels per second
    f32 scrollY = 0.0f;
    bool frameRequested = true;

    // Selection / drag
    i32 selTrack = -1;
    i32 selKey = -1;
    enum class Drag : u8
    {
        None,
        Playhead,
        Key,
        Pan,
    };
    Drag drag = Drag::None;
    Vec2 dragStart, rightPress;
    bool dragMoved = false;
    f32 dragOffset = 0.0f;
    String beforeDrag;
    i32 popupTrack = -1;
    f32 popupTime = 0.0f;

    Array<String> undo, redo; // .zseq snapshots
    String editField;
};

void OpenSequencerEditor(Editor& ed, StringView physicalPath);
void DrawSequencerPanel(Editor& ed);
/// An empty sequence of 5 seconds (returns false if it can't be written).
bool CreateSequenceFile(StringView physicalPath);
/// A "Cinematic" entity with a Sequence Player, a camera child and a ready .zseq that flies the camera forward;
/// opens it in the Sequencer.
void CreateCinematic(Editor& ed);
/// Puts back what the timeline preview posed (call before Play, before saving the scene, before clearing it);
/// the preview resumes by itself afterwards.
void PauseSequencerPreview(Editor& ed);

} // namespace zx::editor
