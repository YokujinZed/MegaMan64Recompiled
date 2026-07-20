#ifndef __PATCH_GRAPHICS_H__
#define __PATCH_GRAPHICS_H__

#include "patch_helpers.h"

DECLARE_FUNC(void, recomp_get_window_resolution, u32*, u32*);
DECLARE_FUNC(float, recomp_get_target_aspect_ratio, float);
DECLARE_FUNC(float, recomp_get_target_hud_aspect_ratio, float);
DECLARE_FUNC(s32, recomp_get_target_framerate, s32);
DECLARE_FUNC(s32, recomp_high_precision_fb_enabled);
DECLARE_FUNC(float, recomp_get_resolution_scale);
// VR first person: hand the host the live player state each frame. The host
// reads the fields it needs by offset, so extending this needs no ABI change.
// frameSeq stamps the sample so the renderer can match it to a frame instead
// of reading "latest" (the patch runs ahead of the workload thread).
DECLARE_FUNC(void, recomp_set_player_pose, void*, u32);

#endif
