// Copyright 2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metrics saving functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_results.h"

#include <stdio.h>


#ifdef __cplusplus
extern "C" {
#endif

struct u_metrics_session_frame
{
	int64_t session_id;
	int64_t frame_id;
	uint64_t predicted_frame_time_ns;
	uint64_t predicted_wake_up_time_ns;
	uint64_t predicted_gpu_done_time_ns;
	uint64_t predicted_display_time_ns;
	uint64_t predicted_display_period_ns;
	uint64_t display_time_ns;
	uint64_t when_predicted_ns;
	uint64_t when_wait_woke_ns;
	uint64_t when_begin_ns;
	uint64_t when_delivered_ns;
	uint64_t when_gpu_done_ns;
	bool discarded;
	uint64_t when_submit_begin_ns;
	uint64_t when_submit_end_ns;
};

struct u_metrics_used
{
	int64_t session_id;
	int64_t session_frame_id;
	int64_t system_frame_id;
	uint64_t when_ns;
};

struct u_metrics_system_frame
{
	int64_t frame_id;
	uint64_t predicted_display_time_ns;
	uint64_t predicted_display_period_ns;
	uint64_t desired_present_time_ns;
	uint64_t wake_up_time_ns;
	uint64_t present_slop_ns;
};

struct u_metrics_system_gpu_info
{
	int64_t frame_id;
	uint64_t gpu_start_ns;
	uint64_t gpu_end_ns;
	uint64_t when_ns;
};

struct u_metrics_system_present_info
{
	int64_t frame_id;
	uint64_t expected_comp_time_ns;
	uint64_t predicted_wake_up_time_ns;
	uint64_t predicted_done_time_ns;
	uint64_t predicted_display_time_ns;
	uint64_t when_predict_ns;
	uint64_t when_woke_ns;
	uint64_t when_began_ns;
	uint64_t when_submitted_ns;
	uint64_t when_infoed_ns;
	uint64_t desired_present_time_ns;
	uint64_t present_slop_ns;
	uint64_t present_margin_ns;
	uint64_t actual_present_time_ns;
	uint64_t earliest_present_time_ns;
};


void
u_metrics_init(void);

xrt_result_t
u_metrics_add_file(FILE *file, bool early_flush);

xrt_result_t
u_metrics_add_file_handle(xrt_file_handle_t handle, bool early_flush);

void
u_metrics_close(void);

bool
u_metrics_is_active(void);

void
u_metrics_write_session_frame(struct u_metrics_session_frame *umsf);

void
u_metrics_write_used(struct u_metrics_used *umu);

void
u_metrics_write_system_frame(struct u_metrics_system_frame *umsf);

void
u_metrics_write_system_gpu_info(struct u_metrics_system_gpu_info *umgi);

void
u_metrics_write_system_present_info(struct u_metrics_system_present_info *umpi);


#ifdef __cplusplus
}
#endif
