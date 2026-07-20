#include "vr_input.h"

#ifdef RT64_XR_SUPPORT

#include <algorithm>
#include <cmath>

#include "recomp_input.h"
#include "zelda_config.h"
#include "zelda_render.h"

// N64 pad bits (see DEFINE_N64_BUTTON_INPUTS in recomp_input.h).
// MM64 tank controls: d-pad up/down = walk, d-pad left/right = TURN,
// L/R shoulder buttons = STRAFE.
constexpr uint16_t N64_A = 0x8000;       // Jump
constexpr uint16_t N64_B = 0x4000;       // Buster
constexpr uint16_t N64_Z = 0x2000;
constexpr uint16_t N64_START = 0x1000;
constexpr uint16_t N64_DPAD_UP = 0x0800;
constexpr uint16_t N64_DPAD_DOWN = 0x0400;
constexpr uint16_t N64_DPAD_LEFT = 0x0200;
constexpr uint16_t N64_DPAD_RIGHT = 0x0100;
constexpr uint16_t N64_L = 0x0020;       // Strafe left
constexpr uint16_t N64_R = 0x0010;       // Strafe right
constexpr uint16_t N64_C_UP = 0x0008;    // Map
constexpr uint16_t N64_C_DOWN = 0x0004;  // Interact
constexpr uint16_t N64_C_LEFT = 0x0002;  // Special Weapon
constexpr uint16_t N64_C_RIGHT = 0x0001; // Look

constexpr float trigger_threshold = 0.6f;
constexpr float squeeze_threshold = 0.7f;

// Per-axis engage/release hysteresis. Each stick axis has exactly one meaning
// (left Y = walk, left X = strafe, right X = turn), so simple sticky
// thresholds replace the old 8-way sector synthesis.
constexpr float axis_engage = 0.40f;
constexpr float axis_release = 0.28f;

namespace {
    // Sticky positive/negative latch for one axis; directions are exclusive.
    void axis_latch(float v, bool &pos, bool &neg) {
        if (pos && (v < axis_release)) {
            pos = false;
        }
        if (neg && (v > -axis_release)) {
            neg = false;
        }
        if (!pos && !neg) {
            if (v > axis_engage) {
                pos = true;
            }
            else if (v < -axis_engage) {
                neg = true;
            }
        }
    }
}

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
        zelda64::renderer::set_vr_follow_injecting(false);
        return connected;
    }

    const RT64::XRInputSnapshot snap = zelda64::renderer::sample_vr_input();
    if (!snap.left.active && !snap.right.active) {
        zelda64::renderer::set_vr_follow_injecting(false);
        return connected;
    }

    uint16_t vr_buttons = 0;

    // Movement latches persist across polls (single SI-thread caller).
    static bool walkForward = false, walkBack = false;
    static bool strafeLeft = false, strafeRight = false;
    static bool turnLeft = false, turnRight = false;
    axis_latch(snap.left.stickY, walkForward, walkBack);
    axis_latch(snap.left.stickX, strafeRight, strafeLeft);
    axis_latch(snap.right.stickX, turnRight, turnLeft);

    // Left stick: walk (d-pad up/down) + strafe (L/R buttons).
    if (walkForward) vr_buttons |= N64_DPAD_UP;
    if (walkBack)    vr_buttons |= N64_DPAD_DOWN;
    if (strafeLeft)  vr_buttons |= N64_L;
    if (strafeRight) vr_buttons |= N64_R;

    // Right stick: turn only (d-pad left/right).
    if (turnLeft)  vr_buttons |= N64_DPAD_LEFT;
    if (turnRight) vr_buttons |= N64_DPAD_RIGHT;

    // Buttons.
    if (snap.left.trigger > trigger_threshold) vr_buttons |= N64_Z;
    if (snap.left.primaryButton)   vr_buttons |= N64_C_LEFT;  // X: Special Weapon
    if (snap.left.secondaryButton) vr_buttons |= N64_C_UP;    // Y: Map
    if (snap.left.menuButton)      vr_buttons |= N64_START;
    if (snap.left.squeeze > squeeze_threshold)  vr_buttons |= N64_L; // alt strafe
    if (snap.right.squeeze > squeeze_threshold) vr_buttons |= N64_R; // alt strafe
    if (snap.right.stickClick) vr_buttons |= N64_C_RIGHT;     // Look
    if (snap.right.trigger > trigger_threshold) vr_buttons |= N64_B;
    if (snap.right.primaryButton)   vr_buttons |= N64_A;      // A: Jump
    if (snap.right.secondaryButton) vr_buttons |= N64_C_DOWN; // B: Interact

    // Camera-follow: only while the player is standing still (no walk, strafe
    // or turn input from any source), gaze pulls the character around by
    // injecting the game's TURN input; the anti-spin transfer keeps the world
    // visually pinned while it happens. While moving, head-look is camera-only.
    bool follow_injecting = false;
    if (zelda64::get_vr_follow_enabled()) {
        static bool follow_active = false;
        static int follow_polls = 0;
        static int backoff_polls = 0;
        static float follow_start_abs = 0.0f;
        static uint64_t last_generation = 0;

        uint64_t generation = 0;
        const float residual = zelda64::renderer::sample_vr_head_offset_deg(generation);
        const bool fresh = (generation != last_generation);
        last_generation = generation;

        constexpr uint16_t locomotion_mask = N64_DPAD_UP | N64_DPAD_DOWN | N64_DPAD_LEFT | N64_DPAD_RIGHT | N64_L | N64_R | N64_Z;
        const bool user_moving = (((*buttons) | vr_buttons) & locomotion_mask) != 0;
        const float signedResidual = residual * zelda64::get_vr_follow_inject_sign();

        if (std::isnan(residual) || user_moving || (backoff_polls > 0)) {
            follow_active = false;
            follow_polls = 0;
            if (backoff_polls > 0) backoff_polls--;
        }
        else {
            const float absResidual = std::abs(residual);
            if (!follow_active && (absResidual > zelda64::get_vr_follow_engage_deg())) {
                follow_active = true;
                follow_polls = 0;
                follow_start_abs = absResidual;
            }
            else if (follow_active && (absResidual < zelda64::get_vr_follow_release_deg())) {
                follow_active = false;
            }

            if (follow_active) {
                vr_buttons |= (signedResidual > 0.0f) ? N64_DPAD_RIGHT : N64_DPAD_LEFT;
                follow_injecting = true;
                // Watchdog: if a second of injection produced no progress, the
                // camera isn't listening here — back off for ~2 seconds.
                if (fresh) follow_polls++;
                if (follow_polls > 60) {
                    if (absResidual > (follow_start_abs - 2.0f)) {
                        follow_active = false;
                        follow_injecting = false;
                        backoff_polls = 120;
                    }
                    follow_polls = 0;
                    follow_start_abs = absResidual;
                }
            }
        }
    }
    zelda64::renderer::set_vr_follow_injecting(follow_injecting);

    *buttons |= vr_buttons;

    // Analog axes mirror the game's own stick semantics (x = turn, y = walk)
    // so an analog-aware code path always agrees with the injected buttons.
    *x = std::clamp(*x + snap.right.stickX, -1.0f, 1.0f);
    *y = std::clamp(*y + snap.left.stickY, -1.0f, 1.0f);

    return connected;
}

#endif
