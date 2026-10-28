/*
 * GPU copies of dma-bufs EGL can't import into the AHardwareBuffers they are shown from.
 *
 * Android's EGL takes no dma-bufs, so such a buffer (a nested compositor's frame, say) was read
 * through the CPU every frame. Here a Vulkan driver that does take them (RADV on Xclipse, from
 * Termux: $PREFIX/lib/libvulkan.so.1 and the ICD named by VK_ICD_FILENAMES) imports both the
 * client's dma-buf and the AHardwareBuffer's own (linear, see ahb_create) as VkBuffers, and
 * copies the damaged rows with vkCmdCopyBuffer. Anything missing: false, and the CPU copies.
 * TERMUX_WAYLAND_NO_VKCOPY=1 turns it off.
 */
#include "twl.h"

#ifdef __ANDROID__
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <android/hardware_buffer.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

typedef struct {
	int version, numFds, numInts;
	int data[];
} native_handle_t;

/* An imported dma-buf. */
struct vk_mem {
	VkBuffer buf;
	VkDeviceMemory mem;
	VkDeviceSize size;
};

static struct {
	int state; /* 0: not tried, 1: ready, -1: unavailable */
	VkInstance inst;
	VkDevice dev;
	VkQueue queue;
	VkCommandPool pool;
	VkCommandBuffer cmd;
	VkFence fence;
	VkBufferCopy *regions;
	int32_t n_regions;
	const native_handle_t *(*get_native_handle)(const AHardwareBuffer *);
#define F(n) PFN_##n n
	F(vkDestroyInstance);
	F(vkCreateBuffer);
	F(vkDestroyBuffer);
	F(vkGetBufferMemoryRequirements);
	F(vkAllocateMemory);
	F(vkFreeMemory);
	F(vkBindBufferMemory);
	F(vkGetMemoryFdPropertiesKHR);
	F(vkResetCommandBuffer);
	F(vkBeginCommandBuffer);
	F(vkCmdCopyBuffer);
	F(vkEndCommandBuffer);
	F(vkQueueSubmit);
	F(vkWaitForFences);
	F(vkResetFences);
#undef F
} vk;

static bool vk_init(void) {
	const char *off = getenv("TERMUX_WAYLAND_NO_VKCOPY");
	if (off && *off && strcmp(off, "0"))
		return false;
	void *nw = dlopen("libnativewindow.so", RTLD_NOW);
	vk.get_native_handle = nw ? dlsym(nw, "AHardwareBuffer_getNativeHandle") : NULL;
	if (!vk.get_native_handle)
		return false;
	const char *prefix = getenv("PREFIX");
	char path[512];
	snprintf(path, sizeof(path), "%s/lib/libvulkan.so.1", prefix && *prefix ? prefix : "/data/data/com.termux/files/usr");
	void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	PFN_vkGetInstanceProcAddr gipa = lib ? (PFN_vkGetInstanceProcAddr) dlsym(lib, "vkGetInstanceProcAddr") : NULL;
	if (!gipa) {
		twl_info("GPU dma-buf copies: no Vulkan loader (%s)", path);
		return false;
	}
	PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "termux-wayland",
			.apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
	if (!create_instance || create_instance(&ici, NULL, &vk.inst) != VK_SUCCESS) {
		twl_info("GPU dma-buf copies: no Vulkan driver (VK_ICD_FILENAMES)");
		return false;
	}
#define I(n) PFN_##n n = (PFN_##n) gipa(vk.inst, #n)
	I(vkEnumeratePhysicalDevices);
	I(vkGetPhysicalDeviceProperties);
	I(vkEnumerateDeviceExtensionProperties);
	I(vkCreateDevice);
	I(vkGetDeviceProcAddr);
	I(vkGetDeviceQueue);
	I(vkCreateCommandPool);
	I(vkAllocateCommandBuffers);
	I(vkCreateFence);
#undef I
	vk.vkDestroyInstance = (PFN_vkDestroyInstance) gipa(vk.inst, "vkDestroyInstance");

	uint32_t n = 1;
	VkPhysicalDevice pd;
	if (vkEnumeratePhysicalDevices(vk.inst, &n, &pd) < 0 || n < 1)
		goto fail;
	uint32_t n_ext = 0;
	vkEnumerateDeviceExtensionProperties(pd, NULL, &n_ext, NULL);
	VkExtensionProperties *exts = calloc(n_ext ? n_ext : 1, sizeof(*exts));
	int found = 0;
	if (exts && vkEnumerateDeviceExtensionProperties(pd, NULL, &n_ext, exts) == VK_SUCCESS)
		for (uint32_t i = 0; i < n_ext; i++)
			found += !strcmp(exts[i].extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) ||
					!strcmp(exts[i].extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
	free(exts);
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	if (found != 2) {
		twl_info("GPU dma-buf copies: %s can't import dma-bufs", props.deviceName);
		goto fail;
	}
	const char *dev_ext[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
	float prio = 1;
	VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
			.queueCount = 1, .pQueuePriorities = &prio};
	VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
			.pQueueCreateInfos = &qci, .enabledExtensionCount = 2, .ppEnabledExtensionNames = dev_ext};
	if (vkCreateDevice(pd, &dci, NULL, &vk.dev) != VK_SUCCESS)
		goto fail;
#define D(n) vk.n = (PFN_##n) vkGetDeviceProcAddr(vk.dev, #n)
	D(vkCreateBuffer);
	D(vkDestroyBuffer);
	D(vkGetBufferMemoryRequirements);
	D(vkAllocateMemory);
	D(vkFreeMemory);
	D(vkBindBufferMemory);
	D(vkGetMemoryFdPropertiesKHR);
	D(vkResetCommandBuffer);
	D(vkBeginCommandBuffer);
	D(vkCmdCopyBuffer);
	D(vkEndCommandBuffer);
	D(vkQueueSubmit);
	D(vkWaitForFences);
	D(vkResetFences);
#undef D
	if (!vk.vkGetMemoryFdPropertiesKHR)
		goto fail;
	vkGetDeviceQueue(vk.dev, 0, 0, &vk.queue);
	VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
	VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
	VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vkCreateCommandPool(vk.dev, &pci, NULL, &vk.pool) != VK_SUCCESS)
		goto fail;
	cai.commandPool = vk.pool;
	if (vkAllocateCommandBuffers(vk.dev, &cai, &vk.cmd) != VK_SUCCESS ||
			vkCreateFence(vk.dev, &fci, NULL, &vk.fence) != VK_SUCCESS)
		goto fail;
	twl_info("GPU dma-buf copies: %s", props.deviceName);
	return true;
fail:
	/* The device and the rest go with the process; this runs once. */
	twl_info("GPU dma-buf copies: Vulkan setup failed");
	return false;
}

static bool vk_ready(void) {
	if (!vk.state)
		vk.state = vk_init() ? 1 : -1;
	return vk.state > 0;
}

/* Imports fd (not taken) as a VkBuffer, or NULL. */
static struct vk_mem *vk_import(int fd, VkBufferUsageFlags usage) {
	off_t size = lseek(fd, 0, SEEK_END);
	int dup_fd = size > 0 ? dup(fd) : -1;
	struct vk_mem *m = dup_fd >= 0 ? calloc(1, sizeof(*m)) : NULL;
	if (!m)
		goto fail;
	m->size = (VkDeviceSize) size;
	VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
			.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
	VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext, .size = m->size,
			.usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	if (vk.vkCreateBuffer(vk.dev, &bci, NULL, &m->buf) != VK_SUCCESS)
		goto fail;
	VkMemoryRequirements req;
	vk.vkGetBufferMemoryRequirements(vk.dev, m->buf, &req);
	VkMemoryFdPropertiesKHR fdp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
	if (vk.vkGetMemoryFdPropertiesKHR(vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dup_fd, &fdp) !=
			VK_SUCCESS)
		goto fail;
	uint32_t types = req.memoryTypeBits & fdp.memoryTypeBits;
	if (!types || req.size > m->size)
		goto fail;
	VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup_fd};
	VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp,
			.allocationSize = m->size, .memoryTypeIndex = (uint32_t) __builtin_ctz(types)};
	if (vk.vkAllocateMemory(vk.dev, &mai, NULL, &m->mem) != VK_SUCCESS)
		goto fail;
	dup_fd = -1; /* the memory owns it now */
	if (vk.vkBindBufferMemory(vk.dev, m->buf, m->mem, 0) != VK_SUCCESS)
		goto fail;
	return m;
fail:
	if (dup_fd >= 0)
		close(dup_fd);
	twl_vkcopy_free(m);
	return NULL;
}

void twl_vkcopy_free(void *p) {
	struct vk_mem *m = p;
	if (!m)
		return;
	if (m->buf)
		vk.vkDestroyBuffer(vk.dev, m->buf, NULL);
	if (m->mem)
		vk.vkFreeMemory(vk.dev, m->mem, NULL);
	free(m);
}

bool twl_vkcopy(struct twl_buffer *b, void *ahb, void **ahb_mem, const struct twl_box *box) {
	const struct twl_dmabuf_attribs *a = &b->attribs;
	if (b->vk_failed || !vk_ready())
		return false;
	if (!b->vk) {
		b->vk = vk_import(a->fd[0], VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
		if (!b->vk) {
			b->vk_failed = true;
			return false;
		}
	}
	if (!*ahb_mem) {
		const native_handle_t *h = vk.get_native_handle(ahb);
		*ahb_mem = h && h->numFds > 0 ? vk_import(h->data[0], VK_BUFFER_USAGE_TRANSFER_DST_BIT) : NULL;
		if (!*ahb_mem)
			return false;
	}
	struct vk_mem *src = b->vk, *dst = *ahb_mem;
	AHardwareBuffer_Desc desc;
	AHardwareBuffer_describe(ahb, &desc);
	VkDeviceSize dst_stride = (VkDeviceSize) desc.stride * 4, row = (VkDeviceSize) box->w * 4;
	if ((VkDeviceSize) a->offset[0] + (VkDeviceSize) a->stride[0] * a->height > src->size ||
			dst_stride * desc.height > dst->size)
		return false;
	if (box->h > vk.n_regions) {
		VkBufferCopy *r = realloc(vk.regions, sizeof(*r) * (size_t) box->h);
		if (!r)
			return false;
		vk.regions = r;
		vk.n_regions = box->h;
	}
	/* One region per row: the strides differ. */
	for (int32_t i = 0; i < box->h; i++) {
		int32_t y = box->y + i;
		vk.regions[i] = (VkBufferCopy){
			.srcOffset = a->offset[0] + (VkDeviceSize) y * a->stride[0] + (VkDeviceSize) box->x * 4,
			.dstOffset = (VkDeviceSize) y * dst_stride + (VkDeviceSize) box->x * 4,
			.size = row,
		};
	}
	VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	if (vk.vkResetCommandBuffer(vk.cmd, 0) != VK_SUCCESS || vk.vkBeginCommandBuffer(vk.cmd, &begin) != VK_SUCCESS)
		return false;
	vk.vkCmdCopyBuffer(vk.cmd, src->buf, dst->buf, (uint32_t) box->h, vk.regions);
	if (vk.vkEndCommandBuffer(vk.cmd) != VK_SUCCESS)
		return false;
	VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
			.pCommandBuffers = &vk.cmd};
	if (vk.vkQueueSubmit(vk.queue, 1, &submit, vk.fence) != VK_SUCCESS)
		return false;
	/* Done before GL samples it; a copy takes well under a frame. */
	if (vk.vkWaitForFences(vk.dev, 1, &vk.fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) {
		twl_error("GPU dma-buf copy timed out, copying through the CPU from now on");
		vk.state = -1; /* the fence may still be pending: never reuse it */
		return false;
	}
	vk.vkResetFences(vk.dev, 1, &vk.fence);
	return true;
}

#else

void twl_vkcopy_free(void *p) {
}

bool twl_vkcopy(struct twl_buffer *b, void *ahb, void **ahb_mem, const struct twl_box *box) {
	return false;
}

#endif
