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
	int remote_fd, fd;
	uint64_t size;
	bool attempted, synced;
};
struct pt_camera
{
	struct pt_texture textures[3];
	VkDescriptorSet set;
	uint8_t metadata[SF_PT_METADATA_SIZE];
	uint64_t seq, map_seq, time;
	struct sf_pt_frame *held;
	struct pt_dma dma[SF_PT_MAX_FRAMES][2];
};
struct sf_comp_passthrough
{
	struct vk_bundle *vk;
	struct sf_pt_shm *shm;
	bool wanted, ready, gpu_copy;
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
	pt->get_fd_properties =
	    (PFN_vkGetMemoryFdPropertiesKHR)vk->vkGetDeviceProcAddr(vk->device, "vkGetMemoryFdPropertiesKHR");
	pt->gpu_copy = vk->has_EXT_external_memory_dma_buf && pt->get_fd_properties && !getenv("SF_PASSTHROUGH_CPU");
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
	VkDescriptorSetLayoutBinding bindings[3];
	for (uint32_t i = 0; i < 3; i++)
		bindings[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
		                                             VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
	VkDescriptorSetLayoutCreateInfo set = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = bindings};
	if (vk->vkCreateDescriptorSetLayout(vk->device, &set, NULL, &pt->set_layout) != VK_SUCCESS)
		goto fail;
	VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
	VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	                                     .setLayoutCount = 1,
	                                     .pSetLayouts = &pt->set_layout,
	                                     .pushConstantRangeCount = 1,
	                                     .pPushConstantRanges = &push};
	if (vk->vkCreatePipelineLayout(vk->device, &layout, NULL, &pt->layout) != VK_SUCCESS)
		goto fail;
	VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
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
	__atomic_store_n(&pt->shm->active, wanted, __ATOMIC_RELEASE);
	__atomic_store_n(&pt->shm->types[SF_PT_TYPE_COLOR].reader, wanted, __ATOMIC_RELEASE);
	return wanted;
}

static void
dma_destroy(struct sf_comp_passthrough *pt, struct pt_dma *dma)
{
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
		if (c->held && pt->pid == pt->shm->pid &&
		    pt->generation == __atomic_load_n(&pt->shm->types[SF_PT_TYPE_COLOR].generation, __ATOMIC_ACQUIRE))
			__atomic_store_n(&c->held->state, SF_PT_FRAME_FREE, __ATOMIC_RELEASE);
		c->held = NULL;
	}
}

static bool
gpu_planes(struct sf_comp_passthrough *pt,
           struct pt_camera *c,
           struct sf_pt_type *t,
           struct sf_pt_frame *f,
           int index,
           VkCommandBuffer cmd)
{
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
		struct dma_buf_sync sync = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
		if (ioctl(dma->fd, DMA_BUF_IOCTL_SYNC, &sync) != 0)
			return false;
		dma->synced = true;
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
	struct sf_pt_type *t = &pt->shm->types[SF_PT_TYPE_COLOR];
	uint32_t generation = __atomic_load_n(&t->generation, __ATOMIC_ACQUIRE);
	if ((generation & 1) || !t->initialized || t->drm_format != 0x3231564e || t->modifier != 0 ||
	    t->plane_count != 2 || !t->width || !t->height || t->width > 4096 || t->height > 4096 || t->width % 2 ||
	    t->height % 2 || !t->frame_count || t->frame_count > SF_PT_MAX_FRAMES)
		return;
	if (pt->pid != pt->shm->pid || pt->generation != generation) {
		for (int c = 0; c < 2; c++) {
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
		U_LOG_I("Passthrough color stream: %ux%u, XRService %d", t->width, t->height, pt->pid);
	}
	if (!pipeline_create(pt, render, d->views[0].squash.gfx.rtr->rgrp->render_pass))
		return;
	uint64_t prepare_start = os_monotonic_get_ns();
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
				bool gpu = gpu_planes(pt, c, t, f, best, render->r->cmd);
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
				if (gpu)
					c->held = f;
				else
					__atomic_store_n(&f->state, SF_PT_FRAME_FREE, __ATOMIC_RELEASE);
			}
		}
		if (!c->textures[2].uploaded || c->map_seq != __atomic_load_n(&source->map_seq, __ATOMIC_ACQUIRE)) {
			memcpy(c->textures[2].staging.data, source->map, mw * mh * 8);
			c->map_seq = __atomic_load_n(&source->map_seq, __ATOMIC_ACQUIRE);
			texture_upload(pt, &c->textures[2], render->r->cmd, VK_NULL_HANDLE, 0, 0);
		}
		VkDescriptorImageInfo images[3];
		VkWriteDescriptorSet writes[3];
		for (uint32_t i = 0; i < 3; i++) {
			images[i] = (VkDescriptorImageInfo){pt->sampler, c->textures[i].view,
			                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			                                   .dstSet = c->set,
			                                   .dstBinding = i,
			                                   .descriptorCount = 1,
			                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			                                   .pImageInfo = &images[i]};
		}
		pt->vk->vkUpdateDescriptorSets(pt->vk->device, 3, writes, 0, NULL);
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
	// Keep the eye assignment verified on the headset: camera 1 left, camera 0 right.
	struct pt_camera *c = &pt->cameras[1 - view];
	if (!c->time || os_monotonic_get_ns() - c->time >= 500000000)
		return;
	float meta[62];
	memcpy(meta, c->metadata, sizeof(meta));
	float push[32] = {0};
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
	for (int i = 0; i < 32; i++)
		if (!isfinite(push[i]))
			return;
	if (push[0] <= 0 || push[1] <= 0)
		return;
	struct vk_bundle *vk = pt->vk;
	vk->vkCmdBindPipeline(render->r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pt->pipeline);
	vk->vkCmdBindDescriptorSets(render->r->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pt->layout, 0, 1, &c->set, 0,
	                            NULL);
	vk->vkCmdPushConstants(render->r->cmd, pt->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
	vk->vkCmdDraw(render->r->cmd, 3, 1, 0, 0);
}

void
sf_comp_passthrough_destroy(struct sf_comp_passthrough **ptr)
{
	struct sf_comp_passthrough *pt = *ptr;
	if (!pt)
		return;
	sf_comp_passthrough_finish(pt);
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
	if (pt->pose_trace) fclose(pt->pose_trace);
	free(pt);
	*ptr = NULL;
}
