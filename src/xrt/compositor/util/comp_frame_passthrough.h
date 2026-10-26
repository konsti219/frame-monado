#pragma once

#include "xrt/xrt_defines.h"
#include <stdbool.h>

struct vk_bundle;
struct render_gfx;
struct comp_render_dispatch_data;
struct comp_layer;
struct sf_comp_passthrough;

struct sf_comp_passthrough *
sf_comp_passthrough_create(struct vk_bundle *vk);
bool
sf_comp_passthrough_request(struct sf_comp_passthrough *pt, bool wanted);
// Whether the frame shell needs floor estimates (calibrating), which come from the cameras' room mesh
bool
sf_comp_passthrough_floor_wanted(struct sf_comp_passthrough *pt);
// Feeds the floor estimate while the cameras run without passthrough drawn
void
sf_comp_passthrough_update_floor(struct sf_comp_passthrough *pt);
void
sf_comp_passthrough_prepare(struct sf_comp_passthrough *pt,
                            struct render_gfx *render,
                            const struct comp_render_dispatch_data *d);
void
sf_comp_passthrough_draw(struct sf_comp_passthrough *pt,
                         struct render_gfx *render,
                         uint32_t view,
                         const struct xrt_fov *fov,
                         const struct xrt_pose *eye_pose,
                         const struct xrt_pose *world_pose);
void
sf_comp_passthrough_destroy(struct sf_comp_passthrough **pt);

