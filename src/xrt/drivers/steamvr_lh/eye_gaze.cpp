// Copyright 2026, Monado contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Reads Steam Frame eye gaze from Valve's eye tracking server shared memory.
 * @ingroup drv_steamvr_lh
 */

#include "eye_gaze.hpp"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>

#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
constexpr const char *PATH = "/dev/shm/eye-server.mmap";
constexpr uint64_t STALE_NS = 200'000'000;

// Header, the same in all known versions
constexpr size_t OFF_VERSION = 0x00;
constexpr size_t OFF_INITIALIZED = 0x04;
constexpr size_t OFF_MUTEX = 0x08;
constexpr size_t OFF_SEQUENCE = 0x38;
constexpr size_t OFF_REQUESTED = 0x3c;

// Within the packed record
constexpr size_t REC_PRODUCER_STATE = 0x00;
constexpr size_t REC_GAZE = 0x0d;      // float[2][3], left and right after fusion
constexpr size_t REC_FIXATION = 0x3d; // float[3], head-relative point in metres

//! Segment size and record offset per version: v4 on SteamOS 0.5.0, v5 since build 20260930
bool
layout(uint32_t version, size_t *size, size_t *record)
{
	switch (version) {
	case 4: *size = 0x4f21a, *record = 0x152; return true;
	case 5: *size = 0x4f21f, *record = 0x157; return true;
	default: return false;
	}
}

template <typename T>
T
read(const uint8_t *base, size_t offset)
{
	T value;
	std::memcpy(&value, base + offset, sizeof(T));
	return value;
}

bool
normalize(xrt_vec3 *v)
{
	float len = std::sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
	if (!(len > 1e-6f) || !std::isfinite(len)) {
		return false;
	}
	v->x /= len, v->y /= len, v->z /= len;
	return true;
}
} // namespace

EyeGazeReader::EyeGazeReader() : thread(&EyeGazeReader::run, this) {}

EyeGazeReader::~EyeGazeReader()
{
	running = false;
	thread.join();
}

bool
EyeGazeReader::latest(xrt_vec3 *out_direction, uint64_t *out_receive_ns)
{
	std::lock_guard lk(mut);
	if (receive_ns == 0 || os_monotonic_get_ns() - receive_ns > STALE_NS) {
		return false;
	}
	*out_direction = direction;
	*out_receive_ns = receive_ns;
	return true;
}

void
EyeGazeReader::run()
{
	uint8_t *base = nullptr;
	size_t map_size = 0;
	size_t record = 0;
	uint32_t unsupported = 0;

	while (running) {
		if (base == nullptr) {
			// The server only exists while the eye tracker runs, so keep trying
			int fd = open(PATH, O_RDWR | O_CLOEXEC);
			struct stat st;
			if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= 0x40) {
				void *mem = mmap(nullptr, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
				size_t needed = 0;
				if (mem != MAP_FAILED &&
				    layout(read<uint32_t>((uint8_t *)mem, OFF_VERSION), &needed, &record) &&
				    (size_t)st.st_size >= needed) {
					base = (uint8_t *)mem;
					map_size = st.st_size;
					U_LOG_I("Reading eye gaze from %s (version %u)", PATH, read<uint32_t>(base, OFF_VERSION));
				} else if (mem != MAP_FAILED) {
					// Once per version, not on every retry
					uint32_t version = read<uint32_t>((uint8_t *)mem, OFF_VERSION);
					if (version != unsupported) {
						U_LOG_W("Unsupported eye tracking shared memory version %u", version);
						unsupported = version;
					}
					munmap(mem, st.st_size);
				}
			}
			if (fd >= 0) {
				close(fd);
			}
			if (base == nullptr) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				continue;
			}
		}

		if (read<uint32_t>(base, OFF_INITIALIZED) != 1) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}

		auto *mutex = reinterpret_cast<pthread_mutex_t *>(base + OFF_MUTEX);
		auto *sequence = reinterpret_cast<volatile uint32_t *>(base + OFF_SEQUENCE);
		auto *requested = reinterpret_cast<volatile uint32_t *>(base + OFF_REQUESTED);

		int ret = pthread_mutex_lock(mutex);
		if (ret == EOWNERDEAD) {
			pthread_mutex_consistent(mutex);
		} else if (ret != 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		uint32_t seq = *sequence;
		*requested = 1;
		pthread_mutex_unlock(mutex);

		timespec timeout{0, 100'000'000};
		syscall(SYS_futex, sequence, FUTEX_WAIT, seq, &timeout, nullptr, 0);

		if (pthread_mutex_lock(mutex) != 0) {
			continue;
		}
		bool fresh = *sequence != seq;
		uint8_t rec[0x80];
		std::memcpy(rec, base + record, sizeof(rec));
		pthread_mutex_unlock(mutex);

		if (!fresh || read<uint32_t>(rec, REC_PRODUCER_STATE) != 1) {
			continue;
		}

		// The fixation point gives one combined gaze; fall back to the mean of both eyes
		xrt_vec3 dir = read<xrt_vec3>(rec, REC_FIXATION);
		if (!normalize(&dir) || dir.z > 0) {
			xrt_vec3 l = read<xrt_vec3>(rec, REC_GAZE);
			xrt_vec3 r = read<xrt_vec3>(rec, REC_GAZE + 12);
			dir = {l.x + r.x, l.y + r.y, l.z + r.z};
			if (!normalize(&dir)) {
				continue;
			}
		}

		std::lock_guard lk(mut);
		direction = dir;
		receive_ns = os_monotonic_get_ns();
	}

	if (base != nullptr) {
		munmap(base, map_size);
	}
}
