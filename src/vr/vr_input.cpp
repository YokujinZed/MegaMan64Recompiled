#include "vr_input.h"

#ifdef RT64_XR_SUPPORT

#include <algorithm>
#include <cmath>

#include "recomp_input.h"
#include "zelda_config.h"
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

constexpr float rotate_threshold = 0.45f;
constexpr float trigger_threshold = 0.6f;
constexpr float squeeze_threshold = 0.7f;

// Sticky 8-way sector mapping for the movement stick. Cardinal sectors are
// 60 degrees wide and diagonals only 30, so pure walk/turn dominate and
// turn-while-walking needs a deliberately diagonal push. The engaged sector is
// held until the stick clearly leaves it (angular + radial hysteresis), which
// is what makes discrete-from-analog input feel stable instead of flickery.
constexpr float engage_radius = 0.42f;
constexpr float release_radius = 0.30f;
constexpr float cardinal_half_width = 30.0f; // degrees
constexpr float sector_hysteresis = 8.0f;    // degrees

namespace {
    struct SectorResult {
        uint16_t bits = 0;
        // Unit direction of the sector center, so analog output can be made
        // consistent with the d-pad bits instead of leaking the raw push.
        float dirX = 0.0f;
        float dirY = 0.0f;
        int sector = -1; // 0 = E, counting counter-clockwise in 45deg steps
    };

    // Sector centers: 0=E(right), 1=NE, 2=N(up), 3=NW, 4=W, 5=SW, 6=S, 7=SE.
    constexpr uint16_t sector_bits[8] = {
        N64_DPAD_RIGHT,
        N64_DPAD_UP | N64_DPAD_RIGHT,
        N64_DPAD_UP,
        N64_DPAD_UP | N64_DPAD_LEFT,
        N64_DPAD_LEFT,
        N64_DPAD_DOWN | N64_DPAD_LEFT,
        N64_DPAD_DOWN,
        N64_DPAD_DOWN | N64_DPAD_RIGHT,
    };

    float angular_distance(float a, float b) {
        float d = std::abs(a - b);
        return (d > 180.0f) ? 360.0f - d : d;
    }

    // Half-width of a sector: cardinals get the wide cones, diagonals the rest
    // (30deg cardinals leave 15deg half-width diagonals).
    float sector_half_width(int sector) {
        return (sector % 2 == 0) ? cardinal_half_width : (45.0f - cardinal_half_width);
    }

    SectorResult map_stick_sector(float x, float y, int held_sector) {
        SectorResult result;
        const float mag = std::sqrt(x * x + y * y);
        const float needed = (held_sector >= 0) ? release_radius : engage_radius;
        if (mag < needed) {
            return result;
        }

        const float angle = std::atan2(y, x) * 57.29578f; // [-180, 180]
        const float wrapped = (angle < 0.0f) ? angle + 360.0f : angle;

        // Stay in the held sector while the angle is within its widened bounds.
        if (held_sector >= 0) {
            const float center = held_sector * 45.0f;
            if (angular_distance(wrapped, center) <= sector_half_width(held_sector) + sector_hysteresis) {
                result.sector = held_sector;
            }
        }

        if (result.sector < 0) {
            // Pick the sector whose (unwidened) bounds contain the angle.
            // Cardinals absorb the tie regions because they are tested first.
            for (int s = 0; s < 8 && result.sector < 0; s += 2) {
                if (angular_distance(wrapped, s * 45.0f) <= cardinal_half_width) {
                    result.sector = s;
                }
            }
            for (int s = 1; s < 8 && result.sector < 0; s += 2) {
                if (angular_distance(wrapped, s * 45.0f) < 45.0f - cardinal_half_width) {
                    result.sector = s;
                }
            }
        }

        if (result.sector >= 0) {
            result.bits = sector_bits[result.sector];
            const float center_rad = result.sector * 45.0f * 0.0174533f;
            result.dirX = std::cos(center_rad) * std::min(mag, 1.0f);
            result.dirY = std::sin(center_rad) * std::min(mag, 1.0f);
        }
        return result;
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
        return connected;
    }

    const RT64::XRInputSnapshot snap = zelda64::renderer::sample_vr_input();
    if (!snap.left.active && !snap.right.active) {
        return connected;
    }

    uint16_t vr_buttons = 0;

    // Left controller: locomotion + support buttons. The held sector persists
    // across polls (single SI-thread caller) for hysteresis.
    static int held_sector = -1;
    const SectorResult move = map_stick_sector(snap.left.stickX, snap.left.stickY, held_sector);
    held_sector = move.sector;
    vr_buttons |= move.bits;
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

    // Camera-follow: inject the game's own rotate button until the camera
    // catches up to the gaze. User rotate input (physical or stick) always
    // wins; a watchdog backs off when the camera provably isn't responding
    // (lock-on, cutscenes, fixed-camera rooms).
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
        const bool user_rotating = ((*buttons | vr_buttons) & (N64_L | N64_R)) != 0;
        const float signedResidual = residual * zelda64::get_vr_follow_inject_sign();

        if (std::isnan(residual) || user_rotating || (backoff_polls > 0)) {
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
                vr_buttons |= (signedResidual > 0.0f) ? N64_R : N64_L;
                // Watchdog: if a second of injection produced no progress, the
                // camera isn't listening here — back off for ~2 seconds.
                if (fresh) follow_polls++;
                if (follow_polls > 60) {
                    if (absResidual > (follow_start_abs - 2.0f)) {
                        follow_active = false;
                        backoff_polls = 120;
                    }
                    follow_polls = 0;
                    follow_start_abs = absResidual;
                }
            }
        }
    }

    *buttons |= vr_buttons;

    // Feed the analog axes the SAME direction the d-pad bits describe (sector
    // center, not the raw push), so a game path reading the stick can never
    // disagree with the synthesized d-pad and cause off-axis drift.
    *x = std::clamp(*x + move.dirX, -1.0f, 1.0f);
    *y = std::clamp(*y + move.dirY, -1.0f, 1.0f);

    return connected;
}

#endif
