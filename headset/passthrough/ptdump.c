// Test reader for the passthrough shared memory: requests a camera type, then dumps the latest frame of each camera
// (Y plane as PGM, needs a linear layout) and the undistortion maps. Run with sudo, for pidfd_getfd on XRService.
// Usage: ptdump [type] [outdir]
#include "sf_passthrough.h"

#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

static int
grab_fd(int pid, int fd)
{
	int pidfd = syscall(SYS_pidfd_open, pid, 0);
	if (pidfd < 0) {
		return -1;
	}
	int ret = syscall(SYS_pidfd_getfd, pidfd, fd, 0);
	close(pidfd);
	return ret;
}

static void
dump_frame(struct sf_pt_shm *shm, int type, int cam, const char *dir)
{
	struct sf_pt_type *t = &shm->types[type];
	struct sf_pt_camera *c = &t->cameras[cam];
	int index = -1;
	uint64_t best = 0;
	for (uint32_t i = 0; i < t->frame_count; i++) {
		if (__atomic_load_n(&c->frames[i].state, __ATOMIC_ACQUIRE) == SF_PT_FRAME_LATEST &&
		    c->frames[i].submit_seq > best) {
			best = c->frames[i].submit_seq;
			index = i;
		}
	}
	// Monado's compositor claims new frames almost at once; then take the newest one without claiming it
	bool claimed = index >= 0;
	for (uint32_t i = 0; !claimed && i < t->frame_count; i++)
		if (index < 0 || c->frames[i].submit_seq > c->frames[index].submit_seq)
			index = i;
	if (index < 0 || !c->frames[index].submit_seq) {
		printf("type %d cam %d: no frame\n", type, cam);
		return;
	}
	struct sf_pt_frame *f = &c->frames[index];
	uint32_t expected = SF_PT_FRAME_LATEST;
	if (claimed && !__atomic_compare_exchange_n(&f->state, &expected, SF_PT_FRAME_READING, 0, __ATOMIC_ACQ_REL,
	                                            __ATOMIC_ACQUIRE)) {
		printf("type %d cam %d: lost the frame\n", type, cam);
		return;
	}
	int fd = grab_fd(shm->pid, f->fds[0]);
	if (fd < 0) {
		printf("pidfd_getfd: %s\n", strerror(errno));
		goto out;
	}
	size_t size = lseek(fd, 0, SEEK_END);
	uint8_t *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		printf("mmap: %s\n", strerror(errno));
		close(fd);
		goto out;
	}
	struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
	ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	char path[256];
	snprintf(path, sizeof(path), "%s/type%d-cam%d.pgm", dir, type, cam);
	FILE *out = fopen(path, "wb");
	fprintf(out, "P5 %u %u 255\n", t->width, t->height);
	for (uint32_t y = 0; y < t->height; y++) {
		fwrite(p + t->planes[0].offset + y * t->planes[0].stride, 1, t->width, out);
	}
	fclose(out);
	snprintf(path, sizeof(path), "%s/type%d-cam%d.nv12", dir, type, cam);
	out = fopen(path, "wb");
	fwrite(p, 1, size, out);
	fclose(out);
	snprintf(path, sizeof(path), "%s/type%d-cam%d.meta", dir, type, cam);
	out = fopen(path, "wb");
	fwrite(f->metadata, 1, SF_PT_METADATA_SIZE, out);
	fclose(out);
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	munmap(p, size);
	close(fd);
	printf("type %d cam %d: frame %d seq %lu, %zu bytes, modifier %#lx\n", type, cam, index,
	       (unsigned long)f->submit_seq, size, (unsigned long)t->modifier);
out:
	if (claimed)
		__atomic_store_n(&f->state, SF_PT_FRAME_FREE, __ATOMIC_RELEASE);
}

int
main(int argc, char **argv)
{
	if (argc > 3) {
		fprintf(stderr, "usage: ptdump [type] [outdir]\n");
		return 1;
	}
	int type = argc > 1 ? atoi(argv[1]) : SF_PT_TYPE_COLOR;
	const char *dir = argc > 2 ? argv[2] : ".";
	if (type < 0 || type >= SF_PT_TYPE_COUNT) {
		fprintf(stderr, "camera type must be 0 or 1\n");
		return 1;
	}
	struct stat dir_stat;
	if (stat(dir, &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) {
		fprintf(stderr, "output directory does not exist: %s\n", dir);
		return 1;
	}
	int fd = open(SF_PT_SHM_PATH, O_RDWR);
	if (fd < 0) {
		perror(SF_PT_SHM_PATH);
		return 1;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size != sizeof(struct sf_pt_shm)) {
		fprintf(stderr, "bad shared memory size\n");
		close(fd);
		return 1;
	}
	struct sf_pt_shm *shm = mmap(NULL, sizeof(*shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (shm == MAP_FAILED || shm->magic != SF_PT_MAGIC || shm->version != SF_PT_VERSION ||
	    shm->shm_size != sizeof(*shm)) {
		fprintf(stderr, "bad shared memory\n");
		return 1;
	}
	if (shm->pid <= 0 || kill(shm->pid, 0) != 0 || !shm->types[type].initialized) {
		fprintf(stderr, "XRService isn't running with this camera initialized\n");
		munmap(shm, sizeof(*shm));
		return 1;
	}
	printf("XRService %d, type %d initialized %u: %ux%u %.4s modifier %#lx\n", shm->pid, type,
	       shm->types[type].initialized, shm->types[type].width, shm->types[type].height,
	       (char *)&shm->types[type].drm_format, (unsigned long)shm->types[type].modifier);
	for (uint32_t p = 0; p < shm->types[type].plane_count && p < SF_PT_MAX_PLANES; p++) {
		printf("plane %u: offset %u stride %u\n", p, shm->types[type].planes[p].offset,
		       shm->types[type].planes[p].stride);
	}
	uint32_t was_active = __atomic_exchange_n(&shm->active, 1, __ATOMIC_ACQ_REL);
	uint32_t was_reader = __atomic_load_n(&shm->types[type].reader, __ATOMIC_ACQUIRE);
	__atomic_store_n(&shm->types[type].reader, 1, __ATOMIC_RELEASE);
	uint64_t start = __atomic_load_n(&shm->types[type].latest_seq, __ATOMIC_ACQUIRE);
	for (int i = 0; i < 100 && __atomic_load_n(&shm->types[type].latest_seq, __ATOMIC_ACQUIRE) < start + 20; i++) {
		usleep(100000);
	}
	printf("%lu frames in the wait\n", (unsigned long)(shm->types[type].latest_seq - start));
	for (int cam = 0; cam < SF_PT_CAMERA_COUNT; cam++) {
		dump_frame(shm, type, cam, dir);
		struct sf_pt_camera *c = &shm->types[type].cameras[cam];
		char path[256];
		snprintf(path, sizeof(path), "%s/type%d-cam%d.map", dir, type, cam);
		FILE *out = fopen(path, "wb");
		fwrite(c->map, sizeof(float), c->map_width * c->map_height * 2, out);
		fclose(out);
		printf("map %ux%u seq %lu\n", c->map_width, c->map_height, (unsigned long)c->map_seq);
	}
	__atomic_store_n(&shm->types[type].reader, was_reader, __ATOMIC_RELEASE);
	__atomic_store_n(&shm->active, was_active, __ATOMIC_RELEASE);
	munmap(shm, sizeof(*shm));
	return 0;
}
