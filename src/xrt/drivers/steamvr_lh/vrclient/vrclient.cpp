// vrclient.so for XRService under Monado: block queues shared with driver_cv in monado-service, stubs for the rest.
// Loaded by libopenvr_api from $VR_OVERRIDE/bin/linuxarm64/, which driver_cv points at Monado's runtime layout.
#include "blockqueue.hpp"
#include "paths.hpp"
#include "passthrough.hpp"
#include "settings.hpp"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
// XRService's stderr isn't kept anywhere, so log to a file (truncated per XRService start)
FILE *
log_file()
{
	static FILE *f = fopen("/tmp/vrclient.log", "w");
	return f ? f : stderr;
}
#define LOG(...)                                                                                                       \
	do {                                                                                                           \
		fprintf(log_file(), __VA_ARGS__);                                                                      \
		fflush(log_file());                                                                                    \
	} while (0)

// Same idea as LH_STUB_UNKNOWN in Monado: every slot logs its first calls and returns 0
struct LoggingStub
{
	void **vtable;
	const char *name;
};

// True for the first few occurrences of a key, so calls XRService repeats every frame don't flood the log
bool
first_few(const std::string &key)
{
	static std::mutex mut;
	static std::unordered_map<std::string, int> counts;
	std::lock_guard lk(mut);
	return counts[key]++ < 3;
}

template <int Slot>
uint64_t
stub_slot(LoggingStub *self, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
	if (!first_few(std::string(self->name) + "/" + std::to_string(Slot))) {
		return 0;
	}
	LOG("[vrclient stub] %s slot %d (%#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ", %#" PRIx64 ")\n", self->name,
	        Slot, a, b, c, d);
	return 0;
}

template <int... Slots>
void **
stub_vtable(std::integer_sequence<int, Slots...>)
{
	static void *vtable[] = {reinterpret_cast<void *>(&stub_slot<Slots>)...};
	return vtable;
}

void *
logging_stub(const char *name)
{
	static std::mutex mut;
	static std::unordered_map<std::string, LoggingStub *> stubs;
	std::lock_guard lk(mut);
	LoggingStub *&stub = stubs[name];
	if (stub == nullptr) {
		stub = new LoggingStub{stub_vtable(std::make_integer_sequence<int, 128>{}), strdup(name)};
	}
	return stub;
}

// IVRSystem_023 as logging stubs, except PollNextEvent (slot 29): under SteamVR the compositor tells XRService that
// room view is shown, which is what starts the RoomView3D mesh; we send that once
constexpr int kPollNextEventSlot = 29;
constexpr uint32_t kVREventRoomViewShown = 526;

bool
poll_next_event(void *self, void *event, uint32_t size)
{
	// at the first poll and again 5 and 15 s later, in case XRService ignores it before its cameras are set up
	static std::atomic<int> sent{0};
	static const auto start = std::chrono::steady_clock::now();
	const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const int due = t >= 15 ? 3 : t >= 5 ? 2 : 1;
	int n = sent.load();
	if (n >= due || !sent.compare_exchange_strong(n, n + 1) ||
	    event == nullptr || size < 12) {
		return false;
	}
	std::memset(event, 0, size);
	uint32_t *fields = static_cast<uint32_t *>(event); // eventType, trackedDeviceIndex, eventAgeSeconds, data
	fields[0] = kVREventRoomViewShown;
	LOG("[vrclient] PollNextEvent -> VREvent_RoomViewShown (size %u)\n", size);
	return true;
}

void *
system_interface(const char *version)
{
	static LoggingStub *stub = [version] {
		auto *base = static_cast<LoggingStub *>(logging_stub(version));
		void **vtable = new void *[128];
		std::memcpy(vtable, base->vtable, sizeof(void *) * 128);
		vtable[kPollNextEventSlot] = reinterpret_cast<void *>(&poll_next_event);
		return new LoggingStub{vtable, base->name};
	}();
	return stub;
}

// Raw call logger for the passthrough interfaces while their semantics are worked out
#define TRACE(name, fmt, ...)                                                                                          \
	do {                                                                                                           \
		static int n = 0;                                                                                      \
		if (n++ < 20) {                                                                                        \
			LOG("[vrclient] " name " " fmt "\n", ##__VA_ARGS__);                                          \
		}                                                                                                      \
	} while (0)

// XRService's queue lifecycle calls, logged (which queues it creates or connects to, and when)
class LoggingBlockQueue : public BlockQueue
{
public:
	vr::EBlockQueueError
	Create(vr::PropertyContainerHandle_t *handle, char *path, uint32_t data, uint32_t header, uint32_t count,
	       uint32_t flags) override
	{
		vr::EBlockQueueError err = BlockQueue::Create(handle, path, data, header, count, flags);
		LOG("[vrclient] queue Create %s (%u + %u x %u) -> %d\n", path, header, data, count, err);
		return err;
	}
	vr::EBlockQueueError
	Connect(vr::PropertyContainerHandle_t *handle, char *path) override
	{
		vr::EBlockQueueError err = BlockQueue::Connect(handle, path);
		static std::mutex mut;
		static std::unordered_map<std::string, int> failures;
		std::lock_guard lk(mut);
		if (err == vr::EBlockQueueError_BlockQueueError_None || failures[path]++ % 100 == 0)
			LOG("[vrclient] queue Connect %s -> %d (%d failures)\n", path, err, failures[path]);
		return err;
	}
};

LoggingBlockQueue blockqueue;

// XRService reads block header properties (e.g. the controller config lengths) through paths
class BlockProperties : public vr::IVRProperties
{
public:
	vr::ETrackedPropertyError
	ReadPropertyBatch(vr::PropertyContainerHandle_t handle, vr::PropertyRead_t *batch, uint32_t count) override
	{
		if (!BlockQueue::is_block_handle(handle)) {
			if (blockqueue.read_queue_properties(handle, batch, count) == vr::TrackedProp_Success) {
				for (uint32_t i = 0; i < count; i++) {
					if (batch[i].eError == vr::TrackedProp_UnknownProperty) {
						TRACE("queue property", "%#lx: unknown prop %#x", (unsigned long)handle,
						      (unsigned)batch[i].prop);
					}
				}
				return vr::TrackedProp_Success;
			}
			for (uint32_t i = 0; i < count; i++) {
				if (first_few("read/" + std::to_string(batch[i].prop))) {
					LOG("[vrclient] property read on non-block container %#lx: prop %u tag %u size %u\n",
					    (unsigned long)handle, batch[i].prop, batch[i].unTag,
					    batch[i].unBufferSize);
				}
			}
			return vr::TrackedProp_InvalidContainer;
		}
		return blockqueue.read_block_properties(handle, batch, count);
	}
	vr::ETrackedPropertyError
	WritePropertyBatch(vr::PropertyContainerHandle_t handle, vr::PropertyWrite_t *batch, uint32_t count) override
	{
		if (!BlockQueue::is_block_handle(handle)) {
			for (uint32_t i = 0; i < count; i++) {
				if (first_few("write/" + std::to_string(batch[i].prop))) {
					LOG("[vrclient] property write on non-block container %#lx: prop %u type %d tag %u size %u\n",
					    (unsigned long)handle, batch[i].prop, batch[i].writeType,
					    batch[i].unTag, batch[i].unBufferSize);
				}
			}
			return vr::TrackedProp_InvalidContainer;
		}
		return blockqueue.write_block_properties(handle, batch, count);
	}
	const char *
	GetPropErrorNameFromEnum(vr::ETrackedPropertyError error) override
	{
		return "";
	}
	vr::PropertyContainerHandle_t
	TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t index) override
	{
		return vr::k_ulInvalidPropertyContainer;
	}
};

BlockProperties block_properties;
Paths paths(&block_properties);

struct DmabufPlane
{
	uint32_t offset;
	uint32_t stride;
	int32_t fd;
};
struct DmabufAttributes
{
	void *next;
	uint32_t width, height, depth, mip_levels, array_layers, sample_count, format;
	uint64_t modifier;
	uint32_t plane_count;
	DmabufPlane plane[4];
};

// IVRIPCResourceManagerClient_002, in the public _003 order (the observed calls match it)
class IPCResourceManager
{
public:
	virtual bool
	NewSharedVulkanImage(uint32_t format, uint32_t w, uint32_t h, bool render, bool map, bool compute, uint32_t mips,
	                     uint32_t layers, uint32_t create_flags, uint32_t usage_flags, uint64_t *handle)
	{
		TRACE("NewSharedVulkanImage", "format %u %ux%u render %d map %d compute %d", format, w, h, render, map,
		      compute);
		return false;
	}
	virtual bool
	NewSharedVulkanBuffer(uint32_t size, uint32_t usage, uint64_t *handle)
	{
		TRACE("NewSharedVulkanBuffer", "size %u usage %#x", size, usage);
		return false;
	}
	virtual bool
	NewSharedVulkanSemaphore(bool counting, uint64_t *handle)
	{
		TRACE("NewSharedVulkanSemaphore", "counting %d", counting);
		return false;
	}
	virtual bool
	RefResource(uint64_t handle, uint64_t *ipc)
	{
		TRACE("RefResource", "%#lx", (unsigned long)handle);
		if (ipc) {
			*ipc = handle;
		}
		return true;
	}
	virtual bool
	UnrefResource(uint64_t handle)
	{
		TRACE("UnrefResource", "%#lx", (unsigned long)handle);
		return true;
	}
	virtual bool
	GetDmabufFormats(uint32_t *count, uint32_t *formats)
	{
		TRACE("GetDmabufFormats", "count %u", count ? *count : 0);
		return false;
	}
	virtual bool
	GetDmabufModifiers(int app_type, uint32_t format, uint32_t *count, uint64_t *modifiers)
	{
		TRACE("GetDmabufModifiers", "app %d format %.4s count %u out %p", app_type, (char *)&format,
		      count ? *count : 0, (void *)modifiers);
		if (count == nullptr) {
			return false;
		}
		if (modifiers != nullptr && *count >= 1) {
			modifiers[0] = 0; // DRM_FORMAT_MOD_LINEAR
		}
		*count = 1;
		return true;
	}
	virtual bool
	ImportDmabuf(int app_type, DmabufAttributes *a, uint64_t *handle)
	{
		static uint64_t next = 0x1000;
		LOG("[vrclient] ImportDmabuf app %d %ux%u format %.4s modifier %#lx planes %u", app_type, a->width,
		    a->height, (char *)&a->format, (unsigned long)a->modifier, a->plane_count);
		for (uint32_t p = 0; p < a->plane_count && p < 4; p++) {
			LOG(" [fd %d offset %u stride %u]", a->plane[p].fd, a->plane[p].offset, a->plane[p].stride);
		}
		*handle = next++;
		LOG(" -> %#lx\n", (unsigned long)*handle);
		return true;
	}
	virtual bool
	ReceiveSharedFd(uint64_t ipc, int *fd)
	{
		TRACE("ReceiveSharedFd", "%#lx", (unsigned long)ipc);
		return false;
	}
};

IPCResourceManager ipc_resources;
ClientSettings settings;

// IVRClientCore_003, from OpenVR's ivrclientcore.h
class ClientCore
{
public:
	virtual vr::EVRInitError
	Init(int application_type, const char *startup_info)
	{
		LOG("[vrclient] Init(type %d)\n", application_type);
		return vr::VRInitError_None;
	}
	virtual void
	Cleanup()
	{}
	virtual vr::EVRInitError
	IsInterfaceVersionValid(const char *version)
	{
		return vr::VRInitError_None;
	}
	virtual void *
	GetGenericInterface(const char *version, vr::EVRInitError *error)
	{
		if (error) {
			*error = vr::VRInitError_None;
		}
		if (std::strcmp(version, "IVRSettings_003") == 0) {
			return &settings;
		}
		if (std::strcmp(version, "IVRBlockQueue_005") == 0) {
			return &blockqueue;
		}
		if (std::strcmp(version, "IVRPaths_002") == 0) {
			return &paths;
		}
		if (std::strcmp(version, "IVRIPCResourceManagerClient_002") == 0) {
			return &ipc_resources;
		}
		if (std::strcmp(version, "IVRCameraPassthroughInternal_001") == 0) {
			if (void *pt = camera_passthrough_interface()) {
				return pt;
			}
		}
		if (std::strcmp(version, "IVRSystem_023") == 0) {
			return system_interface(version);
		}
		LOG("[vrclient] stub for %s\n", version);
		return logging_stub(version);
	}
	virtual bool
	BIsHmdPresent()
	{
		return true;
	}
	virtual const char *
	GetEnglishStringForHmdError(vr::EVRInitError error)
	{
		return "";
	}
	virtual const char *
	GetIDForVRInitError(vr::EVRInitError error)
	{
		return "";
	}
};

ClientCore core;
} // namespace

void
passthrough_log(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vfprintf(log_file(), fmt, args);
	va_end(args);
	fflush(log_file());
}

extern "C" __attribute__((visibility("default"))) void *
VRClientCoreFactory(const char *interface_name, int *return_code)
{
	LOG("[vrclient] VRClientCoreFactory(%s)\n", interface_name);
	if (return_code) {
		*return_code = 0;
	}
	return &core;
}
