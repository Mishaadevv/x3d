// Progress of loading work, like Unity's progress bars: a modal "Hold On" window while the user has to wait (models being
// imported, a scene or project opening) and a small panel above the status bar for work that runs in the background
// (asset cooking, GPU uploads, material shader and C++ compiles). Tasks show up only after they took a moment, so quick
// loads never flash a window.

#include "Editor.h"
#include "Core/Time/Time.h"
#include "UI/Icons.h"

namespace zx::editor {

namespace {

constexpr f64 ShowDelay = 0.4; // seconds a task must run before it is shown

/// One kind of work tracked across frames: progress of the current batch (done / total since it started).
struct Tracker
{
    bool active = false;
    Stopwatch since;
    u32 base = 0;  // finished count when the batch started
    u32 peak = 0;  // largest pending count seen (for pending-only sources)

    /// Started/finished counters (assets).
    f32 UpdateCounters(bool busy, u32 started, u32 finished)
    {
        Update(busy, finished);
        const u32 total = started - base, done = finished - base;
        return total ? f32(done) / f32(total) : 0.0f;
    }
    /// Only a pending count is known (shaders, uploads): progress against the largest backlog of the batch.
    f32 UpdatePending(u32 pending)
    {
        Update(pending > 0, 0);
        peak = Max(peak, pending);
        return peak ? 1.0f - f32(pending) / f32(peak) : 0.0f;
    }
    void Update(bool busy, u32 finished)
    {
        if (busy && !active) {
            active = true;
            since.Restart();
            base = finished;
            peak = 0;
        } else if (!busy) {
            active = false;
        }
    }
    bool Visible() const { return active && since.ElapsedSeconds() >= ShowDelay; }
};

struct Task
{
    StringView icon;
    String title;
    String detail;
    f32 fraction = -1.0f; // < 0: unknown (spinner)
    f64 seconds = 0.0;
};

struct ProgressState
{
    Tracker assets, uploads, shaders, build;
} g_progress;

void DrawTask(const Task& t, f32 width)
{
    ui::Text(String::Format("{}  {}", t.icon, t.title));
    if (!t.detail.Empty())
        ui::TextDisabled(t.detail);
    if (t.fraction >= 0.0f) {
        ui::ProgressBar(t.fraction, Vec2(width, 0.0f), String::Format("{}%   {:.1f} s", i32(t.fraction * 100.0f + 0.5f), t.seconds));
    } else {
        ui::Spinner(7.0f);
        ui::SameLine(0.0f, 8.0f);
        ui::TextDisabled(String::Format("{:.1f} s", t.seconds));
    }
}

} // namespace

void DrawProgress(Editor& ed)
{
    ProgressState& p = g_progress;
    Array<Task> tasks;
    const asset::AssetManager::Stats as = ed.assets->GetStats();
    const f32 assetFraction = p.assets.UpdateCounters(as.inFlight > 0, as.jobsStarted, as.jobsFinished);
    if (p.assets.Visible()) {
        Task t;
        t.icon = ZX_ICON_PACKAGE;
        const u32 total = as.jobsStarted - p.assets.base, done = as.jobsFinished - p.assets.base;
        t.title = String::Format("{} assets ({} / {})", ed.pendingImports.Empty() ? "Loading" : "Importing", done, total);
        const String work = ed.assets->CurrentWork();
        t.detail = work.Empty() ? String() : String::Format("Cooking {}", work);
        t.fraction = total >= 4 ? assetFraction : -1.0f; // a single big cook: no meaningful percentage
        t.seconds = p.assets.since.ElapsedSeconds();
        tasks.PushBack(move(t));
    }
    const u32 uploads = ed.renderAssets->GetStats().pendingUploads;
    const f32 uploadFraction = p.uploads.UpdatePending(uploads);
    if (p.uploads.Visible()) {
        Task t;
        t.icon = ZX_ICON_UPLOAD;
        t.title = String::Format("Uploading to the GPU ({} left)", uploads);
        t.fraction = uploadFraction;
        t.seconds = p.uploads.since.ElapsedSeconds();
        tasks.PushBack(move(t));
    }
    const u32 shaders = ed.renderAssets->Shaders().PendingCount();
    const f32 shaderFraction = p.shaders.UpdatePending(shaders);
    if (p.shaders.Visible()) {
        Task t;
        t.icon = ZX_ICON_PALETTE;
        t.title = String::Format("Compiling material shaders ({} left)", shaders);
        t.fraction = shaderFraction;
        t.seconds = p.shaders.since.ElapsedSeconds();
        tasks.PushBack(move(t));
    }
    p.build.Update(ed.gameBuildState == GameBuildState::Building, 0);
    if (p.build.Visible()) {
        Task t;
        t.icon = ZX_ICON_HAMMER;
        t.title = "Compiling C++ game code";
        t.detail = "MSVC + Ninja; errors go to the Output Log";
        t.seconds = p.build.since.ElapsedSeconds();
        tasks.PushBack(move(t));
    }

    // The user is waiting when models are being placed or a scene is opening: block the editor like Unity does.
    if (ed.waitForAssets && as.inFlight == 0 && uploads == 0 && ed.pendingImports.Empty())
        ed.waitForAssets = false;
    const bool waiting = (ed.waitForAssets || !ed.pendingImports.Empty()) && p.assets.Visible();
    static constexpr StringView ModalTitle = "Hold On";
    if (waiting && !ui::IsPopupOpen(ModalTitle))
        ui::OpenPopup(ModalTitle);
    if (ui::BeginPopupModal(ModalTitle)) {
        if (!waiting) {
            ui::CloseCurrentPopup();
        } else {
            ui::TextDisabled(ed.pendingImports.Empty() ? String::Format("Opening '{}'", ed.sceneName)
                                                       : String::Format("Importing {}", ed.pendingImports[0].name));
            ui::Spacing();
            for (const Task& t : tasks) {
                DrawTask(t, 440.0f);
                ui::Spacing();
            }
        }
        ui::EndPopup();
        return;
    }
    if (tasks.Empty() || ed.mode != EditorMode::Editing)
        return;

    // Background work: a panel in the bottom-right corner, above the status bar.
    ui::Context& ctx = *ui::GetCurrentContext();
    const f32 s = ctx.GetScale();
    const Vec2 display = Vec2(f32(ed.swapChain->GetSize().x), f32(ed.swapChain->GetSize().y)) / s;
    constexpr f32 Width = 340.0f, Margin = 12.0f, StatusBar = 26.0f;
    f32 height = 16.0f;
    for (const Task& t : tasks)
        height += t.detail.Empty() ? 52.0f : 70.0f;
    ui::SetNextWindowPos(Vec2(display.x - Width - Margin, display.y - StatusBar - Margin - height), ui::Cond::Always);
    ui::SetNextWindowSize(Vec2(Width, height), ui::Cond::Always);
    if (ui::Begin("##BackgroundProgress", nullptr,
                  ui::WindowFlags::NoTitleBar | ui::WindowFlags::NoResize | ui::WindowFlags::NoMove | ui::WindowFlags::NoScrollbar |
                      ui::WindowFlags::NoCollapse)) {
        for (const Task& t : tasks)
            DrawTask(t, Width - 24.0f);
    }
    ui::End();
}

} // namespace zx::editor
