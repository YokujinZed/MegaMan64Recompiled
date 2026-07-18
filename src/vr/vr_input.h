#ifndef __VR_INPUT_H__
#define __VR_INPUT_H__

#ifdef RT64_XR_SUPPORT

#include <cstdint>

// VR controller → virtual N64 pad mux. Installed in place of the plain recomp
// input callbacks; forwards to them and ORs in the XR controller state, so
// keyboard/gamepad keep working alongside the VR controllers. With vr_enabled
// off the XR snapshot is permanently inactive and behavior is identical to the
// plain callbacks.
namespace vr {
    void poll_inputs();
    bool get_n64_input(int controller_num, uint16_t *buttons, float *x, float *y);
}

#endif

#endif
