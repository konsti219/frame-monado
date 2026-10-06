// IVRCameraPassthroughInternal_001 for XRService under Monado: publishes the camera frames XRService allocates
// (dma-bufs) through /dev/shm/steamframe-passthrough instead of vrserver's shared memory and IPC imports.
#include "passthrough.hpp"
#include "sf_passthrough.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace {

#define LOG(...) passthrough_log(__VA_ARGS__)
// Call counts per method and first argument, logged every 10 s, for following XRService's polling
void count_call(const char *name, long arg);
#define CALLED(fmt, ...) count_call(__func__, called_arg(0, ##__VA_ARGS__))
template <typename... Args>
long
called_arg(int, Args... args)
{
	long first[] = {long(args)..., -1};
	return first[0];
}

enum Error
{
	OK = 0,
	INVALID_ARGUMENT = 1,
	NO_GRAPHICS_DEVICE = 2,
	NOT_INITIALIZED = 3,
};

struct GraphicsInfo
{
	// OpenVR's VRVulkanDevice_t order
	VkInstance instance;
	VkDevice device;
	VkPhysicalDevice physical_device;
	VkQueue queue;
	uint32_t queue_family;
};

struct GraphicsDevice
{
	uint32_t type; // 2 = Vulkan
	const GraphicsInfo *vulkan;
};

// The leading part of InitializeCamera's config; the rest (e.g. the IR cameras' tone curve) is passed through
struct CameraConfig
{
	uint32_t frame_count, width, height;
	uint8_t unknown[0x24];
	uint32_t map_width, map_height;
};

struct DmabufAttributes
{
	void *next;
	uint32_t width, height, depth, mip_levels, array_layers, sample_count, format;
	uint64_t modifier;
	uint32_t plane_count;
	struct
	{
		uint32_t offset, stride;
		int32_t fd;
	} plane[SF_PT_MAX_PLANES];
};
static_assert(sizeof(DmabufAttributes) == 0x68);

// What GetCameraDistortionImage fills in; XRService owns the rest of its struct
struct DistortionImage
{
	VkImage image;
	VkDeviceMemory memory;
	uint32_t width, height, format, usage;
	uint8_t renderable, mappable, compute;
};

// The 101x101 R32G32_SFLOAT undistortion map XRService writes per camera, backed by a udmabuf so the shim can
// read it back without mapping the VkDeviceMemory XRService may map itself
struct MapImage
{
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	uint32_t width = 0, height = 0;
	int dmabuf = -1;
	void *cpu = nullptr;
	size_t size = 0;
	VkSubresourceLayout layout{};
};

struct Vulkan
{
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
	PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
	PFN_vkCreateImage CreateImage;
	PFN_vkDestroyImage DestroyImage;
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
	PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkBindImageMemory BindImageMemory;
	PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
};

uint64_t
now_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return uint64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

class CameraPassthrough
{
public:
	virtual int
	SetGraphicsDevice(const GraphicsDevice *info)
	{
		if (info == nullptr || info->type != 2 || info->vulkan == nullptr) {
			LOG("[passthrough] SetGraphicsDevice: not Vulkan\n");
			return INVALID_ARGUMENT;
		}
		std::lock_guard lk(mut);
		gfx = *info->vulkan;
		load_vulkan();
		LOG("[passthrough] SetGraphicsDevice device %p queue family %u\n", (void *)gfx.device, gfx.queue_family);
		return OK;
	}

	virtual int
	InitializeCamera(int type, const CameraConfig *config, const DmabufAttributes *frames)
	{
		if (gfx.device == VK_NULL_HANDLE) {
			return NO_GRAPHICS_DEVICE;
		}
		if (!valid_type(type) || config == nullptr || frames == nullptr || config->frame_count > SF_PT_MAX_FRAMES) {
			return INVALID_ARGUMENT;
		}
		std::lock_guard lk(mut);
		sf_pt_type &t = shm->types[type];
		__atomic_fetch_add(&t.generation, 1, __ATOMIC_ACQ_REL);
		release_frames(t);

		const DmabufAttributes &first = frames[0];
		t.frame_count = config->frame_count;
		t.width = first.width;
		t.height = first.height;
		t.drm_format = first.format;
		t.modifier = first.modifier;
		t.plane_count = first.plane_count;
		for (uint32_t p = 0; p < SF_PT_MAX_PLANES; p++) {
			t.planes[p] = {first.plane[p].offset, first.plane[p].stride};
		}
		memcpy(t.config, config, sizeof(t.config));
		LOG("[passthrough] InitializeCamera type %d: %u frames %ux%u %.4s modifier %#lx planes %u map %ux%u\n", type,
		    t.frame_count, t.width, t.height, (const char *)&t.drm_format, (unsigned long)t.modifier,
		    t.plane_count, config->map_width, config->map_height);

		for (uint32_t cam = 0; cam < SF_PT_CAMERA_COUNT; cam++) {
			for (uint32_t i = 0; i < t.frame_count; i++) {
				// Cameras are laid out SF_PT_MAX_FRAMES apart
				const DmabufAttributes &a = frames[cam * SF_PT_MAX_FRAMES + i];
				sf_pt_frame &f = t.cameras[cam].frames[i];
				for (uint32_t p = 0; p < SF_PT_MAX_PLANES; p++) {
					// Own copies so the numbers stay valid for Monado's pidfd_getfd
					f.fds[p] = p < a.plane_count ? fcntl(a.plane[p].fd, F_DUPFD_CLOEXEC, 0) : -1;
				}
			}
			create_map(type, cam, config->map_width, config->map_height);
		}
		t.initialized = 1;
		__atomic_fetch_add(&t.generation, 1, __ATOMIC_ACQ_REL);
		return OK;
	}

	virtual int
	CameraHasReader(int type, bool *has_reader)
	{
		CALLED("type %d", type);
		if (!initialized(type)) {
			return NOT_INITIALIZED;
		}
		if (has_reader == nullptr) {
			return INVALID_ARGUMENT;
		}
		*has_reader = __atomic_load_n(&shm->types[type].reader, __ATOMIC_ACQUIRE) != 0;
		return OK;
	}

	virtual int
	GetCameraFrameStates(int type, uint32_t cam, uint32_t *states, uint32_t size)
	{
		CALLED("type %d cam %u", type, cam);
		if (!initialized(type)) {
			return NOT_INITIALIZED;
		}
		sf_pt_type &t = shm->types[type];
		if (cam >= SF_PT_CAMERA_COUNT || states == nullptr || t.frame_count * 4 > size) {
			return INVALID_ARGUMENT;
		}
		for (uint32_t i = 0; i < t.frame_count; i++) {
			states[i] = __atomic_load_n(&t.cameras[cam].frames[i].state, __ATOMIC_ACQUIRE);
		}
		return OK;
	}

	virtual int
	SubmitCameraFrame(int type, uint32_t cam, uint32_t index)
	{
		CALLED("type %d cam %u index %u", type, cam, index);
		if (!initialized(type)) {
			return NOT_INITIALIZED;
		}
		sf_pt_type &t = shm->types[type];
		if (cam >= SF_PT_CAMERA_COUNT || index >= t.frame_count) {
			return INVALID_ARGUMENT;
		}
		if (!__atomic_load_n(&t.reader, __ATOMIC_ACQUIRE)) {
			return OK;
		}
		sf_pt_camera &c = t.cameras[cam];
		read_map(type, cam);
		// The previous latest frame goes back to XRService unless Monado is reading it
		for (uint32_t i = 0; i < t.frame_count; i++) {
			uint32_t latest = SF_PT_FRAME_LATEST;
			__atomic_compare_exchange_n(&c.frames[i].state, &latest, SF_PT_FRAME_FREE, false, __ATOMIC_ACQ_REL,
			                            __ATOMIC_ACQUIRE);
		}
		sf_pt_frame &f = c.frames[index];
		f.submit_ns = now_ns();
		f.submit_seq = __atomic_add_fetch(&t.latest_seq, 1, __ATOMIC_ACQ_REL);
		__atomic_store_n(&f.state, SF_PT_FRAME_LATEST, __ATOMIC_RELEASE);
		log_rate(type, cam);
		return OK;
	}

	virtual int
	SetCameraFrameMetadata(int type, uint32_t cam, uint32_t index, const void *metadata)
	{
		CALLED("type %d cam %u", type, cam);
		if (!initialized(type)) {
			return NOT_INITIALIZED;
		}
		sf_pt_type &t = shm->types[type];
		if (cam >= SF_PT_CAMERA_COUNT || index >= t.frame_count || metadata == nullptr) {
			return INVALID_ARGUMENT;
		}
		memcpy(t.cameras[cam].frames[index].metadata, metadata, SF_PT_METADATA_SIZE);
		return OK;
	}

	virtual int
	GetCameraDistortionImage(int type, uint32_t cam, DistortionImage *out)
	{
		if (!initialized(type)) {
			return NOT_INITIALIZED;
		}
		if (cam >= SF_PT_CAMERA_COUNT || out == nullptr) {
			return INVALID_ARGUMENT;
		}
		const MapImage &m = maps[type][cam];
		*out = {m.image, m.memory, m.width, m.height, VK_FORMAT_R32G32_SFLOAT,
		        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0,
		        1, 0};
		LOG("[passthrough] GetCameraDistortionImage type %d cam %u: %p\n", type, cam, (void *)m.image);
		return OK;
	}

	virtual int
	IsPassthroughActive(bool *active)
	{
		CALLED("");
		if (active != nullptr) {
			*active = __atomic_load_n(&shm->active, __ATOMIC_ACQUIRE) != 0;
		}
		return OK;
	}

	virtual void
	SetPassthroughActive(bool active)
	{
		LOG("[passthrough] SetPassthroughActive %d\n", active);
	}

	virtual bool
	GetPassthroughSettings(uint8_t *settings)
	{
		CALLED("");
		if (settings != nullptr) {
			memcpy(settings, this->settings, sizeof(this->settings));
		}
		return this->settings[0] != 0;
	}

	virtual void
	SetPassthroughSettings(const uint8_t *settings)
	{
		if (settings != nullptr && memcmp(settings, this->settings, sizeof(this->settings)) != 0) {
			memcpy(this->settings, settings, sizeof(this->settings));
			LOG("[passthrough] SetPassthroughSettings %02x %02x %02x %02x %02x\n", settings[0], settings[1],
			    settings[2], settings[3], settings[4]);
		}
	}

	virtual bool
	GetFlag()
	{
		CALLED("");
		return flag;
	}

	virtual void
	SetFlag(bool value)
	{
		CALLED("%d", value);
		flag = value;
	}

	virtual int
	GetCounter(uint32_t *out)
	{
		CALLED("");
		if (out == nullptr) {
			return INVALID_ARGUMENT;
		}
		*out = 0;
		return OK;
	}

	// vrclient releases imports here when vrserver asks; nothing to release towards Monado
	virtual void
	Poll()
	{
		CALLED("");
	}

	virtual uint64_t
	Unknown15(uint64_t a, uint64_t b, uint64_t c)
	{
		static int n = 0;
		if (n++ < 5) {
			LOG("[passthrough] slot 15 (%#lx, %#lx, %#lx)\n", (unsigned long)a, (unsigned long)b,
			    (unsigned long)c);
		}
		return 0;
	}

	virtual void
	GetTiming(double *time, uint64_t *count, void *entries, size_t size)
	{
		CALLED("");
		if (time != nullptr) {
			*time = 0.0;
		}
		if (count != nullptr) {
			*count = 0;
		}
	}

	bool
	open_shm()
	{
		int fd = open(SF_PT_SHM_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if (fd < 0 || ftruncate(fd, sizeof(sf_pt_shm)) != 0) {
			LOG("[passthrough] can't create %s: %s\n", SF_PT_SHM_PATH, strerror(errno));
			return false;
		}
		void *p = mmap(nullptr, sizeof(sf_pt_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (p == MAP_FAILED) {
			return false;
		}
		shm = static_cast<sf_pt_shm *>(p);
		// Monado re-asserts its flags every frame, so a fresh XRService starts from a clean slate
		memset(shm, 0, sizeof(*shm));
		shm->version = SF_PT_VERSION;
		shm->pid = getpid();
		shm->shm_size = sizeof(sf_pt_shm);
		for (auto &t : shm->types) {
			for (auto &c : t.cameras) {
				for (auto &f : c.frames) {
					std::fill(std::begin(f.fds), std::end(f.fds), -1);
				}
			}
		}
		__atomic_store_n(&shm->magic, SF_PT_MAGIC, __ATOMIC_RELEASE);
		return true;
	}

private:
	std::mutex mut;
	sf_pt_shm *shm = nullptr;
	GraphicsInfo gfx{};
	Vulkan vk{};
	MapImage maps[SF_PT_TYPE_COUNT][SF_PT_CAMERA_COUNT];
	uint8_t settings[5]{};
	bool flag = false;

	static bool
	valid_type(int type)
	{
		return type >= 0 && type < SF_PT_TYPE_COUNT;
	}

	bool
	initialized(int type)
	{
		return valid_type(type) && shm->types[type].initialized;
	}

	void
	load_vulkan()
	{
		auto get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr"));
		if (get_instance_proc == nullptr) {
			void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
			get_instance_proc = lib ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"))
			                        : nullptr;
		}
		if (get_instance_proc == nullptr) {
			LOG("[passthrough] no vkGetInstanceProcAddr\n");
			return;
		}
#define INSTANCE(name) vk.name = reinterpret_cast<PFN_vk##name>(get_instance_proc(gfx.instance, "vk" #name))
#define DEVICE(name) vk.name = reinterpret_cast<PFN_vk##name>(vk.GetDeviceProcAddr(gfx.device, "vk" #name))
		INSTANCE(GetDeviceProcAddr);
		INSTANCE(GetPhysicalDeviceMemoryProperties);
		DEVICE(CreateImage);
		DEVICE(DestroyImage);
		DEVICE(GetImageMemoryRequirements);
		DEVICE(GetImageSubresourceLayout);
		DEVICE(AllocateMemory);
		DEVICE(FreeMemory);
		DEVICE(BindImageMemory);
		DEVICE(GetMemoryFdPropertiesKHR);
#undef INSTANCE
#undef DEVICE
		LOG("[passthrough] vkGetInstanceProcAddr %p vkGetDeviceProcAddr %p vkCreateImage %p vkGetMemoryFdPropertiesKHR %p\n",
		    (void *)get_instance_proc, (void *)vk.GetDeviceProcAddr, (void *)vk.CreateImage,
		    (void *)vk.GetMemoryFdPropertiesKHR);
	}

	void
	release_frames(sf_pt_type &t)
	{
		t.initialized = 0;
		for (auto &c : t.cameras) {
			for (auto &f : c.frames) {
				for (int &fd : f.fds) {
					if (fd >= 0) {
						close(fd);
					}
					fd = -1;
				}
				f.state = SF_PT_FRAME_FREE;
			}
		}
	}

	// A memfd-backed udmabuf, mapped on the CPU, so the map can be read without vkMapMemory
	bool
	create_udmabuf(size_t size, int &dmabuf, void *&cpu)
	{
		int memfd = memfd_create("sf-pt-map", MFD_CLOEXEC | MFD_ALLOW_SEALING);
		int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
		bool ok = memfd >= 0 && dev >= 0 && ftruncate(memfd, size) == 0 &&
		          fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) == 0;
		if (ok) {
			udmabuf_create create{};
			create.memfd = memfd;
			create.flags = UDMABUF_FLAGS_CLOEXEC;
			create.size = size;
			dmabuf = ioctl(dev, UDMABUF_CREATE, &create);
			cpu = mmap(nullptr, size, PROT_READ, MAP_SHARED, memfd, 0);
			ok = dmabuf >= 0 && cpu != MAP_FAILED;
		}
		if (!ok) {
			LOG("[passthrough] udmabuf: %s\n", strerror(errno));
		}
		if (memfd >= 0) {
			close(memfd);
		}
		if (dev >= 0) {
			close(dev);
		}
		return ok;
	}

	void
	destroy_map(MapImage &m)
	{
		if (m.image != VK_NULL_HANDLE) {
			vk.DestroyImage(gfx.device, m.image, nullptr);
		}
		if (m.memory != VK_NULL_HANDLE) {
			vk.FreeMemory(gfx.device, m.memory, nullptr);
		}
		if (m.cpu != nullptr && m.cpu != MAP_FAILED) {
			munmap(m.cpu, m.size);
		}
		if (m.dmabuf >= 0) {
			close(m.dmabuf);
		}
		m = MapImage{};
	}

	void
	create_map(int type, uint32_t cam, uint32_t width, uint32_t height)
	{
		MapImage &m = maps[type][cam];
		if (m.image != VK_NULL_HANDLE && m.width == width && m.height == height) {
			return;
		}
		destroy_map(m);
		if (width == 0 || height == 0 || width > SF_PT_MAP_MAX || height > SF_PT_MAP_MAX || vk.CreateImage == nullptr) {
			LOG("[passthrough] no map for type %d cam %u: %ux%u, vkCreateImage %p\n", type, cam, width, height,
			    (void *)vk.CreateImage);
			return;
		}
		bool udmabuf = vk.GetMemoryFdPropertiesKHR != nullptr;

		VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		info.pNext = udmabuf ? &external : nullptr;
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = VK_FORMAT_R32G32_SFLOAT;
		info.extent = {width, height, 1};
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_LINEAR;
		info.usage =
		    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
		if (vk.CreateImage(gfx.device, &info, nullptr, &m.image) != VK_SUCCESS) {
			LOG("[passthrough] map image creation failed\n");
			return;
		}
		m.width = width;
		m.height = height;
		VkMemoryRequirements reqs;
		vk.GetImageMemoryRequirements(gfx.device, m.image, &reqs);
		VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
		vk.GetImageSubresourceLayout(gfx.device, m.image, &sub, &m.layout);

		uint32_t type_bits = reqs.memoryTypeBits;
		m.size = (reqs.size + 4095) & ~size_t(4095);
		if (udmabuf && create_udmabuf(m.size, m.dmabuf, m.cpu)) {
			VkMemoryFdPropertiesKHR props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
			vk.GetMemoryFdPropertiesKHR(gfx.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, m.dmabuf,
			                            &props);
			type_bits &= props.memoryTypeBits;
		} else {
			udmabuf = false;
		}

		VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dedicated.image = m.image;
		VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
		import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		import.fd = udmabuf ? dup(m.dmabuf) : -1; // Vulkan owns the fd on success
		import.pNext = &dedicated;
		VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		alloc.pNext = udmabuf ? static_cast<void *>(&import) : &dedicated;
		alloc.allocationSize = udmabuf ? m.size : reqs.size;
		alloc.memoryTypeIndex = memory_type(type_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VkResult ret = vk.AllocateMemory(gfx.device, &alloc, nullptr, &m.memory);
		if (ret != VK_SUCCESS) {
			LOG("[passthrough] map memory allocation failed: %d\n", ret);
			if (import.fd >= 0) {
				close(import.fd);
			}
			destroy_map(m);
			return;
		}
		vk.BindImageMemory(gfx.device, m.image, m.memory, 0);
		LOG("[passthrough] map type %d cam %u: %ux%u row pitch %lu, %s\n", type, cam, width, height,
		    (unsigned long)m.layout.rowPitch, udmabuf ? "udmabuf" : "not CPU readable");

		sf_pt_camera &c = shm->types[type].cameras[cam];
		c.map_width = width;
		c.map_height = height;
	}

	uint32_t
	memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
	{
		VkPhysicalDeviceMemoryProperties props;
		vk.GetPhysicalDeviceMemoryProperties(gfx.physical_device, &props);
		for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
			if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
				return i;
			}
		}
		for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
			if (bits & (1u << i)) {
				return i;
			}
		}
		return 0;
	}

	// Copies the map into shared memory when it changed (XRService writes it rarely, if ever, after setup)
	void
	read_map(int type, uint32_t cam)
	{
		MapImage &m = maps[type][cam];
		if (m.cpu == nullptr) {
			return;
		}
		dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
		ioctl(m.dmabuf, DMA_BUF_IOCTL_SYNC, &sync);
		sf_pt_camera &c = shm->types[type].cameras[cam];
		const size_t row = m.width * 2 * sizeof(float);
		bool changed = false;
		for (uint32_t y = 0; y < m.height; y++) {
			const auto *src = static_cast<const uint8_t *>(m.cpu) + m.layout.offset + y * m.layout.rowPitch;
			float *dst = c.map + y * m.width * 2;
			if (memcmp(dst, src, row) != 0) {
				memcpy(dst, src, row);
				changed = true;
			}
		}
		sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
		ioctl(m.dmabuf, DMA_BUF_IOCTL_SYNC, &sync);
		if (changed) {
			__atomic_add_fetch(&c.map_seq, 1, __ATOMIC_ACQ_REL);
			LOG("[passthrough] map type %d cam %u changed, center %f %f\n", type, cam,
			    c.map[(m.height / 2 * m.width + m.width / 2) * 2], c.map[(m.height / 2 * m.width + m.width / 2) * 2 + 1]);
		}
	}

	void
	log_rate(int type, uint32_t cam)
	{
		static uint64_t counts[SF_PT_TYPE_COUNT][SF_PT_CAMERA_COUNT], since[SF_PT_TYPE_COUNT][SF_PT_CAMERA_COUNT];
		uint64_t now = now_ns();
		counts[type][cam]++;
		if (since[type][cam] == 0) {
			since[type][cam] = now;
		} else if (now - since[type][cam] > 10000000000ull) {
			LOG("[passthrough] type %d cam %u: %.1f fps\n", type, cam,
			    counts[type][cam] * 1e9 / double(now - since[type][cam]));
			counts[type][cam] = 0;
			since[type][cam] = now;
		}
	}
};

CameraPassthrough *instance;

void
count_call(const char *name, long arg)
{
	static std::mutex mut;
	static std::vector<std::pair<std::string, uint64_t>> counts;
	static uint64_t since = now_ns();
	std::lock_guard lk(mut);
	std::string key = std::string(name) + "(" + std::to_string(arg) + ")";
	auto it = std::find_if(counts.begin(), counts.end(), [&](auto &c) { return c.first == key; });
	if (it == counts.end()) {
		LOG("[passthrough] first call %s\n", key.c_str());
		counts.emplace_back(key, 1);
	} else {
		it->second++;
	}
	if (now_ns() - since > 10000000000ull) {
		std::string line;
		for (auto &c : counts) {
			line += " " + c.first + "=" + std::to_string(c.second);
			c.second = 0;
		}
		LOG("[passthrough] calls in 10 s:%s\n", line.c_str());
		since = now_ns();
	}
}

} // namespace

void *
camera_passthrough_interface()
{
	static std::once_flag once;
	std::call_once(once, [] {
		static CameraPassthrough p;
		if (p.open_shm()) {
			instance = &p;
		}
	});
	return instance;
}
