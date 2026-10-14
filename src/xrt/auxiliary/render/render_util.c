// Copyright 2019-2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The compositor compute based rendering code.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup aux_render
 */

#include "math/m_api.h"
#include "os/os_time.h"
#include "math/m_matrix_4x4_f64.h"

#include "render/render_interface.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*!
 * Create a simplified projection matrix for timewarp.
 */
static void
calc_projection(const struct xrt_fov *fov, struct xrt_matrix_4x4_f64 *result)
{
	const double tan_left = tan((double)fov->angle_left);
	const double tan_right = tan((double)fov->angle_right);

	const double tan_down = tan((double)fov->angle_down);
	const double tan_up = tan((double)fov->angle_up);

	const bool vulkan_projection_space_y = true;

	const double tan_width = tan_right - tan_left;
	const double tan_height = vulkan_projection_space_y  // Projection space y direction:
	                              ? (tan_down - tan_up)  // Vulkan Y down
	                              : (tan_up - tan_down); // OpenGL Y up

	const double near_plane = 0.5;
	const double far_plane = 1.5;

	const double a11 = 2 / tan_width;
	const double a22 = 2 / tan_height;

	const double a31 = (tan_right + tan_left) / tan_width;
	const double a32 = (tan_up + tan_down) / tan_height;

	const double a33 = -far_plane / (far_plane - near_plane);
	const double a43 = -(far_plane * near_plane) / (far_plane - near_plane);


#if 0
	// We skip a33 & a43 because we don't have depth.
	(void)a33;
	(void)a43;

	// clang-format off
	*result = (struct xrt_matrix_4x4_f64){
		{
			      a11,         0,  0,  0,
			        0,       a22,  0,  0,
			      a31,       a32, -1,  0,
			        0,         0,  0,  1,
		}
	};
	// clang-format on
#else
	/*
	 * Apparently the timewarp doesn't look good without this path being
	 * used. With the above it stretches out. I tried with the code to see
	 * if I could affect the depth where the view was placed but couldn't
	 * see to do it, which is a head scratcher.
	 */
	// clang-format off
	*result = (struct xrt_matrix_4x4_f64) {
		.v = {
			a11, 0, 0, 0,
			0, a22, 0, 0,
			a31, a32, a33, -1,
			0, 0, a43, 0,
		}
	};
	// clang-format on
#endif
}


/*
 *
 * 'Exported' functions.
 *
 */

uint32_t
render_max_layers_capable(const struct vk_bundle *vk, bool use_compute, uint32_t desired_max_layers)
{
	/*!
	 * Graphics pipeline:
	 *
	 * This path has no relevant Vulkan device limits that would
	 * constrain the maximum number of layers (each layer uses a single descriptor
	 * set bound individually per draw).
	 */
	if (!use_compute) {
		// The min required by OpenXR spec is 16.
		return MAX(desired_max_layers, 16);
	}

	/*!
	 * Compute pipeline:
	 *
	 * Clamp max layers based on compute pipeline descriptor limits.
	 *
	 * The compute path uses an array of combined image samplers, with
	 * @ref samplers_per_layer samplers needed per layer. We check both the
	 * per-stage sampler and sampled image limits, then calculate the
	 * maximum number of complete layers that fit within those limits.
	 */
	uint32_t desired_image_sampler_count = desired_max_layers * RENDER_CS_MAX_SAMPLERS_PER_VIEW;

	const uint32_t max_sizes[] = {
	    vk->limits.max_per_stage_descriptor_samplers,
	    vk->limits.max_per_stage_descriptor_sampled_images,
	};
	for (uint32_t i = 0; i < ARRAY_SIZE(max_sizes); ++i) {
		desired_image_sampler_count = MIN(desired_image_sampler_count, max_sizes[i]);
	}

	const uint32_t calculated_max_layers = desired_image_sampler_count / RENDER_CS_MAX_SAMPLERS_PER_VIEW;

	if (calculated_max_layers < 16) {
		VK_WARN(vk,
		        "Device supports only %u compositor layers due to Vulkan limits. "
		        "which is below Vulkan minimum of 16. "
		        "This may indicate a driver bug. Attempting 16 anyway.",
		        calculated_max_layers);
	}

	// The min required by OpenXR spec is 16.
	return MAX(calculated_max_layers, 16);
}

//! Inverse fixation depth (float bits) for positional timewarp, and when it was last set (0 = never)
static uint32_t fixation_inverse_depth_bits;
static uint64_t fixation_updated_ns;

void
render_set_fixation_depth(float inverse_metres)
{
	uint32_t bits;
	memcpy(&bits, &inverse_metres, sizeof(bits));
	__atomic_store_n(&fixation_inverse_depth_bits, bits, __ATOMIC_RELAXED);
	__atomic_store_n(&fixation_updated_ns, os_monotonic_get_ns(), __ATOMIC_RELAXED);
}

//! Positional timewarp tuning, from $XDG_RUNTIME_DIR/frame-shell-timewarp ("key value" lines, written by the
//! frame shell's dashboard); SF_TIMEWARP_DEPTH=<m> fixes the depth instead of following the eyes.
struct timewarp_params
{
	double strength;      //!< 0..1, scales the correction
	double near;          //!< m, nearest depth used
	double far;           //!< m, fixation beyond counts as infinitely far (no correction), 0 = never
	double max_shift;     //!< m, largest translation corrected
	double smoothing;     //!< s, time constant of the depth
	double fixed_depth;   //!< m, SF_TIMEWARP_DEPTH (0 = rotation only), NAN = follow the eyes
};

static const struct timewarp_params *
timewarp_params(void)
{
	static struct timewarp_params params = {1.0, 0.6, 0.0, 0.02, 0.05, NAN};
	static bool env_read;
	static uint64_t next_read_ns;
	if (!env_read) {
		const char *env = getenv("SF_TIMEWARP_DEPTH");
		params.fixed_depth = env != NULL ? atof(env) : NAN;
		env_read = true;
	}
	uint64_t now = os_monotonic_get_ns();
	if (now < next_read_ns) {
		return &params;
	}
	next_read_ns = now + 500000000;
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL) {
		return &params;
	}
	char path[512];
	snprintf(path, sizeof(path), "%s/frame-shell-timewarp", runtime);
	FILE *file = fopen(path, "r");
	if (file == NULL) {
		return &params;
	}
	char key[32];
	double value;
	while (fscanf(file, "%31s %lf", key, &value) == 2) {
		if (strcmp(key, "strength") == 0) {
			params.strength = value;
		} else if (strcmp(key, "near") == 0) {
			params.near = value;
		} else if (strcmp(key, "far") == 0) {
			params.far = value;
		} else if (strcmp(key, "max_shift") == 0) {
			params.max_shift = value;
		} else if (strcmp(key, "smoothing") == 0) {
			params.smoothing = value;
		}
	}
	fclose(file);
	return &params;
}

//! The depth to correct at, as 1 / metres (0 = rotation only): the eyes' fixation, smoothed and limited
static double
timewarp_inverse_depth(const struct timewarp_params *params)
{
	static double smoothed;
	static uint64_t last_ns;

	double target = 0.0;
	if (params->fixed_depth <= 0.0) {
		return 0.0;
	} else if (params->fixed_depth > 0.0) {
		target = 1.0 / params->fixed_depth;
	} else {
		uint64_t updated = __atomic_load_n(&fixation_updated_ns, __ATOMIC_RELAXED);
		uint32_t bits = __atomic_load_n(&fixation_inverse_depth_bits, __ATOMIC_RELAXED);
		if (updated == 0 || os_monotonic_get_ns() - updated > 500000000) {
			smoothed = 0.0;
			return 0.0;
		}
		float value;
		memcpy(&value, &bits, sizeof(value));
		target = value;
	}
	if (params->far > 0.0 && target < 1.0 / params->far) {
		target = 0.0;
	}
	if (params->near > 0.0) {
		target = fmin(target, 1.0 / params->near);
	}

	uint64_t now = os_monotonic_get_ns();
	double dt = last_ns != 0 ? (double)(now - last_ns) * 1e-9 : 1.0;
	last_ns = now;
	double alpha = params->smoothing > 0.0 ? 1.0 - exp(-dt / params->smoothing) : 1.0;
	smoothed += (target - smoothed) * alpha;
	return smoothed * params->strength;
}

void
render_calc_time_warp_matrix(const struct xrt_pose *src_pose,
                             const struct xrt_fov *src_fov,
                             const struct xrt_pose *new_pose,
                             struct xrt_matrix_4x4 *matrix)
{
	// Src projection matrix.
	struct xrt_matrix_4x4_f64 src_proj;
	calc_projection(src_fov, &src_proj);

	// Src rotation matrix.
	struct xrt_matrix_4x4_f64 src_rot_inv;
	struct xrt_quat src_q = src_pose->orientation;
	m_mat4_f64_orientation(&src_q, &src_rot_inv); // This is a model matrix, a inverted view matrix.

	// New rotation matrix.
	struct xrt_matrix_4x4_f64 new_rot, new_rot_inv;
	struct xrt_quat new_q = new_pose->orientation;
	m_mat4_f64_orientation(&new_q, &new_rot_inv); // This is a model matrix, a inverted view matrix.
	m_mat4_f64_invert(&new_rot_inv, &new_rot);    // Invert to make it a view matrix.

	// Combine both rotation matrices to get difference.
	struct xrt_matrix_4x4_f64 delta_rot, delta_rot_inv;
	m_mat4_f64_multiply(&new_rot, &src_rot_inv, &delta_rot);
	m_mat4_f64_invert(&delta_rot, &delta_rot_inv);

	// Also correct the head's translation since the source pose, treating the scene as a plane at depth D: a
	// display ray (x, y, -1) meets it at D * (x, y, -1); in the source view that is D * (R * (x, y, -1) + t / D), and
	// the projection is homogeneous, so t / D is the matrix's translation column. Streamed frames are rendered from
	// a predicted position, and rotation-only timewarp shows the prediction error as uneven sideways steps. D is
	// where the eyes fixate (render_set_fixation_depth), tuned by timewarp_params().
	const struct timewarp_params *params = timewarp_params();
	double inverse_depth = timewarp_inverse_depth(params);
	if (inverse_depth > 0.0) {
		struct xrt_vec3 moved = {
		    new_pose->position.x - src_pose->position.x,
		    new_pose->position.y - src_pose->position.y,
		    new_pose->position.z - src_pose->position.z,
		};
		double length = sqrt(moved.x * moved.x + moved.y * moved.y + moved.z * moved.z);
		if (length > params->max_shift && length < 0.1) {
			double scale = params->max_shift / length;
			moved.x *= scale;
			moved.y *= scale;
			moved.z *= scale;
		}
		// Larger differences are recenters or tracking jumps, not prediction error
		if (length < 0.1) {
			struct xrt_quat src_q_inv;
			math_quat_invert(&src_q, &src_q_inv);
			struct xrt_vec3 t;
			math_quat_rotate_vec3(&src_q_inv, &moved, &t);
			delta_rot_inv.v[12] = t.x * inverse_depth;
			delta_rot_inv.v[13] = t.y * inverse_depth;
			delta_rot_inv.v[14] = t.z * inverse_depth;
		}
	}

	// Combine the source projection matrix and
	struct xrt_matrix_4x4_f64 result;
	m_mat4_f64_multiply(&src_proj, &delta_rot_inv, &result);

	// Convert from f64 to f32.
	for (int i = 0; i < 16; i++) {
		matrix->v[i] = (float)result.v[i];
	}
}

void
render_calc_time_warp_projection(const struct xrt_fov *fov, struct xrt_matrix_4x4 *result)
{
	struct xrt_matrix_4x4_f64 tmp;
	calc_projection(fov, &tmp);

	for (int i = 0; i < 16; i++) {
		result->v[i] = (float)tmp.v[i];
	}
}

void
render_calc_uv_to_tangent_lengths_rect(const struct xrt_fov *fov, struct xrt_normalized_rect *out_rect)
{
	const struct xrt_fov copy = *fov;

	const double tan_left = tan((double)copy.angle_left);
	const double tan_right = tan((double)copy.angle_right);

	const double tan_down = tan((double)copy.angle_down);
	const double tan_up = tan((double)copy.angle_up);

	const double tan_width = tan_right - tan_left;
	const double tan_height = tan_up - tan_down;

	/*
	 * I do not know why we have to calculate the offsets like this, but
	 * this one is the one that seems to work with what is currently in the
	 * calc timewarp matrix function and the distortion shader. It works
	 * with Index (unbalanced left and right angles) and WMR (unbalanced up
	 * and down angles) so here it is. In so far it matches what the gfx
	 * and non-timewarp compute pipeline produces.
	 */
	const double tan_offset_x = ((tan_right + tan_left) - tan_width) / 2;
	const double tan_offset_y = (-(tan_up + tan_down) - tan_height) / 2;

	struct xrt_normalized_rect transform = {
	    .x = (float)tan_offset_x,
	    .y = (float)tan_offset_y,
	    .w = (float)tan_width,
	    .h = (float)tan_height,
	};

	*out_rect = transform;
}
