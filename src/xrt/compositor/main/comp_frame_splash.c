// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Startup splash: the frame shell logo, head-locked on black, until the head is first tracked.
 *
 * Clients render with the head pose from before tracking starts, which shows them rolled with the panels; the
 * splash is in view space, so it's upright without a head pose.
 * @ingroup comp_main
 */

#include "main/comp_frame_splash.h"
#include "main/comp_compositor.h"

#include "render/render_interface.h"
#include "util/comp_layer_accum.h"
#include "util/comp_swapchain.h"
#include "util/u_logging.h"
#include "os/os_time.h"
#include "vk/vk_cmd_pool.h"

#include "frame_shell_logo.h" // frame_shell_logo: generated from frame_shell_logo.rgba

#include <stdlib.h>

#define LOGO_SIZE 256
#define LOGO_DISTANCE_M 1.5f
#define LOGO_WIDTH_M 0.3f

struct sf_comp_splash
{
	struct comp_compositor *c;
	struct xrt_swapchain *xsc;
	bool done;
};

static VkResult
upload_logo(struct vk_bundle *vk, VkImage image)
{
	struct render_buffer staging = {0};
	VkResult ret = render_buffer_init(vk, &staging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	                                  sizeof(frame_shell_logo));
	if (ret != VK_SUCCESS) {
		return ret;
	}
	ret = render_buffer_map_and_write(vk, &staging, (void *)frame_shell_logo, sizeof(frame_shell_logo));
	if (ret != VK_SUCCESS) {
		render_buffer_fini(vk, &staging);
		return ret;
	}

	struct vk_cmd_pool pool = {0};
	ret = vk_cmd_pool_init(vk, &pool, 0);
	if (ret != VK_SUCCESS) {
		render_buffer_fini(vk, &staging);
		return ret;
	}

	vk_cmd_pool_lock(&pool);
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	ret = vk_cmd_pool_create_and_begin_cmd_buffer_locked(vk, &pool, 0, &cmd);
	if (ret == VK_SUCCESS) {
		VkImageSubresourceRange range = {
		    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		    .levelCount = 1,
		    .layerCount = 1,
		};
		vk_cmd_image_barrier_gpu_locked(vk, cmd, image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, range);
		VkBufferImageCopy region = {
		    .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
		    .imageExtent = {LOGO_SIZE, LOGO_SIZE, 1},
		};
		vk->vkCmdCopyBufferToImage(cmd, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		// Where the renderer expects released swapchain images
		vk_cmd_image_barrier_gpu_locked(vk, cmd, image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
		                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, range);
		ret = vk_cmd_pool_end_submit_wait_and_free_cmd_buffer_locked(vk, &pool, cmd);
	}
	vk_cmd_pool_unlock(&pool);

	vk_cmd_pool_destroy(vk, &pool);
	render_buffer_fini(vk, &staging);
	return ret;
}

struct sf_comp_splash *
sf_comp_splash_create(struct comp_compositor *c)
{
	_Static_assert(sizeof(frame_shell_logo) == LOGO_SIZE * LOGO_SIZE * 4, "frame_shell_logo.rgba is 256x256 RGBA");

	struct xrt_swapchain_create_info info = {
	    .bits = XRT_SWAPCHAIN_USAGE_SAMPLED | XRT_SWAPCHAIN_USAGE_TRANSFER_DST,
	    .format = VK_FORMAT_R8G8B8A8_SRGB,
	    .sample_count = 1,
	    .width = LOGO_SIZE,
	    .height = LOGO_SIZE,
	    .face_count = 1,
	    .array_size = 1,
	    .mip_count = 1,
	};
	struct xrt_swapchain *xsc = NULL;
	if (xrt_comp_create_swapchain(&c->base.base.base, &info, &xsc) != XRT_SUCCESS) {
		U_LOG_E("Startup splash: can't create its swapchain");
		return NULL;
	}
	VkResult ret = upload_logo(&c->base.vk, comp_swapchain(xsc)->vkic.images[0].handle);
	if (ret != VK_SUCCESS) {
		U_LOG_E("Startup splash: uploading the logo failed: %s", vk_result_string(ret));
		xrt_swapchain_reference(&xsc, NULL);
		return NULL;
	}

	struct sf_comp_splash *s = calloc(1, sizeof(*s));
	s->c = c;
	s->xsc = xsc;
	return s;
}

void
sf_comp_splash_apply(struct sf_comp_splash *s)
{
	if (s == NULL || s->done) {
		return;
	}
	struct comp_compositor *c = s->c;

	struct xrt_space_relation head = XRT_SPACE_RELATION_ZERO;
	xrt_result_t xret =
	    xrt_device_get_tracked_pose(c->xdev, XRT_INPUT_GENERIC_HEAD_POSE, os_monotonic_get_ns(), &head);
	if (xret == XRT_SUCCESS && (head.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT) != 0) {
		U_LOG_I("Startup splash: head tracked, showing apps");
		s->done = true;
		return;
	}

	struct xrt_layer_frame_data frame = c->base.layer_accum.data;
	frame.env_blend_mode = XRT_BLEND_MODE_OPAQUE;
	comp_layer_accum_begin(&c->base.layer_accum, &frame);

	struct xrt_layer_data layer = {
	    .type = XRT_LAYER_QUAD,
	    .name = XRT_INPUT_GENERIC_HEAD_POSE,
	    .timestamp = frame.display_time_ns,
	    .flags = XRT_LAYER_COMPOSITION_VIEW_SPACE_BIT | XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
	             XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT,
	    .color_scale = {1, 1, 1, 1},
	    .quad =
	        {
	            .visibility = XRT_LAYER_EYE_VISIBILITY_BOTH,
	            .sub =
	                {
	                    .rect = {.extent = {LOGO_SIZE, LOGO_SIZE}},
	                    .norm_rect = {.w = 1, .h = 1},
	                },
	            .pose = {.orientation = {0, 0, 0, 1}, .position = {0, 0, -LOGO_DISTANCE_M}},
	            .size = {LOGO_WIDTH_M, LOGO_WIDTH_M},
	        },
	};
	comp_layer_accum_quad(&c->base.layer_accum, s->xsc, &layer);
}

void
sf_comp_splash_destroy(struct sf_comp_splash **s)
{
	if (*s == NULL) {
		return;
	}
	xrt_swapchain_reference(&(*s)->xsc, NULL);
	free(*s);
	*s = NULL;
}
