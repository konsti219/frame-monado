// Copyright 2023, Shawn Wallace
// Copyright 2026, Monado contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief SteamVR IVRBlockQueue implementation backed by POSIX shared memory.
 * @ingroup drv_steamvr_lh
 */

#include "blockqueue.hpp"
#include "paths.hpp"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <string>

#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
constexpr uint32_t MAGIC = 0x53464251; // "SFBQ"
constexpr size_t MAX_BLOCKS = 64;

struct BlockState
{
	std::atomic<uint64_t> seq;     // publish sequence of the data in this block, 0 if never written
	std::atomic<int32_t> readers;  // readers holding the block
	std::atomic<int32_t> writing;  // writer holding the block
};
} // namespace

struct BlockQueueShared
{
	uint32_t magic;
	uint32_t data_size;
	uint32_t header_size;
	uint32_t count;
	uint32_t flags;
	std::atomic<uint32_t> futex; // bumped on every publish
	std::atomic<int32_t> connections;
	std::atomic<uint64_t> seq; // last published sequence
	BlockState blocks[MAX_BLOCKS];

	size_t
	block_size() const
	{
		return ((size_t)header_size + data_size + 63) & ~(size_t)63;
	}
	//! A block's data; its header (header_size bytes) sits right before it and is used through negative offsets,
	//! e.g. driver_cv writes the controller serial and string lengths there
	void *
	block(uint32_t i)
	{
		return reinterpret_cast<uint8_t *>(this) + ((sizeof(BlockQueueShared) + 63) & ~(size_t)63) + i * block_size() +
		       header_size;
	}
};

namespace {
// A block handle: tag | queue handle << 8 | block index + 1
uint64_t
encode_block(uint64_t tag, vr::PropertyContainerHandle_t queue, uint32_t index)
{
	return tag | (queue << 8) | (index + 1);
}

/*!
 * Block properties, as vrserver lays them out in the block header: count, bytes used by values, magic, then
 * 16-byte entries {id, type tag, size, offset}; values are packed from the end of the header downward.
 */
struct HeaderTable
{
	uint32_t count;
	uint32_t used;
	uint32_t magic;
	uint32_t reserved;
	struct Entry
	{
		uint32_t prop;
		uint32_t tag;
		uint32_t size;
		uint32_t offset;
	} entries[];
};
constexpr uint32_t HEADER_MAGIC = 0xacbdf123;

std::string
shm_name(const char *path)
{
	std::string name = "/sfbq";
	for (const char *c = path; *c; c++) {
		name += (*c == '/') ? '_' : *c;
	}
	return name;
}

void
futex_wake(std::atomic<uint32_t> *addr)
{
	syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

void
futex_wait(std::atomic<uint32_t> *addr, uint32_t expected, uint32_t timeout_ms)
{
	timespec ts{(time_t)(timeout_ms / 1000), (long)(timeout_ms % 1000) * 1000000};
	syscall(SYS_futex, addr, FUTEX_WAIT, expected, &ts, nullptr, 0);
}
} // namespace

BlockQueue::~BlockQueue()
{
	for (auto &q : queues) {
		if (q && q->shared) {
			q->shared->connections.fetch_sub(1);
			munmap(q->shared, q->size);
		}
	}
}

static std::string
queue_name(const char *path)
{
	const char *slash = std::strrchr(path, '/');
	return slash ? slash + 1 : path;
}

vr::ETrackedPropertyError
BlockQueue::read_queue_properties(vr::PropertyContainerHandle_t queue, vr::PropertyRead_t *batch, uint32_t count)
{
	Queue *q = get(queue);
	if (q == nullptr) {
		return vr::TrackedProp_InvalidContainer;
	}
	const std::pair<const char *, uint32_t> props[] = {{"blockDataSize", q->shared->data_size},
	                                                   {"blockHeaderSize", q->shared->header_size},
	                                                   {"blockCount", q->shared->count}};
	for (uint32_t i = 0; i < count; i++) {
		vr::PropertyRead_t &r = batch[i];
		r.eError = vr::TrackedProp_UnknownProperty;
		for (const auto &[name, value] : props) {
			std::string path = "/" + q->name + "/" + name;
			if ((uint32_t)r.prop != (uint32_t)Paths::handle_for(path.c_str())) {
				continue;
			}
			r.unTag = vr::k_unInt32PropertyTag;
			r.unRequiredBufferSize = sizeof(int32_t);
			if (r.pvBuffer == nullptr || r.unBufferSize < sizeof(int32_t)) {
				r.eError = vr::TrackedProp_BufferTooSmall;
			} else {
				int32_t v = (int32_t)value;
				std::memcpy(r.pvBuffer, &v, sizeof(v));
				r.eError = vr::TrackedProp_Success;
			}
		}
	}
	return vr::TrackedProp_Success;
}

BlockQueue::Queue *
BlockQueue::get(vr::PropertyContainerHandle_t handle)
{
	std::lock_guard lk(mut);
	if (handle == 0 || handle > queues.size() || !queues[handle - 1]) {
		return nullptr;
	}
	return queues[handle - 1].get();
}

vr::EBlockQueueError
BlockQueue::Create(vr::PropertyContainerHandle_t *pulQueueHandle,
                   char *pchPath,
                   uint32_t unBlockDataSize,
                   uint32_t unBlockHeaderSize,
                   uint32_t unBlockCount,
                   uint32_t unFlags)
{
	if (pulQueueHandle == nullptr || pchPath == nullptr || unBlockCount == 0 || unBlockCount > MAX_BLOCKS) {
		return vr::EBlockQueueError_BlockQueueError_InvalidParam;
	}
	std::string name = shm_name(pchPath);
	BlockQueueShared probe{};
	probe.data_size = unBlockDataSize;
	probe.header_size = unBlockHeaderSize;
	size_t size = ((sizeof(BlockQueueShared) + 63) & ~(size_t)63) + unBlockCount * probe.block_size();
	// A queue left behind by a process that died is replaced
	shm_unlink(name.c_str());
	int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return vr::EBlockQueueError_BlockQueueError_InternalError;
	}
	if (ftruncate(fd, size) != 0) {
		close(fd);
		return vr::EBlockQueueError_BlockQueueError_InternalError;
	}
	void *mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (mem == MAP_FAILED) {
		return vr::EBlockQueueError_BlockQueueError_InternalError;
	}
	auto *shared = new (mem) BlockQueueShared{};
	shared->data_size = unBlockDataSize;
	shared->header_size = unBlockHeaderSize;
	shared->count = unBlockCount;
	shared->flags = unFlags;
	shared->magic = MAGIC;

	std::lock_guard lk(mut);
	queues.push_back(std::make_unique<Queue>(Queue{shared, size, true, 0, queue_name(pchPath)}));
	*pulQueueHandle = queues.size();
	return vr::EBlockQueueError_BlockQueueError_None;
}

vr::EBlockQueueError
BlockQueue::Connect(vr::PropertyContainerHandle_t *pulQueueHandle, char *pchPath)
{
	if (pulQueueHandle == nullptr || pchPath == nullptr) {
		return vr::EBlockQueueError_BlockQueueError_InvalidParam;
	}
	int fd = shm_open(shm_name(pchPath).c_str(), O_RDWR, 0);
	if (fd < 0) {
		return vr::EBlockQueueError_BlockQueueError_QueueNotFound;
	}
	off_t size = lseek(fd, 0, SEEK_END);
	void *mem = size > 0 ? mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
	close(fd);
	if (mem == MAP_FAILED) {
		return vr::EBlockQueueError_BlockQueueError_InternalError;
	}
	auto *shared = static_cast<BlockQueueShared *>(mem);
	if (shared->magic != MAGIC) {
		munmap(mem, size);
		return vr::EBlockQueueError_BlockQueueError_QueueNotFound;
	}
	shared->connections.fetch_add(1);

	std::lock_guard lk(mut);
	// Start before the oldest block still in the ring: XRService connects after driver_cv already wrote the
	// controller connect events, and has to read them
	queues.push_back(std::make_unique<Queue>(Queue{shared, (size_t)size, false, 0, queue_name(pchPath)}));
	*pulQueueHandle = queues.size();
	return vr::EBlockQueueError_BlockQueueError_None;
}

vr::EBlockQueueError
BlockQueue::Destroy(vr::PropertyContainerHandle_t ulQueueHandle)
{
	std::lock_guard lk(mut);
	if (ulQueueHandle == 0 || ulQueueHandle > queues.size() || !queues[ulQueueHandle - 1]) {
		return vr::EBlockQueueError_BlockQueueError_InvalidHandle;
	}
	Queue &q = *queues[ulQueueHandle - 1];
	if (!q.creator) {
		q.shared->connections.fetch_sub(1);
	}
	munmap(q.shared, q.size);
	queues[ulQueueHandle - 1].reset();
	return vr::EBlockQueueError_BlockQueueError_None;
}

vr::EBlockQueueError
BlockQueue::AcquireWriteOnlyBlock(vr::PropertyContainerHandle_t ulQueueHandle,
                                  vr::PropertyContainerHandle_t *pulBlockHandle,
                                  void **ppvBuffer)
{
	Queue *q = get(ulQueueHandle);
	if (q == nullptr) {
		return vr::EBlockQueueError_BlockQueueError_InvalidHandle;
	}
	BlockQueueShared *s = q->shared;
	int32_t best = -1;
	for (int attempt = 0; attempt < 4 && best < 0; attempt++) {
		// The oldest block nobody is reading or writing
		for (uint32_t i = 0; i < s->count; i++) {
			if (s->blocks[i].readers.load() != 0 || s->blocks[i].writing.load() != 0) {
				continue;
			}
			if (best < 0 || s->blocks[i].seq.load() < s->blocks[best].seq.load()) {
				best = i;
			}
		}
		int32_t expected = 0;
		if (best < 0 || !s->blocks[best].writing.compare_exchange_strong(expected, 1)) {
			best = -1;
			continue;
		}
		// A reader may have counted itself in between the scan and the claim (it backs off on seeing writing)
		if (s->blocks[best].readers.load() != 0) {
			s->blocks[best].writing.store(0);
			best = -1;
		}
	}
	if (best < 0) {
		return vr::EBlockQueueError_BlockQueueError_BlockNotAvailable;
	}
	*pulBlockHandle = encode_block(BLOCK_HANDLE_TAG, ulQueueHandle, best);
	*ppvBuffer = s->block(best);
	if (s->header_size >= sizeof(HeaderTable)) {
		std::memset((uint8_t *)s->block(best) - s->header_size, 0, s->header_size);
	}
	return vr::EBlockQueueError_BlockQueueError_None;
}

vr::EBlockQueueError
BlockQueue::ReleaseWriteOnlyBlock(vr::PropertyContainerHandle_t ulQueueHandle,
                                  vr::PropertyContainerHandle_t ulBlockHandle)
{
	Queue *q = get(ulQueueHandle);
	uint32_t index = (ulBlockHandle & 0xff) - 1;
	if (q == nullptr || !is_block_handle(ulBlockHandle) || index >= q->shared->count) {
		return vr::EBlockQueueError_BlockQueueError_InvalidHandle;
	}
	BlockQueueShared *s = q->shared;
	BlockState &b = s->blocks[index];
	b.seq.store(s->seq.fetch_add(1) + 1);
	b.writing.store(0);
	s->futex.fetch_add(1);
	futex_wake(&s->futex);
	return vr::EBlockQueueError_BlockQueueError_None;
}

vr::EBlockQueueError
BlockQueue::acquire_read(Queue *q,
                         vr::PropertyContainerHandle_t queue_handle,
                         vr::PropertyContainerHandle_t *pulBlockHandle,
                         void **ppvBuffer,
                         vr::EBlockQueueReadType type)
{
	BlockQueueShared *s = q->shared;
	for (int attempt = 0; attempt < 4; attempt++) {
		// Latest: newest block; New: newest if unseen; Next: oldest unseen
		int32_t pick = -1;
		for (uint32_t i = 0; i < s->count; i++) {
			uint64_t seq = s->blocks[i].seq.load();
			if (seq == 0 || s->blocks[i].writing.load() != 0) {
				continue;
			}
			if (type != vr::EBlockQueueReadType_BlockQueueRead_Latest && seq <= q->last_read) {
				continue;
			}
			bool better = pick < 0 || (type == vr::EBlockQueueReadType_BlockQueueRead_Next
			                               ? seq < s->blocks[pick].seq.load()
			                               : seq > s->blocks[pick].seq.load());
			if (better) {
				pick = i;
			}
		}
		if (pick < 0) {
			return vr::EBlockQueueError_BlockQueueError_BlockNotAvailable;
		}
		BlockState &b = s->blocks[pick];
		uint64_t seq = b.seq.load();
		b.readers.fetch_add(1);
		// The writer may have grabbed it between the scan and the reader count
		if (b.writing.load() != 0 || b.seq.load() != seq) {
			b.readers.fetch_sub(1);
			continue;
		}
		q->last_read = seq;
		*pulBlockHandle = encode_block(BLOCK_HANDLE_TAG, queue_handle, pick);
		*ppvBuffer = s->block(pick);
		return vr::EBlockQueueError_BlockQueueError_None;
	}
	return vr::EBlockQueueError_BlockQueueError_BlockNotAvailable;
}

vr::EBlockQueueError
BlockQueue::WaitAndAcquireReadOnlyBlock(vr::PropertyContainerHandle_t ulQueueHandle,
                                        vr::PropertyContainerHandle_t *pulBlockHandle,
                                        void **ppvBuffer,
                                        vr::EBlockQueueReadType eReadType,
                                        uint32_t unTimeoutMs)
{
	Queue *q = get(ulQueueHandle);
	if (q == nullptr) {
		return vr::EBlockQueueError_BlockQueueError_InvalidHandle;
	}
	timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	while (true) {
		uint32_t futex = q->shared->futex.load();
		vr::EBlockQueueError err = acquire_read(q, ulQueueHandle, pulBlockHandle, ppvBuffer, eReadType);
		if (err != vr::EBlockQueueError_BlockQueueError_BlockNotAvailable) {
			return err;
		}
		timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		int64_t elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
		if (elapsed_ms >= unTimeoutMs) {
			return err;
		}
		futex_wait(&q->shared->futex, futex, unTimeoutMs - elapsed_ms);
	}
}

vr::EBlockQueueError
BlockQueue::AcquireReadOnlyBlock(vr::PropertyContainerHandle_t ulQueueHandle,
                                 vr::PropertyContainerHandle_t *pulBlockHandle,
                                 void **ppvBuffer,
                                 vr::EBlockQueueReadType eReadType)
{
	Queue *q = get(ulQueueHandle);
	if (q == nullptr) {
		return vr::EBlockQueueError_BlockQueueError_InvalidHandle;
	}
	return acquire_read(q, ulQueueHandle, pulBlockHandle, ppvBuffer, eReadType);
}

vr::EBlockQueueError
BlockQueue::ReleaseReadOnlyBlock(vr::PropertyContainerHandle_t ulQueueHandle,
                                 vr::PropertyContainerHandle_t ulBlockHandle)
{
	Queue *q = get(ulQueueHandle);
	uint32_t index = (ulBlockHandle & 0xff) - 1;
	if (q == nullptr || !is_block_handle(ulBlockHandle) || index >= q->shared->count) {
		return vr::EBlockQueueError_BlockQueueError_InvalidHandle;
	}
	q->shared->blocks[index].readers.fetch_sub(1);
	return vr::EBlockQueueError_BlockQueueError_None;
}

vr::EBlockQueueError
BlockQueue::QueueHasReader(vr::PropertyContainerHandle_t ulQueueHandle, bool *pbHasReaders)
{
	Queue *q = get(ulQueueHandle);
	if (q == nullptr || pbHasReaders == nullptr) {
		return vr::EBlockQueueError_BlockQueueError_InvalidHandle;
	}
	*pbHasReaders = q->shared->connections.load() > 0;
	return vr::EBlockQueueError_BlockQueueError_None;
}

uint8_t *
BlockQueue::block_header(vr::PropertyContainerHandle_t block, uint32_t *size)
{
	if (!is_block_handle(block)) {
		return nullptr;
	}
	Queue *q = get((block >> 8) & 0xffffffff);
	uint32_t index = (block & 0xff) - 1;
	if (q == nullptr || index >= q->shared->count || q->shared->header_size < sizeof(HeaderTable)) {
		return nullptr;
	}
	*size = q->shared->header_size;
	return (uint8_t *)q->shared->block(index) - q->shared->header_size;
}

vr::ETrackedPropertyError
BlockQueue::write_block_properties(vr::PropertyContainerHandle_t block, vr::PropertyWrite_t *batch, uint32_t count)
{
	uint32_t header_size;
	uint8_t *header = block_header(block, &header_size);
	if (header == nullptr) {
		return vr::TrackedProp_InvalidContainer;
	}
	auto *table = reinterpret_cast<HeaderTable *>(header);
	table->magic = HEADER_MAGIC;
	for (uint32_t i = 0; i < count; i++) {
		vr::PropertyWrite_t &w = batch[i];
		if (w.writeType != vr::PropertyWrite_Set) {
			w.eError = vr::TrackedProp_Success;
			continue;
		}
		uint32_t padded = (w.unBufferSize + 7) & ~7u;
		size_t table_end = sizeof(HeaderTable) + (table->count + 1) * sizeof(HeaderTable::Entry);
		if (table->used + padded + table_end > header_size) {
			w.eError = vr::TrackedProp_BufferTooSmall;
			continue;
		}
		table->used += padded;
		uint32_t offset = header_size - table->used;
		std::memcpy(header + offset, w.pvBuffer, w.unBufferSize);
		table->entries[table->count++] = {(uint32_t)w.prop, w.unTag, w.unBufferSize, offset};
		w.eError = vr::TrackedProp_Success;
	}
	return vr::TrackedProp_Success;
}

vr::ETrackedPropertyError
BlockQueue::read_block_properties(vr::PropertyContainerHandle_t block, vr::PropertyRead_t *batch, uint32_t count)
{
	uint32_t header_size;
	uint8_t *header = block_header(block, &header_size);
	if (header == nullptr) {
		return vr::TrackedProp_InvalidContainer;
	}
	auto *table = reinterpret_cast<HeaderTable *>(header);
	for (uint32_t i = 0; i < count; i++) {
		vr::PropertyRead_t &r = batch[i];
		r.eError = vr::TrackedProp_UnknownProperty;
		// The latest write of a property wins
		for (uint32_t e = table->count; e-- > 0;) {
			const HeaderTable::Entry &entry = table->entries[e];
			if (entry.prop != (uint32_t)r.prop || entry.offset + entry.size > header_size) {
				continue;
			}
			r.unTag = entry.tag;
			r.unRequiredBufferSize = entry.size;
			if (r.pvBuffer == nullptr || r.unBufferSize < entry.size) {
				r.eError = vr::TrackedProp_BufferTooSmall;
			} else {
				std::memcpy(r.pvBuffer, header + entry.offset, entry.size);
				r.eError = vr::TrackedProp_Success;
			}
			break;
		}
	}
	return vr::TrackedProp_Success;
}
