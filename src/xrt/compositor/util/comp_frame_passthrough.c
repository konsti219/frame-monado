#include "comp_frame_passthrough.h"
#include "sf_passthrough.h"
#include "comp_render.h"
#include "comp_base.h"
#include "render/render_interface.h"
#include "render_shaders_interface.h"
#include "vk/vk_helpers.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "math/m_api.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct pt_texture
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	struct vk_buffer staging;
	uint32_t width, height, bytes;
	bool uploaded;
};
struct pt_dma
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint32_t memory_type;
	int remote_fd, fd;
	uint64_t size;
	bool attempted, synced;
	// The plane as a linear image on the same memory, sampled in place instead of copied
	VkImage image;
	VkImageView view;
	bool image_failed, image_used;
};
struct pt_camera
{
	struct pt_texture textures[3];
	VkDescriptorSet set;
	uint8_t metadata[SF_PT_METADATA_SIZE];
	uint64_t seq, map_seq, time;
	struct sf_pt_frame *held;
	// Sampling in place: the frame stays claimed until a newer one replaces it
	bool direct;
	struct sf_pt_frame *direct_frame;
	VkImageView direct_views[2];
	struct pt_dma dma[SF_PT_MAX_FRAMES][2];
};
// RoomView3D depth mesh from XRService, read straight from the block queue driver_cv creates in this process
#define SF_MESH_DATA_PATH "/dev/shm/sfbq_xrservice_roomview3d_meshData"
#define SF_MESH_LAYOUT_PATH "/dev/shm/sfbq_xrservice_roomview3d_meshLayout"
#define SF_MESH_MAX_AGE_S 0.5
#define SF_PUSH_SIZE (52 * sizeof(float))

struct pt_queue
{
	uint8_t *map;
	size_t size;
};
struct pt_mesh
{
	struct pt_queue data, layout;
	uint32_t width, height; // vertex grid, from the layout's triangle indices
	struct pt_texture depth;
	uint64_t seq;
	double time; // posePredictionBaseTime, CLOCK_MONOTONIC_RAW seconds
	float focal, cx, cy;
	float mesh_from_world[12]; // 3x4 rows
	bool valid, logged;
	float *vertices, *floor_points, *floor_vertices, *floor_pending_vertices;
	pthread_t floor_thread;
	pthread_mutex_t floor_mutex;
	pthread_cond_t floor_condition;
	bool floor_started, floor_running, floor_pending;
	float floor_pending_pose[12];
	double floor_pending_time;
	double floor_sample_time, floor_log_time;
};

struct sf_comp_passthrough
{
	struct vk_bundle *vk;
	struct pt_mesh mesh;
	VkFormat mesh_format;
	bool mesh_enabled, mesh_debug; // SF_PASSTHROUGH_MESH=0 / debug (tints what comes from the mesh green)
	struct sf_pt_shm *shm;
	bool wanted, ready, gpu_copy, direct_sampling, dmabuf_sync;
	// Stream chosen for the next frame and the one the textures currently hold (SF_PT_TYPE_*)
	int type, shown;
	uint64_t want_ns;
	PFN_vkGetMemoryFdPropertiesKHR get_fd_properties;
	int pid;
	uint64_t prepare_ns, prepare_count, log_ns;
	uint32_t generation;
	FILE *pose_trace;
	bool pose_trace_checked;
	uint64_t pose_trace_end_ns;
	struct pt_camera cameras[2];
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkDescriptorPool pool;
	VkPipeline pipeline;
	VkRenderPass render_pass;
	VkSampler sampler;
};

static void
texture_destroy(struct sf_comp_passthrough *pt, struct pt_texture *t)
{
	struct vk_bundle *vk = pt->vk;
	if (t->view)
		vk->vkDestroyImageView(vk->device, t->view, NULL);
	if (t->image)
		vk->vkDestroyImage(vk->device, t->image, NULL);
	if (t->memory)
		vk->vkFreeMemory(vk->device, t->memory, NULL);
	vk_buffer_destroy(&t->staging, vk);
	memset(t, 0, sizeof(*t));
}

static bool
texture_create(
    struct sf_comp_passthrough *pt, struct pt_texture *t, uint32_t w, uint32_t h, uint32_t bytes, VkFormat format)
{
	if (t->view && t->staging.data && t->width == w && t->height == h)
		return true;
	texture_destroy(pt, t);
	struct vk_bundle *vk = pt->vk;
	t->width = w;
	t->height = h;
	t->bytes = bytes;
	if (vk_create_image_simple(vk, (VkExtent2D){w, h}, format,
	                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &t->memory,
	                           &t->image) != VK_SUCCESS)
		return false;
	if (vk_create_view(vk, t->image, VK_IMAGE_VIEW_TYPE_2D, format,
	                   (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, &t->view) != VK_SUCCESS)
		return false;
	t->staging.size = w * h * bytes;
	if (!vk_buffer_init(vk, t->staging.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                    &t->staging.handle, &t->staging.memory))
		return false;
	return vk->vkMapMemory(vk->device, t->staging.memory, 0, VK_WHOLE_SIZE, 0, &t->staging.data) == VK_SUCCESS;
}

static void
texture_upload(struct sf_comp_passthrough *pt,
               struct pt_texture *t,
               VkCommandBuffer cmd,
               VkBuffer source,
               uint32_t offset,
               uint32_t stride)
{
	struct vk_bundle *vk = pt->vk;
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vk_cmd_image_barrier_locked(
	    vk, cmd, t->image, t->uploaded ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	    t->uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
	    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    t->uploaded ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT, range);
	VkBufferImageCopy copy = {.bufferOffset = offset,
	                          .bufferRowLength = stride ? stride / t->bytes : 0,
	                          .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	                          .imageExtent = {t->width, t->height, 1}};
	vk->vkCmdCopyBufferToImage(cmd, source ? source : t->staging.handle, t->image,
	                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	vk_cmd_image_barrier_locked(vk, cmd, t->image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
	                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, range);
	t->uploaded = true;
}

static bool
pipeline_create(struct sf_comp_passthrough *pt, struct render_gfx *render, VkRenderPass pass)
{
	struct vk_bundle *vk = pt->vk;
	if (pt->pipeline && pt->render_pass == pass)
		return true;
	if (pt->pipeline)
		vk->vkDestroyPipeline(vk->device, pt->pipeline, NULL);
	pt->pipeline = VK_NULL_HANDLE;
	VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	                                              .stage = VK_SHADER_STAGE_VERTEX_BIT,
	                                              .module = render->r->shaders->frame_passthrough_vert,
	                                              .pName = "main"},
	                                             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	                                              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
	                                              .module = render->r->shaders->frame_passthrough_frag,
	                                              .pName = "main"}};
	VkPipelineVertexInputStateCreateInfo vertex = {.sType =
	                                                   VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo assembly = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
	VkPipelineViewportStateCreateInfo viewport = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
	VkPipelineRasterizationStateCreateInfo raster = {.sType =
	                                                     VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	                                                 .polygonMode = VK_POLYGON_MODE_FILL,
	                                                 .lineWidth = 1};
	VkPipelineMultisampleStateCreateInfo multisample = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
	VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = 0xf};
	VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	                                             .attachmentCount = 1,
	                                             .pAttachments = &attachment};
	VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	                                            .dynamicStateCount = 2,
	                                            .pDynamicStates = dynamics};
	VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	                                     .stageCount = 2,
	                                     .pStages = stages,
	                                     .pVertexInputState = &vertex,
	                                     .pInputAssemblyState = &assembly,
	                                     .pViewportState = &viewport,
	                                     .pRasterizationState = &raster,
	                                     .pMultisampleState = &multisample,
	                                     .pColorBlendState = &blend,
	                                     .pDynamicState = &dynamic,
	                                     .layout = pt->layout,
	                                     .renderPass = pass};
	pt->render_pass = pass;
	return vk->vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &info, NULL, &pt->pipeline) == VK_SUCCESS;
}

struct sf_comp_passthrough *
sf_comp_passthrough_create(struct vk_bundle *vk)
{
	const char *driver = getenv("STEAMVR_LH_DRIVER");
	if (!driver || strcmp(driver, "cv") || getenv("SF_PASSTHROUGH_OFF"))
		return NULL;
	struct sf_comp_passthrough *pt = calloc(1, sizeof(*pt));
	if (!pt)
		return NULL;
	pt->vk = vk;
	pt->type = pt->shown = SF_PT_TYPE_COLOR;
	pt->get_fd_properties =
	    (PFN_vkGetMemoryFdPropertiesKHR)vk->vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdPropertiesKHR");
	pt->gpu_copy = vk->has_EXT_external_memory_dma_buf && pt->get_fd_properties && !getenv("SF_PASSTHROUGH_CPU");
	// SF_PASSTHROUGH_DIRECT=0 copies every camera frame into a texture instead of sampling it in place
	const char *direct_option = getenv("SF_PASSTHROUGH_DIRECT");
	pt->direct_sampling = !direct_option || strcmp(direct_option, "0");
	pt->dmabuf_sync = getenv("SF_PASSTHROUGH_DMABUF_SYNC") != NULL;
	if (pt->gpu_copy) {
		VkPhysicalDeviceExternalBufferInfo info = {
		    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
		    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
		VkExternalBufferProperties props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
		vk->vkGetPhysicalDeviceExternalBufferPropertiesKHR(vk->physical_device, &info, &props);
		U_LOG_I("Passthrough DMA-BUF external buffer features: %#x",
		        props.externalMemoryProperties.externalMemoryFeatures);
		pt->gpu_copy = (props.externalMemoryProperties.externalMemoryFeatures &
		                VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
		               !(props.externalMemoryProperties.externalMemoryFeatures &
		                 VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
	}
	VkPhysicalDeviceProperties props;
	vk->vkGetPhysicalDeviceProperties(vk->physical_device, &props);
	if (props.limits.maxPushConstantsSize < SF_PUSH_SIZE) {
		U_LOG_W("Passthrough needs %zu bytes of push constants, the device has %u", SF_PUSH_SIZE,
		        props.limits.maxPushConstantsSize);
		goto fail;
	}
	VkFormatProperties format;
	vk->vkGetPhysicalDeviceFormatProperties(vk->physical_device, VK_FORMAT_R32_SFLOAT, &format);
	pt->mesh_format = (format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
	                      ? VK_FORMAT_R32_SFLOAT
	                      : VK_FORMAT_R16_SFLOAT;
	const char *mesh_option = getenv("SF_PASSTHROUGH_MESH");
	pt->mesh_enabled = !mesh_option || strcmp(mesh_option, "0");
	pt->mesh_debug = mesh_option && !strcmp(mesh_option, "debug");
	VkDescriptorSetLayoutBinding bindings[4];
	for (uint32_t i = 0; i < 4; i++)
		bindings[i] = (VkDescriptorSetLayoutBinding){
		    i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
		    i < 2 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, NULL};
	VkDescriptorSetLayoutCreateInfo set = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = bindings};
	if (vk->vkCreateDescriptorSetLayout(vk->device, &set, NULL, &pt->set_layout) != VK_SUCCESS)
		goto fail;
	VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, SF_PUSH_SIZE};
	VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	                                     .setLayoutCount = 1,
	                                     .pSetLayouts = &pt->set_layout,
	                                     .pushConstantRangeCount = 1,
	                                     .pPushConstantRanges = &push};
	if (vk->vkCreatePipelineLayout(vk->device, &layout, NULL, &pt->layout) != VK_SUCCESS)
		goto fail;
	VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
	VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	                                   .maxSets = 2,
	                                   .poolSizeCount = 1,
	                                   .pPoolSizes = &size};
	if (vk->vkCreateDescriptorPool(vk->device, &pool, NULL, &pt->pool) != VK_SUCCESS)
		goto fail;
	if (vk_create_sampler(vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, &pt->sampler) != VK_SUCCESS)
		goto fail;
	for (int c = 0; c < 2; c++) {
		VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		                                     .descriptorPool = pt->pool,
		                                     .descriptorSetCount = 1,
		                                     .pSetLayouts = &pt->set_layout};
		if (vk->vkAllocateDescriptorSets(vk->device, &alloc, &pt->cameras[c].set) != VK_SUCCESS)
			goto fail;
	}
	return pt;
fail:
	sf_comp_passthrough_destroy(&pt);
	return NULL;
}

static const char *const stream_names[SF_PT_TYPE_COUNT] = {"IR tracking", "color"};

// Newest frame submission (CLOCK_MONOTONIC) of a stream, or 0
static uint64_t
newest_submit(struct sf_pt_type *t)
{
	uint64_t newest = 0;
	uint32_t count = t->frame_count < SF_PT_MAX_FRAMES ? t->frame_count : SF_PT_MAX_FRAMES;
	for (int cam = 0; t->initialized && cam < SF_PT_CAMERA_COUNT; cam++)
		for (uint32_t i = 0; i < count; i++)
			if (t->cameras[cam].frames[i].submit_ns > newest)
				newest = t->cameras[cam].frames[i].submit_ns;
	return newest;
}

// Prefers the Arcturus color cameras and falls back to the IR tracking cameras while it delivers nothing
static int
choose_stream(struct sf_comp_passthrough *pt, uint64_t now)
{
	const char *forced = getenv("SF_PASSTHROUGH_STREAM");
	if (forced)
		return strcmp(forced, "tracking") ? SF_PT_TYPE_COLOR : SF_PT_TYPE_TRACKING;
	uint64_t color = newest_submit(&pt->shm->types[SF_PT_TYPE_COLOR]);
	if (color && now > color && now - color < 1000000000ull)
		return SF_PT_TYPE_COLOR;
	if (now - pt->want_ns < 1000000000ull)
		return pt->type;
	return SF_PT_TYPE_TRACKING;
}

bool
sf_comp_passthrough_request(struct sf_comp_passthrough *pt, bool wanted)
{
	if (!pt)
		return false;
	pt->wanted = wanted;
	if (!pt->shm) {
		int fd = open(SF_PT_SHM_PATH, O_RDWR | O_CLOEXEC);
		struct stat st;
		if (fd < 0)
			return false;
		if (fstat(fd, &st) == 0 && st.st_size == sizeof(struct sf_pt_shm)) {
			void *p = mmap(NULL, sizeof(struct sf_pt_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (p != MAP_FAILED)
				pt->shm = p;
		}
		close(fd);
	}
	if (!pt->shm || __atomic_load_n(&pt->shm->magic, __ATOMIC_ACQUIRE) != SF_PT_MAGIC ||
	    pt->shm->version != SF_PT_VERSION || pt->shm->shm_size != sizeof(*pt->shm) || pt->shm->pid <= 0 ||
	    kill(pt->shm->pid, 0) != 0)
		return false;
	uint64_t now = os_monotonic_get_ns();
	if (!wanted)
		pt->want_ns = 0;
	else if (!pt->want_ns)
		pt->want_ns = now;
	if (wanted) {
		int type = choose_stream(pt, now);
		if (type != pt->type)
			U_LOG_I("Passthrough switching to the %s cameras", stream_names[type]);
		pt->type = type;
	}
	const char *forced = getenv("SF_PASSTHROUGH_STREAM");
	bool color_reader = wanted && !(forced && !strcmp(forced, "tracking"));
	__atomic_store_n(&pt->shm->active, wanted, __ATOMIC_RELEASE);
	__atomic_store_n(&pt->shm->types[SF_PT_TYPE_COLOR].reader, color_reader, __ATOMIC_RELEASE);
	__atomic_store_n(&pt->shm->types[SF_PT_TYPE_TRACKING].reader, wanted && pt->type == SF_PT_TYPE_TRACKING,
	                 __ATOMIC_RELEASE);
	return wanted;
}

static void
dma_destroy(struct sf_comp_passthrough *pt, struct pt_dma *dma)
{
	if (dma->view)
		pt->vk->vkDestroyImageView(pt->vk->device, dma->view, NULL);
	if (dma->image)
		pt->vk->vkDestroyImage(pt->vk->device, dma->image, NULL);
	if (dma->buffer)
		pt->vk->vkDestroyBuffer(pt->vk->device, dma->buffer, NULL);
	if (dma->memory)
		pt->vk->vkFreeMemory(pt->vk->device, dma->memory, NULL);
	if (dma->fd > 0)
		close(dma->fd);
	memset(dma, 0, sizeof(*dma));
}

static bool
dma_import(struct sf_comp_passthrough *pt, struct pt_dma *dma, int remote_fd)
{
	if (dma->attempted && dma->remote_fd == remote_fd)
		return dma->buffer && dma->memory;
	dma_destroy(pt, dma);
	dma->attempted = true;
	dma->remote_fd = remote_fd;
	int pidfd = syscall(SYS_pidfd_open, pt->pid, 0);
	if (pidfd < 0)
		return false;
	int fd = syscall(SYS_pidfd_getfd, pidfd, remote_fd, 0);
	close(pidfd);
	if (fd < 0)
		return false;
	dma->fd = fd;
	off_t size = lseek(fd, 0, SEEK_END);
	if (size <= 0)
		goto fail;
	dma->size = size;
	struct vk_bundle *vk = pt->vk;
	VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
	                                             .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	                             .pNext = &external,
	                             .size = size,
	                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	if (vk->vkCreateBuffer(vk->device, &buffer, NULL, &dma->buffer) != VK_SUCCESS)
		goto fail;
	VkMemoryRequirements req;
	vk->vkGetBufferMemoryRequirements(vk->device, dma->buffer, &req);
	VkMemoryFdPropertiesKHR props = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
	if (req.size > (uint64_t)size ||
	    pt->get_fd_properties(vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &props) != VK_SUCCESS)
		goto fail;
	uint32_t bits = req.memoryTypeBits & props.memoryTypeBits;
	if (!bits)
		goto fail;
	int imported_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (imported_fd < 0)
		goto fail;
	VkImportMemoryFdInfoKHR import = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
	                                  .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	                                  .fd = imported_fd};
	VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	                                   .pNext = &import,
	                                   .allocationSize = size,
	                                   .memoryTypeIndex = __builtin_ctz(bits)};
	dma->memory_type = __builtin_ctz(bits);
	if (vk->vkAllocateMemory(vk->device, &allocation, NULL, &dma->memory) != VK_SUCCESS) {
		close(imported_fd);
		goto fail;
	}
	if (vk->vkBindBufferMemory(vk->device, dma->buffer, dma->memory, 0) != VK_SUCCESS)
		goto fail;
	return true;
fail:
	dma_destroy(pt, dma);
	dma->attempted = true;
	dma->remote_fd = remote_fd;
	U_LOG_W("Passthrough DMA-BUF import failed; using CPU upload for this buffer");
	return false;
}

// A camera plane as a linear (DRM_FORMAT_MOD_LINEAR) image on its imported dma-buf memory
static bool
dma_image(struct sf_comp_passthrough *pt,
          struct pt_dma *dma,
          uint32_t width,
          uint32_t height,
          VkFormat format,
          uint32_t offset,
          uint32_t pitch)
{
	if (dma->view)
		return true;
	struct vk_bundle *vk = pt->vk;
	if (dma->image_failed || !dma->memory || !vk->has_EXT_image_drm_format_modifier)
		return false;
	dma->image_failed = true;
	VkSubresourceLayout layout = {.offset = offset, .rowPitch = pitch};
	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
	    .drmFormatModifier = 0, // DRM_FORMAT_MOD_LINEAR
	    .drmFormatModifierPlaneCount = 1,
	    .pPlaneLayouts = &layout};
	VkExternalMemoryImageCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	                                            .pNext = &modifier,
	                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                          .pNext = &external,
	                          .imageType = VK_IMAGE_TYPE_2D,
	                          .format = format,
	                          .extent = {width, height, 1},
	                          .mipLevels = 1,
	                          .arrayLayers = 1,
	                          .samples = VK_SAMPLE_COUNT_1_BIT,
	                          .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	                          .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	                          .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	                          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	if (vk->vkCreateImage(vk->device, &info, NULL, &dma->image) != VK_SUCCESS)
		goto fail;
	VkMemoryRequirements req;
	vk->vkGetImageMemoryRequirements(vk->device, dma->image, &req);
	if (!(req.memoryTypeBits & (1u << dma->memory_type)) || req.size > dma->size ||
	    vk->vkBindImageMemory(vk->device, dma->image, dma->memory, 0) != VK_SUCCESS)
		goto fail;
	if (vk_create_view(vk, dma->image, VK_IMAGE_VIEW_TYPE_2D, format,
	                   (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, &dma->view) != VK_SUCCESS)
		goto fail;
	dma->image_failed = false;
	return true;
fail:
	if (dma->image)
		vk->vkDestroyImage(vk->device, dma->image, NULL);
	dma->image = VK_NULL_HANDLE;
	static bool reported;
	if (!reported)
		U_LOG_W("Passthrough: sampling camera frames in place failed, copying them instead");
	reported = true;
	return false;
}

static void
release_frame(struct sf_comp_passthrough *pt, struct sf_pt_frame *f)
{
	if (f && pt->shm && pt->pid == pt->shm->pid &&
	    pt->generation == __atomic_load_n(&pt->shm->types[pt->shown].generation, __ATOMIC_ACQUIRE))
		__atomic_store_n(&f->state, SF_PT_FRAME_FREE, __ATOMIC_RELEASE);
}

static void
release_direct(struct sf_comp_passthrough *pt, struct pt_camera *c)
{
	release_frame(pt, c->direct_frame);
	c->direct_frame = NULL;
	c->direct = false;
}

void
sf_comp_passthrough_finish(struct sf_comp_passthrough *pt)
{
	if (!pt)
		return;
	for (int cam = 0; cam < 2; cam++) {
		struct pt_camera *c = &pt->cameras[cam];
		for (int frame = 0; frame < SF_PT_MAX_FRAMES; frame++)
			for (int plane = 0; plane < 2; plane++) {
				struct pt_dma *dma = &c->dma[frame][plane];
				if (dma->synced) {
					struct dma_buf_sync sync = {DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
					ioctl(dma->fd, DMA_BUF_IOCTL_SYNC, &sync);
					dma->synced = false;
				}
			}
		// a frame sampled in place stays claimed until prepare replaces it
		if (c->held != c->direct_frame)
			release_frame(pt, c->held);
		c->held = NULL;
	}
}

static bool
gpu_planes(struct sf_comp_passthrough *pt,
           struct pt_camera *c,
           struct sf_pt_type *t,
           struct sf_pt_frame *f,
           int index,
           VkCommandBuffer cmd,
           bool *direct)
{
	*direct = false;
	if (!pt->gpu_copy)
		return false;
	for (int plane = 0; plane < 2; plane++) {
		struct pt_dma *dma = &c->dma[index][plane];
		struct pt_texture *texture = &c->textures[plane];
		uint32_t row = texture->width * texture->bytes;
		uint64_t end =
		    t->planes[plane].offset + (uint64_t)(texture->height - 1) * t->planes[plane].stride + row;
		if (!dma_import(pt, dma, f->fds[plane]) || t->planes[plane].stride < row ||
		    t->planes[plane].stride % texture->bytes || t->planes[plane].offset % 4 || end > dma->size)
			return false;
		// CPU cache maintenance over the whole buffer, only needed for CPU access; frames are complete once XRService
		// marks them latest
		if (pt->dmabuf_sync) {
			struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
			if (ioctl(dma->fd, DMA_BUF_IOCTL_SYNC, &sync) != 0)
				return false;
			dma->synced = true;
		}
	}
	if (pt->direct_sampling) {
		bool ok = true;
		for (int plane = 0; plane < 2 && ok; plane++) {
			struct pt_texture *texture = &c->textures[plane];
			ok = dma_image(pt, &c->dma[index][plane], texture->width, texture->height,
			               plane ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM, t->planes[plane].offset,
			               t->planes[plane].stride);
		}
		if (ok) {
			for (int plane = 0; plane < 2; plane++) {
				struct pt_dma *dma = &c->dma[index][plane];
				VkImageMemoryBarrier barrier = {
				    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
				    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				    .oldLayout = dma->image_used ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				                                 : VK_IMAGE_LAYOUT_UNDEFINED,
				    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
				    .dstQueueFamilyIndex = pt->vk->main_queue->family_index,
				    .image = dma->image,
				    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
				pt->vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL,
				                             1, &barrier);
				dma->image_used = true;
				c->direct_views[plane] = dma->view;
			}
			*direct = true;
			return true;
		}
	}
	for (int plane = 0; plane < 2; plane++) {
		struct pt_dma *dma = &c->dma[index][plane];
		VkBufferMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		                                 .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
		                                 .dstQueueFamilyIndex = pt->vk->main_queue->family_index,
		                                 .buffer = dma->buffer,
		                                 .size = VK_WHOLE_SIZE};
		pt->vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
		                             0, NULL, 1, &barrier, 0, NULL);
		texture_upload(pt, &c->textures[plane], cmd, dma->buffer, t->planes[plane].offset,
		               t->planes[plane].stride);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.dstAccessMask = 0;
		barrier.srcQueueFamilyIndex = pt->vk->main_queue->family_index;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		pt->vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		                             0, 0, NULL, 1, &barrier, 0, NULL);
	}
	return true;
}

static bool
copy_plane(
    struct sf_comp_passthrough *pt, struct sf_pt_type *t, struct sf_pt_frame *f, int plane, struct pt_texture *texture)
{
	int pidfd = syscall(SYS_pidfd_open, pt->shm->pid, 0);
	if (pidfd < 0)
		return false;
	int fd = syscall(SYS_pidfd_getfd, pidfd, f->fds[plane], 0);
	close(pidfd);
	if (fd < 0) {
		static bool reported;
		if (!reported)
			U_LOG_W("Passthrough pidfd_getfd: %s", strerror(errno));
		reported = true;
		return false;
	}
	off_t size = lseek(fd, 0, SEEK_END);
	size_t row = texture->width * texture->bytes;
	size_t end = t->planes[plane].offset + (uint64_t)(texture->height - 1) * t->planes[plane].stride + row;
	if (size <= 0 || t->planes[plane].stride < row || end > (uint64_t)size) {
		close(fd);
		return false;
	}
	void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return false;
	}
	struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
	bool ok = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
	if (ok) {
		for (uint32_t y = 0; y < texture->height; y++) {
			memcpy((uint8_t *)texture->staging.data + y * row,
			       (uint8_t *)p + t->planes[plane].offset + y * t->planes[plane].stride, row);
		}
		sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
		ok = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
	}
	munmap(p, size);
	close(fd);
	return ok;
}

// Block queue layout as in steamvr_lh's blockqueue.cpp: a 1064-byte header (sequence of block i at 40 + 16 i),
// then 64-byte aligned blocks of header_size + data_size; property tables at the start of each block header
#define SF_QUEUE_BASE 1088
#define SF_QUEUE_HEADER_MAGIC 0xacbdf123u

static bool
queue_open(struct pt_queue *q, const char *path)
{
	if (q->map)
		return true;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (fd < 0)
		return false;
	if (fstat(fd, &st) == 0 && st.st_size > SF_QUEUE_BASE) {
		void *p = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
		if (p != MAP_FAILED) {
			q->map = p;
			q->size = st.st_size;
		}
	}
	close(fd);
	return q->map != NULL;
}

static void
queue_close(struct pt_queue *q)
{
	if (q->map)
		munmap(q->map, q->size);
	q->map = NULL;
}

// Newest published block of a queue: its header, data size and sequence, or NULL
static const uint8_t *
queue_newest(struct pt_queue *q, uint32_t *header_size, uint32_t *data_size, uint64_t *seq, uint32_t *index)
{
	const uint32_t *h = (const uint32_t *)q->map;
	uint32_t count = h[3];
	*data_size = h[1];
	*header_size = h[2];
	uint64_t block = ((uint64_t)*header_size + *data_size + 63) & ~(uint64_t)63;
	if (!count || count > 64 || SF_QUEUE_BASE + count * block > q->size)
		return NULL;
	*seq = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint64_t s = __atomic_load_n((const uint64_t *)(q->map + 40 + 16 * i), __ATOMIC_ACQUIRE);
		if (s > *seq) {
			*seq = s;
			*index = i;
		}
	}
	return *seq ? q->map + SF_QUEUE_BASE + *index * block : NULL;
}

static uint64_t
queue_block_seq(struct pt_queue *q, uint32_t index)
{
	return __atomic_load_n((const uint64_t *)(q->map + 40 + 16 * index), __ATOMIC_ACQUIRE);
}

// A block header property by path, as XRService names them (FNV-1a path handles, as the vrclient shim makes them)
static const void *
queue_property(const uint8_t *header, uint32_t header_size, const char *path, uint32_t size)
{
	const uint32_t *table = (const uint32_t *)header;
	if (table[2] != SF_QUEUE_HEADER_MAGIC || 16 + table[0] * 16 > header_size)
		return NULL;
	uint32_t hash = 2166136261u;
	for (const char *c = path; *c; c++)
		hash = (hash ^ (uint8_t)*c) * 16777619u;
	hash = 0x10000000u | (hash & 0x0fffffffu);
	for (uint32_t i = 0; i < table[0]; i++) {
		const uint32_t *e = table + 4 + i * 4; // prop, tag, size, offset
		if (e[0] == hash && e[2] == size && e[3] + size <= header_size)
			return header + e[3];
	}
	return NULL;
}

static uint16_t
half_from_float(float value)
{
	uint32_t f;
	memcpy(&f, &value, sizeof(f));
	uint32_t sign = (f >> 16) & 0x8000, exponent = (f >> 23) & 0xff, mantissa = f & 0x7fffff;
	if (exponent < 113)
		return sign;
	if (exponent > 142)
		return sign | 0x7c00;
	return sign | ((exponent - 112) << 10) | (mantissa >> 13);
}

static void *floor_worker(void *data);

// Grid size from the layout's first triangle (0, width, 1) and highest index
static bool
mesh_layout(struct pt_mesh *m)
{
	uint32_t header_size, data_size, index;
	uint64_t seq;
	const uint8_t *block = queue_newest(&m->layout, &header_size, &data_size, &seq, &index);
	if (!block)
		return false;
	const uint32_t *indices = (const uint32_t *)(block + header_size);
	const uint32_t *count_prop = queue_property(block, header_size, "/meshLayout/indexCount", 4);
	uint32_t count = count_prop ? *count_prop : 0;
	if (count < 3 || count > data_size / 4)
		return false;
	uint32_t highest = 0;
	for (uint32_t i = 0; i < count; i++)
		if (indices[i] > highest)
			highest = indices[i];
	uint32_t width = indices[1];
	if (width < 2 || width > 1024 || (highest + 1) % width || (highest + 1) / width < 2)
		return false;
	uint32_t height = (highest + 1) / width;
	if (height > 1024)
		return false;
	float *vertices = calloc((size_t)width * height, 3 * sizeof(float));
	float *points = calloc((size_t)width * height, 4 * sizeof(float));
	if (!vertices || !points) {
		free(vertices);
		free(points);
		return false;
	}
	m->width = width;
	m->height = height;
	m->vertices = vertices;
	m->floor_points = points;
	m->floor_vertices = calloc((size_t)width * height, 3 * sizeof(float));
	m->floor_pending_vertices = calloc((size_t)width * height, 3 * sizeof(float));
	if (m->floor_vertices && m->floor_pending_vertices) {
		pthread_mutex_init(&m->floor_mutex, NULL);
		pthread_cond_init(&m->floor_condition, NULL);
		m->floor_running = true;
		m->floor_started = pthread_create(&m->floor_thread, NULL, floor_worker, m) == 0;
		if (!m->floor_started) {
			pthread_cond_destroy(&m->floor_condition);
			pthread_mutex_destroy(&m->floor_mutex);
		}
	}
	return true;
}

#define SF_FLOOR_BINS 80
#define SF_FLOOR_BIN_M 0.03f

static void
floor_estimate(struct pt_mesh *m, const float pose[12], double now)
{
	uint64_t floor_start = os_monotonic_get_ns();
	const uint32_t count = m->width * m->height;
	for (uint32_t i = 0; i < count; i++) {
		const float *v = &m->floor_vertices[i * 3];
		float *p = &m->floor_points[i * 4];
		for (int r = 0; r < 3; r++)
			p[r] = pose[r * 4] * v[0] + pose[r * 4 + 1] * v[1] + pose[r * 4 + 2] * v[2] + pose[r * 4 + 3];
		p[3] = 0;
		if (v[2] < 0.05f || v[2] > 8 || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
			p[0] = p[1] = p[2] = NAN;
	}
	for (uint32_t y = 0; y + 1 < m->height; y++) {
		for (uint32_t x = 0; x + 1 < m->width; x++) {
			uint32_t i = y * m->width + x;
			float *p = &m->floor_points[i * 4];
			const float *right = &m->floor_points[(i + 1) * 4],
			            *down = &m->floor_points[(i + m->width) * 4];
			struct xrt_vec3 a = {right[0] - p[0], right[1] - p[1], right[2] - p[2]};
			struct xrt_vec3 b = {down[0] - p[0], down[1] - p[1], down[2] - p[2]};
			float nx = a.y * b.z - a.z * b.y, ny = a.z * b.x - a.x * b.z, nz = a.x * b.y - a.y * b.x;
			float norm = sqrtf(nx * nx + ny * ny + nz * nz);
			p[3] = norm > 1e-6f && fabsf(ny) / norm > 0.96f;
		}
	}
	unsigned support[SF_FLOOR_BINS] = {0};
	float sum[SF_FLOOR_BINS] = {0}, min_x[SF_FLOOR_BINS], max_x[SF_FLOOR_BINS];
	float min_z[SF_FLOOR_BINS], max_z[SF_FLOOR_BINS];
	for (int b = 0; b < SF_FLOOR_BINS; b++) {
		min_x[b] = min_z[b] = INFINITY;
		max_x[b] = max_z[b] = -INFINITY;
	}
	for (uint32_t i = 0; i < count; i++) {
		const float *p = &m->floor_points[i * 4];
		float below = pose[7] - p[1];
		if (!p[3] || !isfinite(below) || below < 0.5f || below > 2.9f ||
		    hypotf(p[0] - pose[3], p[2] - pose[11]) > 2.5f)
			continue;
		int centre = (int)((below - 0.5f) / SF_FLOOR_BIN_M);
		for (int b = centre - 1; b <= centre + 1; b++) {
			if (b < 0 || b >= SF_FLOOR_BINS)
				continue;
			support[b]++;
			sum[b] += p[1];
			min_x[b] = fminf(min_x[b], p[0]);
			max_x[b] = fmaxf(max_x[b], p[0]);
			min_z[b] = fminf(min_z[b], p[2]);
			max_z[b] = fmaxf(max_z[b], p[2]);
		}
	}
	bool eligible[SF_FLOOR_BINS];
	unsigned strongest = 0;
	for (int b = 0; b < SF_FLOOR_BINS; b++) {
		eligible[b] = support[b] >= 30 && max_x[b] - min_x[b] >= 0.25f && max_z[b] - min_z[b] >= 0.25f;
		if (eligible[b] && support[b] > strongest)
			strongest = support[b];
	}
	int best = -1;
	// Choose the density peak of the lowest substantial surface, rather than its noisy lower edge.
	for (int b = SF_FLOOR_BINS - 1; b >= 0; b--) {
		if (!eligible[b] || support[b] * 5 < strongest) {
			if (best >= 0)
				break;
			continue;
		}
		if (best < 0 || support[b] > support[best])
			best = b;
	}
	float height = best < 0 ? NAN : sum[best] / support[best];
	char path[512], temporary[520];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || snprintf(path, sizeof(path), "%s/frame-shell-floor", runtime) >= (int)sizeof(path))
		return;
	snprintf(temporary, sizeof(temporary), "%s.tmp", path);
	FILE *file = fopen(temporary, "w");
	if (!file)
		return;
	fprintf(file, "1 %lu %.5f %u\n", (unsigned long)os_monotonic_get_ns(), height, best < 0 ? 0 : support[best]);
	uint32_t selected_count = 0;
	for (uint32_t i = 0; i < count; i++) {
		const float *p = &m->floor_points[i * 4];
		if (p[3] && fabsf(p[1] - height) < 0.06f)
			selected_count++;
	}
	uint32_t selected_step = (selected_count + 127) / 128, selected_seen = 0;
	for (uint32_t i = 0; i < count && selected_count; i++) {
		const float *p = &m->floor_points[i * 4];
		if (p[3] && fabsf(p[1] - height) < 0.06f && selected_seen++ % selected_step == 0)
			fprintf(file, "%.4f %.4f %.4f 1\n", p[0], p[1], p[2]);
	}
	uint32_t step_x = (m->width + 11) / 12, step_y = (m->height + 9) / 10;
	for (uint32_t y = 0; y < m->height; y += step_y) {
		for (uint32_t x = 0; x < m->width; x += step_x) {
			const float *p = &m->floor_points[(y * m->width + x) * 4];
			if (isfinite(p[0]))
				fprintf(file, "%.4f %.4f %.4f %.0f\n", p[0], p[1], p[2], p[3]);
		}
	}
	if (fclose(file) == 0)
		rename(temporary, path);
	if (now - m->floor_log_time > 2) {
		U_LOG_I("Mesh floor prototype: height %.3f m raw, support %u, span %.2f x %.2f m, CPU %.3f ms", height,
		        best < 0 ? 0 : support[best], best < 0 ? 0 : max_x[best] - min_x[best],
		        best < 0 ? 0 : max_z[best] - min_z[best], (os_monotonic_get_ns() - floor_start) * 1e-6);
		m->floor_log_time = now;
	}
}

static void *
floor_worker(void *data)
{
	struct pt_mesh *m = data;
	struct sched_param priority = {0};
	pthread_setschedparam(pthread_self(), SCHED_OTHER, &priority);
	pthread_mutex_lock(&m->floor_mutex);
	while (m->floor_running) {
		while (!m->floor_pending && m->floor_running)
			pthread_cond_wait(&m->floor_condition, &m->floor_mutex);
		if (!m->floor_running)
			break;
		float *vertices = m->floor_vertices;
		m->floor_vertices = m->floor_pending_vertices;
		m->floor_pending_vertices = vertices;
		float pose[12];
		memcpy(pose, m->floor_pending_pose, sizeof(pose));
		double now = m->floor_pending_time;
		m->floor_pending = false;
		pthread_mutex_unlock(&m->floor_mutex);
		floor_estimate(m, pose, now);
		pthread_mutex_lock(&m->floor_mutex);
	}
	pthread_mutex_unlock(&m->floor_mutex);
	return NULL;
}

static void
floor_schedule(struct pt_mesh *m, const float pose[12], double now)
{
	if (!m->floor_started || now - m->floor_sample_time < 0.2)
		return;
	m->floor_sample_time = now;
	pthread_mutex_lock(&m->floor_mutex);
	memcpy(m->floor_pending_vertices, m->vertices, (size_t)m->width * m->height * 3 * sizeof(float));
	memcpy(m->floor_pending_pose, pose, sizeof(m->floor_pending_pose));
	m->floor_pending_time = now;
	m->floor_pending = true;
	pthread_cond_signal(&m->floor_condition);
	pthread_mutex_unlock(&m->floor_mutex);
}

static void
mesh_update(struct sf_comp_passthrough *pt, VkCommandBuffer cmd)
{
	struct pt_mesh *m = &pt->mesh;
	struct timespec raw;
	clock_gettime(CLOCK_MONOTONIC_RAW, &raw);
	double now = raw.tv_sec + raw.tv_nsec * 1e-9;
	m->valid = m->valid && now - m->time < SF_MESH_MAX_AGE_S;
	if (!pt->mesh_enabled || !queue_open(&m->data, SF_MESH_DATA_PATH) ||
	    !queue_open(&m->layout, SF_MESH_LAYOUT_PATH))
		return;
	if (!m->vertices && !mesh_layout(m))
		return;
	uint32_t header_size, data_size, index;
	uint64_t seq;
	const uint8_t *block = queue_newest(&m->data, &header_size, &data_size, &seq, &index);
	if (!block || seq == m->seq || (uint64_t)m->width * m->height * 12 > data_size)
		return;
	const double *time = queue_property(block, header_size, "/meshData/posePredictionBaseTime", 8);
	const float *pose = queue_property(block, header_size, "/meshData/rawFromMeshPose", 48);
	if (!time || !pose || now - *time >= SF_MESH_MAX_AGE_S)
		return;
	uint32_t vertices = m->width * m->height;
	VkFormat format = pt->mesh_format;
	if (!texture_create(pt, &m->depth, m->width, m->height, format == VK_FORMAT_R32_SFLOAT ? 4 : 2, format))
		return;
	// The vertices lie on the rays of a pinhole grid (x/z linear in the column, y/z in the row): fit it while
	// copying out the depths
	memcpy(m->vertices, block + header_size, (size_t)vertices * 3 * sizeof(float));
	const float *v = m->vertices;
	double n = 0, sg[2] = {0}, sgg[2] = {0}, sa[2] = {0}, sga[2] = {0};
	for (uint32_t i = 0; i < vertices; i++) {
		float z = v[i * 3 + 2];
		if (format == VK_FORMAT_R32_SFLOAT)
			((float *)m->depth.staging.data)[i] = z;
		else
			((uint16_t *)m->depth.staging.data)[i] = half_from_float(z);
		if (!(z > 0.05f) || !isfinite(z) || i % 7)
			continue;
		double g[2] = {i % m->width, i / m->width}, a[2] = {v[i * 3] / z, v[i * 3 + 1] / z};
		n++;
		for (int k = 0; k < 2; k++) {
			sg[k] += g[k];
			sgg[k] += g[k] * g[k];
			sa[k] += a[k];
			sga[k] += g[k] * a[k];
		}
	}
	float pose_copy[12];
	memcpy(pose_copy, pose, sizeof(pose_copy));
	double base_time = *time;
	if (queue_block_seq(&m->data, index) != seq || n < 100)
		return; // overwritten while copying, or nothing usable
	double focal[2], centre[2];
	for (int k = 0; k < 2; k++) {
		double slope = (n * sga[k] - sg[k] * sa[k]) / (n * sgg[k] - sg[k] * sg[k]);
		double offset = (sa[k] - slope * sg[k]) / n;
		focal[k] = 1 / slope;
		centre[k] = -offset / slope;
	}
	if (!isfinite(focal[0]) || !isfinite(focal[1]) || focal[0] <= 0 || fabs(focal[0] / focal[1] - 1) > 0.02)
		return;
	floor_schedule(m, pose_copy, now);
	texture_upload(pt, &m->depth, cmd, VK_NULL_HANDLE, 0, 0);
	m->focal = (focal[0] + focal[1]) / 2;
	m->cx = centre[0];
	m->cy = centre[1];
	// mesh_from_world = inverse of rawFromMeshPose (rotation rows, then translation)
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++)
			m->mesh_from_world[r * 4 + c] = pose_copy[c * 4 + r];
		m->mesh_from_world[r * 4 + 3] =
		    -(pose_copy[r] * pose_copy[3] + pose_copy[4 + r] * pose_copy[7] + pose_copy[8 + r] * pose_copy[11]);
	}
	m->seq = seq;
	m->time = base_time;
	m->valid = true;
	if (!m->logged)
		U_LOG_I("Passthrough room mesh: %ux%u grid, focal %.2f cells, centre %.2f %.2f, %s", m->width, m->height,
		        m->focal, m->cx, m->cy, format == VK_FORMAT_R32_SFLOAT ? "R32" : "R16");
	m->logged = true;
}

static bool
capture_orientation(const uint8_t metadata[SF_PT_METADATA_SIZE], struct xrt_quat *orientation)
{
	float meta[62];
	memcpy(meta, metadata, sizeof(meta));
	struct xrt_matrix_3x3 rotation = {0};
	// Remove the camera mounting rotation from its recorded world pose.
	for (int col = 0; col < 3; col++)
		for (int row = 0; row < 3; row++) {
			float value = 0;
			for (int k = 0; k < 3; k++)
				value += meta[12 + row * 4 + k] * meta[24 + col * 4 + k];
			rotation.v[row * 3 + col] = value;
		}
	for (int i = 0; i < 9; i++)
		if (!isfinite(rotation.v[i]))
			return false;
	for (int col = 0; col < 3; col++) {
		float norm = 0;
		for (int row = 0; row < 3; row++)
			norm += rotation.v[row * 3 + col] * rotation.v[row * 3 + col];
		if (fabsf(norm - 1) > 0.01f)
			return false;
	}
	math_quat_from_matrix_3x3(&rotation, orientation);
	math_quat_normalize(orientation);
	return true;
}

void
sf_comp_passthrough_prepare(struct sf_comp_passthrough *pt,
                            struct render_gfx *render,
                            const struct comp_render_dispatch_data *d)
{
	if (!pt)
		return;
	pt->ready = false;
	if (!pt->wanted || !pt->shm)
		return;
	struct sf_pt_type *t = &pt->shm->types[pt->type];
	uint32_t generation = __atomic_load_n(&t->generation, __ATOMIC_ACQUIRE);
	if ((generation & 1) || !t->initialized || t->drm_format != 0x3231564e || t->modifier != 0 ||
	    t->plane_count != 2 || !t->width || !t->height || t->width > 4096 || t->height > 4096 || t->width % 2 ||
	    t->height % 2 || !t->frame_count || t->frame_count > SF_PT_MAX_FRAMES)
		return;
	if (pt->pid != pt->shm->pid || pt->generation != generation || pt->shown != pt->type) {
		sf_comp_passthrough_finish(pt);
		for (int c = 0; c < 2; c++) {
			release_direct(pt, &pt->cameras[c]);
			for (int i = 0; i < 3; i++)
				texture_destroy(pt, &pt->cameras[c].textures[i]);
			for (int frame = 0; frame < SF_PT_MAX_FRAMES; frame++)
				for (int plane = 0; plane < 2; plane++)
					dma_destroy(pt, &pt->cameras[c].dma[frame][plane]);
			pt->cameras[c].held = NULL;
			pt->cameras[c].seq = pt->cameras[c].map_seq = pt->cameras[c].time = 0;
		}
		pt->pid = pt->shm->pid;
		pt->generation = generation;
		pt->shown = pt->type;
		U_LOG_I("Passthrough %s stream: %ux%u, XRService %d", stream_names[pt->shown], t->width, t->height,
		        pt->pid);
	}
	if (!pipeline_create(pt, render, d->views[0].squash.gfx.rtr->rgrp->render_pass))
		return;
	uint64_t prepare_start = os_monotonic_get_ns();
	mesh_update(pt, render->r->cmd);
	for (int cam = 0; cam < 2; cam++) {
		struct pt_camera *c = &pt->cameras[cam];
		struct sf_pt_camera *source = &t->cameras[cam];
		uint32_t mw = source->map_width, mh = source->map_height;
		if (!mw || !mh || mw > SF_PT_MAP_MAX || mh > SF_PT_MAP_MAX)
			continue;
		if (!texture_create(pt, &c->textures[0], t->width, t->height, 1, VK_FORMAT_R8_UNORM) ||
		    !texture_create(pt, &c->textures[1], t->width / 2, t->height / 2, 2, VK_FORMAT_R8G8_UNORM) ||
		    !texture_create(pt, &c->textures[2], mw, mh, 8, VK_FORMAT_R32G32_SFLOAT))
			continue;
		int best = -1;
		uint64_t seq = c->seq;
		for (uint32_t i = 0; i < t->frame_count; i++) {
			struct sf_pt_frame *f = &source->frames[i];
			if (__atomic_load_n(&f->state, __ATOMIC_ACQUIRE) == SF_PT_FRAME_LATEST && f->submit_seq > seq) {
				best = i;
				seq = f->submit_seq;
			}
		}
		if (best >= 0) {
			struct sf_pt_frame *f = &source->frames[best];
			uint32_t expected = SF_PT_FRAME_LATEST;
			if (__atomic_compare_exchange_n(&f->state, &expected, SF_PT_FRAME_READING, false,
			                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
				bool direct;
				bool gpu = gpu_planes(pt, c, t, f, best, render->r->cmd, &direct);
				bool ok = gpu || (copy_plane(pt, t, f, 0, &c->textures[0]) &&
				                  copy_plane(pt, t, f, 1, &c->textures[1]));
				if (ok && __atomic_load_n(&t->generation, __ATOMIC_ACQUIRE) == generation) {
					memcpy(c->metadata, f->metadata, sizeof(c->metadata));
					c->seq = seq;
					c->time = f->submit_ns;
					if (!gpu) {
						texture_upload(pt, &c->textures[0], render->r->cmd, VK_NULL_HANDLE, 0,
						               0);
						texture_upload(pt, &c->textures[1], render->r->cmd, VK_NULL_HANDLE, 0,
						               0);
					}
				}
				if (gpu && direct) {
					// keep sampling this frame until a newer one arrives
					if (c->direct_frame != f)
						release_frame(pt, c->direct_frame);
					c->direct_frame = f;
					c->direct = true;
					c->held = f;
				} else {
					if (c->direct)
						release_direct(pt, c);
					if (gpu)
						c->held = f;
					else
						__atomic_store_n(&f->state, SF_PT_FRAME_FREE, __ATOMIC_RELEASE);
				}
			}
		}
		if (!c->textures[2].uploaded || c->map_seq != __atomic_load_n(&source->map_seq, __ATOMIC_ACQUIRE)) {
			memcpy(c->textures[2].staging.data, source->map, mw * mh * 8);
			c->map_seq = __atomic_load_n(&source->map_seq, __ATOMIC_ACQUIRE);
			texture_upload(pt, &c->textures[2], render->r->cmd, VK_NULL_HANDLE, 0, 0);
		}
		VkDescriptorImageInfo images[4];
		VkWriteDescriptorSet writes[4];
		for (uint32_t i = 0; i < 4; i++) {
			// binding 3 is the room mesh; the luma texture stands in until there is one
			VkImageView view = i < 2 && c->direct ? c->direct_views[i]
			                   : i < 3        ? c->textures[i].view
			                   : pt->mesh.depth.uploaded ? pt->mesh.depth.view
			                                             : c->textures[0].view;
			images[i] = (VkDescriptorImageInfo){pt->sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			                                   .dstSet = c->set,
			                                   .dstBinding = i,
			                                   .descriptorCount = 1,
			                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			                                   .pImageInfo = &images[i]};
		}
		pt->vk->vkUpdateDescriptorSets(pt->vk->device, 4, writes, 0, NULL);
		if (c->time && os_monotonic_get_ns() - c->time < 500000000)
			pt->ready = true;
	}
	uint64_t now = os_monotonic_get_ns();
	pt->prepare_ns += now - prepare_start;
	pt->prepare_count++;
	if (!pt->log_ns)
		pt->log_ns = now;
	if (now - pt->log_ns > 10000000000ull) {
		U_LOG_I("Passthrough upload: %.2f ms/frame, compositor %.1f fps",
		        pt->prepare_ns / (double)pt->prepare_count / 1e6,
		        pt->prepare_count * 1e9 / (double)(now - pt->log_ns));
		struct xrt_quat capture_q, eye_inv, head_q, delta;
		math_quat_invert(&d->views[0].eye_pose.orientation, &eye_inv);
		math_quat_rotate(&d->views[0].world_pose_scanout_begin.orientation, &eye_inv, &head_q);
		if (capture_orientation(pt->cameras[0].metadata, &capture_q)) {
			math_quat_unrotate(&capture_q, &head_q, &delta);
			struct timespec raw;
			double camera_time;
			memcpy(&camera_time, pt->cameras[0].metadata, sizeof(camera_time));
			clock_gettime(CLOCK_MONOTONIC_RAW, &raw);
			double camera_age_ms = (raw.tv_sec + raw.tv_nsec * 1e-9 - camera_time) * 1000;
			U_LOG_I("Passthrough pose delta: %.2f deg, submit age %.1f ms, camera timestamp age %.1f ms, GPU copy %d",
			        2 * acosf(fminf(1, fabsf(delta.w))) * 180 / M_PI, (now - pt->cameras[0].time) / 1e6,
			        camera_age_ms, pt->gpu_copy);
			float m0[62], m1[62];
			memcpy(m0, pt->cameras[0].metadata, sizeof(m0));
			memcpy(m1, pt->cameras[1].metadata, sizeof(m1));
			U_LOG_I("Passthrough %s cameras: mount 0 (%.3f, %.3f, %.3f), mount 1 (%.3f, %.3f, %.3f), focal %.1f/%.1f",
			        stream_names[pt->shown], m0[27], m0[31], m0[35], m1[27], m1[31], m1[35], m0[4], m1[4]);
		}
		if (pt->mesh_enabled) {
			struct timespec raw;
			clock_gettime(CLOCK_MONOTONIC_RAW, &raw);
			U_LOG_I("Passthrough room mesh: %s, seq %lu, age %.0f ms", pt->mesh.valid ? "in use" : "not available",
			        (unsigned long)pt->mesh.seq,
			        pt->mesh.time ? (raw.tv_sec + raw.tv_nsec * 1e-9 - pt->mesh.time) * 1e3 : -1.0);
		}
		pt->prepare_ns = pt->prepare_count = 0;
		pt->log_ns = now;
	}
}

static void
trace_pose(FILE *file, uint64_t now, const char *kind, uint32_t index, uint64_t source_ns,
           const struct xrt_pose *pose, uint32_t flags, uint64_t sample)
{
	fprintf(file, "%lu,%s,%u,%lu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%lu\n", now, kind, index,
	        source_ns, pose->orientation.x, pose->orientation.y, pose->orientation.z, pose->orientation.w,
	        pose->position.x, pose->position.y, pose->position.z, flags, sample);
}

void
sf_comp_passthrough_trace(struct sf_comp_passthrough *pt, const struct comp_render_dispatch_data *d,
                          const struct comp_layer *layers, uint32_t layer_count)
{
	if (!pt || !pt->ready) return;
	uint64_t now = os_monotonic_get_ns();
	if (!pt->pose_trace_checked) {
		pt->pose_trace_checked = true;
		const char *path = getenv("SF_FRAME_POSE_TRACE");
		if (!path) return;
		pt->pose_trace = fopen(path, "w");
		if (!pt->pose_trace) { U_LOG_W("Could not open pose trace %s", path); return; }
		setvbuf(pt->pose_trace, NULL, _IOFBF, 65536);
		pt->pose_trace_end_ns = now + 30000000000ull;
		fprintf(pt->pose_trace, "render_ns,kind,index,source_ns,qx,qy,qz,qw,px,py,pz,flags,sample\n");
		U_LOG_I("Recording 30 seconds of frame poses to %s", path);
	}
	if (!pt->pose_trace) return;
	if (now >= pt->pose_trace_end_ns) {
		fclose(pt->pose_trace);
		pt->pose_trace = NULL;
		U_LOG_I("Frame pose trace complete");
		return;
	}
	struct xrt_pose eye_inverse, head;
	math_pose_invert(&d->views[0].eye_pose, &eye_inverse);
	math_pose_transform(&d->views[0].world_pose_scanout_begin, &eye_inverse, &head);
	trace_pose(pt->pose_trace, now, "head", 0, d->pose_timestamp_ns, &head, 0, 0);
	for (uint32_t view = 0; view < d->squash_view_count; view++)
		trace_pose(pt->pose_trace, now, "eye", view, d->pose_timestamp_ns, &d->views[view].eye_pose, 0, 0);
	for (int cam = 0; cam < 2; cam++) {
		float meta[62];
		memcpy(meta, pt->cameras[cam].metadata, sizeof(meta));
		if (!capture_orientation(pt->cameras[cam].metadata, &head.orientation)) continue;
		struct xrt_vec3 mount = {meta[27], meta[31], meta[35]}, offset;
		math_quat_rotate_vec3(&head.orientation, &mount, &offset);
		head.position = (struct xrt_vec3){meta[15] - offset.x, meta[19] - offset.y, meta[23] - offset.z};
		trace_pose(pt->pose_trace, now, "capture", cam, pt->cameras[cam].time, &head, 0, pt->cameras[cam].seq);
	}
	for (uint32_t i = 0; i < layer_count; i++) {
		const struct xrt_layer_data *data = &layers[i].data;
		if (data->type == XRT_LAYER_QUAD)
			trace_pose(pt->pose_trace, now, "quad", i, data->timestamp, &data->quad.pose, data->flags,
			           data->quad.sub.image_index);
		else if (data->type == XRT_LAYER_PROJECTION || data->type == XRT_LAYER_PROJECTION_DEPTH) {
			for (uint32_t view = 0; view < d->target.view_count; view++) {
				const struct xrt_layer_projection_view_data *v = data->type == XRT_LAYER_PROJECTION
				    ? &data->proj.v[view] : &data->depth.v[view];
				trace_pose(pt->pose_trace, now, "projection", view, data->timestamp, &v->pose, data->flags,
				           v->sub.image_index);
			}
		}
		else if (data->type == XRT_LAYER_CYLINDER)
			trace_pose(pt->pose_trace, now, "cylinder", i, data->timestamp, &data->cylinder.pose, data->flags,
			           data->cylinder.sub.image_index);
	}
}

void
sf_comp_passthrough_draw(struct sf_comp_passthrough *pt,
                         struct render_gfx *render,
                         uint32_t view,
                         const struct xrt_fov *fov,
                         const struct xrt_pose *eye_pose,
                         const struct xrt_pose *world_pose)
{
	if (!pt || !pt->ready || view >= 2)
		return;
	// Color: camera 1 left, camera 0 right, verified on the headset. IR: the camera mounted further left
	// (smaller x of the mounting offset) feeds the left eye.
	int left = 1;
	if (pt->shown == SF_PT_TYPE_TRACKING) {
		float x0, x1;
		memcpy(&x0, pt->cameras[0].metadata + 27 * sizeof(float), sizeof(x0));
		memcpy(&x1, pt->cameras[1].metadata + 27 * sizeof(float), sizeof(x1));
		left = x1 < x0 ? 1 : 0;
	}
	struct pt_camera *c = &pt->cameras[view == 0 ? left : 1 - left];
	if (!c->time || os_monotonic_get_ns() - c->time >= 500000000)
		return;
	float meta[62];
	memcpy(meta, c->metadata, sizeof(meta));
	float push[52] = {0};
	memcpy(push, meta + 4, 8 * sizeof(float));
	struct xrt_quat sample_q = eye_pose->orientation, capture_q;
	const char *reproject = getenv("SF_PASSTHROUGH_REPROJECT");
	bool compensate = (!reproject || strcmp(reproject, "0")) && capture_orientation(c->metadata, &capture_q);
	if (compensate)
		math_quat_unrotate(&capture_q, &world_pose->orientation, &sample_q);
	for (int col = 0; col < 3; col++) {
		struct xrt_vec3 basis = {col == 0, col == 1, col == 2}, head;
		math_quat_rotate_vec3(&sample_q, &basis, &head);
		for (int row = 0; row < 3; row++)
			push[8 + row * 4 + col] =
			    meta[24 + row] * head.x + meta[28 + row] * head.y + meta[32 + row] * head.z;
	}
	push[20] = tanf(fov->angle_left);
	push[21] = tanf(fov->angle_up);
	push[22] = tanf(fov->angle_right) - push[20];
	push[23] = tanf(fov->angle_down) - push[21];
	if (c->metadata[224]) {
		push[24] = meta[57];
		push[25] = meta[58];
		push[11] = meta[59];
		push[15] = meta[60];
	}
	float depth = 2.0f;
	const char *depth_option = getenv("SF_PASSTHROUGH_DEPTH_M");
	if (depth_option) {
		char *end;
		float value = strtof(depth_option, &end);
		if (end != depth_option && !*end && isfinite(value) && (value == 0 || value >= 0.1f)) depth = value;
	}
	if (depth > 0) {
		int offset = compensate ? 12 : 24;
		const struct xrt_vec3 *position = compensate ? &world_pose->position : &eye_pose->position;
		struct xrt_vec3 translation = {position->x - meta[offset + 3], position->y - meta[offset + 7],
		                              position->z - meta[offset + 11]};
		for (int row = 0; row < 3; row++)
			push[28 + row] = meta[offset + row] * translation.x + meta[offset + 4 + row] * translation.y +
			                 meta[offset + 8 + row] * translation.z;
		push[31] = 1.0f / depth;
	}
	push[26] = c->textures[0].width;
	push[27] = c->textures[0].height;
	// Room mesh: camera to mesh frame (mesh_from_world * world_from_camera at capture) and its pinhole grid
	struct pt_mesh *m = &pt->mesh;
	if (m->valid && compensate && depth > 0) {
		const float *mw = m->mesh_from_world, *wc = meta + 12;
		for (int r = 0; r < 3; r++) {
			for (int col = 0; col < 3; col++)
				push[32 + r * 4 + col] =
				    mw[r * 4] * wc[col] + mw[r * 4 + 1] * wc[4 + col] + mw[r * 4 + 2] * wc[8 + col];
			push[44 + r] = mw[r * 4] * wc[3] + mw[r * 4 + 1] * wc[7] + mw[r * 4 + 2] * wc[11] + mw[r * 4 + 3];
		}
		push[35] = m->focal;
		push[39] = m->cx;
		push[43] = m->cy;
		push[47] = 1;
		push[48] = m->width;
		push[49] = m->height;
		push[50] = 6;
		push[51] = pt->mesh_debug;
	}
	for (int i = 0; i < 52; i++)
		if (!isfinite(push[i]))
			return;
	if (push[0] <= 0 || push[1] <= 0)
		return;
	struct vk_bundle *vk = pt->vk;
	vk->vkCmdBindPipeline(render->r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pt->pipeline);
	vk->vkCmdBindDescriptorSets(render->r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pt->layout, 0, 1, &c->set, 0,
	                            NULL);
	vk->vkCmdPushConstants(render->r->cmd, pt->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
	                       sizeof(push), push);
	// a 64 x 64 grid of two triangles per cell, generated in frame_passthrough.vert
	vk->vkCmdDraw(render->r->cmd, 64 * 64 * 6, 1, 0, 0);
}

void
sf_comp_passthrough_destroy(struct sf_comp_passthrough **ptr)
{
	struct sf_comp_passthrough *pt = *ptr;
	if (!pt)
		return;
	sf_comp_passthrough_finish(pt);
	for (int c = 0; c < 2; c++)
		release_direct(pt, &pt->cameras[c]);
	sf_comp_passthrough_request(pt, false);
	struct vk_bundle *vk = pt->vk;
	for (int c = 0; c < 2; c++) {
		for (int i = 0; i < 3; i++)
			texture_destroy(pt, &pt->cameras[c].textures[i]);
		for (int frame = 0; frame < SF_PT_MAX_FRAMES; frame++)
			for (int plane = 0; plane < 2; plane++)
				dma_destroy(pt, &pt->cameras[c].dma[frame][plane]);
	}
	if (pt->pipeline)
		vk->vkDestroyPipeline(vk->device, pt->pipeline, NULL);
	if (pt->layout)
		vk->vkDestroyPipelineLayout(vk->device, pt->layout, NULL);
	if (pt->pool)
		vk->vkDestroyDescriptorPool(vk->device, pt->pool, NULL);
	if (pt->set_layout)
		vk->vkDestroyDescriptorSetLayout(vk->device, pt->set_layout, NULL);
	if (pt->sampler)
		vk->vkDestroySampler(vk->device, pt->sampler, NULL);
	if (pt->shm)
		munmap(pt->shm, sizeof(*pt->shm));
	texture_destroy(pt, &pt->mesh.depth);
	if (pt->mesh.floor_started) {
		pthread_mutex_lock(&pt->mesh.floor_mutex);
		pt->mesh.floor_running = false;
		pthread_cond_signal(&pt->mesh.floor_condition);
		pthread_mutex_unlock(&pt->mesh.floor_mutex);
		pthread_join(pt->mesh.floor_thread, NULL);
		pthread_cond_destroy(&pt->mesh.floor_condition);
		pthread_mutex_destroy(&pt->mesh.floor_mutex);
	}
	free(pt->mesh.floor_vertices);
	free(pt->mesh.floor_pending_vertices);
	free(pt->mesh.vertices);
	free(pt->mesh.floor_points);
	queue_close(&pt->mesh.data);
	queue_close(&pt->mesh.layout);
	if (pt->pose_trace)
		fclose(pt->pose_trace);
	free(pt);
	*ptr = NULL;
}
