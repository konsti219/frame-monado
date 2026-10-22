// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Headset view for other programs: the left eye without lens distortion in SteamVR's v4l2loopback device
 *         (`/dev/video99`, 1920x1080 RGB3), drawn only while a program reads it.
 * @ingroup comp_main
 */

#pragma once

#include "xrt/xrt_defines.h"
#include <stdbool.h>

struct comp_compositor;
struct comp_layer;
struct comp_render_dispatch_data;
struct sf_comp_mirror;

// NULL when turned off ("frame": {"mirror": false} in Monado's config file)
struct sf_comp_mirror *
sf_comp_mirror_create(struct comp_compositor *c);

// Whether to draw the mirror for this frame: someone reads it and it's time for the next frame
bool
sf_comp_mirror_due(struct sf_comp_mirror *m, uint64_t predicted_display_time_ns);

// Draws view 0 from this frame's dispatch data and scratch images and sends it; the queue must be idle
void
sf_comp_mirror_draw(struct sf_comp_mirror *m,
                    const struct comp_layer *layers,
                    uint32_t layer_count,
                    const struct comp_render_dispatch_data *d,
                    uint64_t frame_id,
                    uint64_t predicted_display_time_ns);

void
sf_comp_mirror_destroy(struct sf_comp_mirror **m);
