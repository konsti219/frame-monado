// Records the passthrough cameras' capture poses (from XRService's frame metadata) for a while, to compare them
// with the driver's head poses (SF_POSE_TRACE). Usage: camtrace <seconds> <out.csv> [camera type, default 0]
// Columns: CLOCK_MONOTONIC ns of the capture (converted from the CLOCK_MONOTONIC_RAW metadata timestamp), camera,
// world_from_camera (12, rows), head_from_camera (12, rows).
#include "sf_passthrough.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double
now_s(clockid_t clock)
{
	struct timespec ts;
	clock_gettime(clock, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: camtrace <seconds> <out.csv> [type]\n");
		return 1;
	}
	double seconds = atof(argv[1]);
	int type = argc > 3 ? atoi(argv[3]) : SF_PT_TYPE_TRACKING;
	int fd = open(SF_PT_SHM_PATH, O_RDONLY);
	if (fd < 0) {
		perror(SF_PT_SHM_PATH);
		return 1;
	}
	const struct sf_pt_shm *shm = mmap(NULL, sizeof(*shm), PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (shm == MAP_FAILED)
		return 1;
	FILE *out = fopen(argv[2], "w");
	if (!out)
		return 1;
	const struct sf_pt_type *t = &shm->types[type];
	uint64_t last[SF_PT_CAMERA_COUNT] = {0};
	double end = now_s(CLOCK_MONOTONIC) + seconds;
	while (now_s(CLOCK_MONOTONIC) < end) {
		double offset = now_s(CLOCK_MONOTONIC) - now_s(CLOCK_MONOTONIC_RAW);
		for (int cam = 0; cam < SF_PT_CAMERA_COUNT; cam++)
			for (uint32_t i = 0; i < t->frame_count && i < SF_PT_MAX_FRAMES; i++) {
				const struct sf_pt_frame *f = &t->cameras[cam].frames[i];
				uint64_t seq = __atomic_load_n(&f->submit_seq, __ATOMIC_ACQUIRE);
				if (seq <= last[cam])
					continue;
				last[cam] = seq;
				float meta[62];
				double ts;
				memcpy(meta, f->metadata, sizeof(meta));
				memcpy(&ts, f->metadata, sizeof(ts));
				fprintf(out, "%.0f,%d", (ts + offset) * 1e9, cam);
				for (int k = 12; k < 36; k++)
					fprintf(out, ",%.6f", meta[k]);
				fprintf(out, "\n");
			}
		usleep(2000);
	}
	fclose(out);
	return 0;
}
