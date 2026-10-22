// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "server/ipc_server.h"

void
ipc_capture_filter(volatile struct ipc_client_state *ics, struct xrt_device *xdev, struct xrt_input *inputs);
bool
ipc_capture_blocks_pose(volatile struct ipc_client_state *ics, struct xrt_device *xdev);
void
ipc_capture_cleanup(volatile struct ipc_client_state *ics);

xrt_result_t
ipc_capture_get_pose(volatile struct ipc_client_state *ics,
                     struct xrt_device *xdev,
                     enum xrt_input_name name,
                     int64_t at_timestamp,
                     struct xrt_space_relation *out_relation);
