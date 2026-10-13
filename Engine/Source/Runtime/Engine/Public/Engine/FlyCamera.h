#pragma once

// Editor-style free camera: hold RMB to look around, WASD to move, Q/E down/up, Shift = fast,
// mouse wheel while RMB is held changes speed (otherwise dolly forward/back).

#include "Core/Math/Math.h"
#include "Platform/Input.h"

namespace zx {

class FlyCamera
{
public:
    Vec3 position = Vec3(0, 2, -6);
    f32 yaw = 0.0f;   // radians, 0 = looking along +Z
    f32 pitch = 0.2f; // radians, positive = looking down
    f32 moveSpeed = 5.0f;   // m/s
    f32 lookSensitivity = 0.0025f;
    f32 fovY = DegToRad(60.0f);
    f32 nearPlane = 0.05f;

    /// Returns true while the camera is being controlled (RMB held) - lock/hide the cursor then.
    bool Update(const InputState& input, f32 dt);
    void LookAt(Vec3 target);

    Quat Rotation() const { return Quat::FromEuler(pitch, yaw, 0.0f); }
    Vec3 Forward() const { return Rotation().GetForward(); }
    Mat4 ViewMatrix() const { return Mat4::LookTo(position, Forward(), Vec3::Up()); }
    Mat4 ProjectionMatrix(f32 aspect) const { return Mat4::PerspectiveInfiniteReversedZ(fovY, aspect, nearPlane); }

private:
    Vec3 m_velocity;
};

} // namespace zx
