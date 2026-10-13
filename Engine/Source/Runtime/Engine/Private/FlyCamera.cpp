#include "Engine/FlyCamera.h"

namespace zx {

bool FlyCamera::Update(const InputState& input, f32 dt)
{
    const bool active = input.IsMouseDown(MouseButton::Right);
    if (active) {
        yaw += input.MouseDelta().x * lookSensitivity;
        pitch = Clamp(pitch + input.MouseDelta().y * lookSensitivity, -1.55f, 1.55f);
        if (input.MouseWheel() != 0.0f)
            moveSpeed = Clamp(moveSpeed * Pow(1.2f, input.MouseWheel()), 0.1f, 500.0f);
    }
    Vec3 wish;
    if (active) {
        const Quat rot = Rotation();
        if (input.IsKeyDown(Key::W)) wish += rot.GetForward();
        if (input.IsKeyDown(Key::S)) wish -= rot.GetForward();
        if (input.IsKeyDown(Key::D)) wish += rot.GetRight();
        if (input.IsKeyDown(Key::A)) wish -= rot.GetRight();
        if (input.IsKeyDown(Key::E)) wish += Vec3::Up();
        if (input.IsKeyDown(Key::Q)) wish -= Vec3::Up();
    } else if (input.MouseWheel() != 0.0f) {
        position += Forward() * input.MouseWheel() * moveSpeed * 0.2f;
    }
    const f32 speed = moveSpeed * (input.IsKeyDown(Key::LeftShift) ? 4.0f : 1.0f);
    const Vec3 target = SafeNormalize(wish) * speed;
    // Smooth acceleration (half-life 60 ms) for a pleasant feel.
    const f32 blend = 1.0f - Exp2(-dt / 0.06f);
    m_velocity = Lerp(m_velocity, target, blend);
    position += m_velocity * dt;
    return active;
}

void FlyCamera::LookAt(Vec3 target)
{
    const Vec3 d = SafeNormalize(target - position, Vec3::Forward());
    yaw = Atan2(d.x, d.z);
    pitch = -Asin(d.y);
}

} // namespace zx
