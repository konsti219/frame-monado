// Shared memory between the vrclient shim (in XRService) and Monado for the Frame's passthrough cameras.
// The shim publishes XRService's camera frame dma-bufs (as fd numbers in XRService, fetched with pidfd_getfd),
// frame states, metadata and undistortion maps; Monado sets the reader flags and claims frames.
#pragma once

#include <stdint.h>

#define SF_PT_SHM_PATH "/dev/shm/steamframe-passthrough"
#define SF_PT_MAGIC 0x54505346u // "SFPT"
#define SF_PT_VERSION 1u

// Camera types as XRService numbers them: 0 = IR tracking cameras (grey), 1 = Arcturus color module
#define SF_PT_TYPE_COUNT 2
#define SF_PT_TYPE_TRACKING 0
#define SF_PT_TYPE_COLOR 1
#define SF_PT_CAMERA_COUNT 2
#define SF_PT_MAX_FRAMES 16
#define SF_PT_MAX_PLANES 4
#define SF_PT_METADATA_SIZE 248
#define SF_PT_MAP_MAX 128

// Frame states, same values XRService sees through GetCameraFrameStates
#define SF_PT_FRAME_FREE 0u
#define SF_PT_FRAME_LATEST 1u
#define SF_PT_FRAME_READING 2u

struct sf_pt_frame
{
	uint32_t state; // atomic, see SF_PT_FRAME_*
	int32_t fds[SF_PT_MAX_PLANES]; // in XRService, per plane
	uint32_t _pad;
	uint64_t submit_seq;
	uint64_t submit_ns; // CLOCK_MONOTONIC
	uint8_t metadata[SF_PT_METADATA_SIZE];
};

struct sf_pt_camera
{
	struct sf_pt_frame frames[SF_PT_MAX_FRAMES];
	uint32_t map_width, map_height; // undistortion map, R32G32_SFLOAT rows
	uint64_t map_seq;               // bumped whenever the map contents change
	float map[SF_PT_MAP_MAX * SF_PT_MAP_MAX * 2];
};

struct sf_pt_type
{
	uint32_t generation; // atomic, odd while the shim is (re)initializing this type
	uint32_t initialized;
	uint32_t reader; // atomic, set by Monado to have XRService stream this type
	uint32_t frame_count;
	uint32_t width, height, drm_format, plane_count;
	uint64_t modifier;
	struct
	{
		uint32_t offset, stride;
	} planes[SF_PT_MAX_PLANES];
	uint8_t config[0x60]; // InitializeCamera's config as passed
	uint64_t latest_seq; // atomic
	struct sf_pt_camera cameras[SF_PT_CAMERA_COUNT];
};

struct sf_pt_shm
{
	uint32_t magic, version;
	int32_t pid;       // XRService
	uint32_t active;   // atomic, set by Monado: passthrough wanted at all
	uint64_t shm_size; // sizeof(struct sf_pt_shm) as built by the shim
	struct sf_pt_type types[SF_PT_TYPE_COUNT];
};
