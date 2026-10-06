#pragma once

// IVRCameraPassthroughInternal_001, publishing camera frames to Monado; null if the shared memory can't be set up
void *
camera_passthrough_interface();

// vrclient.cpp's log
void
passthrough_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
