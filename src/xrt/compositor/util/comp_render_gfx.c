// Copyright 2023-2024, Collabora, Ltd.
// Copyright 2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Compositor (gfx - graphics shader) rendering code.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup comp_util
 */

#include "xrt/xrt_compositor.h"
#include "util/comp_swapchain.h"

#include "math/m_api.h"
#include "math/m_mathinclude.h"

#include "util/u_trace_marker.h"

#include "vk/vk_helpers.h"
#include "vk/vk_image_allocator.h"
#include "vk/vk_compositor_flags.h"
#include "vk/vk_format.h"

#include "render/render_interface.h"

#include "util/comp_render.h"
#include "util/comp_frame_passthrough.h"
#include "util/comp_render_helpers.h"


/*
 *
 * Internal structs.
 *
 */

/**
 * Internal per-view for the layer squashing render step.
 */
struct gfx_layer_view_state
{
	/// Filled out descriptor sets.
	VkDescriptorSet descriptor_sets[RENDER_MAX_LAYERS];

	/// The type of layer.
	enum xrt_layer_type types[RENDER_MAX_LAYERS];

	/// Is the alpha premultipled, false means unpremultiplied.
	bool premultiplied_alphas[RENDER_MAX_LAYERS];

	/// To go to this view's tangent lengths.
	struct xrt_normalized_rect to_tangent;

	/// Number of layers filled in.
	/// TODO move to parent struct
	uint32_t layer_count;

	/// Full rotation and translation VP matrix, in world space.
	struct xrt_matrix_4x4 world_vp_full;
	/// Full rotation and translation VP matrix, in view space.
	struct xrt_matrix_4x4 eye_vp_full;

	/// Full rotation and translation inverse V matrix, in world space.
	struct xrt_matrix_4x4 world_v_inv_full;
	/// Full rotation and translation inverse V matrix, in view space.
	struct xrt_matrix_4x4 eye_v_inv_full;

	/// Only rotation and translation VP matrix, in world space.
	struct xrt_matrix_4x4 world_vp_rot_only;
	/// Only rotation and translation VP matrix, in view space.
	struct xrt_matrix_4x4 eye_vp_rot_only;
};

/**
 * Internal state for the layer squashing render step, contains all per-view state
 */
struct gfx_layer_state
{
	struct gfx_layer_view_state views[XRT_MAX_VIEWS];
};

/*
 * Internal state for the mesh rendering step.
 */
struct gfx_mesh_state
{
	VkDescriptorSet descriptor_sets[XRT_MAX_VIEWS];
};

/*
 * Per-view input data for the mesh rendering step.
 */
struct gfx_mesh_view_data
{
	struct xrt_pose src_pose;
	struct xrt_fov src_fov;
	struct xrt_normalized_rect src_norm_rect;
	VkSampler src_sampler;
	VkImageView src_image_view;
	//! An opaque foveated projection view (x and y axes), or NULL.
	const struct xrt_foveation_axis *foveation;
};

/*
 * Input data for the mesh rendering step,
 * combined with comp_render_dispatch_data.
 */
struct gfx_mesh_data
{
	struct gfx_mesh_view_data views[XRT_MAX_VIEWS];
	const struct comp_layer *projection;

	//! Quad and cylinder layers on top of everything else, drawn directly in the distortion pass.
	const struct comp_layer *quads;
	uint32_t quad_count;

	//! Mip chain copies of the quads' images (alpha and no-alpha views), VK_NULL_HANDLE to sample the swapchain.
	VkImageView quad_views[RENDER_MAX_LAYERS][2];
};



/*
 *
 * Static data.
 *
 */

static const VkClearColorValue background_color_idle = {
    .float32 = {0.0f, 0.0f, 0.0f, 1.0f},
};

static const VkClearColorValue background_color_active = {
    .float32 = {0.0f, 0.0f, 0.0f, 1.0f},
};


/*
 *
 * Input builder functions.
 *
 */

inline static void
gfx_mesh_add_view(struct gfx_mesh_data *md,
                  uint32_t view_index,
                  const struct xrt_pose *src_pose,
                  const struct xrt_fov *src_fov,
                  const struct xrt_normalized_rect *src_norm_rect,
                  VkSampler src_sampler,
                  VkImageView src_image_view)
{
	md->views[view_index].src_pose = *src_pose;
	md->views[view_index].src_fov = *src_fov;
	md->views[view_index].src_norm_rect = *src_norm_rect;
	md->views[view_index].src_sampler = src_sampler;
	md->views[view_index].src_image_view = src_image_view;
	md->views[view_index].foveation = NULL;
}

static const struct xrt_foveation_axis *
view_foveation(const struct xrt_layer_projection_view_data *v)
{
	return v->foveation[0].segment_count > 0 && v->foveation[1].segment_count > 0 ? v->foveation : NULL;
}

static void
foveation_lut(const struct xrt_foveation_axis *axis, float out[RENDER_FOVEATION_LUT_SIZE][4]);

/// The segments of a foveated axis as normalized breakpoints from the view into the image.
static int32_t
foveation_points(const struct xrt_foveation_axis *axis, float out[RENDER_FOVEATION_MAX_POINTS][4])
{
	uint32_t n = MIN(axis->segment_count, XRT_MAX_FOVEATION_SEGMENTS);
	double view_total = 0, image_total = 0;
	for (uint32_t i = 0; i < n; i++) {
		image_total += axis->counts[i];
		view_total += axis->counts[i] * (abs((int)axis->center - (int)i) + 1);
	}
	if (image_total <= 0) {
		out[0][0] = 0, out[0][1] = 0, out[1][0] = 1, out[1][1] = 1;
		return 2;
	}
	double view = 0, image = 0;
	int32_t count = 1;
	out[0][0] = 0, out[0][1] = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (axis->counts[i] == 0) {
			continue;
		}
		view += axis->counts[i] * (abs((int)axis->center - (int)i) + 1);
		image += axis->counts[i];
		out[count][0] = (float)(view / view_total);
		out[count][1] = (float)(image / image_total);
		count++;
	}
	return count;
}



/*
 *
 * Model view projection helper functions.
 *
 */

static inline void
calc_mvp_full(struct gfx_layer_view_state *state,
              const struct xrt_layer_data *layer_data,
              const struct xrt_pose *pose,
              const struct xrt_vec3 *scale,
              struct xrt_matrix_4x4 *result)
{
	struct xrt_matrix_4x4 model;
	math_matrix_4x4_model(pose, scale, &model);

	if (is_layer_view_space(layer_data)) {
		math_matrix_4x4_multiply(&state->eye_vp_full, &model, result);
	} else {
		math_matrix_4x4_multiply(&state->world_vp_full, &model, result);
	}
}

static inline void
calc_mv_inv_full(struct gfx_layer_view_state *state,
                 const struct xrt_layer_data *layer_data,
                 const struct xrt_pose *pose,
                 const struct xrt_vec3 *scale,
                 struct xrt_matrix_4x4 *result)
{
	struct xrt_matrix_4x4 model;
	math_matrix_4x4_model(pose, scale, &model);

	struct xrt_matrix_4x4 model_inv;
	math_matrix_4x4_inverse(&model, &model_inv);

	struct xrt_matrix_4x4 *v;
	if (is_layer_view_space(layer_data)) {
		v = &state->eye_v_inv_full;
	} else {
		v = &state->world_v_inv_full;
	}

	math_matrix_4x4_multiply(&model_inv, v, result);
}

static inline void
calc_mvp_rot_only(struct gfx_layer_view_state *state,
                  const struct xrt_layer_data *data,
                  const struct xrt_pose *pose,
                  const struct xrt_vec3 *scale,
                  struct xrt_matrix_4x4 *result)
{
	struct xrt_matrix_4x4 model;
	struct xrt_pose rot_only = {
	    .orientation = pose->orientation,
	    .position = XRT_VEC3_ZERO,
	};
	math_matrix_4x4_model(&rot_only, scale, &model);

	if (is_layer_view_space(data)) {
		math_matrix_4x4_multiply(&state->eye_vp_rot_only, &model, result);
	} else {
		math_matrix_4x4_multiply(&state->world_vp_rot_only, &model, result);
	}
}


/*
 *
 * Graphics layer data builders.
 *
 */

static inline const struct comp_swapchain_image *
get_layer_image(const struct comp_layer *layer, uint32_t swapchain_index, uint32_t image_index)
{

	const struct comp_swapchain *sc = (struct comp_swapchain *)(comp_layer_get_swapchain(layer, swapchain_index));
	return &sc->images[image_index];
}

/*
 *
 * Mip chain copies of quad layer images.
 *
 */

#define QUAD_MIP_CACHE_SIZE 16

struct quad_mip
{
	VkImage src;
	uint32_t src_layer;
	VkExtent2D extent;
	VkFormat format;
	uint32_t levels;

	VkImage image;
	VkDeviceMemory memory;
	VkImageView views[2]; // alpha, no-alpha

	uint64_t used_frame;
	uint64_t copied_frame;
	//! The source image's release count when it was last copied, UINT64_MAX before the first copy.
	uint64_t copied_release;
};

static struct quad_mip quad_mips[QUAD_MIP_CACHE_SIZE];
static uint64_t quad_mip_frame;

static void
quad_mip_destroy(struct vk_bundle *vk, struct quad_mip *m)
{
	for (int i = 0; i < 2; i++) {
		if (m->views[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, m->views[i], NULL);
		}
	}
	if (m->image != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, m->image, NULL);
	}
	if (m->memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, m->memory, NULL);
	}
	*m = (struct quad_mip){0};
}

void
comp_render_gfx_forget_image(struct vk_bundle *vk, VkImage src)
{
	for (uint32_t i = 0; i < QUAD_MIP_CACHE_SIZE; i++) {
		if (quad_mips[i].image != VK_NULL_HANDLE && (src == VK_NULL_HANDLE || quad_mips[i].src == src)) {
			quad_mip_destroy(vk, &quad_mips[i]);
		}
	}
}

static struct quad_mip *
quad_mip_get(struct vk_bundle *vk, VkImage src, uint32_t src_layer, VkExtent2D extent, VkFormat format, uint32_t levels)
{
	struct quad_mip *slot = NULL;
	for (uint32_t i = 0; i < QUAD_MIP_CACHE_SIZE; i++) {
		struct quad_mip *m = &quad_mips[i];
		if (m->image != VK_NULL_HANDLE && m->src == src && m->src_layer == src_layer &&
		    m->extent.width == extent.width && m->extent.height == extent.height && m->format == format &&
		    m->levels == levels) {
			m->used_frame = quad_mip_frame;
			return m;
		}
		if (m->used_frame != quad_mip_frame && (slot == NULL || m->used_frame < slot->used_frame)) {
			slot = m;
		}
	}
	if (slot == NULL) {
		return NULL;
	}

	// Least recently used slot; the queue is in order, so the last frame's reads come before our writes.
	quad_mip_destroy(vk, slot);

	VkImageCreateInfo info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = format,
	    .extent = {extent.width, extent.height, 1},
	    .mipLevels = levels,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkResult ret = vk->vkCreateImage(vk->device, &info, NULL, &slot->image);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateImage: %s", vk_result_string(ret));
		*slot = (struct quad_mip){0};
		return NULL;
	}

	VkMemoryRequirements requirements;
	vk->vkGetImageMemoryRequirements(vk->device, slot->image, &requirements);
	ret = vk_alloc_and_bind_image_memory(vk, slot->image, &requirements, NULL, __func__, &slot->memory);
	if (ret != VK_SUCCESS) {
		quad_mip_destroy(vk, slot);
		return NULL;
	}

	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = levels,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};
	VkComponentMapping no_alpha = {
	    .r = VK_COMPONENT_SWIZZLE_R,
	    .g = VK_COMPONENT_SWIZZLE_G,
	    .b = VK_COMPONENT_SWIZZLE_B,
	    .a = VK_COMPONENT_SWIZZLE_ONE,
	};
	ret = vk_create_view(vk, slot->image, VK_IMAGE_VIEW_TYPE_2D, format, range, &slot->views[0]);
	if (ret == VK_SUCCESS) {
		ret = vk_create_view_swizzle(vk, slot->image, VK_IMAGE_VIEW_TYPE_2D, format, range, no_alpha,
		                             &slot->views[1]);
	}
	if (ret != VK_SUCCESS) {
		quad_mip_destroy(vk, slot);
		return NULL;
	}

	slot->src = src;
	slot->src_layer = src_layer;
	slot->extent = extent;
	slot->format = format;
	slot->levels = levels;
	slot->used_frame = quad_mip_frame;
	slot->copied_frame = 0;
	slot->copied_release = UINT64_MAX;
	return slot;
}

static void
quad_mip_barrier(struct vk_bundle *vk,
                 VkCommandBuffer cmd,
                 VkImage image,
                 uint32_t level,
                 uint32_t level_count,
                 uint32_t layer,
                 VkAccessFlags src_access,
                 VkAccessFlags dst_access,
                 VkImageLayout old_layout,
                 VkImageLayout new_layout,
                 VkPipelineStageFlags src_stage,
                 VkPipelineStageFlags dst_stage)
{
	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = level,
	    .levelCount = level_count,
	    .baseArrayLayer = layer,
	    .layerCount = 1,
	};
	vk_cmd_image_barrier_locked(vk, cmd, image, src_access, dst_access, old_layout, new_layout, src_stage,
	                            dst_stage, range);
}

/// Copies the quad's image into a box filtered mip chain, like SteamVR does every frame, but only when the client
/// released new contents into it since the last copy.
static void
quad_mip_record(struct vk_bundle *vk, VkCommandBuffer cmd, struct quad_mip *m, uint64_t release)
{
	const VkPipelineStageFlags read_stages =
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

	if (m->copied_frame == quad_mip_frame || m->copied_release == release) {
		return;
	}
	m->copied_frame = quad_mip_frame;
	m->copied_release = release;

	quad_mip_barrier(vk, cmd, m->src, 0, 1, m->src_layer, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
	                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_stages,
	                 VK_PIPELINE_STAGE_TRANSFER_BIT);
	quad_mip_barrier(vk, cmd, m->image, 0, m->levels, 0, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, read_stages,
	                 VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkImageCopy copy = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m->src_layer, 1},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .extent = {m->extent.width, m->extent.height, 1},
	};
	vk->vkCmdCopyImage(cmd, m->src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m->image,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

	for (uint32_t level = 1; level < m->levels; level++) {
		quad_mip_barrier(vk, cmd, m->image, level - 1, 1, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
		                 VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                 VK_PIPELINE_STAGE_TRANSFER_BIT);

		int32_t sw = (int32_t)MAX(m->extent.width >> (level - 1), 1u);
		int32_t sh = (int32_t)MAX(m->extent.height >> (level - 1), 1u);
		int32_t dw = (int32_t)MAX(m->extent.width >> level, 1u);
		int32_t dh = (int32_t)MAX(m->extent.height >> level, 1u);
		VkImageBlit blit = {
		    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1},
		    .srcOffsets = {{0, 0, 0}, {sw, sh, 1}},
		    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
		    .dstOffsets = {{0, 0, 0}, {dw, dh, 1}},
		};
		vk->vkCmdBlitImage(cmd, m->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m->image,
		                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
	}

	if (m->levels > 1) {
		quad_mip_barrier(vk, cmd, m->image, 0, m->levels - 1, 0, VK_ACCESS_TRANSFER_READ_BIT,
		                 VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}
	quad_mip_barrier(vk, cmd, m->image, m->levels - 1, 1, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	                 VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	quad_mip_barrier(vk, cmd, m->src, 0, 1, m->src_layer, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
	                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

static inline void
add_layer(struct gfx_layer_view_state *state, const struct xrt_layer_data *data, VkDescriptorSet descriptor_set)
{
	uint32_t cur_layer = state->layer_count++;
	state->descriptor_sets[cur_layer] = descriptor_set;
	state->types[cur_layer] = data->type;
	state->premultiplied_alphas[cur_layer] = !is_layer_unpremultiplied(data);
}

/// Data setup for a cylinder layer
/// Also allocates and writes a descriptor set!
static VkResult
do_cylinder_layer(struct render_gfx *render,
                  const struct comp_layer *layer,
                  uint32_t view_index,
                  VkSampler clamp_to_edge,
                  VkSampler clamp_to_border_black,
                  struct gfx_layer_view_state *state)
{
	const struct xrt_layer_data *layer_data = &layer->data;
	const struct xrt_layer_cylinder_data *c = &layer_data->cylinder;
	const uint32_t array_index = c->sub.array_index;
	const struct comp_swapchain_image *image = get_layer_image(layer, 0, c->sub.image_index);

	struct vk_bundle *vk = render->r->vk;
	VkResult ret;

	// Color
	VkSampler src_sampler = clamp_to_edge; // WIP: Is this correct?
	VkImageView src_image_view = get_image_view(image, layer_data->flags, array_index);

	// Fully initialised below.
	struct render_gfx_layer_cylinder_data data;

	// Used for Subimage and OpenGL flip.
	set_post_transform_rect(   //
	    layer_data,            // data
	    &c->sub.norm_rect,     // src_norm_rect
	    false,                 // invert_flip
	    &data.post_transform); // out_norm_rect

	// Shared scale for all paths.
	struct xrt_vec3 scale = {1, 1, 1};

	// Handle infinite radius.
	if (c->radius == 0 || c->radius == INFINITY) {
		// Use rotation only to center the cylinder on the eye.
		calc_mvp_rot_only(state, layer_data, &c->pose, &scale, &data.mvp);
		data.radius = 1.0; // Fixed radius at one.
		data.central_angle = c->central_angle;
		data.aspect_ratio = c->aspect_ratio;
	} else {
		calc_mvp_full(state, layer_data, &c->pose, &scale, &data.mvp);
		data.radius = c->radius;
		data.central_angle = c->central_angle;
		data.aspect_ratio = c->aspect_ratio;
	}

	apply_bias_and_scale_from_layer(layer_data, &data.color_scale, &data.color_bias);

	// Can fail if we have too many layers.
	VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
	ret = render_gfx_layer_cylinder_alloc_and_write( //
	    render,                                      //
	    &data,                                       //
	    src_sampler,                                 //
	    src_image_view,                              //
	    &descriptor_set);                            // out_descriptor_set
	VK_CHK_AND_RET(ret, "render_gfx_layer_cylinder_alloc_and_write");

	VK_NAME_DESCRIPTOR_SET(vk, descriptor_set, "render_gfx layer quad descriptor set");

	add_layer(state, layer_data, descriptor_set);

	return VK_SUCCESS;
}

/// Data setup for an "equirect2" layer
/// Also allocates and writes a descriptor set!
static VkResult
do_equirect2_layer(struct render_gfx *render,
                   const struct comp_layer *layer,
                   uint32_t view_index,
                   VkSampler clamp_to_edge,
                   VkSampler clamp_to_border_black,
                   struct gfx_layer_view_state *state)
{
	const struct xrt_layer_data *layer_data = &layer->data;
	const struct xrt_layer_equirect2_data *eq2 = &layer_data->equirect2;
	const uint32_t array_index = eq2->sub.array_index;
	const struct comp_swapchain_image *image = get_layer_image(layer, 0, eq2->sub.image_index);

	struct vk_bundle *vk = render->r->vk;
	VkResult ret;

	// Color
	VkSampler src_sampler = clamp_to_edge;
	VkImageView src_image_view = get_image_view(image, layer_data->flags, array_index);

	// Fully initialised below.
	struct render_gfx_layer_equirect2_data data;

	// Used for Subimage and OpenGL flip.
	set_post_transform_rect(   //
	    layer_data,            // data
	    &eq2->sub.norm_rect,   // src_norm_rect
	    false,                 // invert_flip
	    &data.post_transform); // out_norm_rect

	struct xrt_vec3 scale = {1.f, 1.f, 1.f};
	calc_mv_inv_full(state, layer_data, &eq2->pose, &scale, &data.mv_inverse);

	// Make it possible to go tangent lengths.
	data.to_tangent = state->to_tangent;

	// Simplifies the shader.
	if (eq2->radius >= INFINITY) {
		data.radius = 0.0;
	} else {
		data.radius = eq2->radius;
	}

	data.central_horizontal_angle = eq2->central_horizontal_angle;
	data.upper_vertical_angle = eq2->upper_vertical_angle;
	data.lower_vertical_angle = eq2->lower_vertical_angle;

	apply_bias_and_scale_from_layer(layer_data, &data.color_scale, &data.color_bias);

	// Can fail if we have too many layers.
	VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
	ret = render_gfx_layer_equirect2_alloc_and_write( //
	    render,                                       //
	    &data,                                        //
	    src_sampler,                                  //
	    src_image_view,                               //
	    &descriptor_set);                             // out_descriptor_set
	VK_CHK_AND_RET(ret, "render_gfx_layer_equirect2_alloc_and_write");

	VK_NAME_DESCRIPTOR_SET(vk, descriptor_set, "render_gfx layer quad descriptor set");

	add_layer(state, layer_data, descriptor_set);

	return VK_SUCCESS;
}

/// Data setup for a projection layer
/// Also allocates and writes a descriptor set!
static VkResult
do_projection_layer(struct render_gfx *render,
                    const struct comp_layer *layer,
                    uint32_t view_index,
                    VkSampler clamp_to_edge,
                    VkSampler clamp_to_border_black,
                    struct gfx_layer_view_state *state)
{
	const struct xrt_layer_data *layer_data = &layer->data;
	const struct xrt_layer_projection_view_data *vd = NULL;
	const struct xrt_layer_depth_data *dvd = NULL;

	if (layer_data->type == XRT_LAYER_PROJECTION) {
		view_index_to_projection_data(view_index, layer_data, &vd);
	} else {
		view_index_to_depth_data(view_index, layer_data, &vd, &dvd);
	}

	uint32_t sc_array_index = is_view_index_right(view_index) ? 1 : 0;
	uint32_t array_index = vd->sub.array_index;
	const struct comp_swapchain_image *image = get_layer_image(layer, sc_array_index, vd->sub.image_index);

	struct vk_bundle *vk = render->r->vk;
	VkResult ret;
	// Color
	VkSampler src_sampler = clamp_to_border_black;
	VkImageView src_image_view = get_image_view(image, layer_data->flags, array_index);

	// Fully initialised below.
	struct render_gfx_layer_projection_data data = {0};

	// Used for Subimage and OpenGL flip.
	set_post_transform_rect(   //
	    layer_data,            // data
	    &vd->sub.norm_rect,    // src_norm_rect
	    false,                 // invert_flip
	    &data.post_transform); // out_norm_rect

	// Used to go from UV to tangent space.
	render_calc_uv_to_tangent_lengths_rect(&vd->fov, &data.to_tangent);

	// Create MVP matrix, rotation only so we get 3dof timewarp.
	struct xrt_vec3 scale = {1, 1, 1};
	calc_mvp_rot_only(state, layer_data, &vd->pose, &scale, &data.mvp);

	apply_bias_and_scale_from_layer(layer_data, &data.color_scale, &data.color_bias);
	if (view_foveation(vd) != NULL) {
		data.foveation_enabled[0] = 1;
		foveation_lut(&vd->foveation[0], data.lut_x);
		foveation_lut(&vd->foveation[1], data.lut_y);
	}

	// Can fail if we have too many layers.
	VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
	ret = render_gfx_layer_projection_alloc_and_write( //
	    render,                                        //
	    &data,                                         //
	    src_sampler,                                   //
	    src_image_view,                                //
	    &descriptor_set);                              // out_descriptor_set
	VK_CHK_AND_RET(ret, "render_gfx_layer_projection_alloc_and_write");

	VK_NAME_DESCRIPTOR_SET(vk, descriptor_set, "render_gfx layer proj descriptor set");

	add_layer(state, layer_data, descriptor_set);

	return VK_SUCCESS;
}

static void
crg_clear_output(struct render_gfx *render, const struct comp_render_dispatch_data *d)
{
	render_gfx_begin_target(     //
	    render,                  //
	    d->target.gfx.rtr,       //
	    &background_color_idle); //

	render_gfx_end_target(render);
}

/*
 *
 * Graphics distortion helpers.
 *
 */

/// Quad or cylinder layer drawn in the distortion pass: maps the scratch NDC the mesh samples back onto the layer.
/// Panel position (target pixels) of a clip space point, through the inverse distortion table.
static void
quad_direct_scissor_point(const struct render_resources *r,
                          const struct comp_render_dispatch_data *d,
                          uint32_t view,
                          float cx,
                          float cy,
                          float cw,
                          float *min_x,
                          float *min_y,
                          float *max_x,
                          float *max_y)
{
	const struct render_viewport_data *vp = &d->views[view].target.viewport_data;
	const struct xrt_matrix_2x2 *rot = &d->views[view].target.gfx.vertex_rot;
	const int n = RENDER_MESH_INVERSE_SIZE;

	// Scratch uv to table cell (bilinear), clamped to the mesh.
	float gx = CLAMP((cx / cw * 0.5f + 0.5f) * n - 0.5f, 0.0f, (float)(n - 1));
	float gy = CLAMP((cy / cw * 0.5f + 0.5f) * n - 0.5f, 0.0f, (float)(n - 1));
	int x0 = MIN((int)gx, n - 2), y0 = MIN((int)gy, n - 2);
	float fx = gx - (float)x0, fy = gy - (float)y0;
	const struct xrt_vec2 *a = &r->mesh.inverse[view][y0][x0];
	const struct xrt_vec2 *b = &r->mesh.inverse[view][y0][x0 + 1];
	const struct xrt_vec2 *c = &r->mesh.inverse[view][y0 + 1][x0];
	const struct xrt_vec2 *dd = &r->mesh.inverse[view][y0 + 1][x0 + 1];
	float px = (a->x * (1 - fx) + b->x * fx) * (1 - fy) + (c->x * (1 - fx) + dd->x * fx) * fy;
	float py = (a->y * (1 - fx) + b->y * fx) * (1 - fy) + (c->y * (1 - fx) + dd->y * fx) * fy;

	// Mesh position to target pixels, like mesh.vert.
	float ox = rot->v[0] * px + rot->v[2] * py;
	float oy = rot->v[1] * px + rot->v[3] * py;
	float sx = (float)vp->x + (ox * 0.5f + 0.5f) * (float)vp->w;
	float sy = (float)vp->y + (oy * 0.5f + 0.5f) * (float)vp->h;
	*min_x = MIN(*min_x, sx);
	*min_y = MIN(*min_y, sy);
	*max_x = MAX(*max_x, sx);
	*max_y = MAX(*max_y, sy);
}

/// Panel scissor around a layer: its outline (model space points through mvp) through the inverse distortion table,
/// with a margin for timewarp. Layers crossing the eye plane are clipped just in front of it along the segments between
/// each outline point and the next two: a quad's outline runs around its edge, a cylinder's alternates between its
/// arcs (so neighbours are its vertical lines and two apart its arcs and sides).
static void
quad_direct_scissor(const struct render_resources *r,
                    const struct xrt_matrix_4x4 *mvp,
                    const struct xrt_vec3 *outline,
                    int outline_count,
                    const struct comp_render_dispatch_data *d,
                    uint32_t view,
                    render_scissor_data_t *out)
{
	const render_scissor_data_t *full = &d->views[view].target.scissor_data;
	*out = *full;
	if (!r->mesh.has_inverse || d->mirror) {
		return;
	}

	const struct render_viewport_data *vp = &d->views[view].target.viewport_data;
	enum { MAX_OUTLINE = 128 };
	const float near_w = 1e-3f;
	float clip[MAX_OUTLINE][3];
	int count = MIN(outline_count, MAX_OUTLINE);
	float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
	int behind = 0;

	for (int i = 0; i < count; i++) {
		const struct xrt_vec3 *o = &outline[i];
		clip[i][0] = mvp->v[0] * o->x + mvp->v[4] * o->y + mvp->v[8] * o->z + mvp->v[12];
		clip[i][1] = mvp->v[1] * o->x + mvp->v[5] * o->y + mvp->v[9] * o->z + mvp->v[13];
		clip[i][2] = mvp->v[3] * o->x + mvp->v[7] * o->y + mvp->v[11] * o->z + mvp->v[15];
		if (clip[i][2] <= near_w) {
			behind++;
			continue;
		}
		quad_direct_scissor_point(r, d, view, clip[i][0], clip[i][1], clip[i][2], &min_x, &min_y, &max_x, &max_y);
	}

	if (behind == count) {
		out->w = 0; // All behind the eye.
		out->h = 0;
		return;
	}
	if (behind > 0) {
		for (int i = 0; i < count; i++) {
			for (int step = 1; step <= 2 && i + step < count; step++) {
				int f = i, b = i + step;
				if (clip[f][2] <= near_w) {
					f = i + step;
					b = i;
				}
				if (clip[f][2] <= near_w || clip[b][2] > near_w) {
					continue; // Both on the same side
				}
				// Slightly in front of the plane, so the point still projects.
				float t = (clip[f][2] - 2.0f * near_w) / (clip[f][2] - clip[b][2]);
				float cx = clip[f][0] + t * (clip[b][0] - clip[f][0]);
				float cy = clip[f][1] + t * (clip[b][1] - clip[f][1]);
				quad_direct_scissor_point(r, d, view, cx, cy, 2.0f * near_w, &min_x, &min_y, &max_x, &max_y);
			}
		}
	}

	// Timewarp turns the view by up to the rotation between scanout begin and end; pixels per radian at the centre
	// of the lens are about twice the average over the field of view.
	const struct xrt_quat *qa = &d->views[view].world_pose_scanout_begin.orientation;
	const struct xrt_quat *qb = &d->views[view].world_pose_scanout_end.orientation;
	float dot = fabsf(qa->x * qb->x + qa->y * qb->y + qa->z * qb->z + qa->w * qb->w);
	float angle = 2.0f * acosf(MIN(dot, 1.0f));
	const struct xrt_fov *fov = &d->views[view].fov;
	float px_per_rad = 2.0f * (float)vp->w / (fov->angle_right - fov->angle_left);
	float margin = 0.04f * (float)MAX(vp->w, vp->h) + angle * px_per_rad;
	int32_t fx0 = (int32_t)full->x, fy0 = (int32_t)full->y;
	int32_t x0 = MAX((int32_t)(min_x - margin), fx0);
	int32_t y0 = MAX((int32_t)(min_y - margin), fy0);
	int32_t x1 = MIN((int32_t)(max_x + margin), fx0 + (int32_t)full->w);
	int32_t y1 = MIN((int32_t)(max_y + margin), fy0 + (int32_t)full->h);
	if (x1 <= x0 || y1 <= y0) {
		out->w = 0;
		out->h = 0;
		return;
	}
	out->x = (uint32_t)x0;
	out->y = (uint32_t)y0;
	out->w = (uint32_t)(x1 - x0);
	out->h = (uint32_t)(y1 - y0);
}

/// Sub-image of a layer drawn in the distortion pass.
static const struct xrt_sub_image *
direct_layer_sub(const struct comp_layer *layer)
{
	return layer->data.type == XRT_LAYER_CYLINDER ? &layer->data.cylinder.sub : &layer->data.quad.sub;
}

/// Cylinder point for layer uv, as in layer_cylinder.vert.
static struct xrt_vec3
cylinder_point(const struct xrt_layer_cylinder_data *c, float radius, float u, float v)
{
	float a = (u - 0.5f) * c->central_angle;
	float height = c->central_angle * radius / c->aspect_ratio;
	return (struct xrt_vec3){sinf(a) * radius, (0.5f - v) * height, -cosf(a) * radius};
}

/// Cylinder layer: rays from the scratch NDC into cylinder space, intersected in mesh_quad.frag.
static void
cylinder_direct_data(const struct render_resources *r,
                     const struct comp_layer *layer,
                     const struct comp_render_dispatch_data *d,
                     uint32_t view,
                     const struct xrt_matrix_4x4 *vp,
                     struct render_gfx_mesh_quad_ubo_data *data,
                     render_scissor_data_t *out_scissor)
{
	const struct xrt_layer_data *layer_data = &layer->data;
	const struct xrt_layer_cylinder_data *c = &layer_data->cylinder;
	const struct xrt_pose *eye =
	    is_layer_view_space(layer_data) ? &d->views[view].eye_pose : &d->views[view].world_pose_scanout_begin;

	// Eye in cylinder space; an infinite radius keeps the cylinder centred on the eye, like do_cylinder_layer.
	bool infinite = c->radius == 0 || c->radius == INFINITY;
	float radius = infinite ? 1.0f : c->radius;
	struct xrt_pose inv_layer, rel;
	math_pose_invert(&c->pose, &inv_layer);
	math_pose_transform(&inv_layer, eye, &rel);
	if (infinite) {
		rel.position = (struct xrt_vec3){0, 0, 0};
	}

	// Scratch NDC to view direction (Vulkan projection, y down, looking along -z), then into cylinder space.
	const struct xrt_fov *fov = &d->views[view].fov;
	float tl = tanf(fov->angle_left), tr = tanf(fov->angle_right);
	float tu = tanf(fov->angle_up), td = tanf(fov->angle_down);
	struct xrt_vec3 ex = {1, 0, 0}, ey = {0, 1, 0}, ez = {0, 0, 1}, rx, ry, rz;
	math_quat_rotate_vec3(&rel.orientation, &ex, &rx);
	math_quat_rotate_vec3(&rel.orientation, &ey, &ry);
	math_quat_rotate_vec3(&rel.orientation, &ez, &rz);
	float hw = (tr - tl) * 0.5f, cx = (tr + tl) * 0.5f, hh = (td - tu) * 0.5f, cy = (tu + td) * 0.5f;
	const float cols[3][3] = {
	    {hw * rx.x, hw * rx.y, hw * rx.z},
	    {hh * ry.x, hh * ry.y, hh * ry.z},
	    {cx * rx.x + cy * ry.x - rz.x, cx * rx.y + cy * ry.y - rz.y, cx * rx.z + cy * ry.z - rz.z},
	};
	for (int r = 0; r < 3; r++) {
		for (int k = 0; k < 3; k++) {
			data->ndc_to_dir[r][k] = cols[k][r];
		}
	}
	data->cylinder_origin[0] = rel.position.x;
	data->cylinder_origin[1] = rel.position.y;
	data->cylinder_origin[2] = rel.position.z;
	data->cylinder_origin[3] = radius;
	data->cylinder_params[0] = c->central_angle;
	data->cylinder_params[1] = c->central_angle * radius / c->aspect_ratio;
	data->cylinder_params[2] = 1.0f;

	if (infinite) {
		*out_scissor = d->views[view].target.scissor_data;
		return;
	}

	// Outline: both arcs and both sides.
	enum { ARC = 32, SIDE = 8 };
	struct xrt_vec3 outline[2 * (ARC + 1) + 2 * (SIDE + 1)];
	int count = 0;
	for (int i = 0; i <= ARC; i++) {
		outline[count++] = cylinder_point(c, radius, (float)i / ARC, 0.0f);
		outline[count++] = cylinder_point(c, radius, (float)i / ARC, 1.0f);
	}
	for (int i = 0; i <= SIDE; i++) {
		outline[count++] = cylinder_point(c, radius, 0.0f, (float)i / SIDE);
		outline[count++] = cylinder_point(c, radius, 1.0f, (float)i / SIDE);
	}
	struct xrt_matrix_4x4 model, mvp;
	struct xrt_vec3 scale = {1, 1, 1};
	math_matrix_4x4_model(&c->pose, &scale, &model);
	math_matrix_4x4_multiply(vp, &model, &mvp);
	quad_direct_scissor(r, &mvp, outline, count, d, view, out_scissor);
}

static VkResult
quad_direct_alloc_and_write(struct render_gfx *render,
                            const struct comp_layer *layer,
                            const VkImageView mip_views[2],
                            const struct render_gfx_mesh_ubo_data *mesh_data,
                            const struct comp_render_dispatch_data *d,
                            uint32_t view,
                            render_scissor_data_t *out_scissor,
                            VkDescriptorSet *out_descriptor_set)
{
	const struct xrt_layer_data *layer_data = &layer->data;
	const struct xrt_sub_image *sub = direct_layer_sub(layer);
	const struct comp_swapchain_image *image = get_layer_image(layer, 0, sub->image_index);

	*out_descriptor_set = VK_NULL_HANDLE;

	// Same view projection as the squash of this view.
	struct xrt_matrix_4x4 p, v, vp;
	math_matrix_4x4_projection_vulkan_infinite_reverse(&d->views[view].fov, 0.1, &p);
	if (is_layer_view_space(layer_data)) {
		math_matrix_4x4_view_from_pose(&d->views[view].eye_pose, &v);
	} else {
		math_matrix_4x4_view_from_pose(&d->views[view].world_pose_scanout_begin, &v);
	}
	math_matrix_4x4_multiply(&p, &v, &vp);

	struct render_gfx_mesh_quad_ubo_data data = {.mesh = *mesh_data};

	if (layer_data->type == XRT_LAYER_CYLINDER) {
		const struct xrt_layer_cylinder_data *c = &layer_data->cylinder;
		if (!(c->central_angle > 0.0f) || !(c->aspect_ratio > 0.0f)) {
			return VK_SUCCESS;
		}
		cylinder_direct_data(render->r, layer, d, view, &vp, &data, out_scissor);
	} else {
		const struct xrt_layer_quad_data *q = &layer_data->quad;
		if (!(q->size.x > 0.0f) || !(q->size.y > 0.0f)) {
			return VK_SUCCESS;
		}
		struct xrt_matrix_4x4 model, mvp;
		struct xrt_vec3 scale = {q->size.x, q->size.y, 1};
		math_matrix_4x4_model(&q->pose, &scale, &model);
		math_matrix_4x4_multiply(&vp, &model, &mvp);

		// Unit quad outline.
		enum { STEPS = 16 };
		struct xrt_vec3 outline[4 * (STEPS + 1)];
		int count = 0;
		for (int e = 0; e < 4; e++) {
			for (int i = 0; i <= STEPS; i++) {
				float t = (float)i / (float)STEPS - 0.5f;
				outline[count++] = (struct xrt_vec3){
				    e == 0 ? t : e == 1 ? 0.5f : e == 2 ? -t : -0.5f,
				    e == 0 ? -0.5f : e == 1 ? t : e == 2 ? 0.5f : -t,
				    0.0f,
				};
			}
		}
		quad_direct_scissor(render->r, &mvp, outline, count, d, view, out_scissor);

		// Quad plane (x, y, 1) to clip (x, y, w), column-major: element (row, col) is v[col * 4 + row].
		static const int rc[3] = {0, 1, 3};
		double h[3][3];
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) {
				h[r][c] = mvp.v[rc[c] * 4 + rc[r]];
			}
		}
		double det = h[0][0] * (h[1][1] * h[2][2] - h[1][2] * h[2][1]) -
		             h[0][1] * (h[1][0] * h[2][2] - h[1][2] * h[2][0]) +
		             h[0][2] * (h[1][0] * h[2][1] - h[1][1] * h[2][0]);
		if (fabs(det) < 1e-12) {
			return VK_SUCCESS;
		}
		for (int r = 0; r < 3; r++) {
			for (int c = 0; c < 3; c++) {
				// Inverse via cofactors: inv[r][c] = cof[c][r] / det.
				int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
				data.ndc_to_quad[r][c] = (float)((h[r1][c1] * h[r2][c2] - h[r1][c2] * h[r2][c1]) / det);
			}
		}
	}
	if (out_scissor->w == 0) {
		return VK_SUCCESS;
	}

	set_post_transform_rect(layer_data, &sub->norm_rect, false, &data.quad_post_transform);
	apply_bias_and_scale_from_layer(layer_data, &data.color_scale, &data.color_bias);

	VkImageView src_view = get_image_view(image, layer_data->flags, sub->array_index);
	if (mip_views[0] != VK_NULL_HANDLE) {
		bool alpha = (layer_data->flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
		src_view = mip_views[alpha ? 0 : 1];
	}

	return render_gfx_mesh_quad_alloc_and_write( //
	    render,                                  //
	    &data,                                   //
	    render->r->samplers.clamp_to_edge_aniso, //
	    src_view,                                //
	    out_descriptor_set);                     //
}

static struct render_gfx_mesh_ubo_data
mesh_ubo_data(const struct gfx_mesh_view_data *source,
              const struct comp_render_dispatch_data *d,
              uint32_t view,
              bool do_timewarp)
{
	struct render_gfx_mesh_ubo_data data = {
	    .vertex_rot = d->views[view].target.gfx.vertex_rot,
	    .post_transform = source->src_norm_rect,
	};
	if (do_timewarp) {
		data.pre_transform = d->views[view].pre_transform;
		render_calc_time_warp_matrix(&source->src_pose, &source->src_fov,
		                            &d->views[view].world_pose_scanout_begin, &data.transform_scanout_begin);
		render_calc_time_warp_matrix(&source->src_pose, &source->src_fov,
		                            &d->views[view].world_pose_scanout_end, &data.transform_scanout_end);
	}
	return data;
}

/// The axis' breakpoints sampled uniformly over the view, for linear lookups in the shader.
static void
foveation_lut(const struct xrt_foveation_axis *axis, float out[RENDER_FOVEATION_LUT_SIZE][4])
{
	float points[RENDER_FOVEATION_MAX_POINTS][4];
	int32_t count = foveation_points(axis, points);
	int32_t segment = 1;
	float previous = 0;
	for (int k = 0; k <= RENDER_FOVEATION_LUT_SIZE; k++) {
		float v = (float)k / RENDER_FOVEATION_LUT_SIZE;
		while (segment < count - 1 && v > points[segment][0]) {
			segment++;
		}
		const float *a = points[segment - 1], *b = points[segment];
		float t = b[0] > a[0] ? (v - a[0]) / (b[0] - a[0]) : 0;
		float image = a[1] + (b[1] - a[1]) * CLAMP(t, 0.0f, 1.0f);
		if (k > 0) {
			out[k - 1][0] = previous;
			out[k - 1][1] = image;
			out[k - 1][2] = out[k - 1][3] = 0;
		}
		previous = image;
	}
}

/// The plain mesh UBO, or for a foveated view the mesh UBO with the view's foveation breakpoints.
static VkResult
mesh_or_foveated_alloc_and_write(struct render_gfx *render,
                                 const struct gfx_mesh_view_data *view,
                                 const struct render_gfx_mesh_ubo_data *mesh,
                                 VkDescriptorSet *out_descriptor_set)
{
	if (view->foveation == NULL) {
		return render_gfx_mesh_alloc_and_write(render, mesh, view->src_sampler, view->src_image_view,
		                                       out_descriptor_set);
	}
	struct render_gfx_mesh_foveated_ubo_data data = {.mesh = *mesh};
	foveation_lut(&view->foveation[0], data.lut_x);
	foveation_lut(&view->foveation[1], data.lut_y);
	return render_gfx_mesh_foveated_alloc_and_write(render, &data, view->src_sampler, view->src_image_view,
	                                                out_descriptor_set);
}

static void
projection_mesh_views(struct render_gfx *render,
                       const struct comp_layer *layer,
                       uint32_t view_count,
                       struct gfx_mesh_view_data views[XRT_MAX_VIEWS])
{
	bool alpha = (layer->data.flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
	for (uint32_t i = 0; i < view_count; i++) {
		const struct xrt_layer_projection_view_data *v = layer->data.type == XRT_LAYER_PROJECTION
		    ? &layer->data.proj.v[i] : &layer->data.depth.v[i];
		views[i] = (struct gfx_mesh_view_data){
		    .src_pose = v->pose,
		    .src_fov = v->fov,
		    .src_norm_rect = v->sub.norm_rect,
		    .foveation = alpha ? NULL : view_foveation(v),
		    .src_sampler = alpha ? render->r->samplers.clamp_to_border_transparent_black
		                         : render->r->samplers.clamp_to_border_black,
		    .src_image_view = get_image_view(get_layer_image(layer, i, v->sub.image_index),
		                                     layer->data.flags, v->sub.array_index),
		};
		if (layer->data.flip_y) {
			views[i].src_norm_rect.y += views[i].src_norm_rect.h;
			views[i].src_norm_rect.h = -views[i].src_norm_rect.h;
		}
	}
}

/// Used in both fast-path and layer-squashed routes
/// Whether an opaque projection layer drawn in the distortion pass covers the scratch images, which then hold nothing.
static bool
scratch_hidden(const struct comp_layer *projection, const struct comp_render_dispatch_data *d)
{
	return projection != NULL && !d->passthrough &&
	       (projection->data.flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) == 0;
}

static void
crg_distortion_common(struct render_gfx *render,
                      bool do_timewarp,
                      const struct gfx_mesh_data *md,
                      const struct comp_render_dispatch_data *d)
{
	struct vk_bundle *vk = render->r->vk;
	VkResult ret;

	/*
	 * Reserve UBOs, create descriptor sets, and fill in any data ahead of
	 * time. If we ever want to copy UBO data this lets us do that easily:
	 * write a copy command before the other gfx commands.
	 */

	struct gfx_mesh_state ms = XRT_STRUCT_INIT;
	VkDescriptorSet projection_sets[XRT_MAX_VIEWS] = {0};
	struct gfx_mesh_view_data projection_views[XRT_MAX_VIEWS];
	if (md->projection != NULL) {
		projection_mesh_views(render, md->projection, d->target.view_count, projection_views);
	}
	VkDescriptorSet quad_sets[XRT_MAX_VIEWS][RENDER_MAX_LAYERS] = {0};
	render_scissor_data_t quad_scissors[XRT_MAX_VIEWS][RENDER_MAX_LAYERS];

	for (uint32_t i = 0; i < d->target.view_count; i++) {

		struct render_gfx_mesh_ubo_data data = mesh_ubo_data(&md->views[i], d, i, do_timewarp);

		ret = mesh_or_foveated_alloc_and_write(render, &md->views[i], &data, &ms.descriptor_sets[i]);
		VK_CHK_WITH_GOTO(ret, "render_gfx_mesh_alloc_and_write", err_no_memory);

		VK_NAME_DESCRIPTOR_SET(vk, ms.descriptor_sets[i], "render_gfx mesh descriptor sets");
		if (md->projection != NULL) {
			struct render_gfx_mesh_ubo_data projection_data = mesh_ubo_data(&projection_views[i], d, i, do_timewarp);
			ret = mesh_or_foveated_alloc_and_write(render, &projection_views[i], &projection_data,
			                                       &projection_sets[i]);
			VK_CHK_WITH_GOTO(ret, "render_gfx_mesh_alloc_and_write", err_no_memory);
		}

		for (uint32_t q = 0; q < md->quad_count; q++) {
			const struct comp_layer *layer = &md->quads[q];
			if (!is_layer_view_visible(&layer->data, i)) {
				continue;
			}
			ret = quad_direct_alloc_and_write(render, layer, md->quad_views[q], &data, d, i,
			                                  &quad_scissors[i][q], &quad_sets[i][q]);
			VK_CHK_WITH_GOTO(ret, "quad_direct_alloc_and_write", err_no_memory);
		}
	}


	/*
	 * Do command writing here.
	 */

	render_gfx_begin_target(       //
	    render,                    //
	    d->target.gfx.rtr,         //
	    &background_color_active); //

	for (uint32_t i = 0; i < d->target.view_count; i++) {
		// Convenience.
		const struct render_viewport_data *viewport_data = &d->views[i].target.viewport_data;

		const render_scissor_data_t *scissor_data = &d->views[i].target.scissor_data;

		render_gfx_begin_view( //
		    render,            //
		    i,                 // view_index
		    viewport_data,     //
		    scissor_data);     //

		if (!scratch_hidden(md->projection, d)) {
			if (md->views[i].foveation != NULL) {
				render_gfx_mesh_foveated_draw(render, i, ms.descriptor_sets[i], do_timewarp);
			} else {
				render_gfx_mesh_draw(render, i, ms.descriptor_sets[i], do_timewarp);
			}
		}

		if (md->projection != NULL) {
			// Without source alpha the layer is opaque (the plain mesh shader writes alpha 1)
			if (md->projection->data.flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) {
				render_gfx_mesh_projection_draw(render, i, projection_sets[i], do_timewarp,
				                                !is_layer_unpremultiplied(&md->projection->data));
			} else if (projection_views[i].foveation != NULL) {
				render_gfx_mesh_foveated_draw(render, i, projection_sets[i], do_timewarp);
			} else {
				render_gfx_mesh_draw(render, i, projection_sets[i], do_timewarp);
			}
		}
		for (uint32_t q = 0; q < md->quad_count; q++) {
			if (quad_sets[i][q] == VK_NULL_HANDLE || quad_scissors[i][q].w == 0) {
				continue;
			}
			render_gfx_begin_view(render, i, viewport_data, &quad_scissors[i][q]);
			render_gfx_mesh_quad_draw(                             //
			    render,                                            //
			    i,                                                 // mesh_index
			    quad_sets[i][q],                                   //
			    do_timewarp,                                       //
			    !is_layer_unpremultiplied(&md->quads[q].data));    //
		}

		render_gfx_end_view(render);
	}

	render_gfx_end_target(render);

	return;

err_no_memory:
	// Allocator reset at end of frame, nothing to clean up.
	VK_ERROR(vk, "Could not allocate all UBOs for frame, that's really strange and shouldn't happen!");
}

/// For use after squashing layers
static void
crg_distortion_after_squash(struct render_gfx *render,
                            const struct comp_render_dispatch_data *d,
                            const struct comp_layer *quads,
                            uint32_t quad_count,
                            const struct comp_layer *projection)
{

	// Shared between all views.
	VkSampler clamp_to_border_black = render->r->samplers.clamp_to_border_black;

	struct gfx_mesh_data md = XRT_STRUCT_INIT;
	for (uint32_t i = 0; i < d->target.view_count; i++) {
		struct xrt_pose src_pose = d->views[i].world_pose_scanout_begin;
		struct xrt_fov src_fov = d->views[i].fov;
		VkImageView src_image_view = d->views[i].squash_as_src.sample_view;
		struct xrt_normalized_rect src_norm_rect = d->views[i].squash_as_src.norm_rect;

		gfx_mesh_add_view(         //
		    &md,                   //
		    i,                     // view_index
		    &src_pose,             //
		    &src_fov,              //
		    &src_norm_rect,        //
		    clamp_to_border_black, // src_sampler
		    src_image_view);       //
	}
	md.quads = quads;
	md.quad_count = quad_count;
	md.projection = projection;

	// Per-frame mip chain copies of the quads' images, recorded before the distortion render pass.
	const uint32_t levels = 3;
	quad_mip_frame++;
	for (uint32_t q = 0; q < quad_count; q++) {
		const struct xrt_sub_image *sub = direct_layer_sub(&quads[q]);
		const struct comp_swapchain *sc = (struct comp_swapchain *)comp_layer_get_swapchain(&quads[q], 0);
		if ((vk_csci_get_image_usage_flags(render->r->vk, (VkFormat)sc->vkic.info.format, sc->vkic.info.bits) &
		     VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
			continue;
		}
		VkFormat format = (VkFormat)sc->vkic.info.format;
		if (sc->vkic.info.create & XRT_SWAPCHAIN_CREATE_SAMPLE_AS_SRGB) {
			format = vk_format_convert_unorm_to_srgb(format);
		}
		VkExtent2D extent = {sc->vkic.info.width, sc->vkic.info.height};
		uint32_t max_levels = 1;
		while ((MAX(extent.width, extent.height) >> max_levels) > 0) {
			max_levels++;
		}
		struct quad_mip *m = quad_mip_get(render->r->vk, sc->vkic.images[sub->image_index].handle,
		                                  sub->array_index, extent, format, MIN(levels, max_levels));
		if (m == NULL) {
			continue;
		}
		uint64_t release = __atomic_load_n(&sc->images[sub->image_index].release_count, __ATOMIC_ACQUIRE);
		quad_mip_record(render->r->vk, render->r->cmd, m, release);
		md.quad_views[q][0] = m->views[0];
		md.quad_views[q][1] = m->views[1];
	}

	crg_distortion_common( //
	    render,            //
	    d->do_timewarp,    //
	    &md,               //
	    d);                //
}

/// Fast path
static void
crg_distortion_fast_path(struct render_gfx *render,
                         const struct comp_render_dispatch_data *d,
                         const struct comp_layer *layer,
                         const struct xrt_layer_projection_view_data *vds[XRT_MAX_VIEWS])
{
	const struct xrt_layer_data *data = &layer->data;

	VkSampler clamp_to_border_black = render->r->samplers.clamp_to_border_black;

	struct gfx_mesh_data md = XRT_STRUCT_INIT;
	for (uint32_t i = 0; i < d->target.view_count; i++) {
		const uint32_t array_index = vds[i]->sub.array_index;

		const struct comp_swapchain_image *image = get_layer_image(layer, i, vds[i]->sub.image_index);

		struct xrt_pose src_pose;
		struct xrt_fov src_fov;
		struct xrt_normalized_rect src_norm_rect;

		src_pose = vds[i]->pose;
		src_fov = vds[i]->fov;
		src_norm_rect = vds[i]->sub.norm_rect;
		VkImageView src_image_view = get_image_view(image, data->flags, array_index);

		if (data->flip_y) {
			src_norm_rect.y += src_norm_rect.h;
			src_norm_rect.h = -src_norm_rect.h;
		}

		gfx_mesh_add_view(         //
		    &md,                   // md
		    i,                     // view_index
		    &src_pose,             // src_pose
		    &src_fov,              // src_fov
		    &src_norm_rect,        // src_norm_rect
		    clamp_to_border_black, // src_sampler
		    src_image_view);       // src_image_view
		// The mesh shader writes opaque pixels here, so a foveated view can always take the foveated shader
		md.views[i].foveation = view_foveation(vds[i]);
	}

	crg_distortion_common( //
	    render,            //
	    d->do_timewarp,    //
	    &md,               //
	    d);                //
}


/*
 *
 * 'Exported' function(s).
 *
 */

void
comp_render_gfx_layers(struct render_gfx *render,
                       const struct comp_layer *layers,
                       uint32_t layer_count,
                       const struct comp_render_dispatch_data *d,
                       VkImageLayout transition_to)
{
	COMP_TRACE_MARKER();

	struct vk_bundle *vk = render->r->vk;
	VkResult ret;

	struct gfx_layer_state ls = XRT_STRUCT_INIT;
	sf_comp_passthrough_prepare(d->passthrough, render, d);

	// Compute MVP matrices per eye: populates gfx_layer_view_state elements in `ls`
	// from `comp_render_dispatch_data *d`
	for (uint32_t view = 0; view < d->squash_view_count; view++) {

		// Data for this view, convenience.
		const struct xrt_pose world_pose = d->views[view].world_pose_scanout_begin;
		const struct xrt_pose eye_pose = d->views[view].eye_pose;
		const struct xrt_fov new_fov = d->views[view].fov;

		// Current state we are writing to.
		struct gfx_layer_view_state *state = &ls.views[view];

		// Used to go from UV to tangent space.
		render_calc_uv_to_tangent_lengths_rect(&new_fov, &state->to_tangent);

		// Projection
		struct xrt_matrix_4x4 p;
		math_matrix_4x4_projection_vulkan_infinite_reverse(&new_fov, 0.1, &p);

		// Reused view matrix.
		struct xrt_matrix_4x4 v;

		// World
		math_matrix_4x4_view_from_pose(&world_pose, &v);
		math_matrix_4x4_multiply(&p, &v, &state->world_vp_full);
		math_matrix_4x4_inverse(&v, &state->world_v_inv_full);

		struct xrt_pose world_rot_only = {world_pose.orientation, XRT_VEC3_ZERO};
		math_matrix_4x4_view_from_pose(&world_rot_only, &v);
		math_matrix_4x4_multiply(&p, &v, &state->world_vp_rot_only);

		// Eye
		math_matrix_4x4_view_from_pose(&eye_pose, &v);
		math_matrix_4x4_multiply(&p, &v, &state->eye_vp_full);
		math_matrix_4x4_inverse(&v, &state->eye_v_inv_full);

		struct xrt_pose eye_rot_only = {eye_pose.orientation, XRT_VEC3_ZERO};
		math_matrix_4x4_view_from_pose(&eye_rot_only, &v);
		math_matrix_4x4_multiply(&p, &v, &state->eye_vp_rot_only);
	}

	/*
	 * Reserve UBOs, create descriptor sets, and fill in any data ahead of
	 * time. If we ever want to copy UBO data this lets us do that easily:
	 * write a copy command before the other gfx commands.
	 */

	assert(layer_count <= RENDER_MAX_LAYERS && "Too many layers");

	VkSampler clamp_to_edge = render->r->samplers.clamp_to_edge;
	VkSampler clamp_to_border_black = render->r->samplers.clamp_to_border_black;

	for (uint32_t view = 0; view < d->squash_view_count; view++) {

		// Source for data and written to as well, read and write.
		struct gfx_layer_view_state *state = &ls.views[view];

		for (uint32_t i = 0; i < layer_count; i++) {
			const struct xrt_layer_data *data = &layers[i].data;
			if (!is_layer_view_visible(data, view)) {
				continue;
			}

			switch (data->type) {
			case XRT_LAYER_CYLINDER:
				ret = do_cylinder_layer(   //
				    render,                //
				    &layers[i],            //
				    view,                  // view_index
				    clamp_to_edge,         //
				    clamp_to_border_black, //
				    state);                //
				VK_CHK_WITH_GOTO(ret, "do_cylinder_layer", err_layer);
				break;
			case XRT_LAYER_EQUIRECT2:
				ret = do_equirect2_layer(  //
				    render,                //
				    &layers[i],            //
				    view,                  // view_index
				    clamp_to_edge,         //
				    clamp_to_border_black, //
				    state);                //
				VK_CHK_WITH_GOTO(ret, "do_equirect2_layer", err_layer);
				break;
			case XRT_LAYER_PROJECTION:
			case XRT_LAYER_PROJECTION_DEPTH:
				ret = do_projection_layer( //
				    render,                //
				    &layers[i],            //
				    view,                  // view_index
				    clamp_to_edge,         //
				    clamp_to_border_black, //
				    state);                //
				VK_CHK_WITH_GOTO(ret, "do_projection_layer", err_layer);
				break;
			default: break;
			}
		}

	}


	/*
	 * Do command writing here.
	 */

	const VkClearColorValue *color = layer_count == 0 ? &background_color_idle : &background_color_active;

	for (uint32_t view = 0; view < d->squash_view_count; view++) {

		// Convenience.
		const struct render_viewport_data *viewport_data = &d->views[view].squash.viewport_data;

		// Only the part of the scratch image that is drawn, see chl_frame_state::squash_scale
		const VkRect2D squash_area = {
		    .offset = {(int32_t)viewport_data->x, (int32_t)viewport_data->y},
		    .extent = {viewport_data->w, viewport_data->h},
		};
		render_gfx_begin_target_area(      //
		    render,                        //
		    d->views[view].squash.gfx.rtr, //
		    color,                         //
		    &squash_area);                 //

		render_gfx_begin_view( //
		    render,            //
		    view,              // view_index
		    viewport_data,     // viewport_data
		    viewport_data);    // scissor_data

		sf_comp_passthrough_draw(d->passthrough, render, view, &d->views[view].fov, &d->views[view].eye_pose, &d->views[view].world_pose_scanout_begin);

		// Only source for data here, read only.
		const struct gfx_layer_view_state *state = &ls.views[view];

		for (uint32_t i = 0; i < state->layer_count; i++) {
			switch (state->types[i]) {
			case XRT_LAYER_CYLINDER:
				render_gfx_layer_cylinder(          //
				    render,                         //
				    state->premultiplied_alphas[i], //
				    state->descriptor_sets[i]);     //
				break;
			case XRT_LAYER_EQUIRECT2:
				render_gfx_layer_equirect2(         //
				    render,                         //
				    state->premultiplied_alphas[i], //
				    state->descriptor_sets[i]);     //
				break;
			case XRT_LAYER_PROJECTION:
			case XRT_LAYER_PROJECTION_DEPTH:
				render_gfx_layer_projection(        //
				    render,                         //
				    state->premultiplied_alphas[i], //
				    state->descriptor_sets[i]);     //
				break;
			default: break;
			}
		}

		render_gfx_end_view(render);

		render_gfx_end_target(render);
	}


	cmd_barrier_view_squash_images(                    //
	    render->r->vk,                                 //
	    d,                                             //
	    render->r->cmd,                                // cmd
	    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,          // src_access_mask
	    VK_ACCESS_SHADER_READ_BIT,                     // dst_access_mask
	    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,      // transition_from
	    transition_to,                                 //
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // src_stage_mask
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);        // dst_stage_mask

	return;

err_layer:
	// Allocator reset at end of frame, nothing to clean up.
	VK_ERROR(vk, "Layer processing failed, that shouldn't happen!");
}



void
comp_render_gfx_dispatch(struct render_gfx *render,
                         const struct comp_layer *layers,
                         const uint32_t layer_count,
                         const struct comp_render_dispatch_data *d)
{
	if (!d->target.initialized) {
		VK_ERROR(render->r->vk, "Target hasn't been initialized, not rendering anything.");
		assert(d->target.initialized);
		return;
	}

	// Convenience.
	bool fast_path = d->fast_path;

	// Only used if fast_path is true.
	const struct comp_layer *layer = &layers[0];

	// Consistency check.
	assert(!fast_path || layer_count >= 1);

	// We want to read from the images afterwards.
	VkImageLayout transition_to = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	if (fast_path && layer->data.type == XRT_LAYER_PROJECTION) {
		// Fast path.
		const struct xrt_layer_projection_data *proj = &layer->data.proj;
		const struct xrt_layer_projection_view_data *vds[XRT_MAX_VIEWS];
		for (uint32_t view = 0; view < d->target.view_count; ++view) {
			vds[view] = &proj->v[view];
		}
		crg_distortion_fast_path( //
		    render,               //
		    d,                    //
		    layer,                //
		    vds);                 //

	} else if (fast_path && layer->data.type == XRT_LAYER_PROJECTION_DEPTH) {
		// Fast path.
		const struct xrt_layer_projection_depth_data *depth = &layer->data.depth;
		const struct xrt_layer_projection_view_data *vds[XRT_MAX_VIEWS];
		for (uint32_t view = 0; view < d->target.view_count; ++view) {
			vds[view] = &depth->v[view];
		}
		crg_distortion_fast_path( //
		    render,               //
		    d,                    //
		    layer,                //
		    vds);                 //

	} else if (layer_count > 0 || d->passthrough) {
		// Graphics layer squasher
		if (fast_path) {
			U_LOG_W("Wanted fast path but no projection layer, falling back to layer squasher.");
		}

		// SteamVR draws quads in the distortion pass, after all projection content: split them (and cylinders) off
		// the squash.
		static struct comp_layer squashed[RENDER_MAX_LAYERS];
		static struct comp_layer quads[RENDER_MAX_LAYERS];
		uint32_t squash_count = 0;
		uint32_t quad_count = 0;
		for (uint32_t i = 0; i < layer_count; i++) {
			if (layers[i].data.type == XRT_LAYER_QUAD || layers[i].data.type == XRT_LAYER_CYLINDER) {
				quads[quad_count++] = layers[i];
			} else {
				squashed[squash_count++] = layers[i];
			}
		}
		const struct comp_layer *projection = NULL;
		const uint32_t unsupported = XRT_LAYER_COMPOSITION_VIEW_SPACE_BIT |
		    XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE | XRT_LAYER_COMPOSITION_ADVANCED_BLENDING_BIT |
		    XRT_LAYER_COMPOSITION_DEPTH_TEST | XRT_LAYER_COMPOSITION_INVERTED_ALPHA_BIT;
		if (squash_count == 1 &&
		    (squashed[0].data.type == XRT_LAYER_PROJECTION || squashed[0].data.type == XRT_LAYER_PROJECTION_DEPTH) &&
		    (squashed[0].data.flags & unsupported) == 0) {
			projection = &squashed[0];
			squash_count = 0;
			static bool logged = false;
			if (!logged) {
				U_LOG_I("Drawing the projection layer directly in the distortion pass with overlays (timewarp %s)",
				        d->do_timewarp ? "enabled" : "disabled");
				logged = true;
			}
		}

		/*
		 * Layer squashing.
		 */
		if (!d->mirror && !scratch_hidden(projection, d)) {
			comp_render_gfx_layers( //
			    render,             //
			    squashed,           //
			    squash_count,       //
			    d,                  //
			    transition_to);     //
		}

		/*
		 * Distortion.
		 */
		crg_distortion_after_squash( //
		    render,                  //
		    d,                       //
		    quads,                   //
		    quad_count,              //
		    projection);             //

	} else {
		// Just clear the screen
		crg_clear_output( //
		    render,       //
		    d);           //
	}
}
