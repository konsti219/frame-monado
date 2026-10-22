// SPDX-License-Identifier: BSL-1.0
#include "server/ipc_server_input_capture.h"
#include "ipc_server_generated.h"
#include "ipc_input_capture_bindings.h"
#include "os/os_time.h"
#include "util/u_misc.h"
#include <stdlib.h>
#include <string.h>

static void
compile_capture_locked(struct ipc_server *s, const struct xrt_system_roles *roles)
{
	const struct ipc_input_capture *policy = &s->global_state.capture.policy;
	int32_t indices[] = {roles->left, roles->right};
	memset(s->global_state.capture.counts, 0, sizeof(s->global_state.capture.counts));
	for (uint32_t hand = 0; hand < 2; hand++) {
		if (indices[hand] < 0)
			continue;
		struct xrt_device *device = s->xsysd->static_xdevs[indices[hand]];
		for (uint32_t row = 0; row < ARRAY_SIZE(capture_bindings); row++) {
			const struct capture_binding *binding = &capture_bindings[row];
			if (binding->device != device->name || strcmp(binding->profile, policy->profiles[hand]) != 0)
				continue;
			for (uint32_t path = 0; path < policy->count; path++) {
				if (strcmp(binding->path, policy->paths[path]) != 0)
					continue;
				uint32_t count = s->global_state.capture.counts[hand];
				bool duplicate = false;
				for (uint32_t i = 0; i < count; i++)
					duplicate |= s->global_state.capture.inputs[hand][i] == binding->input;
				if (!duplicate) {
					assert(count < IPC_CAPTURE_MAX_PATHS);
					s->global_state.capture.inputs[hand][count] = binding->input;
					s->global_state.capture.counts[hand]++;
				}
			}
		}
	}
	s->global_state.capture.roles_generation = roles->generation_id;
}

xrt_result_t
ipc_handle_system_set_input_capture(volatile struct ipc_client_state *ics, const struct ipc_input_capture *capture)
{
	if (capture->count > IPC_CAPTURE_MAX_PATHS || capture->pose_mask > 3)
		return XRT_ERROR_IPC_FAILURE;
	for (uint32_t hand = 0; hand < 2; hand++) {
		if (!memchr(capture->profiles[hand], 0, IPC_CAPTURE_PATH_SIZE))
			return XRT_ERROR_IPC_FAILURE;
	}
	for (uint32_t i = 0; i < capture->count; i++) {
		if (!memchr(capture->paths[i], 0, IPC_CAPTURE_PATH_SIZE))
			return XRT_ERROR_IPC_FAILURE;
	}
	struct ipc_server *s = ics->server;
	os_mutex_lock(&s->global_state.lock);
	s->global_state.capture.owner_id = ics->client_state.id;
	s->global_state.capture.owner_pid = ics->client_state.pid;
	s->global_state.capture.policy = *capture;
	s->global_state.capture.roles_generation = 0;
	os_mutex_unlock(&s->global_state.lock);
	return XRT_SUCCESS;
}

static int
capture_hand_locked(volatile struct ipc_client_state *ics,
                    struct xrt_device *xdev,
                    const struct xrt_system_roles *roles)
{
	struct ipc_server *s = ics->server;
	if (!s->global_state.capture.owner_id || s->global_state.capture.owner_pid == ics->client_state.pid)
		return -1;
	int32_t indices[] = {roles->left, roles->right};
	for (int hand = 0; hand < 2; hand++) {
		if (indices[hand] >= 0 && s->xsysd->static_xdevs[indices[hand]] == xdev)
			return hand;
	}
	return -1;
}

bool
ipc_capture_blocks_pose(volatile struct ipc_client_state *ics, struct xrt_device *xdev)
{
	struct ipc_server *s = ics->server;
	struct xrt_system_roles roles = {0};
	xrt_system_devices_get_roles(s->xsysd, &roles);
	os_mutex_lock(&s->global_state.lock);
	int hand = capture_hand_locked(ics, xdev, &roles);
	bool blocked = hand >= 0 && (s->global_state.capture.policy.pose_mask & (1u << hand));
	os_mutex_unlock(&s->global_state.lock);
	return blocked;
}

static bool
value_equal(enum xrt_input_name name, union xrt_input_value a, union xrt_input_value b)
{
	switch (XRT_GET_INPUT_TYPE(name)) {
	case XRT_INPUT_TYPE_BOOLEAN: return a.boolean == b.boolean;
	case XRT_INPUT_TYPE_VEC1_ZERO_TO_ONE:
	case XRT_INPUT_TYPE_VEC1_MINUS_ONE_TO_ONE: return a.vec1.x == b.vec1.x;
	case XRT_INPUT_TYPE_VEC2_MINUS_ONE_TO_ONE: return a.vec2.x == b.vec2.x && a.vec2.y == b.vec2.y;
	default: return true;
	}
}

static struct ipc_capture_device_state *
capture_device_state(volatile struct ipc_client_state *ics,
                     struct xrt_device *xdev,
                     const struct xrt_system_roles *roles)
{
	struct ipc_capture_device_state *state = NULL;
	for (uint32_t i = 0; i < XRT_SYSTEM_MAX_DEVICES; i++) {
		struct ipc_capture_device_state *candidate =
		    (struct ipc_capture_device_state *)&ics->capture_devices[i];
		if (candidate->device == xdev) {
			state = candidate;
			break;
		}
		if (!candidate->device && !state)
			state = candidate;
	}
	assert(state);
	if (state->device != xdev || state->count != xdev->input_count ||
	    state->roles_generation != roles->generation_id) {
		free(state->inputs);
		*state = (struct ipc_capture_device_state){.device = xdev,
		                                           .count = xdev->input_count,
		                                           .roles_generation = roles->generation_id,
		                                           .inputs = calloc(xdev->input_count, sizeof(*state->inputs))};
		for (uint32_t i = 0; i < xdev->input_count; i++)
			state->inputs[i].group = i;
		for (uint32_t family = 0; family < ARRAY_SIZE(capture_families); family++) {
			if (capture_families[family].device != xdev->name)
				continue;
			uint32_t group = xdev->input_count;
			for (uint32_t i = 0; i < xdev->input_count; i++) {
				for (uint32_t member = 0; member < capture_families[family].count; member++) {
					if (xdev->inputs[i].name != capture_families[family].inputs[member])
						continue;
					if (group == xdev->input_count)
						group = state->inputs[i].group;
					uint32_t old_group = state->inputs[i].group;
					for (uint32_t j = 0; j < xdev->input_count; j++) {
						if (state->inputs[j].group == old_group)
							state->inputs[j].group = group;
					}
				}
			}
		}
	}
	return state;
}

void
ipc_capture_filter(volatile struct ipc_client_state *ics, struct xrt_device *xdev, struct xrt_input *inputs)
{
	struct ipc_server *s = ics->server;
	struct xrt_system_roles roles = {0};
	xrt_system_devices_get_roles(s->xsysd, &roles);
	bool blocked[1024] = {0};
	assert(xdev->input_count <= 1024);
	os_mutex_lock(&s->global_state.lock);
	int hand = capture_hand_locked(ics, xdev, &roles);
	if (hand >= 0) {
		if (s->global_state.capture.roles_generation != roles.generation_id)
			compile_capture_locked(s, &roles);
		for (uint32_t i = 0; i < xdev->input_count; i++) {
			for (uint32_t j = 0; j < s->global_state.capture.counts[hand]; j++) {
				blocked[i] |= s->global_state.capture.inputs[hand][j] == inputs[i].name;
			}
		}
	}
	os_mutex_unlock(&s->global_state.lock);
	struct ipc_capture_device_state *state = capture_device_state(ics, xdev, &roles);
	int64_t now = os_monotonic_get_ns();
	union xrt_input_value zero = {0};
	bool active[1024] = {0}, captured[1024] = {0};
	for (uint32_t i = 0; i < xdev->input_count; i++) {
		uint32_t group = state->inputs[i].group;
		active[group] |= inputs[i].active && !value_equal(inputs[i].name, inputs[i].value, zero);
		captured[group] |= blocked[i];
	}
	for (uint32_t group = 0; group < xdev->input_count; group++) {
		state->inputs[group].held = active[group] && (state->inputs[group].held || captured[group]);
	}
	for (uint32_t i = 0; i < xdev->input_count; i++) {
		struct xrt_input *input = &inputs[i];
		enum xrt_input_type type = XRT_GET_INPUT_TYPE(input->name);
		if (type != XRT_INPUT_TYPE_BOOLEAN && type != XRT_INPUT_TYPE_VEC1_ZERO_TO_ONE &&
		    type != XRT_INPUT_TYPE_VEC1_MINUS_ONE_TO_ONE && type != XRT_INPUT_TYPE_VEC2_MINUS_ONE_TO_ONE)
			continue;
		struct ipc_capture_input_state *held = &state->inputs[i];
		if (held->initialized && held->previous.name != input->name)
			held->initialized = false;
		uint32_t group = held->group;
		bool suppress = captured[group] || state->inputs[group].held;
		if (suppress)
			input->value = zero;
		if (held->initialized && held->previous.name == input->name && held->previous.active == input->active &&
		    value_equal(input->name, input->value, held->previous.value)) {
			input->timestamp = held->previous.timestamp;
		} else if (suppress) {
			input->timestamp = now;
		}
		held->previous = *input;
		held->initialized = true;
	}
}

xrt_result_t
ipc_capture_get_pose(volatile struct ipc_client_state *ics,
                     struct xrt_device *xdev,
                     enum xrt_input_name name,
                     int64_t at_timestamp,
                     struct xrt_space_relation *out_relation)
{
	struct xrt_system_roles roles = {0};
	xrt_system_devices_get_roles(ics->server->xsysd, &roles);
	struct ipc_server *s = ics->server;
	os_mutex_lock(&s->global_state.lock);
	int hand = capture_hand_locked(ics, xdev, &roles);
	os_mutex_unlock(&s->global_state.lock);
	if (hand < 0)
		return xrt_device_get_tracked_pose(xdev, name, at_timestamp, out_relation);
	struct ipc_capture_device_state *state = capture_device_state(ics, xdev, &roles);
	uint32_t slot;
	for (slot = 0; slot < ARRAY_SIZE(state->poses); slot++) {
		if (state->poses[slot].name == name || !state->poses[slot].name)
			break;
	}
	assert(slot < ARRAY_SIZE(state->poses));
	if (state->poses[slot].name && ipc_capture_blocks_pose(ics, xdev)) {
		*out_relation = state->poses[slot].relation;
		out_relation->linear_velocity = (struct xrt_vec3){0};
		out_relation->angular_velocity = (struct xrt_vec3){0};
		return XRT_SUCCESS;
	}
	xrt_result_t result = xrt_device_get_tracked_pose(xdev, name, at_timestamp, out_relation);
	if (result == XRT_SUCCESS) {
		state->poses[slot].name = name;
		state->poses[slot].relation = *out_relation;
	}
	return result;
}

void
ipc_capture_cleanup(volatile struct ipc_client_state *ics)
{
	struct ipc_server *s = ics->server;
	if (s->global_state.capture.owner_id == ics->client_state.id) {
		memset(&s->global_state.capture, 0, sizeof(s->global_state.capture));
	}
	for (uint32_t i = 0; i < XRT_SYSTEM_MAX_DEVICES; i++) {
		free(ics->capture_devices[i].inputs);
		memset((void *)&ics->capture_devices[i], 0, sizeof(ics->capture_devices[i]));
	}
}
