// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Headset view in SteamVR's v4l2loopback device, see comp_frame_mirror.h.
 * @ingroup comp_main
 */

#include "main/comp_frame_mirror.h"
#include "main/comp_compositor.h"
#include "main/comp_mirror_to_debug_gui.h"
#include "util/comp_render.h"
#include "render/render_interface.h"
#include "vk/vk_helpers.h"
#include "vk/vk_mini_helpers.h"
#include "util/u_config_json.h"
#include "util/u_json.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "math/m_api.h"
#include "os/os_time.h"

#include <linux/videodev2.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIRROR_DEVICE "/dev/video99"
#define MIRROR_WIDTH 1920
#define MIRROR_HEIGHT 1080
#define MIRROR_FRAME_SIZE (MIRROR_WIDTH * MIRROR_HEIGHT * 3)

// v4l2loopback's event for whether a reader streams (v4l2loopback.c)
#define V4L2_EVENT_PRI_CLIENT_USAGE (V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1)

struct sf_comp_mirror
{
	struct comp_compositor *c;
	struct xrt_frame_sink sink;
	struct comp_mirror_to_debug_gui readback;

	// Target of the undistorted draw, sampled by the readback through the unorm view (bytes as encoded)
	VkImage image;
	VkDeviceMemory memory;
	VkImageView srgb_view;
	VkImageView unorm_view;
	struct render_gfx_render_pass rgrp;
	struct render_gfx_target_resources rtr;
	bool target_ready;

	// Part of view 0 the mirror mesh shows
	struct xrt_vec2 corners[4];
	uint64_t interval_ns;
	uint64_t last_ns;

	// Stabilization: the smoothed orientation the mirror is drawn from
	struct xrt_quat stable;
	uint64_t stable_ns;

	// Writer thread: frames from the readback, woken through an eventfd
	pthread_t thread;
	int wake;
	pthread_mutex_t lock;
	struct xrt_frame *pending;
	atomic_bool watched;
	atomic_bool stop;
};

/// Corners of the mirror in view 0's uv: an upright 16:9 rectangle around the eye's forward direction in tangent
/// space, turned by the eye's roll against the head (the Frame's panels sit upside down and canted), zoomed in from
/// the largest that fits inside the view. Also the angle the view may turn before the rectangle leaves it.
static void
mirror_corners(const struct xrt_fov *fov, const struct xrt_quat *eye, struct xrt_vec2 out[4], float *out_margin)
{
	float l = tanf(fov->angle_left), r = tanf(fov->angle_right);
	float u = tanf(fov->angle_up), dn = tanf(fov->angle_down);

	struct xrt_vec3 x_axis = {1, 0, 0};
	math_quat_rotate_vec3(eye, &x_axis, &x_axis);
	float roll = atan2f(x_axis.y, x_axis.x);
	float c = cosf(roll), s = sinf(roll);

	// Half extents of the largest centred 16:9 rectangle whose turned corners stay inside the view
	const float k = (float)MIRROR_WIDTH / (float)MIRROR_HEIGHT;
	float a = fminf(-l, r), b = fminf(u, -dn);
	float half_h = fminf(a / (k * fabsf(c) + fabsf(s)), b / (k * fabsf(s) + fabsf(c)));
	// Clear of the passthrough camera's edge (81% of the view's nasal half) and apps' hidden areas
	half_h *= 0.75f;
	float half_w = k * half_h;
	*out_margin = fminf(atanf(a) - atanf(half_w), atanf(b) - atanf(half_h));

	for (int i = 0; i < 4; i++) {
		float px = (float)(i % 2 ? 1 : -1) * half_w;
		float py = (float)(i / 2 ? -1 : 1) * half_h;
		// Head-upright direction into the eye's tangent space (the inverse roll)
		float ex = c * px + s * py;
		float ey = -s * px + c * py;
		out[i] = (struct xrt_vec2){(ex - l) / (r - l), (u - ey) / (u - dn)};
	}
}

// Time constant of the stabilization: removes jitter and small head movement, follows turns closely
#define MIRROR_STABILIZATION_S 0.1f

/// The orientation the mirror is drawn from: the eye's, low-pass filtered, never further behind than `margin`
static struct xrt_quat
mirror_stabilize(struct sf_comp_mirror *m, const struct xrt_quat *actual, uint64_t now_ns, float margin)
{
	float dt = (float)((double)((int64_t)(now_ns - m->stable_ns)) * 1e-9);
	if (m->stable_ns == 0 || dt < 0.0f || dt > 0.5f) {
		m->stable = *actual;
	} else {
		math_quat_slerp(&m->stable, actual, 1.0f - expf(-dt / MIRROR_STABILIZATION_S), &m->stable);
	}
	m->stable_ns = now_ns;

	float angle = 2.0f * acosf(fminf(fabsf(math_quat_dot(&m->stable, actual)), 1.0f));
	if (angle > margin) {
		math_quat_slerp(&m->stable, actual, 1.0f - margin / angle, &m->stable);
	}
	return m->stable;
}

static void
mirror_push_frame(struct xrt_frame_sink *sink, struct xrt_frame *frame)
{
	struct sf_comp_mirror *m = container_of(sink, struct sf_comp_mirror, sink);
	pthread_mutex_lock(&m->lock);
	xrt_frame_reference(&m->pending, frame);
	pthread_mutex_unlock(&m->lock);
	uint64_t one = 1;
	(void)!write(m->wake, &one, sizeof(one));
}

static void
mirror_pack_rgb(const struct xrt_frame *frame, uint8_t *rgb)
{
	uint32_t width = MIN(frame->width, MIRROR_WIDTH), height = MIN(frame->height, MIRROR_HEIGHT);
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *src = frame->data + (size_t)y * frame->stride;
		uint8_t *dst = rgb + (size_t)y * MIRROR_WIDTH * 3;
		for (uint32_t x = 0; x < width; x++) {
			dst[x * 3 + 0] = src[x * 4 + 0];
			dst[x * 3 + 1] = src[x * 4 + 1];
			dst[x * 3 + 2] = src[x * 4 + 2];
		}
	}
}

/// Holds the device as its writer (like SteamVR), tracks whether someone reads it, and writes the frames
static void *
mirror_thread(void *ptr)
{
	struct sf_comp_mirror *m = ptr;

	int fd = open(MIRROR_DEVICE, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		U_LOG_W("Mirror: %s: %s", MIRROR_DEVICE, strerror(errno));
		return NULL;
	}
	struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT};
	format.fmt.pix = (struct v4l2_pix_format){
	    .width = MIRROR_WIDTH,
	    .height = MIRROR_HEIGHT,
	    .pixelformat = V4L2_PIX_FMT_RGB24,
	    .field = V4L2_FIELD_NONE,
	    .bytesperline = MIRROR_WIDTH * 3,
	    .sizeimage = MIRROR_FRAME_SIZE,
	    .colorspace = V4L2_COLORSPACE_SRGB,
	};
	struct v4l2_event_subscription subscription = {.type = V4L2_EVENT_PRI_CLIENT_USAGE};
	if (ioctl(fd, VIDIOC_S_FMT, &format) < 0 || format.fmt.pix.width != MIRROR_WIDTH ||
	    format.fmt.pix.height != MIRROR_HEIGHT || format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB24) {
		U_LOG_W("Mirror: %s doesn't take %dx%d RGB3: %s", MIRROR_DEVICE, MIRROR_WIDTH, MIRROR_HEIGHT,
		        strerror(errno));
		close(fd);
		return NULL;
	}
	if (ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription) < 0) {
		U_LOG_W("Mirror: %s doesn't report readers: %s", MIRROR_DEVICE, strerror(errno));
		close(fd);
		return NULL;
	}

	// A first frame makes the device readable
	uint8_t *rgb = calloc(1, MIRROR_FRAME_SIZE);
	(void)!write(fd, rgb, MIRROR_FRAME_SIZE);
	U_LOG_I("Mirror: writing the headset view to %s while it's read", MIRROR_DEVICE);

	struct pollfd fds[2] = {{.fd = fd, .events = POLLPRI}, {.fd = m->wake, .events = POLLIN}};
	while (!atomic_load(&m->stop)) {
		if (poll(fds, 2, -1) < 0 && errno != EINTR) {
			break;
		}
		// Dequeuing blocks once none are left: only as many as are pending
		struct v4l2_event event = {.pending = fds[0].revents & POLLPRI ? 1 : 0};
		while (event.pending > 0 && ioctl(fd, VIDIOC_DQEVENT, &event) == 0) {
			if (event.type == V4L2_EVENT_PRI_CLIENT_USAGE) {
				uint32_t count;
				memcpy(&count, event.u.data, sizeof(count));
				atomic_store(&m->watched, count > 0);
			}
		}
		if (fds[1].revents & POLLIN) {
			uint64_t count;
			(void)!read(m->wake, &count, sizeof(count));
		}

		struct xrt_frame *frame = NULL;
		pthread_mutex_lock(&m->lock);
		frame = m->pending;
		m->pending = NULL;
		pthread_mutex_unlock(&m->lock);
		if (frame != NULL) {
			mirror_pack_rgb(frame, rgb);
			xrt_frame_reference(&frame, NULL);
			(void)!write(fd, rgb, MIRROR_FRAME_SIZE);
		}
	}

	atomic_store(&m->watched, false);
	free(rgb);
	close(fd);
	return NULL;
}

static bool
mirror_target_init(struct sf_comp_mirror *m)
{
	struct comp_compositor *c = m->c;
	struct vk_bundle *vk = &c->base.vk;
	VkExtent2D extent = {MIRROR_WIDTH, MIRROR_HEIGHT};
	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .levelCount = 1,
	    .layerCount = 1,
	};

	VkResult ret = vk_create_image_mutable_rgba(
	    vk, extent, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &m->memory, &m->image);
	if (ret == VK_SUCCESS) {
		ret = vk_create_view_usage(vk, m->image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_SRGB,
		                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, range, &m->srgb_view);
	}
	if (ret == VK_SUCCESS) {
		ret = vk_create_view_usage(vk, m->image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
		                           VK_IMAGE_USAGE_SAMPLED_BIT, range, &m->unorm_view);
	}
	if (ret == VK_SUCCESS) {
		ret = comp_mirror_init(&m->readback, vk, &c->shaders, extent);
	}
	if (ret != VK_SUCCESS) {
		U_LOG_E("Mirror: %s", vk_result_string(ret));
		return false;
	}
	m->readback.sink = &m->sink;

	if (!render_gfx_render_pass_init(&m->rgrp, &c->nr, VK_FORMAT_R8G8B8A8_SRGB, VK_ATTACHMENT_LOAD_OP_CLEAR,
	                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
		return false;
	}
	render_gfx_target_resources_init(&m->rtr, &c->nr, &m->rgrp, m->srgb_view, extent);
	m->target_ready = true;
	return true;
}

struct sf_comp_mirror *
sf_comp_mirror_create(struct comp_compositor *c)
{
	bool enabled = true;
	int fps = 60;
	struct u_config_json json = {0};
	u_config_json_open_or_create_main_file(&json);
	const cJSON *frame = json.root ? u_json_get(json.root, "frame") : NULL;
	if (frame) {
		u_json_get_bool(u_json_get(frame, "mirror"), &enabled);
		u_json_get_int(u_json_get(frame, "mirror_fps"), &fps);
	}
	u_config_json_close(&json);
	if (!enabled) {
		return NULL;
	}

	struct sf_comp_mirror *m = U_TYPED_CALLOC(struct sf_comp_mirror);
	m->c = c;
	m->sink.push_frame = mirror_push_frame;
	m->interval_ns = U_TIME_1S_IN_NS / (uint64_t)CLAMP(fps, 1, 120);
	m->wake = eventfd(0, EFD_CLOEXEC);
	pthread_mutex_init(&m->lock, NULL);
	if (!mirror_target_init(m) || m->wake < 0 || pthread_create(&m->thread, NULL, mirror_thread, m) != 0) {
		m->thread = 0;
		sf_comp_mirror_destroy(&m);
		return NULL;
	}
	return m;
}

bool
sf_comp_mirror_due(struct sf_comp_mirror *m, uint64_t predicted_display_time_ns)
{
	if (m == NULL || !atomic_load(&m->watched)) {
		return false;
	}
	// A little early still counts, display times jitter
	const uint64_t slop_ns = 2 * U_TIME_1MS_IN_NS;
	if (predicted_display_time_ns + slop_ns < m->last_ns + m->interval_ns) {
		return false;
	}
	m->last_ns = predicted_display_time_ns;
	return true;
}

void
sf_comp_mirror_draw(struct sf_comp_mirror *m,
                    const struct comp_layer *layers,
                    uint32_t layer_count,
                    const struct comp_render_dispatch_data *d,
                    uint64_t frame_id,
                    uint64_t predicted_display_time_ns)
{
	struct comp_compositor *c = m->c;
	struct vk_bundle *vk = &c->base.vk;

	struct xrt_vec2 corners[4];
	float margin;
	mirror_corners(&d->views[0].fov, &d->views[0].eye_pose.orientation, corners, &margin);
	if (memcmp(corners, m->corners, sizeof(corners)) != 0) {
		if (!render_resources_set_mirror_mesh(&c->nr, corners)) {
			return;
		}
		memcpy(m->corners, corners, sizeof(corners));
	}

	// View 0 only, onto the whole mirror image, upright
	struct comp_render_dispatch_data md = *d;
	const struct render_viewport_data full = {0, 0, MIRROR_WIDTH, MIRROR_HEIGHT};
	md.mirror = true;
	md.target.view_count = 1;
	md.target.gfx.rtr = &m->rtr;
	md.views[0].target.viewport_data = full;
	md.views[0].target.scissor_data = full;
	md.views[0].target.gfx.vertex_rot = (struct xrt_matrix_2x2){.v = {1, 0, 0, 1}};
	// Stabilized: drawn from a smoothed orientation; the scratch image is reprojected from the pose it was drawn for
	md.mirror_scratch_pose = d->views[0].world_pose_scanout_begin;
	struct xrt_pose pose = d->views[0].world_pose_scanout_begin;
	pose.orientation = mirror_stabilize(m, &pose.orientation, predicted_display_time_ns, 0.9f * margin);
	md.views[0].world_pose_scanout_begin = pose;
	md.views[0].world_pose_scanout_end = pose;

	struct render_gfx render;
	render_gfx_init(&render, &c->nr);
	render.mirror_mesh = true;
	render_gfx_begin(&render);
	comp_render_gfx_dispatch(&render, layers, layer_count, &md);
	render_gfx_end(&render);

	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &c->nr.cmd,
	};
	vk_queue_lock(vk->main_queue);
	VkResult ret = vk->vkQueueSubmit(vk->main_queue->queue, 1, &submit, VK_NULL_HANDLE);
	if (ret == VK_SUCCESS) {
		ret = vk->vkQueueWaitIdle(vk->main_queue->queue);
	}
	vk_queue_unlock(vk->main_queue);
	render_gfx_fini(&render);
	if (ret != VK_SUCCESS) {
		U_LOG_E("Mirror: %s", vk_result_string(ret));
		return;
	}

	const struct xrt_normalized_rect whole = {0, 0, 1, 1};
	xrt_result_t xret = comp_mirror_do_blit(&m->readback, vk, frame_id, predicted_display_time_ns, m->image,
	                                        m->unorm_view, c->nr.samplers.clamp_to_edge,
	                                        (VkExtent2D){MIRROR_WIDTH, MIRROR_HEIGHT}, whole);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Mirror: readback failed");
	}
}

void
sf_comp_mirror_destroy(struct sf_comp_mirror **mp)
{
	struct sf_comp_mirror *m = *mp;
	if (m == NULL) {
		return;
	}
	struct vk_bundle *vk = &m->c->base.vk;

	if (m->thread) {
		atomic_store(&m->stop, true);
		uint64_t one = 1;
		(void)!write(m->wake, &one, sizeof(one));
		pthread_join(m->thread, NULL);
	}
	if (m->wake >= 0) {
		close(m->wake);
	}
	xrt_frame_reference(&m->pending, NULL);
	pthread_mutex_destroy(&m->lock);

	if (m->target_ready) {
		render_gfx_target_resources_fini(&m->rtr);
		render_gfx_render_pass_fini(&m->rgrp);
	}
	if (m->readback.pool != NULL) {
		comp_mirror_fini(&m->readback, vk);
	}
	D(ImageView, m->unorm_view);
	D(ImageView, m->srgb_view);
	D(Image, m->image);
	DF(Memory, m->memory);

	free(m);
	*mp = NULL;
}
