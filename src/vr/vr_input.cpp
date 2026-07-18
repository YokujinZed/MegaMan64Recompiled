#include "vr_input.h"

#ifdef RT64_XR_SUPPORT

#include <algorithm>

#include "recomp_input.h"
#include "zelda_render.h"

// N64 pad bits (see DEFINE_N64_BUTTON_INPUTS in recomp_input.h).
constexpr uint16_t N64_A = 0x8000;       // Jump
constexpr uint16_t N64_B = 0x4000;       // Buster
constexpr uint16_t N64_Z = 0x2000;       // Strafe/Rotate L
constexpr uint16_t N64_START = 0x1000;
constexpr uint16_t N64_DPAD_UP = 0x0800; // D-pad is MM64's movement control
constexpr uint16_t N64_DPAD_DOWN = 0x0400;
constexpr uint16_t N64_DPAD_LEFT = 0x0200;
constexpr uint16_t N64_DPAD_RIGHT = 0x0100;
constexpr uint16_t N64_L = 0x0020;       // Strafe/Rotate L
constexpr uint16_t N64_R = 0x0010;       // Strafe/Rotate R
constexpr uint16_t N64_C_UP = 0x0008;    // Map
constexpr uint16_t N64_C_DOWN = 0x0004;  // Interact
constexpr uint16_t N64_C_LEFT = 0x0002;  // Special Weapon
constexpr uint16_t N64_C_RIGHT = 0x0001; // Look

constexpr float dpad_threshold = 0.4f;
constexpr float rotate_threshold = 0.5f;
constexpr float trigger_threshold = 0.6f;
constexpr float squeeze_threshold = 0.7f;

void vr::poll_inputs() {
    // XR action sampling runs on the XR frame thread; the SI thread only ever
    // reads snapshots, so there is nothing extra to poll here.
    recomp::poll_inputs();
}

bool vr::get_n64_input(int controller_num, uint16_t *buttons, float *x, float *y) {
    const bool connected = recomp::get_n64_input(controller_num, buttons, x, y);

    // Contract with osContGetReadData: ports 1-3 report disconnected and their
    // outputs must stay untouched.
    if (controller_num != 0) {
        return connected;
    }

    // Mirror the gate recomp::get_n64_input applies to its own inputs: while a
    // menu captures input, the game must keep seeing a neutral pad.
    if (recomp::game_input_disabled()) {
        return connected;
    }

    const RT64::XRInputSnapshot snap = zelda64::renderer::sample_vr_input();
    if (!snap.left.active && !snap.right.active) {
        return connected;
    }

    uint16_t vr_buttons = 0;

    // Left controller: locomotion + support buttons.
    if (snap.left.stickY > dpad_threshold)  vr_buttons |= N64_DPAD_UP;
    if (snap.left.stickY < -dpad_threshold) vr_buttons |= N64_DPAD_DOWN;
    if (snap.left.stickX < -dpad_threshold) vr_buttons |= N64_DPAD_LEFT;
    if (snap.left.stickX > dpad_threshold)  vr_buttons |= N64_DPAD_RIGHT;
    if (snap.left.trigger > trigger_threshold) vr_buttons |= N64_Z;
    if (snap.left.squeeze > squeeze_threshold) vr_buttons |= N64_L;
    if (snap.left.primaryButton)   vr_buttons |= N64_C_LEFT;  // X: Special Weapon
    if (snap.left.secondaryButton) vr_buttons |= N64_C_UP;    // Y: Map
    if (snap.left.menuButton)      vr_buttons |= N64_START;

    // Right controller: rotation + action buttons.
    if (snap.right.stickX < -rotate_threshold) vr_buttons |= N64_L;
    if (snap.right.stickX > rotate_threshold)  vr_buttons |= N64_R;
    if (snap.right.stickClick) vr_buttons |= N64_C_RIGHT;     // Look
    if (snap.right.trigger > trigger_threshold) vr_buttons |= N64_B;
    if (snap.right.squeeze > squeeze_threshold) vr_buttons |= N64_R;
    if (snap.right.primaryButton)   vr_buttons |= N64_A;      // A: Jump
    if (snap.right.secondaryButton) vr_buttons |= N64_C_DOWN; // B: Interact

    *buttons |= vr_buttons;

    // Also feed the left stick into the analog axes in case any code path reads
    // them (movement is d-pad driven in MM64, but this costs nothing).
    *x = std::clamp(*x + snap.left.stickX, -1.0f, 1.0f);
    *y = std::clamp(*y + snap.left.stickY, -1.0f, 1.0f);

    return connected;
}

#endif
