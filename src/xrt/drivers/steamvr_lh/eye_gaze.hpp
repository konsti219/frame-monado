// Copyright 2026, Monado contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Reads Steam Frame eye gaze from Valve's eye tracking server shared memory.
 * @ingroup drv_steamvr_lh
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

/*!
 * Client of /dev/shm/eye-server.mmap, which Valve's `eyetracking` server publishes (layout versions 4 and 5).
 * The same protocol as driver_cv's CEyeTrackingMmapClient: request a sample, wait for the sequence to change.
 */
class EyeGazeReader
{
public:
	EyeGazeReader();
	~EyeGazeReader();

	//! Gaze direction in head space (-Z forward); false if there is no recent valid sample
	bool
	latest(xrt_vec3 *out_direction, uint64_t *out_receive_ns);

private:
	std::atomic<bool> running{true};
	std::thread thread;
	std::mutex mut;
	xrt_vec3 direction{0, 0, -1};
	uint64_t receive_ns{0};

	void
	run();
};
