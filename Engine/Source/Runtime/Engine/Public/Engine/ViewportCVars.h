#pragma once

namespace zx {

class ViewportRenderer;

/// Binds the render console variables (r.ScreenPercentage, r.GI.Spacing, r.Nano, ...) to `viewport`: they start from
/// its current settings (values set earlier with a higher priority win) and console changes go to it. nullptr unbinds.
/// One viewport at a time: the editor's or the game's.
void BindViewportCVars(ViewportRenderer* viewport);

} // namespace zx
