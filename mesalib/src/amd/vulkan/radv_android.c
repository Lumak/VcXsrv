/*
 * Copyright © 2017, Google Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>
#include <vulkan/vk_android_native_buffer.h>
#include <vulkan/vk_icd.h>
#include <libsync.h>

#include "radv_private.h"

static int radv_hal_open(const struct hw_module_t* mod, const char* id, struct hw_device_t** dev);
static int radv_hal_close(struct hw_device_t *dev);

static void UNUSED
static_asserts(void)
{
	STATIC_ASSERT(HWVULKAN_DISPATCH_MAGIC == ICD_LOADER_MAGIC);
}

PUBLIC struct hwvulkan_module_t HAL_MODULE_INFO_SYM = {
	.common = {
		.tag = HARDWARE_MODULE_TAG,
		.module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
		.hal_api_version = HARDWARE_MAKE_API_VERSION(1, 0),
		.id = HWVULKAN_HARDWARE_MODULE_ID,
		.name = "AMD Vulkan HAL",
		.author = "Google",
		.methods = &(hw_module_methods_t) {
			.open = radv_hal_open,
		},
	},
};

/* If any bits in test_mask are set, then unset them and return true. */
static inline bool
unmask32(uint32_t *inout_mask, uint32_t test_mask)
{
	uint32_t orig_mask = *inout_mask;
	*inout_mask &= ~test_mask;
	return *inout_mask != orig_mask;
}

static int
radv_hal_open(const struct hw_module_t* mod, const char* id,
             struct hw_device_t** dev)
{
	assert(mod == &HAL_MODULE_INFO_SYM.common);
	assert(strcmp(id, HWVULKAN_DEVICE_0) == 0);

	hwvulkan_device_t *hal_dev = malloc(sizeof(*hal_dev));
	if (!hal_dev)
		return -1;

	*hal_dev = (hwvulkan_device_t) {
		.common = {
			.tag = HARDWARE_DEVICE_TAG,
			.version = HWVULKAN_DEVICE_API_VERSION_0_1,
			.module = &HAL_MODULE_INFO_SYM.common,
			.close = radv_hal_close,
		},
		.EnumerateInstanceExtensionProperties = radv_EnumerateInstanceExtensionProperties,
		.CreateInstance = radv_CreateInstance,
		.GetInstanceProcAddr = radv_GetInstanceProcAddr,
	};

	*dev = &hal_dev->common;
	return 0;
}

static int
radv_hal_close(struct hw_device_t *dev)
{
	/* hwvulkan.h claims that hw_device_t::close() is never called. */
	return -1;
}

VkResult
radv_image_from_gralloc(VkDevice device_h,
                       const VkImageCreateInfo *base_info,
                       const VkNativeBufferANDROID *gralloc_info,
                       const VkAllocationCallbacks *alloc,
                       VkImage *out_image_h)

{
	RADV_FROM_HANDLE(radv_device, device, device_h);
	VkImage image_h = VK_NULL_HANDLE;
	struct radv_image *image = NULL;
	struct radv_bo *bo = NULL;
	VkResult result;

	result = radv_image_create(device_h,
	                           &(struct radv_image_create_info) {
	                               .vk_info = base_info,
	                               .scanout = true,
	                               .no_metadata_planes = true},
	                           alloc,
	                           &image_h);

	if (result != VK_SUCCESS)
		return result;

	if (gralloc_info->handle->numFds != 1) {
		return vk_errorf(device->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR,
		                 "VkNativeBufferANDROID::handle::numFds is %d, "
		                 "expected 1", gralloc_info->handle->numFds);
	}

	/* Do not close the gralloc handle's dma_buf. The lifetime of the dma_buf
	 * must exceed that of the gralloc handle, and we do not own the gralloc
	 * handle.
	 */
	int dma_buf = gralloc_info->handle->data[0];

	image = radv_image_from_handle(image_h);

	VkDeviceMemory memory_h;

	const VkMemoryDedicatedAllocateInfoKHR ded_alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
		.pNext = NULL,
		.buffer = VK_NULL_HANDLE,
		.image = image_h
	};

	const VkImportMemoryFdInfoKHR import_info = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.pNext = &ded_alloc,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
		.fd = dup(dma_buf),
	};
	/* Find the first VRAM memory type, or GART for PRIME images. */
	int memory_type_index = -1;
	for (int i = 0; i < device->physical_device->memory_properties.memoryTypeCount; ++i) {
		bool is_local = !!(device->physical_device->memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (is_local) {
			memory_type_index = i;
			break;
		}
	}

	/* fallback */
	if (memory_type_index == -1)
		memory_type_index = 0;

	result = radv_AllocateMemory(device_h,
				     &(VkMemoryAllocateInfo) {
					     .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
					     .pNext = &import_info,
					     .allocationSize = image->size,
					     .memoryTypeIndex = memory_type_index,
				     },
				     alloc,
				     &memory_h);
	if (result != VK_SUCCESS)
		goto fail_create_image;

	radv_BindImageMemory(device_h, image_h, memory_h, 0);

	image->owned_memory = memory_h;
	/* Don't clobber the out-parameter until success is certain. */
	*out_image_h = image_h;

	return VK_SUCCESS;

fail_create_image:
fail_size:
	radv_DestroyImage(device_h, image_h, alloc);

	return result;
}

VkResult radv_GetSwapchainGrallocUsageANDROID(
    VkDevice            device_h,
    VkFormat            format,
    VkImageUsageFlags   imageUsage,
    int*                grallocUsage)
{
	RADV_FROM_HANDLE(radv_device, device, device_h);
	struct radv_physical_device *phys_dev = device->physical_device;
	VkPhysicalDevice phys_dev_h = radv_physical_device_to_handle(phys_dev);
	VkResult result;

	*grallocUsage = 0;

	/* WARNING: Android Nougat's libvulkan.so hardcodes the VkImageUsageFlags
	 * returned to applications via VkSurfaceCapabilitiesKHR::supportedUsageFlags.
	 * The relevant code in libvulkan/swapchain.cpp contains this fun comment:
	 *
	 *     TODO(jessehall): I think these are right, but haven't thought hard
	 *     about it. Do we need to query the driver for support of any of
	 *     these?
	 *
	 * Any disagreement between this function and the hardcoded
	 * VkSurfaceCapabilitiesKHR:supportedUsageFlags causes tests
	 * dEQP-VK.wsi.android.swapchain.*.image_usage to fail.
	 */

	const VkPhysicalDeviceImageFormatInfo2KHR image_format_info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2_KHR,
		.format = format,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = imageUsage,
	};

	VkImageFormatProperties2KHR image_format_props = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2_KHR,
	};

	/* Check that requested format and usage are supported. */
	result = radv_GetPhysicalDeviceImageFormatProperties2(phys_dev_h,
	                                                      &image_format_info, &image_format_props);
	if (result != VK_SUCCESS) {
		return vk_errorf(device->instance, result,
		                 "radv_GetPhysicalDeviceImageFormatProperties2 failed "
		                 "inside %s", __func__);
	}

	if (unmask32(&imageUsage, VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
		*grallocUsage |= GRALLOC_USAGE_HW_RENDER;

	if (unmask32(&imageUsage, VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                          VK_IMAGE_USAGE_SAMPLED_BIT |
	                          VK_IMAGE_USAGE_STORAGE_BIT |
	                          VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
		*grallocUsage |= GRALLOC_USAGE_HW_TEXTURE;

	/* All VkImageUsageFlags not explicitly checked here are unsupported for
	 * gralloc swapchains.
	 */
	if (imageUsage != 0) {
	return vk_errorf(device->instance, VK_ERROR_FORMAT_NOT_SUPPORTED,
	                "unsupported VkImageUsageFlags(0x%x) for gralloc "
	                "swapchain", imageUsage);
	}

	/*
	* FINISHME: Advertise all display-supported formats. Mostly
	* DRM_FORMAT_ARGB2101010 and DRM_FORMAT_ABGR2101010, but need to check
	* what we need for 30-bit colors.
	*/
	if (format == VK_FORMAT_B8G8R8A8_UNORM ||
	    format == VK_FORMAT_B5G6R5_UNORM_PACK16) {
		*grallocUsage |= GRALLOC_USAGE_HW_FB |
		                 GRALLOC_USAGE_HW_COMPOSER |
		                 GRALLOC_USAGE_EXTERNAL_DISP;
	}

	if (*grallocUsage == 0)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	return VK_SUCCESS;
}

VkResult
radv_AcquireImageANDROID(
      VkDevice            device,
      VkImage             image_h,
      int                 nativeFenceFd,
      VkSemaphore         semaphore,
      VkFence             fence)
{
	VkResult semaphore_result = VK_SUCCESS, fence_result = VK_SUCCESS;

	if (semaphore != VK_NULL_HANDLE) {
		int semaphore_fd = nativeFenceFd >= 0 ? dup(nativeFenceFd) : nativeFenceFd;
		semaphore_result = radv_ImportSemaphoreFdKHR(device,
		                                             &(VkImportSemaphoreFdInfoKHR) {
		                                                 .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
		                                                 .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT_KHR,
		                                                 .fd = semaphore_fd,
		                                                 .semaphore = semaphore,
		                                            });
	}

	if (fence != VK_NULL_HANDLE) {
		int fence_fd = nativeFenceFd >= 0 ? dup(nativeFenceFd) : nativeFenceFd;
		fence_result = radv_ImportFenceFdKHR(device,
		                                     &(VkImportFenceFdInfoKHR) {
		                                         .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
		                                         .flags = VK_FENCE_IMPORT_TEMPORARY_BIT_KHR,
		                                         .fd = fence_fd,
		                                         .fence = fence,
		                                     });
	}

	close(nativeFenceFd);

	if (semaphore_result != VK_SUCCESS)
		return semaphore_result;
	return fence_result;
}

VkResult
radv_QueueSignalReleaseImageANDROID(
      VkQueue             _queue,
      uint32_t            waitSemaphoreCount,
      const VkSemaphore*  pWaitSemaphores,
      VkImage             image,
      int*                pNativeFenceFd)
{
	RADV_FROM_HANDLE(radv_queue, queue, _queue);
	VkResult result = VK_SUCCESS;

	if (waitSemaphoreCount == 0) {
		if (pNativeFenceFd)
			*pNativeFenceFd = -1;
		return VK_SUCCESS;
	}

	int fd = -1;

	for (uint32_t i = 0; i < waitSemaphoreCount; ++i) {
		int tmp_fd;
		result = radv_GetSemaphoreFdKHR(radv_device_to_handle(queue->device),
		                                &(VkSemaphoreGetFdInfoKHR) {
		                                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
		                                    .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR,
		                                    .semaphore = pWaitSemaphores[i],
		                            }, &tmp_fd);
		if (result != VK_SUCCESS) {
			if (fd >= 0)
				close (fd);
			return result;
		}

		if (fd < 0)
			fd = tmp_fd;
		else if (tmp_fd >= 0) {
			sync_accumulate("radv", &fd, tmp_fd);
			close(tmp_fd);
		}
	}

	if (pNativeFenceFd) {
		*pNativeFenceFd = fd;
	} else if (fd >= 0) {
		close(fd);
		/* We still need to do the exports, to reset the semaphores, but
		 * otherwise we don't wait on them. */
	}
	return VK_SUCCESS;
}
