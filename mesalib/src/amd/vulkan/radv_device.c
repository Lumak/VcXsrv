/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
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

#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "radv_debug.h"
#include "radv_private.h"
#include "radv_shader.h"
#include "radv_cs.h"
#include "util/disk_cache.h"
#include "util/strtod.h"
#include "vk_util.h"
#include <xf86drm.h>
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include "winsys/amdgpu/radv_amdgpu_winsys_public.h"
#include "ac_llvm_util.h"
#include "vk_format.h"
#include "sid.h"
#include "git_sha1.h"
#include "gfx9d.h"
#include "addrlib/gfx9/chip/gfx9_enum.h"
#include "util/build_id.h"
#include "util/debug.h"
#include "util/mesa-sha1.h"

static int
radv_device_get_cache_uuid(enum radeon_family family, void *uuid)
{
	struct mesa_sha1 ctx;
	unsigned char sha1[20];
	unsigned ptr_size = sizeof(void*);

	memset(uuid, 0, VK_UUID_SIZE);
	_mesa_sha1_init(&ctx);

	if (!disk_cache_get_function_identifier(radv_device_get_cache_uuid, &ctx) ||
	    !disk_cache_get_function_identifier(LLVMInitializeAMDGPUTargetInfo, &ctx))
		return -1;

	_mesa_sha1_update(&ctx, &family, sizeof(family));
	_mesa_sha1_update(&ctx, &ptr_size, sizeof(ptr_size));
	_mesa_sha1_final(&ctx, sha1);

	memcpy(uuid, sha1, VK_UUID_SIZE);
	return 0;
}

static void
radv_get_driver_uuid(void *uuid)
{
	ac_compute_driver_uuid(uuid, VK_UUID_SIZE);
}

static void
radv_get_device_uuid(struct radeon_info *info, void *uuid)
{
	ac_compute_device_uuid(info, uuid, VK_UUID_SIZE);
}

static void
radv_get_device_name(enum radeon_family family, char *name, size_t name_len)
{
	const char *chip_string;
	char llvm_string[32] = {};

	switch (family) {
	case CHIP_TAHITI: chip_string = "AMD RADV TAHITI"; break;
	case CHIP_PITCAIRN: chip_string = "AMD RADV PITCAIRN"; break;
	case CHIP_VERDE: chip_string = "AMD RADV CAPE VERDE"; break;
	case CHIP_OLAND: chip_string = "AMD RADV OLAND"; break;
	case CHIP_HAINAN: chip_string = "AMD RADV HAINAN"; break;
	case CHIP_BONAIRE: chip_string = "AMD RADV BONAIRE"; break;
	case CHIP_KAVERI: chip_string = "AMD RADV KAVERI"; break;
	case CHIP_KABINI: chip_string = "AMD RADV KABINI"; break;
	case CHIP_HAWAII: chip_string = "AMD RADV HAWAII"; break;
	case CHIP_MULLINS: chip_string = "AMD RADV MULLINS"; break;
	case CHIP_TONGA: chip_string = "AMD RADV TONGA"; break;
	case CHIP_ICELAND: chip_string = "AMD RADV ICELAND"; break;
	case CHIP_CARRIZO: chip_string = "AMD RADV CARRIZO"; break;
	case CHIP_FIJI: chip_string = "AMD RADV FIJI"; break;
	case CHIP_POLARIS10: chip_string = "AMD RADV POLARIS10"; break;
	case CHIP_POLARIS11: chip_string = "AMD RADV POLARIS11"; break;
	case CHIP_POLARIS12: chip_string = "AMD RADV POLARIS12"; break;
	case CHIP_STONEY: chip_string = "AMD RADV STONEY"; break;
	case CHIP_VEGAM: chip_string = "AMD RADV VEGA M"; break;
	case CHIP_VEGA10: chip_string = "AMD RADV VEGA10"; break;
	case CHIP_VEGA12: chip_string = "AMD RADV VEGA12"; break;
	case CHIP_RAVEN: chip_string = "AMD RADV RAVEN"; break;
	case CHIP_RAVEN2: chip_string = "AMD RADV RAVEN2"; break;
	default: chip_string = "AMD RADV unknown"; break;
	}

	snprintf(llvm_string, sizeof(llvm_string),
		 " (LLVM %i.%i.%i)", (HAVE_LLVM >> 8) & 0xff,
		 HAVE_LLVM & 0xff, MESA_LLVM_VERSION_PATCH);
	snprintf(name, name_len, "%s%s", chip_string, llvm_string);
}

static void
radv_physical_device_init_mem_types(struct radv_physical_device *device)
{
	STATIC_ASSERT(RADV_MEM_HEAP_COUNT <= VK_MAX_MEMORY_HEAPS);
	uint64_t visible_vram_size = MIN2(device->rad_info.vram_size,
	                                  device->rad_info.vram_vis_size);

	int vram_index = -1, visible_vram_index = -1, gart_index = -1;
	device->memory_properties.memoryHeapCount = 0;
	if (device->rad_info.vram_size - visible_vram_size > 0) {
		vram_index = device->memory_properties.memoryHeapCount++;
		device->memory_properties.memoryHeaps[vram_index] = (VkMemoryHeap) {
			.size = device->rad_info.vram_size - visible_vram_size,
			.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
		};
	}
	if (visible_vram_size) {
		visible_vram_index = device->memory_properties.memoryHeapCount++;
		device->memory_properties.memoryHeaps[visible_vram_index] = (VkMemoryHeap) {
			.size = visible_vram_size,
			.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
		};
	}
	if (device->rad_info.gart_size > 0) {
		gart_index = device->memory_properties.memoryHeapCount++;
		device->memory_properties.memoryHeaps[gart_index] = (VkMemoryHeap) {
			.size = device->rad_info.gart_size,
			.flags = device->rad_info.has_dedicated_vram ? 0 : VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
		};
	}

	STATIC_ASSERT(RADV_MEM_TYPE_COUNT <= VK_MAX_MEMORY_TYPES);
	unsigned type_count = 0;
	if (vram_index >= 0) {
		device->mem_type_indices[type_count] = RADV_MEM_TYPE_VRAM;
		device->memory_properties.memoryTypes[type_count++] = (VkMemoryType) {
			.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			.heapIndex = vram_index,
		};
	}
	if (gart_index >= 0) {
		device->mem_type_indices[type_count] = RADV_MEM_TYPE_GTT_WRITE_COMBINE;
		device->memory_properties.memoryTypes[type_count++] = (VkMemoryType) {
			.propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
			(device->rad_info.has_dedicated_vram ? 0 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
			.heapIndex = gart_index,
		};
	}
	if (visible_vram_index >= 0) {
		device->mem_type_indices[type_count] = RADV_MEM_TYPE_VRAM_CPU_ACCESS;
		device->memory_properties.memoryTypes[type_count++] = (VkMemoryType) {
			.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			.heapIndex = visible_vram_index,
		};
	}
	if (gart_index >= 0) {
		device->mem_type_indices[type_count] = RADV_MEM_TYPE_GTT_CACHED;
		device->memory_properties.memoryTypes[type_count++] = (VkMemoryType) {
			.propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
			VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
			(device->rad_info.has_dedicated_vram ? 0 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
			.heapIndex = gart_index,
		};
	}
	device->memory_properties.memoryTypeCount = type_count;
}

static void
radv_handle_env_var_force_family(struct radv_physical_device *device)
{
	const char *family = getenv("RADV_FORCE_FAMILY");
	unsigned i;

	if (!family)
		return;

	for (i = CHIP_TAHITI; i < CHIP_LAST; i++) {
		if (!strcmp(family, ac_get_llvm_processor_name(i))) {
			/* Override family and chip_class. */
			device->rad_info.family = i;

			if (i >= CHIP_VEGA10)
				device->rad_info.chip_class = GFX9;
			else if (i >= CHIP_TONGA)
				device->rad_info.chip_class = VI;
			else if (i >= CHIP_BONAIRE)
				device->rad_info.chip_class = CIK;
			else
				device->rad_info.chip_class = SI;

			return;
		}
	}

	fprintf(stderr, "radv: Unknown family: %s\n", family);
	exit(1);
}

static VkResult
radv_physical_device_init(struct radv_physical_device *device,
			  struct radv_instance *instance,
			  drmDevicePtr drm_device)
{
	const char *path = drm_device->nodes[DRM_NODE_RENDER];
	VkResult result;
	drmVersionPtr version;
	int fd;
	int master_fd = -1;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		if (instance->debug_flags & RADV_DEBUG_STARTUP)
			radv_logi("Could not open device '%s'", path);

		return vk_error(instance, VK_ERROR_INCOMPATIBLE_DRIVER);
	}

	version = drmGetVersion(fd);
	if (!version) {
		close(fd);

		if (instance->debug_flags & RADV_DEBUG_STARTUP)
			radv_logi("Could not get the kernel driver version for device '%s'", path);

		return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
				 "failed to get version %s: %m", path);
	}

	if (strcmp(version->name, "amdgpu")) {
		drmFreeVersion(version);
		close(fd);

		if (instance->debug_flags & RADV_DEBUG_STARTUP)
			radv_logi("Device '%s' is not using the amdgpu kernel driver.", path);

		return VK_ERROR_INCOMPATIBLE_DRIVER;
	}
	drmFreeVersion(version);

	if (instance->debug_flags & RADV_DEBUG_STARTUP)
			radv_logi("Found compatible device '%s'.", path);

	device->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
	device->instance = instance;
	assert(strlen(path) < ARRAY_SIZE(device->path));
	strncpy(device->path, path, ARRAY_SIZE(device->path));

	device->ws = radv_amdgpu_winsys_create(fd, instance->debug_flags,
					       instance->perftest_flags);
	if (!device->ws) {
		result = vk_error(instance, VK_ERROR_INCOMPATIBLE_DRIVER);
		goto fail;
	}

	if (instance->enabled_extensions.KHR_display) {
		master_fd = open(drm_device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
		if (master_fd >= 0) {
			uint32_t accel_working = 0;
			struct drm_amdgpu_info request = {
				.return_pointer = (uintptr_t)&accel_working,
				.return_size = sizeof(accel_working),
				.query = AMDGPU_INFO_ACCEL_WORKING
			};

			if (drmCommandWrite(master_fd, DRM_AMDGPU_INFO, &request, sizeof (struct drm_amdgpu_info)) < 0 || !accel_working) {
				close(master_fd);
				master_fd = -1;
			}
		}
	}

	device->master_fd = master_fd;
	device->local_fd = fd;
	device->ws->query_info(device->ws, &device->rad_info);

	radv_handle_env_var_force_family(device);

	radv_get_device_name(device->rad_info.family, device->name, sizeof(device->name));

	if (radv_device_get_cache_uuid(device->rad_info.family, device->cache_uuid)) {
		device->ws->destroy(device->ws);
		result = vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
				   "cannot generate UUID");
		goto fail;
	}

	/* These flags affect shader compilation. */
	uint64_t shader_env_flags =
		(device->instance->perftest_flags & RADV_PERFTEST_SISCHED ? 0x1 : 0) |
		(device->instance->debug_flags & RADV_DEBUG_UNSAFE_MATH ? 0x2 : 0);

	/* The gpu id is already embedded in the uuid so we just pass "radv"
	 * when creating the cache.
	 */
	char buf[VK_UUID_SIZE * 2 + 1];
	disk_cache_format_hex_id(buf, device->cache_uuid, VK_UUID_SIZE * 2);
	device->disk_cache = disk_cache_create(device->name, buf, shader_env_flags);

	if (device->rad_info.chip_class < VI ||
	    device->rad_info.chip_class > GFX9)
		fprintf(stderr, "WARNING: radv is not a conformant vulkan implementation, testing use only.\n");

	radv_get_driver_uuid(&device->device_uuid);
	radv_get_device_uuid(&device->rad_info, &device->device_uuid);

	if (device->rad_info.family == CHIP_STONEY ||
	    device->rad_info.chip_class >= GFX9) {
		device->has_rbplus = true;
		device->rbplus_allowed = device->rad_info.family == CHIP_STONEY ||
					 device->rad_info.family == CHIP_VEGA12 ||
		                         device->rad_info.family == CHIP_RAVEN ||
		                         device->rad_info.family == CHIP_RAVEN2;
	}

	/* The mere presence of CLEAR_STATE in the IB causes random GPU hangs
	 * on SI.
	 */
	device->has_clear_state = device->rad_info.chip_class >= CIK;

	device->cpdma_prefetch_writes_memory = device->rad_info.chip_class <= VI;

	/* Vega10/Raven need a special workaround for a hardware bug. */
	device->has_scissor_bug = device->rad_info.family == CHIP_VEGA10 ||
				  device->rad_info.family == CHIP_RAVEN;

	/* Out-of-order primitive rasterization. */
	device->has_out_of_order_rast = device->rad_info.chip_class >= VI &&
					device->rad_info.max_se >= 2;
	device->out_of_order_rast_allowed = device->has_out_of_order_rast &&
					    !(device->instance->debug_flags & RADV_DEBUG_NO_OUT_OF_ORDER);

	device->dcc_msaa_allowed =
		(device->instance->perftest_flags & RADV_PERFTEST_DCC_MSAA);

	radv_physical_device_init_mem_types(device);
	radv_fill_device_extension_table(device, &device->supported_extensions);

	device->bus_info = *drm_device->businfo.pci;

	if ((device->instance->debug_flags & RADV_DEBUG_INFO))
		ac_print_gpu_info(&device->rad_info);

	/* The WSI is structured as a layer on top of the driver, so this has
	 * to be the last part of initialization (at least until we get other
	 * semi-layers).
	 */
	result = radv_init_wsi(device);
	if (result != VK_SUCCESS) {
		device->ws->destroy(device->ws);
		vk_error(instance, result);
		goto fail;
	}

	return VK_SUCCESS;

fail:
	close(fd);
	if (master_fd != -1)
		close(master_fd);
	return result;
}

static void
radv_physical_device_finish(struct radv_physical_device *device)
{
	radv_finish_wsi(device);
	device->ws->destroy(device->ws);
	disk_cache_destroy(device->disk_cache);
	close(device->local_fd);
	if (device->master_fd != -1)
		close(device->master_fd);
}

static void *
default_alloc_func(void *pUserData, size_t size, size_t align,
                   VkSystemAllocationScope allocationScope)
{
	return malloc(size);
}

static void *
default_realloc_func(void *pUserData, void *pOriginal, size_t size,
                     size_t align, VkSystemAllocationScope allocationScope)
{
	return realloc(pOriginal, size);
}

static void
default_free_func(void *pUserData, void *pMemory)
{
	free(pMemory);
}

static const VkAllocationCallbacks default_alloc = {
	.pUserData = NULL,
	.pfnAllocation = default_alloc_func,
	.pfnReallocation = default_realloc_func,
	.pfnFree = default_free_func,
};

static const struct debug_control radv_debug_options[] = {
	{"nofastclears", RADV_DEBUG_NO_FAST_CLEARS},
	{"nodcc", RADV_DEBUG_NO_DCC},
	{"shaders", RADV_DEBUG_DUMP_SHADERS},
	{"nocache", RADV_DEBUG_NO_CACHE},
	{"shaderstats", RADV_DEBUG_DUMP_SHADER_STATS},
	{"nohiz", RADV_DEBUG_NO_HIZ},
	{"nocompute", RADV_DEBUG_NO_COMPUTE_QUEUE},
	{"unsafemath", RADV_DEBUG_UNSAFE_MATH},
	{"allbos", RADV_DEBUG_ALL_BOS},
	{"noibs", RADV_DEBUG_NO_IBS},
	{"spirv", RADV_DEBUG_DUMP_SPIRV},
	{"vmfaults", RADV_DEBUG_VM_FAULTS},
	{"zerovram", RADV_DEBUG_ZERO_VRAM},
	{"syncshaders", RADV_DEBUG_SYNC_SHADERS},
	{"nosisched", RADV_DEBUG_NO_SISCHED},
	{"preoptir", RADV_DEBUG_PREOPTIR},
	{"nodynamicbounds", RADV_DEBUG_NO_DYNAMIC_BOUNDS},
	{"nooutoforder", RADV_DEBUG_NO_OUT_OF_ORDER},
	{"info", RADV_DEBUG_INFO},
	{"errors", RADV_DEBUG_ERRORS},
	{"startup", RADV_DEBUG_STARTUP},
	{"checkir", RADV_DEBUG_CHECKIR},
	{"nothreadllvm", RADV_DEBUG_NOTHREADLLVM},
	{"nobinning", RADV_DEBUG_NOBINNING},
	{NULL, 0}
};

const char *
radv_get_debug_option_name(int id)
{
	assert(id < ARRAY_SIZE(radv_debug_options) - 1);
	return radv_debug_options[id].string;
}

static const struct debug_control radv_perftest_options[] = {
	{"nobatchchain", RADV_PERFTEST_NO_BATCHCHAIN},
	{"sisched", RADV_PERFTEST_SISCHED},
	{"localbos", RADV_PERFTEST_LOCAL_BOS},
	{"dccmsaa", RADV_PERFTEST_DCC_MSAA},
	{NULL, 0}
};

const char *
radv_get_perftest_option_name(int id)
{
	assert(id < ARRAY_SIZE(radv_perftest_options) - 1);
	return radv_perftest_options[id].string;
}

static void
radv_handle_per_app_options(struct radv_instance *instance,
			    const VkApplicationInfo *info)
{
	const char *name = info ? info->pApplicationName : NULL;

	if (!name)
		return;

	if (!strcmp(name, "Talos - Linux - 32bit") ||
	    !strcmp(name, "Talos - Linux - 64bit")) {
		if (!(instance->debug_flags & RADV_DEBUG_NO_SISCHED)) {
			/* Force enable LLVM sisched for Talos because it looks
			 * safe and it gives few more FPS.
			 */
			instance->perftest_flags |= RADV_PERFTEST_SISCHED;
		}
	} else if (!strcmp(name, "DOOM_VFR")) {
		/* Work around a Doom VFR game bug */
		instance->debug_flags |= RADV_DEBUG_NO_DYNAMIC_BOUNDS;
	}
}

static int radv_get_instance_extension_index(const char *name)
{
	for (unsigned i = 0; i < RADV_INSTANCE_EXTENSION_COUNT; ++i) {
		if (strcmp(name, radv_instance_extensions[i].extensionName) == 0)
			return i;
	}
	return -1;
}


VkResult radv_CreateInstance(
	const VkInstanceCreateInfo*                 pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkInstance*                                 pInstance)
{
	struct radv_instance *instance;
	VkResult result;

	assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

	uint32_t client_version;
	if (pCreateInfo->pApplicationInfo &&
	    pCreateInfo->pApplicationInfo->apiVersion != 0) {
		client_version = pCreateInfo->pApplicationInfo->apiVersion;
	} else {
		radv_EnumerateInstanceVersion(&client_version);
	}

	instance = vk_zalloc2(&default_alloc, pAllocator, sizeof(*instance), 8,
			      VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
	if (!instance)
		return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

	instance->_loader_data.loaderMagic = ICD_LOADER_MAGIC;

	if (pAllocator)
		instance->alloc = *pAllocator;
	else
		instance->alloc = default_alloc;

	instance->apiVersion = client_version;
	instance->physicalDeviceCount = -1;

	instance->debug_flags = parse_debug_string(getenv("RADV_DEBUG"),
						   radv_debug_options);

	instance->perftest_flags = parse_debug_string(getenv("RADV_PERFTEST"),
						   radv_perftest_options);


	if (instance->debug_flags & RADV_DEBUG_STARTUP)
		radv_logi("Created an instance");

	for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
		const char *ext_name = pCreateInfo->ppEnabledExtensionNames[i];
		int index = radv_get_instance_extension_index(ext_name);

		if (index < 0 || !radv_supported_instance_extensions.extensions[index]) {
			vk_free2(&default_alloc, pAllocator, instance);
			return vk_error(instance, VK_ERROR_EXTENSION_NOT_PRESENT);
		}

		instance->enabled_extensions.extensions[index] = true;
	}

	result = vk_debug_report_instance_init(&instance->debug_report_callbacks);
	if (result != VK_SUCCESS) {
		vk_free2(&default_alloc, pAllocator, instance);
		return vk_error(instance, result);
	}

	_mesa_locale_init();

	VG(VALGRIND_CREATE_MEMPOOL(instance, 0, false));

	radv_handle_per_app_options(instance, pCreateInfo->pApplicationInfo);

	*pInstance = radv_instance_to_handle(instance);

	return VK_SUCCESS;
}

void radv_DestroyInstance(
	VkInstance                                  _instance,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_instance, instance, _instance);

	if (!instance)
		return;

	for (int i = 0; i < instance->physicalDeviceCount; ++i) {
		radv_physical_device_finish(instance->physicalDevices + i);
	}

	VG(VALGRIND_DESTROY_MEMPOOL(instance));

	_mesa_locale_fini();

	vk_debug_report_instance_destroy(&instance->debug_report_callbacks);

	vk_free(&instance->alloc, instance);
}

static VkResult
radv_enumerate_devices(struct radv_instance *instance)
{
	/* TODO: Check for more devices ? */
	drmDevicePtr devices[8];
	VkResult result = VK_ERROR_INCOMPATIBLE_DRIVER;
	int max_devices;

	instance->physicalDeviceCount = 0;

	max_devices = drmGetDevices2(0, devices, ARRAY_SIZE(devices));

	if (instance->debug_flags & RADV_DEBUG_STARTUP)
		radv_logi("Found %d drm nodes", max_devices);

	if (max_devices < 1)
		return vk_error(instance, VK_ERROR_INCOMPATIBLE_DRIVER);

	for (unsigned i = 0; i < (unsigned)max_devices; i++) {
		if (devices[i]->available_nodes & 1 << DRM_NODE_RENDER &&
		    devices[i]->bustype == DRM_BUS_PCI &&
		    devices[i]->deviceinfo.pci->vendor_id == ATI_VENDOR_ID) {

			result = radv_physical_device_init(instance->physicalDevices +
			                                   instance->physicalDeviceCount,
			                                   instance,
			                                   devices[i]);
			if (result == VK_SUCCESS)
				++instance->physicalDeviceCount;
			else if (result != VK_ERROR_INCOMPATIBLE_DRIVER)
				break;
		}
	}
	drmFreeDevices(devices, max_devices);

	return result;
}

VkResult radv_EnumeratePhysicalDevices(
	VkInstance                                  _instance,
	uint32_t*                                   pPhysicalDeviceCount,
	VkPhysicalDevice*                           pPhysicalDevices)
{
	RADV_FROM_HANDLE(radv_instance, instance, _instance);
	VkResult result;

	if (instance->physicalDeviceCount < 0) {
		result = radv_enumerate_devices(instance);
		if (result != VK_SUCCESS &&
		    result != VK_ERROR_INCOMPATIBLE_DRIVER)
			return result;
	}

	if (!pPhysicalDevices) {
		*pPhysicalDeviceCount = instance->physicalDeviceCount;
	} else {
		*pPhysicalDeviceCount = MIN2(*pPhysicalDeviceCount, instance->physicalDeviceCount);
		for (unsigned i = 0; i < *pPhysicalDeviceCount; ++i)
			pPhysicalDevices[i] = radv_physical_device_to_handle(instance->physicalDevices + i);
	}

	return *pPhysicalDeviceCount < instance->physicalDeviceCount ? VK_INCOMPLETE
	                                                             : VK_SUCCESS;
}

VkResult radv_EnumeratePhysicalDeviceGroups(
    VkInstance                                  _instance,
    uint32_t*                                   pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties*            pPhysicalDeviceGroupProperties)
{
	RADV_FROM_HANDLE(radv_instance, instance, _instance);
	VkResult result;

	if (instance->physicalDeviceCount < 0) {
		result = radv_enumerate_devices(instance);
		if (result != VK_SUCCESS &&
		    result != VK_ERROR_INCOMPATIBLE_DRIVER)
			return result;
	}

	if (!pPhysicalDeviceGroupProperties) {
		*pPhysicalDeviceGroupCount = instance->physicalDeviceCount;
	} else {
		*pPhysicalDeviceGroupCount = MIN2(*pPhysicalDeviceGroupCount, instance->physicalDeviceCount);
		for (unsigned i = 0; i < *pPhysicalDeviceGroupCount; ++i) {
			pPhysicalDeviceGroupProperties[i].physicalDeviceCount = 1;
			pPhysicalDeviceGroupProperties[i].physicalDevices[0] = radv_physical_device_to_handle(instance->physicalDevices + i);
			pPhysicalDeviceGroupProperties[i].subsetAllocation = false;
		}
	}
	return *pPhysicalDeviceGroupCount < instance->physicalDeviceCount ? VK_INCOMPLETE
	                                                                  : VK_SUCCESS;
}

void radv_GetPhysicalDeviceFeatures(
	VkPhysicalDevice                            physicalDevice,
	VkPhysicalDeviceFeatures*                   pFeatures)
{
	RADV_FROM_HANDLE(radv_physical_device, pdevice, physicalDevice);
	memset(pFeatures, 0, sizeof(*pFeatures));

	*pFeatures = (VkPhysicalDeviceFeatures) {
		.robustBufferAccess                       = true,
		.fullDrawIndexUint32                      = true,
		.imageCubeArray                           = true,
		.independentBlend                         = true,
		.geometryShader                           = true,
		.tessellationShader                       = true,
		.sampleRateShading                        = true,
		.dualSrcBlend                             = true,
		.logicOp                                  = true,
		.multiDrawIndirect                        = true,
		.drawIndirectFirstInstance                = true,
		.depthClamp                               = true,
		.depthBiasClamp                           = true,
		.fillModeNonSolid                         = true,
		.depthBounds                              = true,
		.wideLines                                = true,
		.largePoints                              = true,
		.alphaToOne                               = true,
		.multiViewport                            = true,
		.samplerAnisotropy                        = true,
		.textureCompressionETC2                   = pdevice->rad_info.chip_class >= GFX9 ||
		                                            pdevice->rad_info.family == CHIP_STONEY,
		.textureCompressionASTC_LDR               = false,
		.textureCompressionBC                     = true,
		.occlusionQueryPrecise                    = true,
		.pipelineStatisticsQuery                  = true,
		.vertexPipelineStoresAndAtomics           = true,
		.fragmentStoresAndAtomics                 = true,
		.shaderTessellationAndGeometryPointSize   = true,
		.shaderImageGatherExtended                = true,
		.shaderStorageImageExtendedFormats        = true,
		.shaderStorageImageMultisample            = false,
		.shaderUniformBufferArrayDynamicIndexing  = true,
		.shaderSampledImageArrayDynamicIndexing   = true,
		.shaderStorageBufferArrayDynamicIndexing  = true,
		.shaderStorageImageArrayDynamicIndexing   = true,
		.shaderStorageImageReadWithoutFormat      = true,
		.shaderStorageImageWriteWithoutFormat     = true,
		.shaderClipDistance                       = true,
		.shaderCullDistance                       = true,
		.shaderFloat64                            = true,
		.shaderInt64                              = true,
		.shaderInt16                              = pdevice->rad_info.chip_class >= GFX9 && HAVE_LLVM >= 0x700,
		.sparseBinding                            = true,
		.variableMultisampleRate                  = true,
		.inheritedQueries                         = true,
	};
}

void radv_GetPhysicalDeviceFeatures2(
	VkPhysicalDevice                            physicalDevice,
	VkPhysicalDeviceFeatures2KHR               *pFeatures)
{
	RADV_FROM_HANDLE(radv_physical_device, pdevice, physicalDevice);
	vk_foreach_struct(ext, pFeatures->pNext) {
		switch (ext->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTER_FEATURES_KHR: {
			VkPhysicalDeviceVariablePointerFeaturesKHR *features = (void *)ext;
			features->variablePointersStorageBuffer = true;
			features->variablePointers = false;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES_KHR: {
			VkPhysicalDeviceMultiviewFeaturesKHR *features = (VkPhysicalDeviceMultiviewFeaturesKHR*)ext;
			features->multiview = true;
			features->multiviewGeometryShader = true;
			features->multiviewTessellationShader = true;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETER_FEATURES: {
			VkPhysicalDeviceShaderDrawParameterFeatures *features =
			    (VkPhysicalDeviceShaderDrawParameterFeatures*)ext;
			features->shaderDrawParameters = true;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES: {
			VkPhysicalDeviceProtectedMemoryFeatures *features =
			    (VkPhysicalDeviceProtectedMemoryFeatures*)ext;
			features->protectedMemory = false;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
			VkPhysicalDevice16BitStorageFeatures *features =
			    (VkPhysicalDevice16BitStorageFeatures*)ext;
			bool enabled = HAVE_LLVM >= 0x0700 && pdevice->rad_info.chip_class >= VI;
			features->storageBuffer16BitAccess = enabled;
			features->uniformAndStorageBuffer16BitAccess = enabled;
			features->storagePushConstant16 = enabled;
			features->storageInputOutput16 = enabled;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: {
			VkPhysicalDeviceSamplerYcbcrConversionFeatures *features =
			    (VkPhysicalDeviceSamplerYcbcrConversionFeatures*)ext;
			features->samplerYcbcrConversion = false;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT: {
			VkPhysicalDeviceDescriptorIndexingFeaturesEXT *features =
				(VkPhysicalDeviceDescriptorIndexingFeaturesEXT*)ext;
			features->shaderInputAttachmentArrayDynamicIndexing = true;
			features->shaderUniformTexelBufferArrayDynamicIndexing = true;
			features->shaderStorageTexelBufferArrayDynamicIndexing = true;
			features->shaderUniformBufferArrayNonUniformIndexing = false;
			features->shaderSampledImageArrayNonUniformIndexing = false;
			features->shaderStorageBufferArrayNonUniformIndexing = false;
			features->shaderStorageImageArrayNonUniformIndexing = false;
			features->shaderInputAttachmentArrayNonUniformIndexing = false;
			features->shaderUniformTexelBufferArrayNonUniformIndexing = false;
			features->shaderStorageTexelBufferArrayNonUniformIndexing = false;
			features->descriptorBindingUniformBufferUpdateAfterBind = true;
			features->descriptorBindingSampledImageUpdateAfterBind = true;
			features->descriptorBindingStorageImageUpdateAfterBind = true;
			features->descriptorBindingStorageBufferUpdateAfterBind = true;
			features->descriptorBindingUniformTexelBufferUpdateAfterBind = true;
			features->descriptorBindingStorageTexelBufferUpdateAfterBind = true;
			features->descriptorBindingUpdateUnusedWhilePending = true;
			features->descriptorBindingPartiallyBound = true;
			features->descriptorBindingVariableDescriptorCount = true;
			features->runtimeDescriptorArray = true;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT: {
			VkPhysicalDeviceConditionalRenderingFeaturesEXT *features =
				(VkPhysicalDeviceConditionalRenderingFeaturesEXT*)ext;
			features->conditionalRendering = true;
			features->inheritedConditionalRendering = false;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT: {
			VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *features =
				(VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *)ext;
			features->vertexAttributeInstanceRateDivisor = VK_TRUE;
			features->vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
			VkPhysicalDeviceTransformFeedbackFeaturesEXT *features =
				(VkPhysicalDeviceTransformFeedbackFeaturesEXT*)ext;
			features->transformFeedback = true;
			features->geometryStreams = true;
			break;
		}
		default:
			break;
		}
	}
	return radv_GetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);
}

void radv_GetPhysicalDeviceProperties(
	VkPhysicalDevice                            physicalDevice,
	VkPhysicalDeviceProperties*                 pProperties)
{
	RADV_FROM_HANDLE(radv_physical_device, pdevice, physicalDevice);
	VkSampleCountFlags sample_counts = 0xf;

	/* make sure that the entire descriptor set is addressable with a signed
	 * 32-bit int. So the sum of all limits scaled by descriptor size has to
	 * be at most 2 GiB. the combined image & samples object count as one of
	 * both. This limit is for the pipeline layout, not for the set layout, but
	 * there is no set limit, so we just set a pipeline limit. I don't think
	 * any app is going to hit this soon. */
	size_t max_descriptor_set_size = ((1ull << 31) - 16 * MAX_DYNAMIC_BUFFERS) /
	          (32 /* uniform buffer, 32 due to potential space wasted on alignment */ +
	           32 /* storage buffer, 32 due to potential space wasted on alignment */ +
	           32 /* sampler, largest when combined with image */ +
	           64 /* sampled image */ +
	           64 /* storage image */);

	VkPhysicalDeviceLimits limits = {
		.maxImageDimension1D                      = (1 << 14),
		.maxImageDimension2D                      = (1 << 14),
		.maxImageDimension3D                      = (1 << 11),
		.maxImageDimensionCube                    = (1 << 14),
		.maxImageArrayLayers                      = (1 << 11),
		.maxTexelBufferElements                   = 128 * 1024 * 1024,
		.maxUniformBufferRange                    = UINT32_MAX,
		.maxStorageBufferRange                    = UINT32_MAX,
		.maxPushConstantsSize                     = MAX_PUSH_CONSTANTS_SIZE,
		.maxMemoryAllocationCount                 = UINT32_MAX,
		.maxSamplerAllocationCount                = 64 * 1024,
		.bufferImageGranularity                   = 64, /* A cache line */
		.sparseAddressSpaceSize                   = 0xffffffffu, /* buffer max size */
		.maxBoundDescriptorSets                   = MAX_SETS,
		.maxPerStageDescriptorSamplers            = max_descriptor_set_size,
		.maxPerStageDescriptorUniformBuffers      = max_descriptor_set_size,
		.maxPerStageDescriptorStorageBuffers      = max_descriptor_set_size,
		.maxPerStageDescriptorSampledImages       = max_descriptor_set_size,
		.maxPerStageDescriptorStorageImages       = max_descriptor_set_size,
		.maxPerStageDescriptorInputAttachments    = max_descriptor_set_size,
		.maxPerStageResources                     = max_descriptor_set_size,
		.maxDescriptorSetSamplers                 = max_descriptor_set_size,
		.maxDescriptorSetUniformBuffers           = max_descriptor_set_size,
		.maxDescriptorSetUniformBuffersDynamic    = MAX_DYNAMIC_UNIFORM_BUFFERS,
		.maxDescriptorSetStorageBuffers           = max_descriptor_set_size,
		.maxDescriptorSetStorageBuffersDynamic    = MAX_DYNAMIC_STORAGE_BUFFERS,
		.maxDescriptorSetSampledImages            = max_descriptor_set_size,
		.maxDescriptorSetStorageImages            = max_descriptor_set_size,
		.maxDescriptorSetInputAttachments         = max_descriptor_set_size,
		.maxVertexInputAttributes                 = 32,
		.maxVertexInputBindings                   = 32,
		.maxVertexInputAttributeOffset            = 2047,
		.maxVertexInputBindingStride              = 2048,
		.maxVertexOutputComponents                = 128,
		.maxTessellationGenerationLevel           = 64,
		.maxTessellationPatchSize                 = 32,
		.maxTessellationControlPerVertexInputComponents = 128,
		.maxTessellationControlPerVertexOutputComponents = 128,
		.maxTessellationControlPerPatchOutputComponents = 120,
		.maxTessellationControlTotalOutputComponents = 4096,
		.maxTessellationEvaluationInputComponents = 128,
		.maxTessellationEvaluationOutputComponents = 128,
		.maxGeometryShaderInvocations             = 127,
		.maxGeometryInputComponents               = 64,
		.maxGeometryOutputComponents              = 128,
		.maxGeometryOutputVertices                = 256,
		.maxGeometryTotalOutputComponents         = 1024,
		.maxFragmentInputComponents               = 128,
		.maxFragmentOutputAttachments             = 8,
		.maxFragmentDualSrcAttachments            = 1,
		.maxFragmentCombinedOutputResources       = 8,
		.maxComputeSharedMemorySize               = 32768,
		.maxComputeWorkGroupCount                 = { 65535, 65535, 65535 },
		.maxComputeWorkGroupInvocations           = 2048,
		.maxComputeWorkGroupSize = {
			2048,
			2048,
			2048
		},
		.subPixelPrecisionBits                    = 4 /* FIXME */,
		.subTexelPrecisionBits                    = 4 /* FIXME */,
		.mipmapPrecisionBits                      = 4 /* FIXME */,
		.maxDrawIndexedIndexValue                 = UINT32_MAX,
		.maxDrawIndirectCount                     = UINT32_MAX,
		.maxSamplerLodBias                        = 16,
		.maxSamplerAnisotropy                     = 16,
		.maxViewports                             = MAX_VIEWPORTS,
		.maxViewportDimensions                    = { (1 << 14), (1 << 14) },
		.viewportBoundsRange                      = { INT16_MIN, INT16_MAX },
		.viewportSubPixelBits                     = 8,
		.minMemoryMapAlignment                    = 4096, /* A page */
		.minTexelBufferOffsetAlignment            = 1,
		.minUniformBufferOffsetAlignment          = 4,
		.minStorageBufferOffsetAlignment          = 4,
		.minTexelOffset                           = -32,
		.maxTexelOffset                           = 31,
		.minTexelGatherOffset                     = -32,
		.maxTexelGatherOffset                     = 31,
		.minInterpolationOffset                   = -2,
		.maxInterpolationOffset                   = 2,
		.subPixelInterpolationOffsetBits          = 8,
		.maxFramebufferWidth                      = (1 << 14),
		.maxFramebufferHeight                     = (1 << 14),
		.maxFramebufferLayers                     = (1 << 10),
		.framebufferColorSampleCounts             = sample_counts,
		.framebufferDepthSampleCounts             = sample_counts,
		.framebufferStencilSampleCounts           = sample_counts,
		.framebufferNoAttachmentsSampleCounts     = sample_counts,
		.maxColorAttachments                      = MAX_RTS,
		.sampledImageColorSampleCounts            = sample_counts,
		.sampledImageIntegerSampleCounts          = VK_SAMPLE_COUNT_1_BIT,
		.sampledImageDepthSampleCounts            = sample_counts,
		.sampledImageStencilSampleCounts          = sample_counts,
		.storageImageSampleCounts                 = VK_SAMPLE_COUNT_1_BIT,
		.maxSampleMaskWords                       = 1,
		.timestampComputeAndGraphics              = true,
		.timestampPeriod                          = 1000000.0 / pdevice->rad_info.clock_crystal_freq,
		.maxClipDistances                         = 8,
		.maxCullDistances                         = 8,
		.maxCombinedClipAndCullDistances          = 8,
		.discreteQueuePriorities                  = 2,
		.pointSizeRange                           = { 0.125, 255.875 },
		.lineWidthRange                           = { 0.0, 7.9921875 },
		.pointSizeGranularity                     = (1.0 / 8.0),
		.lineWidthGranularity                     = (1.0 / 128.0),
		.strictLines                              = false, /* FINISHME */
		.standardSampleLocations                  = true,
		.optimalBufferCopyOffsetAlignment         = 128,
		.optimalBufferCopyRowPitchAlignment       = 128,
		.nonCoherentAtomSize                      = 64,
	};

	*pProperties = (VkPhysicalDeviceProperties) {
		.apiVersion = radv_physical_device_api_version(pdevice),
		.driverVersion = vk_get_driver_version(),
		.vendorID = ATI_VENDOR_ID,
		.deviceID = pdevice->rad_info.pci_id,
		.deviceType = pdevice->rad_info.has_dedicated_vram ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU : VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
		.limits = limits,
		.sparseProperties = {0},
	};

	strcpy(pProperties->deviceName, pdevice->name);
	memcpy(pProperties->pipelineCacheUUID, pdevice->cache_uuid, VK_UUID_SIZE);
}

void radv_GetPhysicalDeviceProperties2(
	VkPhysicalDevice                            physicalDevice,
	VkPhysicalDeviceProperties2KHR             *pProperties)
{
	RADV_FROM_HANDLE(radv_physical_device, pdevice, physicalDevice);
	radv_GetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);

	vk_foreach_struct(ext, pProperties->pNext) {
		switch (ext->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR: {
			VkPhysicalDevicePushDescriptorPropertiesKHR *properties =
				(VkPhysicalDevicePushDescriptorPropertiesKHR *) ext;
			properties->maxPushDescriptors = MAX_PUSH_DESCRIPTORS;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES_KHR: {
			VkPhysicalDeviceIDPropertiesKHR *properties = (VkPhysicalDeviceIDPropertiesKHR*)ext;
			memcpy(properties->driverUUID, pdevice->driver_uuid, VK_UUID_SIZE);
			memcpy(properties->deviceUUID, pdevice->device_uuid, VK_UUID_SIZE);
			properties->deviceLUIDValid = false;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES_KHR: {
			VkPhysicalDeviceMultiviewPropertiesKHR *properties = (VkPhysicalDeviceMultiviewPropertiesKHR*)ext;
			properties->maxMultiviewViewCount = MAX_VIEWS;
			properties->maxMultiviewInstanceIndex = INT_MAX;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES_KHR: {
			VkPhysicalDevicePointClippingPropertiesKHR *properties =
			    (VkPhysicalDevicePointClippingPropertiesKHR*)ext;
			properties->pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES_KHR;
			break;
		}
		case  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DISCARD_RECTANGLE_PROPERTIES_EXT: {
			VkPhysicalDeviceDiscardRectanglePropertiesEXT *properties =
			    (VkPhysicalDeviceDiscardRectanglePropertiesEXT*)ext;
			properties->maxDiscardRectangles = MAX_DISCARD_RECTANGLES;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT: {
			VkPhysicalDeviceExternalMemoryHostPropertiesEXT *properties =
			    (VkPhysicalDeviceExternalMemoryHostPropertiesEXT *) ext;
			properties->minImportedHostPointerAlignment = 4096;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
			VkPhysicalDeviceSubgroupProperties *properties =
			    (VkPhysicalDeviceSubgroupProperties*)ext;
			properties->subgroupSize = 64;
			properties->supportedStages = VK_SHADER_STAGE_ALL;
			properties->supportedOperations =
							VK_SUBGROUP_FEATURE_BASIC_BIT |
							VK_SUBGROUP_FEATURE_BALLOT_BIT |
							VK_SUBGROUP_FEATURE_QUAD_BIT |
							VK_SUBGROUP_FEATURE_VOTE_BIT;
			if (pdevice->rad_info.chip_class >= VI) {
				properties->supportedOperations |=
							VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
							VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
							VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;
			}
			properties->quadOperationsInAllStages = true;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES: {
			VkPhysicalDeviceMaintenance3Properties *properties =
			    (VkPhysicalDeviceMaintenance3Properties*)ext;
			/* Make sure everything is addressable by a signed 32-bit int, and
			 * our largest descriptors are 96 bytes. */
			properties->maxPerSetDescriptors = (1ull << 31) / 96;
			/* Our buffer size fields allow only this much */
			properties->maxMemoryAllocationSize = 0xFFFFFFFFull;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES_EXT: {
			VkPhysicalDeviceSamplerFilterMinmaxPropertiesEXT *properties =
				(VkPhysicalDeviceSamplerFilterMinmaxPropertiesEXT *)ext;
			/* GFX6-8 only support single channel min/max filter. */
			properties->filterMinmaxImageComponentMapping = pdevice->rad_info.chip_class >= GFX9;
			properties->filterMinmaxSingleComponentFormats = true;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_AMD: {
			VkPhysicalDeviceShaderCorePropertiesAMD *properties =
				(VkPhysicalDeviceShaderCorePropertiesAMD *)ext;

			/* Shader engines. */
			properties->shaderEngineCount =
				pdevice->rad_info.max_se;
			properties->shaderArraysPerEngineCount =
				pdevice->rad_info.max_sh_per_se;
			properties->computeUnitsPerShaderArray =
				pdevice->rad_info.num_good_cu_per_sh;
			properties->simdPerComputeUnit = 4;
			properties->wavefrontsPerSimd =
				pdevice->rad_info.family == CHIP_TONGA ||
				pdevice->rad_info.family == CHIP_ICELAND ||
				pdevice->rad_info.family == CHIP_POLARIS10 ||
				pdevice->rad_info.family == CHIP_POLARIS11 ||
				pdevice->rad_info.family == CHIP_POLARIS12 ||
				pdevice->rad_info.family == CHIP_VEGAM ? 8 : 10;
			properties->wavefrontSize = 64;

			/* SGPR. */
			properties->sgprsPerSimd =
				radv_get_num_physical_sgprs(pdevice);
			properties->minSgprAllocation =
				pdevice->rad_info.chip_class >= VI ? 16 : 8;
			properties->maxSgprAllocation =
				pdevice->rad_info.family == CHIP_TONGA ||
				pdevice->rad_info.family == CHIP_ICELAND ? 96 : 104;
			properties->sgprAllocationGranularity =
				pdevice->rad_info.chip_class >= VI ? 16 : 8;

			/* VGPR. */
			properties->vgprsPerSimd = RADV_NUM_PHYSICAL_VGPRS;
			properties->minVgprAllocation = 4;
			properties->maxVgprAllocation = 256;
			properties->vgprAllocationGranularity = 4;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT: {
			VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *properties =
				(VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *)ext;
			properties->maxVertexAttribDivisor = UINT32_MAX;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES_EXT: {
			VkPhysicalDeviceDescriptorIndexingPropertiesEXT *properties =
				(VkPhysicalDeviceDescriptorIndexingPropertiesEXT*)ext;
			properties->maxUpdateAfterBindDescriptorsInAllPools = UINT32_MAX / 64;
			properties->shaderUniformBufferArrayNonUniformIndexingNative = false;
			properties->shaderSampledImageArrayNonUniformIndexingNative = false;
			properties->shaderStorageBufferArrayNonUniformIndexingNative = false;
			properties->shaderStorageImageArrayNonUniformIndexingNative = false;
			properties->shaderInputAttachmentArrayNonUniformIndexingNative = false;
			properties->robustBufferAccessUpdateAfterBind = false;
			properties->quadDivergentImplicitLod = false;

			size_t max_descriptor_set_size = ((1ull << 31) - 16 * MAX_DYNAMIC_BUFFERS) /
			          (32 /* uniform buffer, 32 due to potential space wasted on alignment */ +
			           32 /* storage buffer, 32 due to potential space wasted on alignment */ +
			           32 /* sampler, largest when combined with image */ +
			           64 /* sampled image */ +
			           64 /* storage image */);
			properties->maxPerStageDescriptorUpdateAfterBindSamplers = max_descriptor_set_size;
			properties->maxPerStageDescriptorUpdateAfterBindUniformBuffers = max_descriptor_set_size;
			properties->maxPerStageDescriptorUpdateAfterBindStorageBuffers = max_descriptor_set_size;
			properties->maxPerStageDescriptorUpdateAfterBindSampledImages = max_descriptor_set_size;
			properties->maxPerStageDescriptorUpdateAfterBindStorageImages = max_descriptor_set_size;
			properties->maxPerStageDescriptorUpdateAfterBindInputAttachments = max_descriptor_set_size;
			properties->maxPerStageUpdateAfterBindResources = max_descriptor_set_size;
			properties->maxDescriptorSetUpdateAfterBindSamplers = max_descriptor_set_size;
			properties->maxDescriptorSetUpdateAfterBindUniformBuffers = max_descriptor_set_size;
			properties->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = MAX_DYNAMIC_UNIFORM_BUFFERS;
			properties->maxDescriptorSetUpdateAfterBindStorageBuffers = max_descriptor_set_size;
			properties->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = MAX_DYNAMIC_STORAGE_BUFFERS;
			properties->maxDescriptorSetUpdateAfterBindSampledImages = max_descriptor_set_size;
			properties->maxDescriptorSetUpdateAfterBindStorageImages = max_descriptor_set_size;
			properties->maxDescriptorSetUpdateAfterBindInputAttachments = max_descriptor_set_size;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES: {
			VkPhysicalDeviceProtectedMemoryProperties *properties =
				(VkPhysicalDeviceProtectedMemoryProperties *)ext;
			properties->protectedNoFault = false;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT: {
			VkPhysicalDeviceConservativeRasterizationPropertiesEXT *properties =
				(VkPhysicalDeviceConservativeRasterizationPropertiesEXT *)ext;
			properties->primitiveOverestimationSize = 0;
			properties->maxExtraPrimitiveOverestimationSize = 0;
			properties->extraPrimitiveOverestimationSizeGranularity = 0;
			properties->primitiveUnderestimation = VK_FALSE;
			properties->conservativePointAndLineRasterization = VK_FALSE;
			properties->degenerateTrianglesRasterized = VK_FALSE;
			properties->degenerateLinesRasterized = VK_FALSE;
			properties->fullyCoveredFragmentShaderInputVariable = VK_FALSE;
			properties->conservativeRasterizationPostDepthCoverage = VK_FALSE;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT: {
			VkPhysicalDevicePCIBusInfoPropertiesEXT *properties =
				(VkPhysicalDevicePCIBusInfoPropertiesEXT *)ext;
			properties->pciDomain = pdevice->bus_info.domain;
			properties->pciBus = pdevice->bus_info.bus;
			properties->pciDevice = pdevice->bus_info.dev;
			properties->pciFunction = pdevice->bus_info.func;
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR: {
			VkPhysicalDeviceDriverPropertiesKHR *driver_props =
				(VkPhysicalDeviceDriverPropertiesKHR *) ext;

			driver_props->driverID = VK_DRIVER_ID_MESA_RADV_KHR;
			memset(driver_props->driverName, 0, VK_MAX_DRIVER_NAME_SIZE_KHR);
			strcpy(driver_props->driverName, "radv");

			memset(driver_props->driverInfo, 0, VK_MAX_DRIVER_INFO_SIZE_KHR);
			snprintf(driver_props->driverInfo, VK_MAX_DRIVER_INFO_SIZE_KHR,
				"Mesa " PACKAGE_VERSION MESA_GIT_SHA1
				" (LLVM %d.%d.%d)",
				 (HAVE_LLVM >> 8) & 0xff, HAVE_LLVM & 0xff,
				 MESA_LLVM_VERSION_PATCH);

			driver_props->conformanceVersion = (VkConformanceVersionKHR) {
				.major = 1,
				.minor = 1,
				.subminor = 2,
				.patch = 0,
			};
			break;
		}
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT: {
			VkPhysicalDeviceTransformFeedbackPropertiesEXT *properties =
				(VkPhysicalDeviceTransformFeedbackPropertiesEXT *)ext;
			properties->maxTransformFeedbackStreams = MAX_SO_STREAMS;
			properties->maxTransformFeedbackBuffers = MAX_SO_BUFFERS;
			properties->maxTransformFeedbackBufferSize = UINT32_MAX;
			properties->maxTransformFeedbackStreamDataSize = 512;
			properties->maxTransformFeedbackBufferDataSize = UINT32_MAX;
			properties->maxTransformFeedbackBufferDataStride = 512;
			properties->transformFeedbackQueries = true;
			properties->transformFeedbackStreamsLinesTriangles = false;
			properties->transformFeedbackRasterizationStreamSelect = false;
			properties->transformFeedbackDraw = true;
			break;
		}
		default:
			break;
		}
	}
}

static void radv_get_physical_device_queue_family_properties(
	struct radv_physical_device*                pdevice,
	uint32_t*                                   pCount,
	VkQueueFamilyProperties**                    pQueueFamilyProperties)
{
	int num_queue_families = 1;
	int idx;
	if (pdevice->rad_info.num_compute_rings > 0 &&
	    !(pdevice->instance->debug_flags & RADV_DEBUG_NO_COMPUTE_QUEUE))
		num_queue_families++;

	if (pQueueFamilyProperties == NULL) {
		*pCount = num_queue_families;
		return;
	}

	if (!*pCount)
		return;

	idx = 0;
	if (*pCount >= 1) {
		*pQueueFamilyProperties[idx] = (VkQueueFamilyProperties) {
			.queueFlags = VK_QUEUE_GRAPHICS_BIT |
			              VK_QUEUE_COMPUTE_BIT |
			              VK_QUEUE_TRANSFER_BIT |
			              VK_QUEUE_SPARSE_BINDING_BIT,
			.queueCount = 1,
			.timestampValidBits = 64,
			.minImageTransferGranularity = (VkExtent3D) { 1, 1, 1 },
		};
		idx++;
	}

	if (pdevice->rad_info.num_compute_rings > 0 &&
	    !(pdevice->instance->debug_flags & RADV_DEBUG_NO_COMPUTE_QUEUE)) {
		if (*pCount > idx) {
			*pQueueFamilyProperties[idx] = (VkQueueFamilyProperties) {
				.queueFlags = VK_QUEUE_COMPUTE_BIT |
				              VK_QUEUE_TRANSFER_BIT |
				              VK_QUEUE_SPARSE_BINDING_BIT,
				.queueCount = pdevice->rad_info.num_compute_rings,
				.timestampValidBits = 64,
				.minImageTransferGranularity = (VkExtent3D) { 1, 1, 1 },
			};
			idx++;
		}
	}
	*pCount = idx;
}

void radv_GetPhysicalDeviceQueueFamilyProperties(
	VkPhysicalDevice                            physicalDevice,
	uint32_t*                                   pCount,
	VkQueueFamilyProperties*                    pQueueFamilyProperties)
{
	RADV_FROM_HANDLE(radv_physical_device, pdevice, physicalDevice);
	if (!pQueueFamilyProperties) {
		return radv_get_physical_device_queue_family_properties(pdevice, pCount, NULL);
		return;
	}
	VkQueueFamilyProperties *properties[] = {
		pQueueFamilyProperties + 0,
		pQueueFamilyProperties + 1,
		pQueueFamilyProperties + 2,
	};
	radv_get_physical_device_queue_family_properties(pdevice, pCount, properties);
	assert(*pCount <= 3);
}

void radv_GetPhysicalDeviceQueueFamilyProperties2(
	VkPhysicalDevice                            physicalDevice,
	uint32_t*                                   pCount,
	VkQueueFamilyProperties2KHR                *pQueueFamilyProperties)
{
	RADV_FROM_HANDLE(radv_physical_device, pdevice, physicalDevice);
	if (!pQueueFamilyProperties) {
		return radv_get_physical_device_queue_family_properties(pdevice, pCount, NULL);
		return;
	}
	VkQueueFamilyProperties *properties[] = {
		&pQueueFamilyProperties[0].queueFamilyProperties,
		&pQueueFamilyProperties[1].queueFamilyProperties,
		&pQueueFamilyProperties[2].queueFamilyProperties,
	};
	radv_get_physical_device_queue_family_properties(pdevice, pCount, properties);
	assert(*pCount <= 3);
}

void radv_GetPhysicalDeviceMemoryProperties(
	VkPhysicalDevice                            physicalDevice,
	VkPhysicalDeviceMemoryProperties           *pMemoryProperties)
{
	RADV_FROM_HANDLE(radv_physical_device, physical_device, physicalDevice);

	*pMemoryProperties = physical_device->memory_properties;
}

void radv_GetPhysicalDeviceMemoryProperties2(
	VkPhysicalDevice                            physicalDevice,
	VkPhysicalDeviceMemoryProperties2KHR       *pMemoryProperties)
{
	return radv_GetPhysicalDeviceMemoryProperties(physicalDevice,
						      &pMemoryProperties->memoryProperties);
}

VkResult radv_GetMemoryHostPointerPropertiesEXT(
	VkDevice                                    _device,
	VkExternalMemoryHandleTypeFlagBitsKHR       handleType,
	const void                                 *pHostPointer,
	VkMemoryHostPointerPropertiesEXT           *pMemoryHostPointerProperties)
{
	RADV_FROM_HANDLE(radv_device, device, _device);

	switch (handleType)
	{
	case VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT: {
		const struct radv_physical_device *physical_device = device->physical_device;
		uint32_t memoryTypeBits = 0;
		for (int i = 0; i < physical_device->memory_properties.memoryTypeCount; i++) {
			if (physical_device->mem_type_indices[i] == RADV_MEM_TYPE_GTT_CACHED) {
				memoryTypeBits = (1 << i);
				break;
			}
		}
		pMemoryHostPointerProperties->memoryTypeBits = memoryTypeBits;
		return VK_SUCCESS;
	}
	default:
		return VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR;
	}
}

static enum radeon_ctx_priority
radv_get_queue_global_priority(const VkDeviceQueueGlobalPriorityCreateInfoEXT *pObj)
{
	/* Default to MEDIUM when a specific global priority isn't requested */
	if (!pObj)
		return RADEON_CTX_PRIORITY_MEDIUM;

	switch(pObj->globalPriority) {
	case VK_QUEUE_GLOBAL_PRIORITY_REALTIME_EXT:
		return RADEON_CTX_PRIORITY_REALTIME;
	case VK_QUEUE_GLOBAL_PRIORITY_HIGH_EXT:
		return RADEON_CTX_PRIORITY_HIGH;
	case VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_EXT:
		return RADEON_CTX_PRIORITY_MEDIUM;
	case VK_QUEUE_GLOBAL_PRIORITY_LOW_EXT:
		return RADEON_CTX_PRIORITY_LOW;
	default:
		unreachable("Illegal global priority value");
		return RADEON_CTX_PRIORITY_INVALID;
	}
}

static int
radv_queue_init(struct radv_device *device, struct radv_queue *queue,
		uint32_t queue_family_index, int idx,
		VkDeviceQueueCreateFlags flags,
		const VkDeviceQueueGlobalPriorityCreateInfoEXT *global_priority)
{
	queue->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
	queue->device = device;
	queue->queue_family_index = queue_family_index;
	queue->queue_idx = idx;
	queue->priority = radv_get_queue_global_priority(global_priority);
	queue->flags = flags;

	queue->hw_ctx = device->ws->ctx_create(device->ws, queue->priority);
	if (!queue->hw_ctx)
		return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

	return VK_SUCCESS;
}

static void
radv_queue_finish(struct radv_queue *queue)
{
	if (queue->hw_ctx)
		queue->device->ws->ctx_destroy(queue->hw_ctx);

	if (queue->initial_full_flush_preamble_cs)
		queue->device->ws->cs_destroy(queue->initial_full_flush_preamble_cs);
	if (queue->initial_preamble_cs)
		queue->device->ws->cs_destroy(queue->initial_preamble_cs);
	if (queue->continue_preamble_cs)
		queue->device->ws->cs_destroy(queue->continue_preamble_cs);
	if (queue->descriptor_bo)
		queue->device->ws->buffer_destroy(queue->descriptor_bo);
	if (queue->scratch_bo)
		queue->device->ws->buffer_destroy(queue->scratch_bo);
	if (queue->esgs_ring_bo)
		queue->device->ws->buffer_destroy(queue->esgs_ring_bo);
	if (queue->gsvs_ring_bo)
		queue->device->ws->buffer_destroy(queue->gsvs_ring_bo);
	if (queue->tess_rings_bo)
		queue->device->ws->buffer_destroy(queue->tess_rings_bo);
	if (queue->compute_scratch_bo)
		queue->device->ws->buffer_destroy(queue->compute_scratch_bo);
}

static void
radv_bo_list_init(struct radv_bo_list *bo_list)
{
	pthread_mutex_init(&bo_list->mutex, NULL);
	bo_list->list.count = bo_list->capacity = 0;
	bo_list->list.bos = NULL;
}

static void
radv_bo_list_finish(struct radv_bo_list *bo_list)
{
	free(bo_list->list.bos);
	pthread_mutex_destroy(&bo_list->mutex);
}

static VkResult radv_bo_list_add(struct radv_device *device,
				 struct radeon_winsys_bo *bo)
{
	struct radv_bo_list *bo_list = &device->bo_list;

	if (unlikely(!device->use_global_bo_list))
		return VK_SUCCESS;

	pthread_mutex_lock(&bo_list->mutex);
	if (bo_list->list.count == bo_list->capacity) {
		unsigned capacity = MAX2(4, bo_list->capacity * 2);
		void *data = realloc(bo_list->list.bos, capacity * sizeof(struct radeon_winsys_bo*));

		if (!data) {
			pthread_mutex_unlock(&bo_list->mutex);
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		bo_list->list.bos = (struct radeon_winsys_bo**)data;
		bo_list->capacity = capacity;
	}

	bo_list->list.bos[bo_list->list.count++] = bo;
	pthread_mutex_unlock(&bo_list->mutex);
	return VK_SUCCESS;
}

static void radv_bo_list_remove(struct radv_device *device,
				struct radeon_winsys_bo *bo)
{
	struct radv_bo_list *bo_list = &device->bo_list;

	if (unlikely(!device->use_global_bo_list))
		return;

	pthread_mutex_lock(&bo_list->mutex);
	for(unsigned i = 0; i < bo_list->list.count; ++i) {
		if (bo_list->list.bos[i] == bo) {
			bo_list->list.bos[i] = bo_list->list.bos[bo_list->list.count - 1];
			--bo_list->list.count;
			break;
		}
	}
	pthread_mutex_unlock(&bo_list->mutex);
}

static void
radv_device_init_gs_info(struct radv_device *device)
{
	device->gs_table_depth = ac_get_gs_table_depth(device->physical_device->rad_info.chip_class,
						       device->physical_device->rad_info.family);
}

static int radv_get_device_extension_index(const char *name)
{
	for (unsigned i = 0; i < RADV_DEVICE_EXTENSION_COUNT; ++i) {
		if (strcmp(name, radv_device_extensions[i].extensionName) == 0)
			return i;
	}
	return -1;
}

static int
radv_get_int_debug_option(const char *name, int default_value)
{
	const char *str;
	int result;

	str = getenv(name);
	if (!str) {
		result = default_value;
	} else {
		char *endptr;

		result = strtol(str, &endptr, 0);
		if (str == endptr) {
			/* No digits founs. */
			result = default_value;
		}
	}

	return result;
}

VkResult radv_CreateDevice(
	VkPhysicalDevice                            physicalDevice,
	const VkDeviceCreateInfo*                   pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkDevice*                                   pDevice)
{
	RADV_FROM_HANDLE(radv_physical_device, physical_device, physicalDevice);
	VkResult result;
	struct radv_device *device;

	bool keep_shader_info = false;

	/* Check enabled features */
	if (pCreateInfo->pEnabledFeatures) {
		VkPhysicalDeviceFeatures supported_features;
		radv_GetPhysicalDeviceFeatures(physicalDevice, &supported_features);
		VkBool32 *supported_feature = (VkBool32 *)&supported_features;
		VkBool32 *enabled_feature = (VkBool32 *)pCreateInfo->pEnabledFeatures;
		unsigned num_features = sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
		for (uint32_t i = 0; i < num_features; i++) {
			if (enabled_feature[i] && !supported_feature[i])
				return vk_error(physical_device->instance, VK_ERROR_FEATURE_NOT_PRESENT);
		}
	}

	device = vk_zalloc2(&physical_device->instance->alloc, pAllocator,
			    sizeof(*device), 8,
			    VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (!device)
		return vk_error(physical_device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

	device->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
	device->instance = physical_device->instance;
	device->physical_device = physical_device;

	device->ws = physical_device->ws;
	if (pAllocator)
		device->alloc = *pAllocator;
	else
		device->alloc = physical_device->instance->alloc;

	for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
		const char *ext_name = pCreateInfo->ppEnabledExtensionNames[i];
		int index = radv_get_device_extension_index(ext_name);
		if (index < 0 || !physical_device->supported_extensions.extensions[index]) {
			vk_free(&device->alloc, device);
			return vk_error(physical_device->instance, VK_ERROR_EXTENSION_NOT_PRESENT);
		}

		device->enabled_extensions.extensions[index] = true;
	}

	keep_shader_info = device->enabled_extensions.AMD_shader_info;

	/* With update after bind we can't attach bo's to the command buffer
	 * from the descriptor set anymore, so we have to use a global BO list.
	 */
	device->use_global_bo_list =
		device->enabled_extensions.EXT_descriptor_indexing;

	mtx_init(&device->shader_slab_mutex, mtx_plain);
	list_inithead(&device->shader_slabs);

	radv_bo_list_init(&device->bo_list);

	for (unsigned i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
		const VkDeviceQueueCreateInfo *queue_create = &pCreateInfo->pQueueCreateInfos[i];
		uint32_t qfi = queue_create->queueFamilyIndex;
		const VkDeviceQueueGlobalPriorityCreateInfoEXT *global_priority =
			vk_find_struct_const(queue_create->pNext, DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT);

		assert(!global_priority || device->physical_device->rad_info.has_ctx_priority);

		device->queues[qfi] = vk_alloc(&device->alloc,
					       queue_create->queueCount * sizeof(struct radv_queue), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
		if (!device->queues[qfi]) {
			result = VK_ERROR_OUT_OF_HOST_MEMORY;
			goto fail;
		}

		memset(device->queues[qfi], 0, queue_create->queueCount * sizeof(struct radv_queue));

		device->queue_count[qfi] = queue_create->queueCount;

		for (unsigned q = 0; q < queue_create->queueCount; q++) {
			result = radv_queue_init(device, &device->queues[qfi][q],
						 qfi, q, queue_create->flags,
						 global_priority);
			if (result != VK_SUCCESS)
				goto fail;
		}
	}

	device->pbb_allowed = device->physical_device->rad_info.chip_class >= GFX9 &&
			      !(device->instance->debug_flags & RADV_DEBUG_NOBINNING);

	/* Disabled and not implemented for now. */
	device->dfsm_allowed = device->pbb_allowed &&
	                       (device->physical_device->rad_info.family == CHIP_RAVEN ||
	                        device->physical_device->rad_info.family == CHIP_RAVEN2);

#ifdef ANDROID
	device->always_use_syncobj = device->physical_device->rad_info.has_syncobj_wait_for_submit;
#endif

	/* The maximum number of scratch waves. Scratch space isn't divided
	 * evenly between CUs. The number is only a function of the number of CUs.
	 * We can decrease the constant to decrease the scratch buffer size.
	 *
	 * sctx->scratch_waves must be >= the maximum possible size of
	 * 1 threadgroup, so that the hw doesn't hang from being unable
	 * to start any.
	 *
	 * The recommended value is 4 per CU at most. Higher numbers don't
	 * bring much benefit, but they still occupy chip resources (think
	 * async compute). I've seen ~2% performance difference between 4 and 32.
	 */
	uint32_t max_threads_per_block = 2048;
	device->scratch_waves = MAX2(32 * physical_device->rad_info.num_good_compute_units,
				     max_threads_per_block / 64);

	device->dispatch_initiator = S_00B800_COMPUTE_SHADER_EN(1);

	if (device->physical_device->rad_info.chip_class >= CIK) {
		/* If the KMD allows it (there is a KMD hw register for it),
		 * allow launching waves out-of-order.
		 */
		device->dispatch_initiator |= S_00B800_ORDER_MODE(1);
	}

	radv_device_init_gs_info(device);

	device->tess_offchip_block_dw_size =
		device->physical_device->rad_info.family == CHIP_HAWAII ? 4096 : 8192;
	device->has_distributed_tess =
		device->physical_device->rad_info.chip_class >= VI &&
		device->physical_device->rad_info.max_se >= 2;

	if (getenv("RADV_TRACE_FILE")) {
		const char *filename = getenv("RADV_TRACE_FILE");

		keep_shader_info = true;

		if (!radv_init_trace(device))
			goto fail;

		fprintf(stderr, "*****************************************************************************\n");
		fprintf(stderr, "* WARNING: RADV_TRACE_FILE is costly and should only be used for debugging! *\n");
		fprintf(stderr, "*****************************************************************************\n");

		fprintf(stderr, "Trace file will be dumped to %s\n", filename);
		radv_dump_enabled_options(device, stderr);
	}

	device->keep_shader_info = keep_shader_info;

	result = radv_device_init_meta(device);
	if (result != VK_SUCCESS)
		goto fail;

	radv_device_init_msaa(device);

	for (int family = 0; family < RADV_MAX_QUEUE_FAMILIES; ++family) {
		device->empty_cs[family] = device->ws->cs_create(device->ws, family);
		switch (family) {
		case RADV_QUEUE_GENERAL:
			radeon_emit(device->empty_cs[family], PKT3(PKT3_CONTEXT_CONTROL, 1, 0));
			radeon_emit(device->empty_cs[family], CONTEXT_CONTROL_LOAD_ENABLE(1));
			radeon_emit(device->empty_cs[family], CONTEXT_CONTROL_SHADOW_ENABLE(1));
			break;
		case RADV_QUEUE_COMPUTE:
			radeon_emit(device->empty_cs[family], PKT3(PKT3_NOP, 0, 0));
			radeon_emit(device->empty_cs[family], 0);
			break;
		}
		device->ws->cs_finalize(device->empty_cs[family]);
	}

	if (device->physical_device->rad_info.chip_class >= CIK)
		cik_create_gfx_config(device);

	VkPipelineCacheCreateInfo ci;
	ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	ci.pNext = NULL;
	ci.flags = 0;
	ci.pInitialData = NULL;
	ci.initialDataSize = 0;
	VkPipelineCache pc;
	result = radv_CreatePipelineCache(radv_device_to_handle(device),
					  &ci, NULL, &pc);
	if (result != VK_SUCCESS)
		goto fail_meta;

	device->mem_cache = radv_pipeline_cache_from_handle(pc);

	device->force_aniso =
		MIN2(16, radv_get_int_debug_option("RADV_TEX_ANISO", -1));
	if (device->force_aniso >= 0) {
		fprintf(stderr, "radv: Forcing anisotropy filter to %ix\n",
			1 << util_logbase2(device->force_aniso));
	}

	*pDevice = radv_device_to_handle(device);
	return VK_SUCCESS;

fail_meta:
	radv_device_finish_meta(device);
fail:
	radv_bo_list_finish(&device->bo_list);

	if (device->trace_bo)
		device->ws->buffer_destroy(device->trace_bo);

	if (device->gfx_init)
		device->ws->buffer_destroy(device->gfx_init);

	for (unsigned i = 0; i < RADV_MAX_QUEUE_FAMILIES; i++) {
		for (unsigned q = 0; q < device->queue_count[i]; q++)
			radv_queue_finish(&device->queues[i][q]);
		if (device->queue_count[i])
			vk_free(&device->alloc, device->queues[i]);
	}

	vk_free(&device->alloc, device);
	return result;
}

void radv_DestroyDevice(
	VkDevice                                    _device,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);

	if (!device)
		return;

	if (device->trace_bo)
		device->ws->buffer_destroy(device->trace_bo);

	if (device->gfx_init)
		device->ws->buffer_destroy(device->gfx_init);

	for (unsigned i = 0; i < RADV_MAX_QUEUE_FAMILIES; i++) {
		for (unsigned q = 0; q < device->queue_count[i]; q++)
			radv_queue_finish(&device->queues[i][q]);
		if (device->queue_count[i])
			vk_free(&device->alloc, device->queues[i]);
		if (device->empty_cs[i])
			device->ws->cs_destroy(device->empty_cs[i]);
	}
	radv_device_finish_meta(device);

	VkPipelineCache pc = radv_pipeline_cache_to_handle(device->mem_cache);
	radv_DestroyPipelineCache(radv_device_to_handle(device), pc, NULL);

	radv_destroy_shader_slabs(device);

	radv_bo_list_finish(&device->bo_list);
	vk_free(&device->alloc, device);
}

VkResult radv_EnumerateInstanceLayerProperties(
	uint32_t*                                   pPropertyCount,
	VkLayerProperties*                          pProperties)
{
	if (pProperties == NULL) {
		*pPropertyCount = 0;
		return VK_SUCCESS;
	}

	/* None supported at this time */
	return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);
}

VkResult radv_EnumerateDeviceLayerProperties(
	VkPhysicalDevice                            physicalDevice,
	uint32_t*                                   pPropertyCount,
	VkLayerProperties*                          pProperties)
{
	if (pProperties == NULL) {
		*pPropertyCount = 0;
		return VK_SUCCESS;
	}

	/* None supported at this time */
	return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);
}

void radv_GetDeviceQueue2(
	VkDevice                                    _device,
	const VkDeviceQueueInfo2*                   pQueueInfo,
	VkQueue*                                    pQueue)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	struct radv_queue *queue;

	queue = &device->queues[pQueueInfo->queueFamilyIndex][pQueueInfo->queueIndex];
	if (pQueueInfo->flags != queue->flags) {
		/* From the Vulkan 1.1.70 spec:
		 *
		 * "The queue returned by vkGetDeviceQueue2 must have the same
		 * flags value from this structure as that used at device
		 * creation time in a VkDeviceQueueCreateInfo instance. If no
		 * matching flags were specified at device creation time then
		 * pQueue will return VK_NULL_HANDLE."
		 */
		*pQueue = VK_NULL_HANDLE;
		return;
	}

	*pQueue = radv_queue_to_handle(queue);
}

void radv_GetDeviceQueue(
	VkDevice                                    _device,
	uint32_t                                    queueFamilyIndex,
	uint32_t                                    queueIndex,
	VkQueue*                                    pQueue)
{
	const VkDeviceQueueInfo2 info = (VkDeviceQueueInfo2) {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
		.queueFamilyIndex = queueFamilyIndex,
		.queueIndex = queueIndex
	};

	radv_GetDeviceQueue2(_device, &info, pQueue);
}

static void
fill_geom_tess_rings(struct radv_queue *queue,
		     uint32_t *map,
		     bool add_sample_positions,
		     uint32_t esgs_ring_size,
		     struct radeon_winsys_bo *esgs_ring_bo,
		     uint32_t gsvs_ring_size,
		     struct radeon_winsys_bo *gsvs_ring_bo,
		     uint32_t tess_factor_ring_size,
		     uint32_t tess_offchip_ring_offset,
		     uint32_t tess_offchip_ring_size,
		     struct radeon_winsys_bo *tess_rings_bo)
{
	uint64_t esgs_va = 0, gsvs_va = 0;
	uint64_t tess_va = 0, tess_offchip_va = 0;
	uint32_t *desc = &map[4];

	if (esgs_ring_bo)
		esgs_va = radv_buffer_get_va(esgs_ring_bo);
	if (gsvs_ring_bo)
		gsvs_va = radv_buffer_get_va(gsvs_ring_bo);
	if (tess_rings_bo) {
		tess_va = radv_buffer_get_va(tess_rings_bo);
		tess_offchip_va = tess_va + tess_offchip_ring_offset;
	}

	/* stride 0, num records - size, add tid, swizzle, elsize4,
	   index stride 64 */
	desc[0] = esgs_va;
	desc[1] = S_008F04_BASE_ADDRESS_HI(esgs_va >> 32) |
		S_008F04_STRIDE(0) |
		S_008F04_SWIZZLE_ENABLE(true);
	desc[2] = esgs_ring_size;
	desc[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
		S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
		S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
		S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
		S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
		S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32) |
		S_008F0C_ELEMENT_SIZE(1) |
		S_008F0C_INDEX_STRIDE(3) |
		S_008F0C_ADD_TID_ENABLE(true);

	desc += 4;
	/* GS entry for ES->GS ring */
	/* stride 0, num records - size, elsize0,
	   index stride 0 */
	desc[0] = esgs_va;
	desc[1] = S_008F04_BASE_ADDRESS_HI(esgs_va >> 32)|
		S_008F04_STRIDE(0) |
		S_008F04_SWIZZLE_ENABLE(false);
	desc[2] = esgs_ring_size;
	desc[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
		S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
		S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
		S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
		S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
		S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32) |
		S_008F0C_ELEMENT_SIZE(0) |
		S_008F0C_INDEX_STRIDE(0) |
		S_008F0C_ADD_TID_ENABLE(false);

	desc += 4;
	/* VS entry for GS->VS ring */
	/* stride 0, num records - size, elsize0,
	   index stride 0 */
	desc[0] = gsvs_va;
	desc[1] = S_008F04_BASE_ADDRESS_HI(gsvs_va >> 32)|
		S_008F04_STRIDE(0) |
		S_008F04_SWIZZLE_ENABLE(false);
	desc[2] = gsvs_ring_size;
	desc[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
		S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
		S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
		S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
		S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
		S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32) |
		S_008F0C_ELEMENT_SIZE(0) |
		S_008F0C_INDEX_STRIDE(0) |
		S_008F0C_ADD_TID_ENABLE(false);
	desc += 4;

	/* stride gsvs_itemsize, num records 64
	   elsize 4, index stride 16 */
	/* shader will patch stride and desc[2] */
	desc[0] = gsvs_va;
	desc[1] = S_008F04_BASE_ADDRESS_HI(gsvs_va >> 32)|
		S_008F04_STRIDE(0) |
		S_008F04_SWIZZLE_ENABLE(true);
	desc[2] = 0;
	desc[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
		S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
		S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
		S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
		S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
		S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32) |
		S_008F0C_ELEMENT_SIZE(1) |
		S_008F0C_INDEX_STRIDE(1) |
		S_008F0C_ADD_TID_ENABLE(true);
	desc += 4;

	desc[0] = tess_va;
	desc[1] = S_008F04_BASE_ADDRESS_HI(tess_va >> 32) |
		S_008F04_STRIDE(0) |
		S_008F04_SWIZZLE_ENABLE(false);
	desc[2] = tess_factor_ring_size;
	desc[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
		S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
		S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
		S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
		S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
		S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32) |
		S_008F0C_ELEMENT_SIZE(0) |
		S_008F0C_INDEX_STRIDE(0) |
		S_008F0C_ADD_TID_ENABLE(false);
	desc += 4;

	desc[0] = tess_offchip_va;
	desc[1] = S_008F04_BASE_ADDRESS_HI(tess_offchip_va >> 32) |
		S_008F04_STRIDE(0) |
		S_008F04_SWIZZLE_ENABLE(false);
	desc[2] = tess_offchip_ring_size;
	desc[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
		S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
		S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
		S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
		S_008F0C_NUM_FORMAT(V_008F0C_BUF_NUM_FORMAT_FLOAT) |
		S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32) |
		S_008F0C_ELEMENT_SIZE(0) |
		S_008F0C_INDEX_STRIDE(0) |
		S_008F0C_ADD_TID_ENABLE(false);
	desc += 4;

	/* add sample positions after all rings */
	memcpy(desc, queue->device->sample_locations_1x, 8);
	desc += 2;
	memcpy(desc, queue->device->sample_locations_2x, 16);
	desc += 4;
	memcpy(desc, queue->device->sample_locations_4x, 32);
	desc += 8;
	memcpy(desc, queue->device->sample_locations_8x, 64);
	desc += 16;
	memcpy(desc, queue->device->sample_locations_16x, 128);
}

static unsigned
radv_get_hs_offchip_param(struct radv_device *device, uint32_t *max_offchip_buffers_p)
{
	bool double_offchip_buffers = device->physical_device->rad_info.chip_class >= CIK &&
		device->physical_device->rad_info.family != CHIP_CARRIZO &&
		device->physical_device->rad_info.family != CHIP_STONEY;
	unsigned max_offchip_buffers_per_se = double_offchip_buffers ? 128 : 64;
	unsigned max_offchip_buffers;
	unsigned offchip_granularity;
	unsigned hs_offchip_param;

	/*
	 * Per RadeonSI:
	 * This must be one less than the maximum number due to a hw limitation.
         * Various hardware bugs in SI, CIK, and GFX9 need this.
	 *
	 * Per AMDVLK:
	 * Vega10 should limit max_offchip_buffers to 508 (4 * 127).
	 * Gfx7 should limit max_offchip_buffers to 508
	 * Gfx6 should limit max_offchip_buffers to 126 (2 * 63)
	 *
	 * Follow AMDVLK here.
	 */
	if (device->physical_device->rad_info.family == CHIP_VEGA10 ||
	    device->physical_device->rad_info.chip_class == CIK ||
	    device->physical_device->rad_info.chip_class == SI)
		--max_offchip_buffers_per_se;

	max_offchip_buffers = max_offchip_buffers_per_se *
		device->physical_device->rad_info.max_se;

	/* Hawaii has a bug with offchip buffers > 256 that can be worked
	 * around by setting 4K granularity.
	 */
	if (device->tess_offchip_block_dw_size == 4096) {
		assert(device->physical_device->rad_info.family == CHIP_HAWAII);
		offchip_granularity = V_03093C_X_4K_DWORDS;
	} else {
		assert(device->tess_offchip_block_dw_size == 8192);
		offchip_granularity = V_03093C_X_8K_DWORDS;
	}

	switch (device->physical_device->rad_info.chip_class) {
	case SI:
		max_offchip_buffers = MIN2(max_offchip_buffers, 126);
		break;
	case CIK:
	case VI:
	case GFX9:
	default:
		max_offchip_buffers = MIN2(max_offchip_buffers, 508);
		break;
	}

	*max_offchip_buffers_p = max_offchip_buffers;
	if (device->physical_device->rad_info.chip_class >= CIK) {
		if (device->physical_device->rad_info.chip_class >= VI)
			--max_offchip_buffers;
		hs_offchip_param =
			S_03093C_OFFCHIP_BUFFERING(max_offchip_buffers) |
			S_03093C_OFFCHIP_GRANULARITY(offchip_granularity);
	} else {
		hs_offchip_param =
			S_0089B0_OFFCHIP_BUFFERING(max_offchip_buffers);
	}
	return hs_offchip_param;
}

static void
radv_emit_gs_ring_sizes(struct radv_queue *queue, struct radeon_cmdbuf *cs,
			struct radeon_winsys_bo *esgs_ring_bo,
			uint32_t esgs_ring_size,
			struct radeon_winsys_bo *gsvs_ring_bo,
			uint32_t gsvs_ring_size)
{
	if (!esgs_ring_bo && !gsvs_ring_bo)
		return;

	if (esgs_ring_bo)
		radv_cs_add_buffer(queue->device->ws, cs, esgs_ring_bo);

	if (gsvs_ring_bo)
		radv_cs_add_buffer(queue->device->ws, cs, gsvs_ring_bo);

	if (queue->device->physical_device->rad_info.chip_class >= CIK) {
		radeon_set_uconfig_reg_seq(cs, R_030900_VGT_ESGS_RING_SIZE, 2);
		radeon_emit(cs, esgs_ring_size >> 8);
		radeon_emit(cs, gsvs_ring_size >> 8);
	} else {
		radeon_set_config_reg_seq(cs, R_0088C8_VGT_ESGS_RING_SIZE, 2);
		radeon_emit(cs, esgs_ring_size >> 8);
		radeon_emit(cs, gsvs_ring_size >> 8);
	}
}

static void
radv_emit_tess_factor_ring(struct radv_queue *queue, struct radeon_cmdbuf *cs,
			   unsigned hs_offchip_param, unsigned tf_ring_size,
			   struct radeon_winsys_bo *tess_rings_bo)
{
	uint64_t tf_va;

	if (!tess_rings_bo)
		return;

	tf_va = radv_buffer_get_va(tess_rings_bo);

	radv_cs_add_buffer(queue->device->ws, cs, tess_rings_bo);

	if (queue->device->physical_device->rad_info.chip_class >= CIK) {
		radeon_set_uconfig_reg(cs, R_030938_VGT_TF_RING_SIZE,
				       S_030938_SIZE(tf_ring_size / 4));
		radeon_set_uconfig_reg(cs, R_030940_VGT_TF_MEMORY_BASE,
				       tf_va >> 8);
		if (queue->device->physical_device->rad_info.chip_class >= GFX9) {
			radeon_set_uconfig_reg(cs, R_030944_VGT_TF_MEMORY_BASE_HI,
					       S_030944_BASE_HI(tf_va >> 40));
		}
		radeon_set_uconfig_reg(cs, R_03093C_VGT_HS_OFFCHIP_PARAM,
				       hs_offchip_param);
	} else {
		radeon_set_config_reg(cs, R_008988_VGT_TF_RING_SIZE,
				      S_008988_SIZE(tf_ring_size / 4));
		radeon_set_config_reg(cs, R_0089B8_VGT_TF_MEMORY_BASE,
				      tf_va >> 8);
		radeon_set_config_reg(cs, R_0089B0_VGT_HS_OFFCHIP_PARAM,
				     hs_offchip_param);
	}
}

static void
radv_emit_compute_scratch(struct radv_queue *queue, struct radeon_cmdbuf *cs,
			  struct radeon_winsys_bo *compute_scratch_bo)
{
	uint64_t scratch_va;

	if (!compute_scratch_bo)
		return;

	scratch_va = radv_buffer_get_va(compute_scratch_bo);

	radv_cs_add_buffer(queue->device->ws, cs, compute_scratch_bo);

	radeon_set_sh_reg_seq(cs, R_00B900_COMPUTE_USER_DATA_0, 2);
	radeon_emit(cs, scratch_va);
	radeon_emit(cs, S_008F04_BASE_ADDRESS_HI(scratch_va >> 32) |
			S_008F04_SWIZZLE_ENABLE(1));
}

static void
radv_emit_global_shader_pointers(struct radv_queue *queue,
				 struct radeon_cmdbuf *cs,
				 struct radeon_winsys_bo *descriptor_bo)
{
	uint64_t va;

	if (!descriptor_bo)
		return;

	va = radv_buffer_get_va(descriptor_bo);

	radv_cs_add_buffer(queue->device->ws, cs, descriptor_bo);

	if (queue->device->physical_device->rad_info.chip_class >= GFX9) {
		uint32_t regs[] = {R_00B030_SPI_SHADER_USER_DATA_PS_0,
				   R_00B130_SPI_SHADER_USER_DATA_VS_0,
				   R_00B208_SPI_SHADER_USER_DATA_ADDR_LO_GS,
				   R_00B408_SPI_SHADER_USER_DATA_ADDR_LO_HS};

		for (int i = 0; i < ARRAY_SIZE(regs); ++i) {
			radv_emit_shader_pointer(queue->device, cs, regs[i],
						 va, true);
		}
	} else {
		uint32_t regs[] = {R_00B030_SPI_SHADER_USER_DATA_PS_0,
				   R_00B130_SPI_SHADER_USER_DATA_VS_0,
				   R_00B230_SPI_SHADER_USER_DATA_GS_0,
				   R_00B330_SPI_SHADER_USER_DATA_ES_0,
				   R_00B430_SPI_SHADER_USER_DATA_HS_0,
				   R_00B530_SPI_SHADER_USER_DATA_LS_0};

		for (int i = 0; i < ARRAY_SIZE(regs); ++i) {
			radv_emit_shader_pointer(queue->device, cs, regs[i],
						 va, true);
		}
	}
}

static void
radv_init_graphics_state(struct radeon_cmdbuf *cs, struct radv_queue *queue)
{
	struct radv_device *device = queue->device;

	if (device->gfx_init) {
		uint64_t va = radv_buffer_get_va(device->gfx_init);

		radeon_emit(cs, PKT3(PKT3_INDIRECT_BUFFER_CIK, 2, 0));
		radeon_emit(cs, va);
		radeon_emit(cs, va >> 32);
		radeon_emit(cs, device->gfx_init_size_dw & 0xffff);

		radv_cs_add_buffer(device->ws, cs, device->gfx_init);
	} else {
		struct radv_physical_device *physical_device = device->physical_device;
		si_emit_graphics(physical_device, cs);
	}
}

static void
radv_init_compute_state(struct radeon_cmdbuf *cs, struct radv_queue *queue)
{
	struct radv_physical_device *physical_device = queue->device->physical_device;
	si_emit_compute(physical_device, cs);
}

static VkResult
radv_get_preamble_cs(struct radv_queue *queue,
                     uint32_t scratch_size,
                     uint32_t compute_scratch_size,
		     uint32_t esgs_ring_size,
		     uint32_t gsvs_ring_size,
		     bool needs_tess_rings,
		     bool needs_sample_positions,
		     struct radeon_cmdbuf **initial_full_flush_preamble_cs,
                     struct radeon_cmdbuf **initial_preamble_cs,
                     struct radeon_cmdbuf **continue_preamble_cs)
{
	struct radeon_winsys_bo *scratch_bo = NULL;
	struct radeon_winsys_bo *descriptor_bo = NULL;
	struct radeon_winsys_bo *compute_scratch_bo = NULL;
	struct radeon_winsys_bo *esgs_ring_bo = NULL;
	struct radeon_winsys_bo *gsvs_ring_bo = NULL;
	struct radeon_winsys_bo *tess_rings_bo = NULL;
	struct radeon_cmdbuf *dest_cs[3] = {0};
	bool add_tess_rings = false, add_sample_positions = false;
	unsigned tess_factor_ring_size = 0, tess_offchip_ring_size = 0;
	unsigned max_offchip_buffers;
	unsigned hs_offchip_param = 0;
	unsigned tess_offchip_ring_offset;
	uint32_t ring_bo_flags = RADEON_FLAG_NO_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING;
	if (!queue->has_tess_rings) {
		if (needs_tess_rings)
			add_tess_rings = true;
	}
	if (!queue->has_sample_positions) {
		if (needs_sample_positions)
			add_sample_positions = true;
	}
	tess_factor_ring_size = 32768 * queue->device->physical_device->rad_info.max_se;
	hs_offchip_param = radv_get_hs_offchip_param(queue->device,
						     &max_offchip_buffers);
	tess_offchip_ring_offset = align(tess_factor_ring_size, 64 * 1024);
	tess_offchip_ring_size = max_offchip_buffers *
		queue->device->tess_offchip_block_dw_size * 4;

	if (scratch_size <= queue->scratch_size &&
	    compute_scratch_size <= queue->compute_scratch_size &&
	    esgs_ring_size <= queue->esgs_ring_size &&
	    gsvs_ring_size <= queue->gsvs_ring_size &&
	    !add_tess_rings && !add_sample_positions &&
	    queue->initial_preamble_cs) {
		*initial_full_flush_preamble_cs = queue->initial_full_flush_preamble_cs;
		*initial_preamble_cs = queue->initial_preamble_cs;
		*continue_preamble_cs = queue->continue_preamble_cs;
		if (!scratch_size && !compute_scratch_size && !esgs_ring_size && !gsvs_ring_size)
			*continue_preamble_cs = NULL;
		return VK_SUCCESS;
	}

	if (scratch_size > queue->scratch_size) {
		scratch_bo = queue->device->ws->buffer_create(queue->device->ws,
		                                              scratch_size,
		                                              4096,
		                                              RADEON_DOMAIN_VRAM,
		                                              ring_bo_flags);
		if (!scratch_bo)
			goto fail;
	} else
		scratch_bo = queue->scratch_bo;

	if (compute_scratch_size > queue->compute_scratch_size) {
		compute_scratch_bo = queue->device->ws->buffer_create(queue->device->ws,
		                                                      compute_scratch_size,
		                                                      4096,
		                                                      RADEON_DOMAIN_VRAM,
		                                                      ring_bo_flags);
		if (!compute_scratch_bo)
			goto fail;

	} else
		compute_scratch_bo = queue->compute_scratch_bo;

	if (esgs_ring_size > queue->esgs_ring_size) {
		esgs_ring_bo = queue->device->ws->buffer_create(queue->device->ws,
								esgs_ring_size,
								4096,
								RADEON_DOMAIN_VRAM,
								ring_bo_flags);
		if (!esgs_ring_bo)
			goto fail;
	} else {
		esgs_ring_bo = queue->esgs_ring_bo;
		esgs_ring_size = queue->esgs_ring_size;
	}

	if (gsvs_ring_size > queue->gsvs_ring_size) {
		gsvs_ring_bo = queue->device->ws->buffer_create(queue->device->ws,
								gsvs_ring_size,
								4096,
								RADEON_DOMAIN_VRAM,
								ring_bo_flags);
		if (!gsvs_ring_bo)
			goto fail;
	} else {
		gsvs_ring_bo = queue->gsvs_ring_bo;
		gsvs_ring_size = queue->gsvs_ring_size;
	}

	if (add_tess_rings) {
		tess_rings_bo = queue->device->ws->buffer_create(queue->device->ws,
								 tess_offchip_ring_offset + tess_offchip_ring_size,
								 256,
								 RADEON_DOMAIN_VRAM,
								 ring_bo_flags);
		if (!tess_rings_bo)
			goto fail;
	} else {
		tess_rings_bo = queue->tess_rings_bo;
	}

	if (scratch_bo != queue->scratch_bo ||
	    esgs_ring_bo != queue->esgs_ring_bo ||
	    gsvs_ring_bo != queue->gsvs_ring_bo ||
	    tess_rings_bo != queue->tess_rings_bo ||
	    add_sample_positions) {
		uint32_t size = 0;
		if (gsvs_ring_bo || esgs_ring_bo ||
		    tess_rings_bo || add_sample_positions) {
			size = 112; /* 2 dword + 2 padding + 4 dword * 6 */
			if (add_sample_positions)
				size += 256; /* 32+16+8+4+2+1 samples * 4 * 2 = 248 bytes. */
		}
		else if (scratch_bo)
			size = 8; /* 2 dword */

		descriptor_bo = queue->device->ws->buffer_create(queue->device->ws,
		                                                 size,
		                                                 4096,
		                                                 RADEON_DOMAIN_VRAM,
		                                                 RADEON_FLAG_CPU_ACCESS |
								 RADEON_FLAG_NO_INTERPROCESS_SHARING |
								 RADEON_FLAG_READ_ONLY);
		if (!descriptor_bo)
			goto fail;
	} else
		descriptor_bo = queue->descriptor_bo;

	for(int i = 0; i < 3; ++i) {
		struct radeon_cmdbuf *cs = NULL;
		cs = queue->device->ws->cs_create(queue->device->ws,
						  queue->queue_family_index ? RING_COMPUTE : RING_GFX);
		if (!cs)
			goto fail;

		dest_cs[i] = cs;

		if (scratch_bo)
			radv_cs_add_buffer(queue->device->ws, cs, scratch_bo);

		/* Emit initial configuration. */
		switch (queue->queue_family_index) {
		case RADV_QUEUE_GENERAL:
			radv_init_graphics_state(cs, queue);
			break;
		case RADV_QUEUE_COMPUTE:
			radv_init_compute_state(cs, queue);
			break;
		case RADV_QUEUE_TRANSFER:
			break;
		}

		if (descriptor_bo != queue->descriptor_bo) {
			uint32_t *map = (uint32_t*)queue->device->ws->buffer_map(descriptor_bo);

			if (scratch_bo) {
				uint64_t scratch_va = radv_buffer_get_va(scratch_bo);
				uint32_t rsrc1 = S_008F04_BASE_ADDRESS_HI(scratch_va >> 32) |
				                 S_008F04_SWIZZLE_ENABLE(1);
				map[0] = scratch_va;
				map[1] = rsrc1;
			}

			if (esgs_ring_bo || gsvs_ring_bo || tess_rings_bo ||
			    add_sample_positions)
				fill_geom_tess_rings(queue, map, add_sample_positions,
						     esgs_ring_size, esgs_ring_bo,
						     gsvs_ring_size, gsvs_ring_bo,
						     tess_factor_ring_size,
						     tess_offchip_ring_offset,
						     tess_offchip_ring_size,
						     tess_rings_bo);

			queue->device->ws->buffer_unmap(descriptor_bo);
		}

		if (esgs_ring_bo || gsvs_ring_bo || tess_rings_bo)  {
			radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
			radeon_emit(cs, EVENT_TYPE(V_028A90_VS_PARTIAL_FLUSH) | EVENT_INDEX(4));
			radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
			radeon_emit(cs, EVENT_TYPE(V_028A90_VGT_FLUSH) | EVENT_INDEX(0));
		}

		radv_emit_gs_ring_sizes(queue, cs, esgs_ring_bo, esgs_ring_size,
					gsvs_ring_bo, gsvs_ring_size);
		radv_emit_tess_factor_ring(queue, cs, hs_offchip_param,
					   tess_factor_ring_size, tess_rings_bo);
		radv_emit_global_shader_pointers(queue, cs, descriptor_bo);
		radv_emit_compute_scratch(queue, cs, compute_scratch_bo);

		if (i == 0) {
			si_cs_emit_cache_flush(cs,
			                       queue->device->physical_device->rad_info.chip_class,
					       NULL, 0,
			                       queue->queue_family_index == RING_COMPUTE &&
			                         queue->device->physical_device->rad_info.chip_class >= CIK,
			                       (queue->queue_family_index == RADV_QUEUE_COMPUTE ? RADV_CMD_FLAG_CS_PARTIAL_FLUSH : (RADV_CMD_FLAG_CS_PARTIAL_FLUSH | RADV_CMD_FLAG_PS_PARTIAL_FLUSH)) |
			                       RADV_CMD_FLAG_INV_ICACHE |
			                       RADV_CMD_FLAG_INV_SMEM_L1 |
			                       RADV_CMD_FLAG_INV_VMEM_L1 |
			                       RADV_CMD_FLAG_INV_GLOBAL_L2 |
					       RADV_CMD_FLAG_START_PIPELINE_STATS, 0);
		} else if (i == 1) {
			si_cs_emit_cache_flush(cs,
			                       queue->device->physical_device->rad_info.chip_class,
					       NULL, 0,
			                       queue->queue_family_index == RING_COMPUTE &&
			                         queue->device->physical_device->rad_info.chip_class >= CIK,
			                       RADV_CMD_FLAG_INV_ICACHE |
			                       RADV_CMD_FLAG_INV_SMEM_L1 |
			                       RADV_CMD_FLAG_INV_VMEM_L1 |
			                       RADV_CMD_FLAG_INV_GLOBAL_L2 |
					       RADV_CMD_FLAG_START_PIPELINE_STATS, 0);
		}

		if (!queue->device->ws->cs_finalize(cs))
			goto fail;
	}

	if (queue->initial_full_flush_preamble_cs)
			queue->device->ws->cs_destroy(queue->initial_full_flush_preamble_cs);

	if (queue->initial_preamble_cs)
			queue->device->ws->cs_destroy(queue->initial_preamble_cs);

	if (queue->continue_preamble_cs)
			queue->device->ws->cs_destroy(queue->continue_preamble_cs);

	queue->initial_full_flush_preamble_cs = dest_cs[0];
	queue->initial_preamble_cs = dest_cs[1];
	queue->continue_preamble_cs = dest_cs[2];

	if (scratch_bo != queue->scratch_bo) {
		if (queue->scratch_bo)
			queue->device->ws->buffer_destroy(queue->scratch_bo);
		queue->scratch_bo = scratch_bo;
		queue->scratch_size = scratch_size;
	}

	if (compute_scratch_bo != queue->compute_scratch_bo) {
		if (queue->compute_scratch_bo)
			queue->device->ws->buffer_destroy(queue->compute_scratch_bo);
		queue->compute_scratch_bo = compute_scratch_bo;
		queue->compute_scratch_size = compute_scratch_size;
	}

	if (esgs_ring_bo != queue->esgs_ring_bo) {
		if (queue->esgs_ring_bo)
			queue->device->ws->buffer_destroy(queue->esgs_ring_bo);
		queue->esgs_ring_bo = esgs_ring_bo;
		queue->esgs_ring_size = esgs_ring_size;
	}

	if (gsvs_ring_bo != queue->gsvs_ring_bo) {
		if (queue->gsvs_ring_bo)
			queue->device->ws->buffer_destroy(queue->gsvs_ring_bo);
		queue->gsvs_ring_bo = gsvs_ring_bo;
		queue->gsvs_ring_size = gsvs_ring_size;
	}

	if (tess_rings_bo != queue->tess_rings_bo) {
		queue->tess_rings_bo = tess_rings_bo;
		queue->has_tess_rings = true;
	}

	if (descriptor_bo != queue->descriptor_bo) {
		if (queue->descriptor_bo)
			queue->device->ws->buffer_destroy(queue->descriptor_bo);

		queue->descriptor_bo = descriptor_bo;
	}

	if (add_sample_positions)
		queue->has_sample_positions = true;

	*initial_full_flush_preamble_cs = queue->initial_full_flush_preamble_cs;
	*initial_preamble_cs = queue->initial_preamble_cs;
	*continue_preamble_cs = queue->continue_preamble_cs;
	if (!scratch_size && !compute_scratch_size && !esgs_ring_size && !gsvs_ring_size)
			*continue_preamble_cs = NULL;
	return VK_SUCCESS;
fail:
	for (int i = 0; i < ARRAY_SIZE(dest_cs); ++i)
		if (dest_cs[i])
			queue->device->ws->cs_destroy(dest_cs[i]);
	if (descriptor_bo && descriptor_bo != queue->descriptor_bo)
		queue->device->ws->buffer_destroy(descriptor_bo);
	if (scratch_bo && scratch_bo != queue->scratch_bo)
		queue->device->ws->buffer_destroy(scratch_bo);
	if (compute_scratch_bo && compute_scratch_bo != queue->compute_scratch_bo)
		queue->device->ws->buffer_destroy(compute_scratch_bo);
	if (esgs_ring_bo && esgs_ring_bo != queue->esgs_ring_bo)
		queue->device->ws->buffer_destroy(esgs_ring_bo);
	if (gsvs_ring_bo && gsvs_ring_bo != queue->gsvs_ring_bo)
		queue->device->ws->buffer_destroy(gsvs_ring_bo);
	if (tess_rings_bo && tess_rings_bo != queue->tess_rings_bo)
		queue->device->ws->buffer_destroy(tess_rings_bo);
	return vk_error(queue->device->instance, VK_ERROR_OUT_OF_DEVICE_MEMORY);
}

static VkResult radv_alloc_sem_counts(struct radv_instance *instance,
				      struct radv_winsys_sem_counts *counts,
				      int num_sems,
				      const VkSemaphore *sems,
				      VkFence _fence,
				      bool reset_temp)
{
	int syncobj_idx = 0, sem_idx = 0;

	if (num_sems == 0 && _fence == VK_NULL_HANDLE)
		return VK_SUCCESS;

	for (uint32_t i = 0; i < num_sems; i++) {
		RADV_FROM_HANDLE(radv_semaphore, sem, sems[i]);

		if (sem->temp_syncobj || sem->syncobj)
			counts->syncobj_count++;
		else
			counts->sem_count++;
	}

	if (_fence != VK_NULL_HANDLE) {
		RADV_FROM_HANDLE(radv_fence, fence, _fence);
		if (fence->temp_syncobj || fence->syncobj)
			counts->syncobj_count++;
	}

	if (counts->syncobj_count) {
		counts->syncobj = (uint32_t *)malloc(sizeof(uint32_t) * counts->syncobj_count);
		if (!counts->syncobj)
			return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
	}

	if (counts->sem_count) {
		counts->sem = (struct radeon_winsys_sem **)malloc(sizeof(struct radeon_winsys_sem *) * counts->sem_count);
		if (!counts->sem) {
			free(counts->syncobj);
			return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
		}
	}

	for (uint32_t i = 0; i < num_sems; i++) {
		RADV_FROM_HANDLE(radv_semaphore, sem, sems[i]);

		if (sem->temp_syncobj) {
			counts->syncobj[syncobj_idx++] = sem->temp_syncobj;
		}
		else if (sem->syncobj)
			counts->syncobj[syncobj_idx++] = sem->syncobj;
		else {
			assert(sem->sem);
			counts->sem[sem_idx++] = sem->sem;
		}
	}

	if (_fence != VK_NULL_HANDLE) {
		RADV_FROM_HANDLE(radv_fence, fence, _fence);
		if (fence->temp_syncobj)
			counts->syncobj[syncobj_idx++] = fence->temp_syncobj;
		else if (fence->syncobj)
			counts->syncobj[syncobj_idx++] = fence->syncobj;
	}

	return VK_SUCCESS;
}

static void
radv_free_sem_info(struct radv_winsys_sem_info *sem_info)
{
	free(sem_info->wait.syncobj);
	free(sem_info->wait.sem);
	free(sem_info->signal.syncobj);
	free(sem_info->signal.sem);
}


static void radv_free_temp_syncobjs(struct radv_device *device,
				    int num_sems,
				    const VkSemaphore *sems)
{
	for (uint32_t i = 0; i < num_sems; i++) {
		RADV_FROM_HANDLE(radv_semaphore, sem, sems[i]);

		if (sem->temp_syncobj) {
			device->ws->destroy_syncobj(device->ws, sem->temp_syncobj);
			sem->temp_syncobj = 0;
		}
	}
}

static VkResult
radv_alloc_sem_info(struct radv_instance *instance,
		    struct radv_winsys_sem_info *sem_info,
		    int num_wait_sems,
		    const VkSemaphore *wait_sems,
		    int num_signal_sems,
		    const VkSemaphore *signal_sems,
		    VkFence fence)
{
	VkResult ret;
	memset(sem_info, 0, sizeof(*sem_info));

	ret = radv_alloc_sem_counts(instance, &sem_info->wait, num_wait_sems, wait_sems, VK_NULL_HANDLE, true);
	if (ret)
		return ret;
	ret = radv_alloc_sem_counts(instance, &sem_info->signal, num_signal_sems, signal_sems, fence, false);
	if (ret)
		radv_free_sem_info(sem_info);

	/* caller can override these */
	sem_info->cs_emit_wait = true;
	sem_info->cs_emit_signal = true;
	return ret;
}

/* Signals fence as soon as all the work currently put on queue is done. */
static VkResult radv_signal_fence(struct radv_queue *queue,
                              struct radv_fence *fence)
{
	int ret;
	VkResult result;
	struct radv_winsys_sem_info sem_info;

	result = radv_alloc_sem_info(queue->device->instance, &sem_info, 0, NULL, 0, NULL,
	                             radv_fence_to_handle(fence));
	if (result != VK_SUCCESS)
		return result;

	ret = queue->device->ws->cs_submit(queue->hw_ctx, queue->queue_idx,
	                                   &queue->device->empty_cs[queue->queue_family_index],
	                                   1, NULL, NULL, &sem_info, NULL,
	                                   false, fence->fence);
	radv_free_sem_info(&sem_info);

	if (ret)
		return vk_error(queue->device->instance, VK_ERROR_DEVICE_LOST);

	return VK_SUCCESS;
}

VkResult radv_QueueSubmit(
	VkQueue                                     _queue,
	uint32_t                                    submitCount,
	const VkSubmitInfo*                         pSubmits,
	VkFence                                     _fence)
{
	RADV_FROM_HANDLE(radv_queue, queue, _queue);
	RADV_FROM_HANDLE(radv_fence, fence, _fence);
	struct radeon_winsys_fence *base_fence = fence ? fence->fence : NULL;
	struct radeon_winsys_ctx *ctx = queue->hw_ctx;
	int ret;
	uint32_t max_cs_submission = queue->device->trace_bo ? 1 : UINT32_MAX;
	uint32_t scratch_size = 0;
	uint32_t compute_scratch_size = 0;
	uint32_t esgs_ring_size = 0, gsvs_ring_size = 0;
	struct radeon_cmdbuf *initial_preamble_cs = NULL, *initial_flush_preamble_cs = NULL, *continue_preamble_cs = NULL;
	VkResult result;
	bool fence_emitted = false;
	bool tess_rings_needed = false;
	bool sample_positions_needed = false;

	/* Do this first so failing to allocate scratch buffers can't result in
	 * partially executed submissions. */
	for (uint32_t i = 0; i < submitCount; i++) {
		for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++) {
			RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer,
					 pSubmits[i].pCommandBuffers[j]);

			scratch_size = MAX2(scratch_size, cmd_buffer->scratch_size_needed);
			compute_scratch_size = MAX2(compute_scratch_size,
			                            cmd_buffer->compute_scratch_size_needed);
			esgs_ring_size = MAX2(esgs_ring_size, cmd_buffer->esgs_ring_size_needed);
			gsvs_ring_size = MAX2(gsvs_ring_size, cmd_buffer->gsvs_ring_size_needed);
			tess_rings_needed |= cmd_buffer->tess_rings_needed;
			sample_positions_needed |= cmd_buffer->sample_positions_needed;
		}
	}

	result = radv_get_preamble_cs(queue, scratch_size, compute_scratch_size,
	                              esgs_ring_size, gsvs_ring_size, tess_rings_needed,
				      sample_positions_needed, &initial_flush_preamble_cs,
	                              &initial_preamble_cs, &continue_preamble_cs);
	if (result != VK_SUCCESS)
		return result;

	for (uint32_t i = 0; i < submitCount; i++) {
		struct radeon_cmdbuf **cs_array;
		bool do_flush = !i || pSubmits[i].pWaitDstStageMask;
		bool can_patch = true;
		uint32_t advance;
		struct radv_winsys_sem_info sem_info;

		result = radv_alloc_sem_info(queue->device->instance,
					     &sem_info,
					     pSubmits[i].waitSemaphoreCount,
					     pSubmits[i].pWaitSemaphores,
					     pSubmits[i].signalSemaphoreCount,
					     pSubmits[i].pSignalSemaphores,
					     _fence);
		if (result != VK_SUCCESS)
			return result;

		if (!pSubmits[i].commandBufferCount) {
			if (pSubmits[i].waitSemaphoreCount || pSubmits[i].signalSemaphoreCount) {
				ret = queue->device->ws->cs_submit(ctx, queue->queue_idx,
								   &queue->device->empty_cs[queue->queue_family_index],
								   1, NULL, NULL,
								   &sem_info, NULL,
								   false, base_fence);
				if (ret) {
					radv_loge("failed to submit CS %d\n", i);
					abort();
				}
				fence_emitted = true;
			}
			radv_free_sem_info(&sem_info);
			continue;
		}

		cs_array = malloc(sizeof(struct radeon_cmdbuf *) *
					        (pSubmits[i].commandBufferCount));

		for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++) {
			RADV_FROM_HANDLE(radv_cmd_buffer, cmd_buffer,
					 pSubmits[i].pCommandBuffers[j]);
			assert(cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);

			cs_array[j] = cmd_buffer->cs;
			if ((cmd_buffer->usage_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT))
				can_patch = false;

			cmd_buffer->status = RADV_CMD_BUFFER_STATUS_PENDING;
		}

		for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j += advance) {
			struct radeon_cmdbuf *initial_preamble = (do_flush && !j) ? initial_flush_preamble_cs : initial_preamble_cs;
			const struct radv_winsys_bo_list *bo_list = NULL;

			advance = MIN2(max_cs_submission,
				       pSubmits[i].commandBufferCount - j);

			if (queue->device->trace_bo)
				*queue->device->trace_id_ptr = 0;

			sem_info.cs_emit_wait = j == 0;
			sem_info.cs_emit_signal = j + advance == pSubmits[i].commandBufferCount;

			if (unlikely(queue->device->use_global_bo_list)) {
				pthread_mutex_lock(&queue->device->bo_list.mutex);
				bo_list = &queue->device->bo_list.list;
			}

			ret = queue->device->ws->cs_submit(ctx, queue->queue_idx, cs_array + j,
							advance, initial_preamble, continue_preamble_cs,
							&sem_info, bo_list,
							can_patch, base_fence);

			if (unlikely(queue->device->use_global_bo_list))
				pthread_mutex_unlock(&queue->device->bo_list.mutex);

			if (ret) {
				radv_loge("failed to submit CS %d\n", i);
				abort();
			}
			fence_emitted = true;
			if (queue->device->trace_bo) {
				radv_check_gpu_hangs(queue, cs_array[j]);
			}
		}

		radv_free_temp_syncobjs(queue->device,
					pSubmits[i].waitSemaphoreCount,
					pSubmits[i].pWaitSemaphores);
		radv_free_sem_info(&sem_info);
		free(cs_array);
	}

	if (fence) {
		if (!fence_emitted) {
			result = radv_signal_fence(queue, fence);
			if (result != VK_SUCCESS)
				return result;
		}
		fence->submitted = true;
	}

	return VK_SUCCESS;
}

VkResult radv_QueueWaitIdle(
	VkQueue                                     _queue)
{
	RADV_FROM_HANDLE(radv_queue, queue, _queue);

	queue->device->ws->ctx_wait_idle(queue->hw_ctx,
	                                 radv_queue_family_to_ring(queue->queue_family_index),
	                                 queue->queue_idx);
	return VK_SUCCESS;
}

VkResult radv_DeviceWaitIdle(
	VkDevice                                    _device)
{
	RADV_FROM_HANDLE(radv_device, device, _device);

	for (unsigned i = 0; i < RADV_MAX_QUEUE_FAMILIES; i++) {
		for (unsigned q = 0; q < device->queue_count[i]; q++) {
			radv_QueueWaitIdle(radv_queue_to_handle(&device->queues[i][q]));
		}
	}
	return VK_SUCCESS;
}

VkResult radv_EnumerateInstanceExtensionProperties(
    const char*                                 pLayerName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties)
{
	VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);

	for (int i = 0; i < RADV_INSTANCE_EXTENSION_COUNT; i++) {
		if (radv_supported_instance_extensions.extensions[i]) {
			vk_outarray_append(&out, prop) {
				*prop = radv_instance_extensions[i];
			}
		}
	}

	return vk_outarray_status(&out);
}

VkResult radv_EnumerateDeviceExtensionProperties(
    VkPhysicalDevice                            physicalDevice,
    const char*                                 pLayerName,
    uint32_t*                                   pPropertyCount,
    VkExtensionProperties*                      pProperties)
{
	RADV_FROM_HANDLE(radv_physical_device, device, physicalDevice);
	VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);

	for (int i = 0; i < RADV_DEVICE_EXTENSION_COUNT; i++) {
		if (device->supported_extensions.extensions[i]) {
			vk_outarray_append(&out, prop) {
				*prop = radv_device_extensions[i];
			}
		}
	}

	return vk_outarray_status(&out);
}

PFN_vkVoidFunction radv_GetInstanceProcAddr(
	VkInstance                                  _instance,
	const char*                                 pName)
{
	RADV_FROM_HANDLE(radv_instance, instance, _instance);

	return radv_lookup_entrypoint_checked(pName,
	                                      instance ? instance->apiVersion : 0,
	                                      instance ? &instance->enabled_extensions : NULL,
	                                      NULL);
}

/* The loader wants us to expose a second GetInstanceProcAddr function
 * to work around certain LD_PRELOAD issues seen in apps.
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
	VkInstance                                  instance,
	const char*                                 pName);

PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(
	VkInstance                                  instance,
	const char*                                 pName)
{
	return radv_GetInstanceProcAddr(instance, pName);
}

PFN_vkVoidFunction radv_GetDeviceProcAddr(
	VkDevice                                    _device,
	const char*                                 pName)
{
	RADV_FROM_HANDLE(radv_device, device, _device);

	return radv_lookup_entrypoint_checked(pName,
	                                      device->instance->apiVersion,
	                                      &device->instance->enabled_extensions,
	                                      &device->enabled_extensions);
}

bool radv_get_memory_fd(struct radv_device *device,
			struct radv_device_memory *memory,
			int *pFD)
{
	struct radeon_bo_metadata metadata;

	if (memory->image) {
		radv_init_metadata(device, memory->image, &metadata);
		device->ws->buffer_set_metadata(memory->bo, &metadata);
	}

	return device->ws->buffer_get_fd(device->ws, memory->bo,
					 pFD);
}

static VkResult radv_alloc_memory(struct radv_device *device,
				  const VkMemoryAllocateInfo*     pAllocateInfo,
				  const VkAllocationCallbacks*    pAllocator,
				  VkDeviceMemory*                 pMem)
{
	struct radv_device_memory *mem;
	VkResult result;
	enum radeon_bo_domain domain;
	uint32_t flags = 0;
	enum radv_mem_type mem_type_index = device->physical_device->mem_type_indices[pAllocateInfo->memoryTypeIndex];

	assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

	if (pAllocateInfo->allocationSize == 0) {
		/* Apparently, this is allowed */
		*pMem = VK_NULL_HANDLE;
		return VK_SUCCESS;
	}

	const VkImportMemoryFdInfoKHR *import_info =
		vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);
	const VkMemoryDedicatedAllocateInfoKHR *dedicate_info =
		vk_find_struct_const(pAllocateInfo->pNext, MEMORY_DEDICATED_ALLOCATE_INFO_KHR);
	const VkExportMemoryAllocateInfoKHR *export_info =
		vk_find_struct_const(pAllocateInfo->pNext, EXPORT_MEMORY_ALLOCATE_INFO_KHR);
	const VkImportMemoryHostPointerInfoEXT *host_ptr_info =
		vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_HOST_POINTER_INFO_EXT);

	const struct wsi_memory_allocate_info *wsi_info =
		vk_find_struct_const(pAllocateInfo->pNext, WSI_MEMORY_ALLOCATE_INFO_MESA);

	mem = vk_alloc2(&device->alloc, pAllocator, sizeof(*mem), 8,
			  VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (mem == NULL)
		return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

	if (wsi_info && wsi_info->implicit_sync)
		flags |= RADEON_FLAG_IMPLICIT_SYNC;

	if (dedicate_info) {
		mem->image = radv_image_from_handle(dedicate_info->image);
		mem->buffer = radv_buffer_from_handle(dedicate_info->buffer);
	} else {
		mem->image = NULL;
		mem->buffer = NULL;
	}

	mem->user_ptr = NULL;

	if (import_info) {
		assert(import_info->handleType ==
		       VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR ||
		       import_info->handleType ==
		       VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
		mem->bo = device->ws->buffer_from_fd(device->ws, import_info->fd,
						     NULL, NULL);
		if (!mem->bo) {
			result = VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR;
			goto fail;
		} else {
			close(import_info->fd);
		}
	} else if (host_ptr_info) {
		assert(host_ptr_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
		assert(mem_type_index == RADV_MEM_TYPE_GTT_CACHED);
		mem->bo = device->ws->buffer_from_ptr(device->ws, host_ptr_info->pHostPointer,
		                                      pAllocateInfo->allocationSize);
		if (!mem->bo) {
			result = VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR;
			goto fail;
		} else {
			mem->user_ptr = host_ptr_info->pHostPointer;
		}
	} else {
		uint64_t alloc_size = align_u64(pAllocateInfo->allocationSize, 4096);
		if (mem_type_index == RADV_MEM_TYPE_GTT_WRITE_COMBINE ||
		    mem_type_index == RADV_MEM_TYPE_GTT_CACHED)
			domain = RADEON_DOMAIN_GTT;
		else
			domain = RADEON_DOMAIN_VRAM;

		if (mem_type_index == RADV_MEM_TYPE_VRAM)
			flags |= RADEON_FLAG_NO_CPU_ACCESS;
		else
			flags |= RADEON_FLAG_CPU_ACCESS;

		if (mem_type_index == RADV_MEM_TYPE_GTT_WRITE_COMBINE)
			flags |= RADEON_FLAG_GTT_WC;

		if (!dedicate_info && !import_info && (!export_info || !export_info->handleTypes))
			flags |= RADEON_FLAG_NO_INTERPROCESS_SHARING;

		mem->bo = device->ws->buffer_create(device->ws, alloc_size, device->physical_device->rad_info.max_alignment,
		                                    domain, flags);

		if (!mem->bo) {
			result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
			goto fail;
		}
		mem->type_index = mem_type_index;
	}

	result = radv_bo_list_add(device, mem->bo);
	if (result != VK_SUCCESS)
		goto fail_bo;

	*pMem = radv_device_memory_to_handle(mem);

	return VK_SUCCESS;

fail_bo:
	device->ws->buffer_destroy(mem->bo);
fail:
	vk_free2(&device->alloc, pAllocator, mem);

	return result;
}

VkResult radv_AllocateMemory(
	VkDevice                                    _device,
	const VkMemoryAllocateInfo*                 pAllocateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkDeviceMemory*                             pMem)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	return radv_alloc_memory(device, pAllocateInfo, pAllocator, pMem);
}

void radv_FreeMemory(
	VkDevice                                    _device,
	VkDeviceMemory                              _mem,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_device_memory, mem, _mem);

	if (mem == NULL)
		return;

	radv_bo_list_remove(device, mem->bo);
	device->ws->buffer_destroy(mem->bo);
	mem->bo = NULL;

	vk_free2(&device->alloc, pAllocator, mem);
}

VkResult radv_MapMemory(
	VkDevice                                    _device,
	VkDeviceMemory                              _memory,
	VkDeviceSize                                offset,
	VkDeviceSize                                size,
	VkMemoryMapFlags                            flags,
	void**                                      ppData)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_device_memory, mem, _memory);

	if (mem == NULL) {
		*ppData = NULL;
		return VK_SUCCESS;
	}

	if (mem->user_ptr)
		*ppData = mem->user_ptr;
	else
		*ppData = device->ws->buffer_map(mem->bo);

	if (*ppData) {
		*ppData += offset;
		return VK_SUCCESS;
	}

	return vk_error(device->instance, VK_ERROR_MEMORY_MAP_FAILED);
}

void radv_UnmapMemory(
	VkDevice                                    _device,
	VkDeviceMemory                              _memory)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_device_memory, mem, _memory);

	if (mem == NULL)
		return;

	if (mem->user_ptr == NULL)
		device->ws->buffer_unmap(mem->bo);
}

VkResult radv_FlushMappedMemoryRanges(
	VkDevice                                    _device,
	uint32_t                                    memoryRangeCount,
	const VkMappedMemoryRange*                  pMemoryRanges)
{
	return VK_SUCCESS;
}

VkResult radv_InvalidateMappedMemoryRanges(
	VkDevice                                    _device,
	uint32_t                                    memoryRangeCount,
	const VkMappedMemoryRange*                  pMemoryRanges)
{
	return VK_SUCCESS;
}

void radv_GetBufferMemoryRequirements(
	VkDevice                                    _device,
	VkBuffer                                    _buffer,
	VkMemoryRequirements*                       pMemoryRequirements)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_buffer, buffer, _buffer);

	pMemoryRequirements->memoryTypeBits = (1u << device->physical_device->memory_properties.memoryTypeCount) - 1;

	if (buffer->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT)
		pMemoryRequirements->alignment = 4096;
	else
		pMemoryRequirements->alignment = 16;

	pMemoryRequirements->size = align64(buffer->size, pMemoryRequirements->alignment);
}

void radv_GetBufferMemoryRequirements2(
	VkDevice                                     device,
	const VkBufferMemoryRequirementsInfo2KHR*    pInfo,
	VkMemoryRequirements2KHR*                    pMemoryRequirements)
{
	radv_GetBufferMemoryRequirements(device, pInfo->buffer,
                                        &pMemoryRequirements->memoryRequirements);
	RADV_FROM_HANDLE(radv_buffer, buffer, pInfo->buffer);
	vk_foreach_struct(ext, pMemoryRequirements->pNext) {
		switch (ext->sType) {
		case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS_KHR: {
			VkMemoryDedicatedRequirementsKHR *req =
			               (VkMemoryDedicatedRequirementsKHR *) ext;
			req->requiresDedicatedAllocation = buffer->shareable;
			req->prefersDedicatedAllocation = req->requiresDedicatedAllocation;
			break;
		}
		default:
			break;
		}
	}
}

void radv_GetImageMemoryRequirements(
	VkDevice                                    _device,
	VkImage                                     _image,
	VkMemoryRequirements*                       pMemoryRequirements)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_image, image, _image);

	pMemoryRequirements->memoryTypeBits = (1u << device->physical_device->memory_properties.memoryTypeCount) - 1;

	pMemoryRequirements->size = image->size;
	pMemoryRequirements->alignment = image->alignment;
}

void radv_GetImageMemoryRequirements2(
	VkDevice                                    device,
	const VkImageMemoryRequirementsInfo2KHR*    pInfo,
	VkMemoryRequirements2KHR*                   pMemoryRequirements)
{
	radv_GetImageMemoryRequirements(device, pInfo->image,
                                        &pMemoryRequirements->memoryRequirements);

	RADV_FROM_HANDLE(radv_image, image, pInfo->image);

	vk_foreach_struct(ext, pMemoryRequirements->pNext) {
		switch (ext->sType) {
		case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS_KHR: {
			VkMemoryDedicatedRequirementsKHR *req =
			               (VkMemoryDedicatedRequirementsKHR *) ext;
			req->requiresDedicatedAllocation = image->shareable;
			req->prefersDedicatedAllocation = req->requiresDedicatedAllocation;
			break;
		}
		default:
			break;
		}
	}
}

void radv_GetImageSparseMemoryRequirements(
	VkDevice                                    device,
	VkImage                                     image,
	uint32_t*                                   pSparseMemoryRequirementCount,
	VkSparseImageMemoryRequirements*            pSparseMemoryRequirements)
{
	stub();
}

void radv_GetImageSparseMemoryRequirements2(
	VkDevice                                    device,
	const VkImageSparseMemoryRequirementsInfo2KHR* pInfo,
	uint32_t*                                   pSparseMemoryRequirementCount,
	VkSparseImageMemoryRequirements2KHR*            pSparseMemoryRequirements)
{
	stub();
}

void radv_GetDeviceMemoryCommitment(
	VkDevice                                    device,
	VkDeviceMemory                              memory,
	VkDeviceSize*                               pCommittedMemoryInBytes)
{
	*pCommittedMemoryInBytes = 0;
}

VkResult radv_BindBufferMemory2(VkDevice device,
                                uint32_t bindInfoCount,
                                const VkBindBufferMemoryInfoKHR *pBindInfos)
{
	for (uint32_t i = 0; i < bindInfoCount; ++i) {
		RADV_FROM_HANDLE(radv_device_memory, mem, pBindInfos[i].memory);
		RADV_FROM_HANDLE(radv_buffer, buffer, pBindInfos[i].buffer);

		if (mem) {
			buffer->bo = mem->bo;
			buffer->offset = pBindInfos[i].memoryOffset;
		} else {
			buffer->bo = NULL;
		}
	}
	return VK_SUCCESS;
}

VkResult radv_BindBufferMemory(
	VkDevice                                    device,
	VkBuffer                                    buffer,
	VkDeviceMemory                              memory,
	VkDeviceSize                                memoryOffset)
{
	const VkBindBufferMemoryInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO_KHR,
		.buffer = buffer,
		.memory = memory,
		.memoryOffset = memoryOffset
	};

	return radv_BindBufferMemory2(device, 1, &info);
}

VkResult radv_BindImageMemory2(VkDevice device,
                               uint32_t bindInfoCount,
                               const VkBindImageMemoryInfoKHR *pBindInfos)
{
	for (uint32_t i = 0; i < bindInfoCount; ++i) {
		RADV_FROM_HANDLE(radv_device_memory, mem, pBindInfos[i].memory);
		RADV_FROM_HANDLE(radv_image, image, pBindInfos[i].image);

		if (mem) {
			image->bo = mem->bo;
			image->offset = pBindInfos[i].memoryOffset;
		} else {
			image->bo = NULL;
			image->offset = 0;
		}
	}
	return VK_SUCCESS;
}


VkResult radv_BindImageMemory(
	VkDevice                                    device,
	VkImage                                     image,
	VkDeviceMemory                              memory,
	VkDeviceSize                                memoryOffset)
{
	const VkBindImageMemoryInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO_KHR,
		.image = image,
		.memory = memory,
		.memoryOffset = memoryOffset
	};

	return radv_BindImageMemory2(device, 1, &info);
}


static void
radv_sparse_buffer_bind_memory(struct radv_device *device,
                               const VkSparseBufferMemoryBindInfo *bind)
{
	RADV_FROM_HANDLE(radv_buffer, buffer, bind->buffer);

	for (uint32_t i = 0; i < bind->bindCount; ++i) {
		struct radv_device_memory *mem = NULL;

		if (bind->pBinds[i].memory != VK_NULL_HANDLE)
			mem = radv_device_memory_from_handle(bind->pBinds[i].memory);

		device->ws->buffer_virtual_bind(buffer->bo,
		                                bind->pBinds[i].resourceOffset,
		                                bind->pBinds[i].size,
		                                mem ? mem->bo : NULL,
		                                bind->pBinds[i].memoryOffset);
	}
}

static void
radv_sparse_image_opaque_bind_memory(struct radv_device *device,
                                     const VkSparseImageOpaqueMemoryBindInfo *bind)
{
	RADV_FROM_HANDLE(radv_image, image, bind->image);

	for (uint32_t i = 0; i < bind->bindCount; ++i) {
		struct radv_device_memory *mem = NULL;

		if (bind->pBinds[i].memory != VK_NULL_HANDLE)
			mem = radv_device_memory_from_handle(bind->pBinds[i].memory);

		device->ws->buffer_virtual_bind(image->bo,
		                                bind->pBinds[i].resourceOffset,
		                                bind->pBinds[i].size,
		                                mem ? mem->bo : NULL,
		                                bind->pBinds[i].memoryOffset);
	}
}

 VkResult radv_QueueBindSparse(
	VkQueue                                     _queue,
	uint32_t                                    bindInfoCount,
	const VkBindSparseInfo*                     pBindInfo,
	VkFence                                     _fence)
{
	RADV_FROM_HANDLE(radv_fence, fence, _fence);
	RADV_FROM_HANDLE(radv_queue, queue, _queue);
	struct radeon_winsys_fence *base_fence = fence ? fence->fence : NULL;
	bool fence_emitted = false;
	VkResult result;
	int ret;

	for (uint32_t i = 0; i < bindInfoCount; ++i) {
		struct radv_winsys_sem_info sem_info;
		for (uint32_t j = 0; j < pBindInfo[i].bufferBindCount; ++j) {
			radv_sparse_buffer_bind_memory(queue->device,
			                               pBindInfo[i].pBufferBinds + j);
		}

		for (uint32_t j = 0; j < pBindInfo[i].imageOpaqueBindCount; ++j) {
			radv_sparse_image_opaque_bind_memory(queue->device,
			                                     pBindInfo[i].pImageOpaqueBinds + j);
		}

		VkResult result;
		result = radv_alloc_sem_info(queue->device->instance,
					     &sem_info,
					     pBindInfo[i].waitSemaphoreCount,
					     pBindInfo[i].pWaitSemaphores,
					     pBindInfo[i].signalSemaphoreCount,
					     pBindInfo[i].pSignalSemaphores,
					     _fence);
		if (result != VK_SUCCESS)
			return result;

		if (pBindInfo[i].waitSemaphoreCount || pBindInfo[i].signalSemaphoreCount) {
			ret = queue->device->ws->cs_submit(queue->hw_ctx, queue->queue_idx,
							  &queue->device->empty_cs[queue->queue_family_index],
							  1, NULL, NULL,
							  &sem_info, NULL,
							  false, base_fence);
			if (ret) {
				radv_loge("failed to submit CS %d\n", i);
				abort();
			}

			fence_emitted = true;
			if (fence)
				fence->submitted = true;
		}

		radv_free_sem_info(&sem_info);

	}

	if (fence) {
		if (!fence_emitted) {
			result = radv_signal_fence(queue, fence);
			if (result != VK_SUCCESS)
				return result;
		}
		fence->submitted = true;
	}

	return VK_SUCCESS;
}

VkResult radv_CreateFence(
	VkDevice                                    _device,
	const VkFenceCreateInfo*                    pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkFence*                                    pFence)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	const VkExportFenceCreateInfoKHR *export =
		vk_find_struct_const(pCreateInfo->pNext, EXPORT_FENCE_CREATE_INFO_KHR);
	VkExternalFenceHandleTypeFlagsKHR handleTypes =
		export ? export->handleTypes : 0;

	struct radv_fence *fence = vk_alloc2(&device->alloc, pAllocator,
					       sizeof(*fence), 8,
					       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

	if (!fence)
		return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

	fence->fence_wsi = NULL;
	fence->submitted = false;
	fence->signalled = !!(pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT);
	fence->temp_syncobj = 0;
	if (device->always_use_syncobj || handleTypes) {
		int ret = device->ws->create_syncobj(device->ws, &fence->syncobj);
		if (ret) {
			vk_free2(&device->alloc, pAllocator, fence);
			return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
		}
		if (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) {
			device->ws->signal_syncobj(device->ws, fence->syncobj);
		}
		fence->fence = NULL;
	} else {
		fence->fence = device->ws->create_fence();
		if (!fence->fence) {
			vk_free2(&device->alloc, pAllocator, fence);
			return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
		}
		fence->syncobj = 0;
	}

	*pFence = radv_fence_to_handle(fence);

	return VK_SUCCESS;
}

void radv_DestroyFence(
	VkDevice                                    _device,
	VkFence                                     _fence,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_fence, fence, _fence);

	if (!fence)
		return;

	if (fence->temp_syncobj)
		device->ws->destroy_syncobj(device->ws, fence->temp_syncobj);
	if (fence->syncobj)
		device->ws->destroy_syncobj(device->ws, fence->syncobj);
	if (fence->fence)
		device->ws->destroy_fence(fence->fence);
	if (fence->fence_wsi)
		fence->fence_wsi->destroy(fence->fence_wsi);
	vk_free2(&device->alloc, pAllocator, fence);
}


static uint64_t radv_get_current_time()
{
	struct timespec tv;
	clock_gettime(CLOCK_MONOTONIC, &tv);
	return tv.tv_nsec + tv.tv_sec*1000000000ull;
}

static uint64_t radv_get_absolute_timeout(uint64_t timeout)
{
	uint64_t current_time = radv_get_current_time();

	timeout = MIN2(UINT64_MAX - current_time, timeout);

	return current_time + timeout;
}


static bool radv_all_fences_plain_and_submitted(uint32_t fenceCount, const VkFence *pFences)
{
	for (uint32_t i = 0; i < fenceCount; ++i) {
		RADV_FROM_HANDLE(radv_fence, fence, pFences[i]);
		if (fence->fence == NULL || fence->syncobj ||
		    fence->temp_syncobj ||
		    (!fence->signalled && !fence->submitted))
			return false;
	}
	return true;
}

static bool radv_all_fences_syncobj(uint32_t fenceCount, const VkFence *pFences)
{
	for (uint32_t i = 0; i < fenceCount; ++i) {
		RADV_FROM_HANDLE(radv_fence, fence, pFences[i]);
		if (fence->syncobj == 0 && fence->temp_syncobj == 0)
			return false;
	}
	return true;
}

VkResult radv_WaitForFences(
	VkDevice                                    _device,
	uint32_t                                    fenceCount,
	const VkFence*                              pFences,
	VkBool32                                    waitAll,
	uint64_t                                    timeout)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	timeout = radv_get_absolute_timeout(timeout);

	if (device->always_use_syncobj &&
	    radv_all_fences_syncobj(fenceCount, pFences))
	{
		uint32_t *handles = malloc(sizeof(uint32_t) * fenceCount);
		if (!handles)
			return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

		for (uint32_t i = 0; i < fenceCount; ++i) {
			RADV_FROM_HANDLE(radv_fence, fence, pFences[i]);
			handles[i] = fence->temp_syncobj ? fence->temp_syncobj : fence->syncobj;
		}

		bool success = device->ws->wait_syncobj(device->ws, handles, fenceCount, waitAll, timeout);

		free(handles);
		return success ? VK_SUCCESS : VK_TIMEOUT;
	}

	if (!waitAll && fenceCount > 1) {
		/* Not doing this by default for waitAll, due to needing to allocate twice. */
		if (device->physical_device->rad_info.drm_minor >= 10 && radv_all_fences_plain_and_submitted(fenceCount, pFences)) {
			uint32_t wait_count = 0;
			struct radeon_winsys_fence **fences = malloc(sizeof(struct radeon_winsys_fence *) * fenceCount);
			if (!fences)
				return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

			for (uint32_t i = 0; i < fenceCount; ++i) {
				RADV_FROM_HANDLE(radv_fence, fence, pFences[i]);

				if (fence->signalled) {
					free(fences);
					return VK_SUCCESS;
				}

				fences[wait_count++] = fence->fence;
			}

			bool success = device->ws->fences_wait(device->ws, fences, wait_count,
							       waitAll, timeout - radv_get_current_time());

			free(fences);
			return success ? VK_SUCCESS : VK_TIMEOUT;
		}

		while(radv_get_current_time() <= timeout) {
			for (uint32_t i = 0; i < fenceCount; ++i) {
				if (radv_GetFenceStatus(_device, pFences[i]) == VK_SUCCESS)
					return VK_SUCCESS;
			}
		}
		return VK_TIMEOUT;
	}

	for (uint32_t i = 0; i < fenceCount; ++i) {
		RADV_FROM_HANDLE(radv_fence, fence, pFences[i]);
		bool expired = false;

		if (fence->temp_syncobj) {
			if (!device->ws->wait_syncobj(device->ws, &fence->temp_syncobj, 1, true, timeout))
				return VK_TIMEOUT;
			continue;
		}

		if (fence->syncobj) {
			if (!device->ws->wait_syncobj(device->ws, &fence->syncobj, 1, true, timeout))
				return VK_TIMEOUT;
			continue;
		}

		if (fence->signalled)
			continue;

		if (fence->fence) {
			if (!fence->submitted) {
				while(radv_get_current_time() <= timeout &&
				      !fence->submitted)
					/* Do nothing */;

				if (!fence->submitted)
					return VK_TIMEOUT;

				/* Recheck as it may have been set by
				 * submitting operations. */

				if (fence->signalled)
					continue;
			}

			expired = device->ws->fence_wait(device->ws,
							 fence->fence,
							 true, timeout);
			if (!expired)
				return VK_TIMEOUT;
		}

		if (fence->fence_wsi) {
			VkResult result = fence->fence_wsi->wait(fence->fence_wsi, timeout);
			if (result != VK_SUCCESS)
				return result;
		}

		fence->signalled = true;
	}

	return VK_SUCCESS;
}

VkResult radv_ResetFences(VkDevice _device,
			  uint32_t fenceCount,
			  const VkFence *pFences)
{
	RADV_FROM_HANDLE(radv_device, device, _device);

	for (unsigned i = 0; i < fenceCount; ++i) {
		RADV_FROM_HANDLE(radv_fence, fence, pFences[i]);
		fence->submitted = fence->signalled = false;

		/* Per spec, we first restore the permanent payload, and then reset, so
		 * having a temp syncobj should not skip resetting the permanent syncobj. */
		if (fence->temp_syncobj) {
			device->ws->destroy_syncobj(device->ws, fence->temp_syncobj);
			fence->temp_syncobj = 0;
		}

		if (fence->syncobj) {
			device->ws->reset_syncobj(device->ws, fence->syncobj);
		}
	}

	return VK_SUCCESS;
}

VkResult radv_GetFenceStatus(VkDevice _device, VkFence _fence)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_fence, fence, _fence);

	if (fence->temp_syncobj) {
			bool success = device->ws->wait_syncobj(device->ws, &fence->temp_syncobj, 1, true, 0);
			return success ? VK_SUCCESS : VK_NOT_READY;
	}

	if (fence->syncobj) {
			bool success = device->ws->wait_syncobj(device->ws, &fence->syncobj, 1, true, 0);
			return success ? VK_SUCCESS : VK_NOT_READY;
	}

	if (fence->signalled)
		return VK_SUCCESS;
	if (!fence->submitted)
		return VK_NOT_READY;
	if (fence->fence) {
		if (!device->ws->fence_wait(device->ws, fence->fence, false, 0))
			return VK_NOT_READY;
	}
	if (fence->fence_wsi) {
		VkResult result = fence->fence_wsi->wait(fence->fence_wsi, 0);

		if (result != VK_SUCCESS) {
			if (result == VK_TIMEOUT)
				return VK_NOT_READY;
			return result;
		}
	}
	return VK_SUCCESS;
}


// Queue semaphore functions

VkResult radv_CreateSemaphore(
	VkDevice                                    _device,
	const VkSemaphoreCreateInfo*                pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkSemaphore*                                pSemaphore)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	const VkExportSemaphoreCreateInfoKHR *export =
		vk_find_struct_const(pCreateInfo->pNext, EXPORT_SEMAPHORE_CREATE_INFO_KHR);
	VkExternalSemaphoreHandleTypeFlagsKHR handleTypes =
		export ? export->handleTypes : 0;

	struct radv_semaphore *sem = vk_alloc2(&device->alloc, pAllocator,
					       sizeof(*sem), 8,
					       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!sem)
		return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

	sem->temp_syncobj = 0;
	/* create a syncobject if we are going to export this semaphore */
	if (device->always_use_syncobj || handleTypes) {
		assert (device->physical_device->rad_info.has_syncobj);
		int ret = device->ws->create_syncobj(device->ws, &sem->syncobj);
		if (ret) {
			vk_free2(&device->alloc, pAllocator, sem);
			return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
		}
		sem->sem = NULL;
	} else {
		sem->sem = device->ws->create_sem(device->ws);
		if (!sem->sem) {
			vk_free2(&device->alloc, pAllocator, sem);
			return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
		}
		sem->syncobj = 0;
	}

	*pSemaphore = radv_semaphore_to_handle(sem);
	return VK_SUCCESS;
}

void radv_DestroySemaphore(
	VkDevice                                    _device,
	VkSemaphore                                 _semaphore,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_semaphore, sem, _semaphore);
	if (!_semaphore)
		return;

	if (sem->syncobj)
		device->ws->destroy_syncobj(device->ws, sem->syncobj);
	else
		device->ws->destroy_sem(sem->sem);
	vk_free2(&device->alloc, pAllocator, sem);
}

VkResult radv_CreateEvent(
	VkDevice                                    _device,
	const VkEventCreateInfo*                    pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkEvent*                                    pEvent)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	struct radv_event *event = vk_alloc2(&device->alloc, pAllocator,
					       sizeof(*event), 8,
					       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

	if (!event)
		return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

	event->bo = device->ws->buffer_create(device->ws, 8, 8,
					      RADEON_DOMAIN_GTT,
					      RADEON_FLAG_VA_UNCACHED | RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING);
	if (!event->bo) {
		vk_free2(&device->alloc, pAllocator, event);
		return vk_error(device->instance, VK_ERROR_OUT_OF_DEVICE_MEMORY);
	}

	event->map = (uint64_t*)device->ws->buffer_map(event->bo);

	*pEvent = radv_event_to_handle(event);

	return VK_SUCCESS;
}

void radv_DestroyEvent(
	VkDevice                                    _device,
	VkEvent                                     _event,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_event, event, _event);

	if (!event)
		return;
	device->ws->buffer_destroy(event->bo);
	vk_free2(&device->alloc, pAllocator, event);
}

VkResult radv_GetEventStatus(
	VkDevice                                    _device,
	VkEvent                                     _event)
{
	RADV_FROM_HANDLE(radv_event, event, _event);

	if (*event->map == 1)
		return VK_EVENT_SET;
	return VK_EVENT_RESET;
}

VkResult radv_SetEvent(
	VkDevice                                    _device,
	VkEvent                                     _event)
{
	RADV_FROM_HANDLE(radv_event, event, _event);
	*event->map = 1;

	return VK_SUCCESS;
}

VkResult radv_ResetEvent(
    VkDevice                                    _device,
    VkEvent                                     _event)
{
	RADV_FROM_HANDLE(radv_event, event, _event);
	*event->map = 0;

	return VK_SUCCESS;
}

VkResult radv_CreateBuffer(
	VkDevice                                    _device,
	const VkBufferCreateInfo*                   pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkBuffer*                                   pBuffer)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	struct radv_buffer *buffer;

	assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);

	buffer = vk_alloc2(&device->alloc, pAllocator, sizeof(*buffer), 8,
			     VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (buffer == NULL)
		return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

	buffer->size = pCreateInfo->size;
	buffer->usage = pCreateInfo->usage;
	buffer->bo = NULL;
	buffer->offset = 0;
	buffer->flags = pCreateInfo->flags;

	buffer->shareable = vk_find_struct_const(pCreateInfo->pNext,
						 EXTERNAL_MEMORY_BUFFER_CREATE_INFO_KHR) != NULL;

	if (pCreateInfo->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT) {
		buffer->bo = device->ws->buffer_create(device->ws,
		                                       align64(buffer->size, 4096),
		                                       4096, 0, RADEON_FLAG_VIRTUAL);
		if (!buffer->bo) {
			vk_free2(&device->alloc, pAllocator, buffer);
			return vk_error(device->instance, VK_ERROR_OUT_OF_DEVICE_MEMORY);
		}
	}

	*pBuffer = radv_buffer_to_handle(buffer);

	return VK_SUCCESS;
}

void radv_DestroyBuffer(
	VkDevice                                    _device,
	VkBuffer                                    _buffer,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_buffer, buffer, _buffer);

	if (!buffer)
		return;

	if (buffer->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT)
		device->ws->buffer_destroy(buffer->bo);

	vk_free2(&device->alloc, pAllocator, buffer);
}

static inline unsigned
si_tile_mode_index(const struct radv_image *image, unsigned level, bool stencil)
{
	if (stencil)
		return image->surface.u.legacy.stencil_tiling_index[level];
	else
		return image->surface.u.legacy.tiling_index[level];
}

static uint32_t radv_surface_max_layer_count(struct radv_image_view *iview)
{
	return iview->type == VK_IMAGE_VIEW_TYPE_3D ? iview->extent.depth : (iview->base_layer + iview->layer_count);
}

static uint32_t
radv_init_dcc_control_reg(struct radv_device *device,
			  struct radv_image_view *iview)
{
	unsigned max_uncompressed_block_size = V_028C78_MAX_BLOCK_SIZE_256B;
	unsigned min_compressed_block_size = V_028C78_MIN_BLOCK_SIZE_32B;
	unsigned max_compressed_block_size;
	unsigned independent_64b_blocks;

	if (!radv_image_has_dcc(iview->image))
		return 0;

	if (iview->image->info.samples > 1) {
		if (iview->image->surface.bpe == 1)
			max_uncompressed_block_size = V_028C78_MAX_BLOCK_SIZE_64B;
		else if (iview->image->surface.bpe == 2)
			max_uncompressed_block_size = V_028C78_MAX_BLOCK_SIZE_128B;
	}

	if (!device->physical_device->rad_info.has_dedicated_vram) {
		/* amdvlk: [min-compressed-block-size] should be set to 32 for
		 * dGPU and 64 for APU because all of our APUs to date use
		 * DIMMs which have a request granularity size of 64B while all
		 * other chips have a 32B request size.
		 */
		min_compressed_block_size = V_028C78_MIN_BLOCK_SIZE_64B;
	}

	if (iview->image->usage & (VK_IMAGE_USAGE_SAMPLED_BIT |
				   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				   VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) {
		/* If this DCC image is potentially going to be used in texture
		 * fetches, we need some special settings.
		 */
		independent_64b_blocks = 1;
		max_compressed_block_size = V_028C78_MAX_BLOCK_SIZE_64B;
	} else {
		/* MAX_UNCOMPRESSED_BLOCK_SIZE must be >=
		 * MAX_COMPRESSED_BLOCK_SIZE. Set MAX_COMPRESSED_BLOCK_SIZE as
		 * big as possible for better compression state.
		 */
		independent_64b_blocks = 0;
		max_compressed_block_size = max_uncompressed_block_size;
	}

	return S_028C78_MAX_UNCOMPRESSED_BLOCK_SIZE(max_uncompressed_block_size) |
	       S_028C78_MAX_COMPRESSED_BLOCK_SIZE(max_compressed_block_size) |
	       S_028C78_MIN_COMPRESSED_BLOCK_SIZE(min_compressed_block_size) |
	       S_028C78_INDEPENDENT_64B_BLOCKS(independent_64b_blocks);
}

static void
radv_initialise_color_surface(struct radv_device *device,
			      struct radv_color_buffer_info *cb,
			      struct radv_image_view *iview)
{
	const struct vk_format_description *desc;
	unsigned ntype, format, swap, endian;
	unsigned blend_clamp = 0, blend_bypass = 0;
	uint64_t va;
	const struct radeon_surf *surf = &iview->image->surface;

	desc = vk_format_description(iview->vk_format);

	memset(cb, 0, sizeof(*cb));

	/* Intensity is implemented as Red, so treat it that way. */
	cb->cb_color_attrib = S_028C74_FORCE_DST_ALPHA_1(desc->swizzle[3] == VK_SWIZZLE_1);

	va = radv_buffer_get_va(iview->bo) + iview->image->offset;

	cb->cb_color_base = va >> 8;

	if (device->physical_device->rad_info.chip_class >= GFX9) {
		struct gfx9_surf_meta_flags meta;
		if (iview->image->dcc_offset)
			meta = iview->image->surface.u.gfx9.dcc;
		else
			meta = iview->image->surface.u.gfx9.cmask;

		cb->cb_color_attrib |= S_028C74_COLOR_SW_MODE(iview->image->surface.u.gfx9.surf.swizzle_mode) |
			S_028C74_FMASK_SW_MODE(iview->image->surface.u.gfx9.fmask.swizzle_mode) |
			S_028C74_RB_ALIGNED(meta.rb_aligned) |
			S_028C74_PIPE_ALIGNED(meta.pipe_aligned);

		cb->cb_color_base += iview->image->surface.u.gfx9.surf_offset >> 8;
		cb->cb_color_base |= iview->image->surface.tile_swizzle;
	} else {
		const struct legacy_surf_level *level_info = &surf->u.legacy.level[iview->base_mip];
		unsigned pitch_tile_max, slice_tile_max, tile_mode_index;

		cb->cb_color_base += level_info->offset >> 8;
		if (level_info->mode == RADEON_SURF_MODE_2D)
			cb->cb_color_base |= iview->image->surface.tile_swizzle;

		pitch_tile_max = level_info->nblk_x / 8 - 1;
		slice_tile_max = (level_info->nblk_x * level_info->nblk_y) / 64 - 1;
		tile_mode_index = si_tile_mode_index(iview->image, iview->base_mip, false);

		cb->cb_color_pitch = S_028C64_TILE_MAX(pitch_tile_max);
		cb->cb_color_slice = S_028C68_TILE_MAX(slice_tile_max);
		cb->cb_color_cmask_slice = iview->image->cmask.slice_tile_max;

		cb->cb_color_attrib |= S_028C74_TILE_MODE_INDEX(tile_mode_index);

		if (radv_image_has_fmask(iview->image)) {
			if (device->physical_device->rad_info.chip_class >= CIK)
				cb->cb_color_pitch |= S_028C64_FMASK_TILE_MAX(iview->image->fmask.pitch_in_pixels / 8 - 1);
			cb->cb_color_attrib |= S_028C74_FMASK_TILE_MODE_INDEX(iview->image->fmask.tile_mode_index);
			cb->cb_color_fmask_slice = S_028C88_TILE_MAX(iview->image->fmask.slice_tile_max);
		} else {
			/* This must be set for fast clear to work without FMASK. */
			if (device->physical_device->rad_info.chip_class >= CIK)
				cb->cb_color_pitch |= S_028C64_FMASK_TILE_MAX(pitch_tile_max);
			cb->cb_color_attrib |= S_028C74_FMASK_TILE_MODE_INDEX(tile_mode_index);
			cb->cb_color_fmask_slice = S_028C88_TILE_MAX(slice_tile_max);
		}
	}

	/* CMASK variables */
	va = radv_buffer_get_va(iview->bo) + iview->image->offset;
	va += iview->image->cmask.offset;
	cb->cb_color_cmask = va >> 8;

	va = radv_buffer_get_va(iview->bo) + iview->image->offset;
	va += iview->image->dcc_offset;
	cb->cb_dcc_base = va >> 8;
	cb->cb_dcc_base |= iview->image->surface.tile_swizzle;

	uint32_t max_slice = radv_surface_max_layer_count(iview) - 1;
	cb->cb_color_view = S_028C6C_SLICE_START(iview->base_layer) |
		S_028C6C_SLICE_MAX(max_slice);

	if (iview->image->info.samples > 1) {
		unsigned log_samples = util_logbase2(iview->image->info.samples);

		cb->cb_color_attrib |= S_028C74_NUM_SAMPLES(log_samples) |
			S_028C74_NUM_FRAGMENTS(log_samples);
	}

	if (radv_image_has_fmask(iview->image)) {
		va = radv_buffer_get_va(iview->bo) + iview->image->offset + iview->image->fmask.offset;
		cb->cb_color_fmask = va >> 8;
		cb->cb_color_fmask |= iview->image->fmask.tile_swizzle;
	} else {
		cb->cb_color_fmask = cb->cb_color_base;
	}

	ntype = radv_translate_color_numformat(iview->vk_format,
					       desc,
					       vk_format_get_first_non_void_channel(iview->vk_format));
	format = radv_translate_colorformat(iview->vk_format);
	if (format == V_028C70_COLOR_INVALID || ntype == ~0u)
		radv_finishme("Illegal color\n");
	swap = radv_translate_colorswap(iview->vk_format, FALSE);
	endian = radv_colorformat_endian_swap(format);

	/* blend clamp should be set for all NORM/SRGB types */
	if (ntype == V_028C70_NUMBER_UNORM ||
	    ntype == V_028C70_NUMBER_SNORM ||
	    ntype == V_028C70_NUMBER_SRGB)
		blend_clamp = 1;

	/* set blend bypass according to docs if SINT/UINT or
	   8/24 COLOR variants */
	if (ntype == V_028C70_NUMBER_UINT || ntype == V_028C70_NUMBER_SINT ||
	    format == V_028C70_COLOR_8_24 || format == V_028C70_COLOR_24_8 ||
	    format == V_028C70_COLOR_X24_8_32_FLOAT) {
		blend_clamp = 0;
		blend_bypass = 1;
	}
#if 0
	if ((ntype == V_028C70_NUMBER_UINT || ntype == V_028C70_NUMBER_SINT) &&
	    (format == V_028C70_COLOR_8 ||
	     format == V_028C70_COLOR_8_8 ||
	     format == V_028C70_COLOR_8_8_8_8))
		->color_is_int8 = true;
#endif
	cb->cb_color_info = S_028C70_FORMAT(format) |
		S_028C70_COMP_SWAP(swap) |
		S_028C70_BLEND_CLAMP(blend_clamp) |
		S_028C70_BLEND_BYPASS(blend_bypass) |
		S_028C70_SIMPLE_FLOAT(1) |
		S_028C70_ROUND_MODE(ntype != V_028C70_NUMBER_UNORM &&
				    ntype != V_028C70_NUMBER_SNORM &&
				    ntype != V_028C70_NUMBER_SRGB &&
				    format != V_028C70_COLOR_8_24 &&
				    format != V_028C70_COLOR_24_8) |
		S_028C70_NUMBER_TYPE(ntype) |
		S_028C70_ENDIAN(endian);
	if (radv_image_has_fmask(iview->image)) {
		cb->cb_color_info |= S_028C70_COMPRESSION(1);
		if (device->physical_device->rad_info.chip_class == SI) {
			unsigned fmask_bankh = util_logbase2(iview->image->fmask.bank_height);
			cb->cb_color_attrib |= S_028C74_FMASK_BANK_HEIGHT(fmask_bankh);
		}
	}

	if (radv_image_has_cmask(iview->image) &&
	    !(device->instance->debug_flags & RADV_DEBUG_NO_FAST_CLEARS))
		cb->cb_color_info |= S_028C70_FAST_CLEAR(1);

	if (radv_dcc_enabled(iview->image, iview->base_mip))
		cb->cb_color_info |= S_028C70_DCC_ENABLE(1);

	cb->cb_dcc_control = radv_init_dcc_control_reg(device, iview);

	/* This must be set for fast clear to work without FMASK. */
	if (!radv_image_has_fmask(iview->image) &&
	    device->physical_device->rad_info.chip_class == SI) {
		unsigned bankh = util_logbase2(iview->image->surface.u.legacy.bankh);
		cb->cb_color_attrib |= S_028C74_FMASK_BANK_HEIGHT(bankh);
	}

	if (device->physical_device->rad_info.chip_class >= GFX9) {
		unsigned mip0_depth = iview->image->type == VK_IMAGE_TYPE_3D ?
		  (iview->extent.depth - 1) : (iview->image->info.array_size - 1);

		cb->cb_color_view |= S_028C6C_MIP_LEVEL(iview->base_mip);
		cb->cb_color_attrib |= S_028C74_MIP0_DEPTH(mip0_depth) |
			S_028C74_RESOURCE_TYPE(iview->image->surface.u.gfx9.resource_type);
		cb->cb_color_attrib2 = S_028C68_MIP0_WIDTH(iview->extent.width - 1) |
			S_028C68_MIP0_HEIGHT(iview->extent.height - 1) |
			S_028C68_MAX_MIP(iview->image->info.levels - 1);
	}
}

static unsigned
radv_calc_decompress_on_z_planes(struct radv_device *device,
				 struct radv_image_view *iview)
{
	unsigned max_zplanes = 0;

	assert(radv_image_is_tc_compat_htile(iview->image));

	if (device->physical_device->rad_info.chip_class >= GFX9) {
		/* Default value for 32-bit depth surfaces. */
		max_zplanes = 4;

		if (iview->vk_format == VK_FORMAT_D16_UNORM &&
		    iview->image->info.samples > 1)
			max_zplanes = 2;

		max_zplanes = max_zplanes + 1;
	} else {
		if (iview->vk_format == VK_FORMAT_D16_UNORM) {
			/* Do not enable Z plane compression for 16-bit depth
			 * surfaces because isn't supported on GFX8. Only
			 * 32-bit depth surfaces are supported by the hardware.
			 * This allows to maintain shader compatibility and to
			 * reduce the number of depth decompressions.
			 */
			max_zplanes = 1;
		} else {
			if (iview->image->info.samples <= 1)
				max_zplanes = 5;
			else if (iview->image->info.samples <= 4)
				max_zplanes = 3;
			else
				max_zplanes = 2;
		}
	}

	return max_zplanes;
}

static void
radv_initialise_ds_surface(struct radv_device *device,
			   struct radv_ds_buffer_info *ds,
			   struct radv_image_view *iview)
{
	unsigned level = iview->base_mip;
	unsigned format, stencil_format;
	uint64_t va, s_offs, z_offs;
	bool stencil_only = false;
	memset(ds, 0, sizeof(*ds));
	switch (iview->image->vk_format) {
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
		ds->pa_su_poly_offset_db_fmt_cntl = S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-24);
		ds->offset_scale = 2.0f;
		break;
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_D16_UNORM_S8_UINT:
		ds->pa_su_poly_offset_db_fmt_cntl = S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-16);
		ds->offset_scale = 4.0f;
		break;
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		ds->pa_su_poly_offset_db_fmt_cntl = S_028B78_POLY_OFFSET_NEG_NUM_DB_BITS(-23) |
			S_028B78_POLY_OFFSET_DB_IS_FLOAT_FMT(1);
		ds->offset_scale = 1.0f;
		break;
	case VK_FORMAT_S8_UINT:
		stencil_only = true;
		break;
	default:
		break;
	}

	format = radv_translate_dbformat(iview->image->vk_format);
	stencil_format = iview->image->surface.has_stencil ?
		V_028044_STENCIL_8 : V_028044_STENCIL_INVALID;

	uint32_t max_slice = radv_surface_max_layer_count(iview) - 1;
	ds->db_depth_view = S_028008_SLICE_START(iview->base_layer) |
		S_028008_SLICE_MAX(max_slice);

	ds->db_htile_data_base = 0;
	ds->db_htile_surface = 0;

	va = radv_buffer_get_va(iview->bo) + iview->image->offset;
	s_offs = z_offs = va;

	if (device->physical_device->rad_info.chip_class >= GFX9) {
		assert(iview->image->surface.u.gfx9.surf_offset == 0);
		s_offs += iview->image->surface.u.gfx9.stencil_offset;

		ds->db_z_info = S_028038_FORMAT(format) |
			S_028038_NUM_SAMPLES(util_logbase2(iview->image->info.samples)) |
			S_028038_SW_MODE(iview->image->surface.u.gfx9.surf.swizzle_mode) |
			S_028038_MAXMIP(iview->image->info.levels - 1) |
			S_028038_ZRANGE_PRECISION(1);
		ds->db_stencil_info = S_02803C_FORMAT(stencil_format) |
			S_02803C_SW_MODE(iview->image->surface.u.gfx9.stencil.swizzle_mode);

		ds->db_z_info2 = S_028068_EPITCH(iview->image->surface.u.gfx9.surf.epitch);
		ds->db_stencil_info2 = S_02806C_EPITCH(iview->image->surface.u.gfx9.stencil.epitch);
		ds->db_depth_view |= S_028008_MIPID(level);

		ds->db_depth_size = S_02801C_X_MAX(iview->image->info.width - 1) |
			S_02801C_Y_MAX(iview->image->info.height - 1);

		if (radv_htile_enabled(iview->image, level)) {
			ds->db_z_info |= S_028038_TILE_SURFACE_ENABLE(1);

			if (radv_image_is_tc_compat_htile(iview->image)) {
				unsigned max_zplanes =
					radv_calc_decompress_on_z_planes(device, iview);

				ds->db_z_info |= S_028038_DECOMPRESS_ON_N_ZPLANES(max_zplanes) |
						 S_028038_ITERATE_FLUSH(1);
				ds->db_stencil_info |= S_02803C_ITERATE_FLUSH(1);
			}

			if (!iview->image->surface.has_stencil)
				/* Use all of the htile_buffer for depth if there's no stencil. */
				ds->db_stencil_info |= S_02803C_TILE_STENCIL_DISABLE(1);
			va = radv_buffer_get_va(iview->bo) + iview->image->offset +
				iview->image->htile_offset;
			ds->db_htile_data_base = va >> 8;
			ds->db_htile_surface = S_028ABC_FULL_CACHE(1) |
				S_028ABC_PIPE_ALIGNED(iview->image->surface.u.gfx9.htile.pipe_aligned) |
				S_028ABC_RB_ALIGNED(iview->image->surface.u.gfx9.htile.rb_aligned);
		}
	} else {
		const struct legacy_surf_level *level_info = &iview->image->surface.u.legacy.level[level];

		if (stencil_only)
			level_info = &iview->image->surface.u.legacy.stencil_level[level];

		z_offs += iview->image->surface.u.legacy.level[level].offset;
		s_offs += iview->image->surface.u.legacy.stencil_level[level].offset;

		ds->db_depth_info = S_02803C_ADDR5_SWIZZLE_MASK(!radv_image_is_tc_compat_htile(iview->image));
		ds->db_z_info = S_028040_FORMAT(format) | S_028040_ZRANGE_PRECISION(1);
		ds->db_stencil_info = S_028044_FORMAT(stencil_format);

		if (iview->image->info.samples > 1)
			ds->db_z_info |= S_028040_NUM_SAMPLES(util_logbase2(iview->image->info.samples));

		if (device->physical_device->rad_info.chip_class >= CIK) {
			struct radeon_info *info = &device->physical_device->rad_info;
			unsigned tiling_index = iview->image->surface.u.legacy.tiling_index[level];
			unsigned stencil_index = iview->image->surface.u.legacy.stencil_tiling_index[level];
			unsigned macro_index = iview->image->surface.u.legacy.macro_tile_index;
			unsigned tile_mode = info->si_tile_mode_array[tiling_index];
			unsigned stencil_tile_mode = info->si_tile_mode_array[stencil_index];
			unsigned macro_mode = info->cik_macrotile_mode_array[macro_index];

			if (stencil_only)
				tile_mode = stencil_tile_mode;

			ds->db_depth_info |=
				S_02803C_ARRAY_MODE(G_009910_ARRAY_MODE(tile_mode)) |
				S_02803C_PIPE_CONFIG(G_009910_PIPE_CONFIG(tile_mode)) |
				S_02803C_BANK_WIDTH(G_009990_BANK_WIDTH(macro_mode)) |
				S_02803C_BANK_HEIGHT(G_009990_BANK_HEIGHT(macro_mode)) |
				S_02803C_MACRO_TILE_ASPECT(G_009990_MACRO_TILE_ASPECT(macro_mode)) |
				S_02803C_NUM_BANKS(G_009990_NUM_BANKS(macro_mode));
			ds->db_z_info |= S_028040_TILE_SPLIT(G_009910_TILE_SPLIT(tile_mode));
			ds->db_stencil_info |= S_028044_TILE_SPLIT(G_009910_TILE_SPLIT(stencil_tile_mode));
		} else {
			unsigned tile_mode_index = si_tile_mode_index(iview->image, level, false);
			ds->db_z_info |= S_028040_TILE_MODE_INDEX(tile_mode_index);
			tile_mode_index = si_tile_mode_index(iview->image, level, true);
			ds->db_stencil_info |= S_028044_TILE_MODE_INDEX(tile_mode_index);
			if (stencil_only)
				ds->db_z_info |= S_028040_TILE_MODE_INDEX(tile_mode_index);
		}

		ds->db_depth_size = S_028058_PITCH_TILE_MAX((level_info->nblk_x / 8) - 1) |
			S_028058_HEIGHT_TILE_MAX((level_info->nblk_y / 8) - 1);
		ds->db_depth_slice = S_02805C_SLICE_TILE_MAX((level_info->nblk_x * level_info->nblk_y) / 64 - 1);

		if (radv_htile_enabled(iview->image, level)) {
			ds->db_z_info |= S_028040_TILE_SURFACE_ENABLE(1);

			if (!iview->image->surface.has_stencil &&
			    !radv_image_is_tc_compat_htile(iview->image))
				/* Use all of the htile_buffer for depth if there's no stencil. */
				ds->db_stencil_info |= S_028044_TILE_STENCIL_DISABLE(1);

			va = radv_buffer_get_va(iview->bo) + iview->image->offset +
				iview->image->htile_offset;
			ds->db_htile_data_base = va >> 8;
			ds->db_htile_surface = S_028ABC_FULL_CACHE(1);

			if (radv_image_is_tc_compat_htile(iview->image)) {
				unsigned max_zplanes =
					radv_calc_decompress_on_z_planes(device, iview);

				ds->db_htile_surface |= S_028ABC_TC_COMPATIBLE(1);
				ds->db_z_info |= S_028040_DECOMPRESS_ON_N_ZPLANES(max_zplanes);
			}
		}
	}

	ds->db_z_read_base = ds->db_z_write_base = z_offs >> 8;
	ds->db_stencil_read_base = ds->db_stencil_write_base = s_offs >> 8;
}

VkResult radv_CreateFramebuffer(
	VkDevice                                    _device,
	const VkFramebufferCreateInfo*              pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkFramebuffer*                              pFramebuffer)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	struct radv_framebuffer *framebuffer;

	assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);

	size_t size = sizeof(*framebuffer) +
		sizeof(struct radv_attachment_info) * pCreateInfo->attachmentCount;
	framebuffer = vk_alloc2(&device->alloc, pAllocator, size, 8,
				  VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (framebuffer == NULL)
		return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

	framebuffer->attachment_count = pCreateInfo->attachmentCount;
	framebuffer->width = pCreateInfo->width;
	framebuffer->height = pCreateInfo->height;
	framebuffer->layers = pCreateInfo->layers;
	for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
		VkImageView _iview = pCreateInfo->pAttachments[i];
		struct radv_image_view *iview = radv_image_view_from_handle(_iview);
		framebuffer->attachments[i].attachment = iview;
		if (iview->aspect_mask & VK_IMAGE_ASPECT_COLOR_BIT) {
			radv_initialise_color_surface(device, &framebuffer->attachments[i].cb, iview);
		} else if (iview->aspect_mask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
			radv_initialise_ds_surface(device, &framebuffer->attachments[i].ds, iview);
		}
		framebuffer->width = MIN2(framebuffer->width, iview->extent.width);
		framebuffer->height = MIN2(framebuffer->height, iview->extent.height);
		framebuffer->layers = MIN2(framebuffer->layers, radv_surface_max_layer_count(iview));
	}

	*pFramebuffer = radv_framebuffer_to_handle(framebuffer);
	return VK_SUCCESS;
}

void radv_DestroyFramebuffer(
	VkDevice                                    _device,
	VkFramebuffer                               _fb,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_framebuffer, fb, _fb);

	if (!fb)
		return;
	vk_free2(&device->alloc, pAllocator, fb);
}

static unsigned radv_tex_wrap(VkSamplerAddressMode address_mode)
{
	switch (address_mode) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:
		return V_008F30_SQ_TEX_WRAP;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		return V_008F30_SQ_TEX_MIRROR;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
		return V_008F30_SQ_TEX_CLAMP_LAST_TEXEL;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
		return V_008F30_SQ_TEX_CLAMP_BORDER;
	case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
		return V_008F30_SQ_TEX_MIRROR_ONCE_LAST_TEXEL;
	default:
		unreachable("illegal tex wrap mode");
		break;
	}
}

static unsigned
radv_tex_compare(VkCompareOp op)
{
	switch (op) {
	case VK_COMPARE_OP_NEVER:
		return V_008F30_SQ_TEX_DEPTH_COMPARE_NEVER;
	case VK_COMPARE_OP_LESS:
		return V_008F30_SQ_TEX_DEPTH_COMPARE_LESS;
	case VK_COMPARE_OP_EQUAL:
		return V_008F30_SQ_TEX_DEPTH_COMPARE_EQUAL;
	case VK_COMPARE_OP_LESS_OR_EQUAL:
		return V_008F30_SQ_TEX_DEPTH_COMPARE_LESSEQUAL;
	case VK_COMPARE_OP_GREATER:
		return V_008F30_SQ_TEX_DEPTH_COMPARE_GREATER;
	case VK_COMPARE_OP_NOT_EQUAL:
		return V_008F30_SQ_TEX_DEPTH_COMPARE_NOTEQUAL;
	case VK_COMPARE_OP_GREATER_OR_EQUAL:
		return V_008F30_SQ_TEX_DEPTH_COMPARE_GREATEREQUAL;
	case VK_COMPARE_OP_ALWAYS:
		return V_008F30_SQ_TEX_DEPTH_COMPARE_ALWAYS;
	default:
		unreachable("illegal compare mode");
		break;
	}
}

static unsigned
radv_tex_filter(VkFilter filter, unsigned max_ansio)
{
	switch (filter) {
	case VK_FILTER_NEAREST:
		return (max_ansio > 1 ? V_008F38_SQ_TEX_XY_FILTER_ANISO_POINT :
			V_008F38_SQ_TEX_XY_FILTER_POINT);
	case VK_FILTER_LINEAR:
		return (max_ansio > 1 ? V_008F38_SQ_TEX_XY_FILTER_ANISO_BILINEAR :
			V_008F38_SQ_TEX_XY_FILTER_BILINEAR);
	case VK_FILTER_CUBIC_IMG:
	default:
		fprintf(stderr, "illegal texture filter");
		return 0;
	}
}

static unsigned
radv_tex_mipfilter(VkSamplerMipmapMode mode)
{
	switch (mode) {
	case VK_SAMPLER_MIPMAP_MODE_NEAREST:
		return V_008F38_SQ_TEX_Z_FILTER_POINT;
	case VK_SAMPLER_MIPMAP_MODE_LINEAR:
		return V_008F38_SQ_TEX_Z_FILTER_LINEAR;
	default:
		return V_008F38_SQ_TEX_Z_FILTER_NONE;
	}
}

static unsigned
radv_tex_bordercolor(VkBorderColor bcolor)
{
	switch (bcolor) {
	case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:
	case VK_BORDER_COLOR_INT_TRANSPARENT_BLACK:
		return V_008F3C_SQ_TEX_BORDER_COLOR_TRANS_BLACK;
	case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
	case VK_BORDER_COLOR_INT_OPAQUE_BLACK:
		return V_008F3C_SQ_TEX_BORDER_COLOR_OPAQUE_BLACK;
	case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
	case VK_BORDER_COLOR_INT_OPAQUE_WHITE:
		return V_008F3C_SQ_TEX_BORDER_COLOR_OPAQUE_WHITE;
	default:
		break;
	}
	return 0;
}

static unsigned
radv_tex_aniso_filter(unsigned filter)
{
	if (filter < 2)
		return 0;
	if (filter < 4)
		return 1;
	if (filter < 8)
		return 2;
	if (filter < 16)
		return 3;
	return 4;
}

static unsigned
radv_tex_filter_mode(VkSamplerReductionModeEXT mode)
{
	switch (mode) {
	case VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT:
		return SQ_IMG_FILTER_MODE_BLEND;
	case VK_SAMPLER_REDUCTION_MODE_MIN_EXT:
		return SQ_IMG_FILTER_MODE_MIN;
	case VK_SAMPLER_REDUCTION_MODE_MAX_EXT:
		return SQ_IMG_FILTER_MODE_MAX;
	default:
		break;
	}
	return 0;
}

static uint32_t
radv_get_max_anisotropy(struct radv_device *device,
			const VkSamplerCreateInfo *pCreateInfo)
{
	if (device->force_aniso >= 0)
		return device->force_aniso;

	if (pCreateInfo->anisotropyEnable &&
	    pCreateInfo->maxAnisotropy > 1.0f)
		return (uint32_t)pCreateInfo->maxAnisotropy;

	return 0;
}

static void
radv_init_sampler(struct radv_device *device,
		  struct radv_sampler *sampler,
		  const VkSamplerCreateInfo *pCreateInfo)
{
	uint32_t max_aniso = radv_get_max_anisotropy(device, pCreateInfo);
	uint32_t max_aniso_ratio = radv_tex_aniso_filter(max_aniso);
	bool is_vi = (device->physical_device->rad_info.chip_class >= VI);
	unsigned filter_mode = SQ_IMG_FILTER_MODE_BLEND;

	const struct VkSamplerReductionModeCreateInfoEXT *sampler_reduction =
		vk_find_struct_const(pCreateInfo->pNext,
				     SAMPLER_REDUCTION_MODE_CREATE_INFO_EXT);
	if (sampler_reduction)
		filter_mode = radv_tex_filter_mode(sampler_reduction->reductionMode);

	sampler->state[0] = (S_008F30_CLAMP_X(radv_tex_wrap(pCreateInfo->addressModeU)) |
			     S_008F30_CLAMP_Y(radv_tex_wrap(pCreateInfo->addressModeV)) |
			     S_008F30_CLAMP_Z(radv_tex_wrap(pCreateInfo->addressModeW)) |
			     S_008F30_MAX_ANISO_RATIO(max_aniso_ratio) |
			     S_008F30_DEPTH_COMPARE_FUNC(radv_tex_compare(pCreateInfo->compareOp)) |
			     S_008F30_FORCE_UNNORMALIZED(pCreateInfo->unnormalizedCoordinates ? 1 : 0) |
			     S_008F30_ANISO_THRESHOLD(max_aniso_ratio >> 1) |
			     S_008F30_ANISO_BIAS(max_aniso_ratio) |
			     S_008F30_DISABLE_CUBE_WRAP(0) |
			     S_008F30_COMPAT_MODE(is_vi) |
			     S_008F30_FILTER_MODE(filter_mode));
	sampler->state[1] = (S_008F34_MIN_LOD(S_FIXED(CLAMP(pCreateInfo->minLod, 0, 15), 8)) |
			     S_008F34_MAX_LOD(S_FIXED(CLAMP(pCreateInfo->maxLod, 0, 15), 8)) |
			     S_008F34_PERF_MIP(max_aniso_ratio ? max_aniso_ratio + 6 : 0));
	sampler->state[2] = (S_008F38_LOD_BIAS(S_FIXED(CLAMP(pCreateInfo->mipLodBias, -16, 16), 8)) |
			     S_008F38_XY_MAG_FILTER(radv_tex_filter(pCreateInfo->magFilter, max_aniso)) |
			     S_008F38_XY_MIN_FILTER(radv_tex_filter(pCreateInfo->minFilter, max_aniso)) |
			     S_008F38_MIP_FILTER(radv_tex_mipfilter(pCreateInfo->mipmapMode)) |
			     S_008F38_MIP_POINT_PRECLAMP(0) |
			     S_008F38_DISABLE_LSB_CEIL(device->physical_device->rad_info.chip_class <= VI) |
			     S_008F38_FILTER_PREC_FIX(1) |
			     S_008F38_ANISO_OVERRIDE(is_vi));
	sampler->state[3] = (S_008F3C_BORDER_COLOR_PTR(0) |
			     S_008F3C_BORDER_COLOR_TYPE(radv_tex_bordercolor(pCreateInfo->borderColor)));
}

VkResult radv_CreateSampler(
	VkDevice                                    _device,
	const VkSamplerCreateInfo*                  pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkSampler*                                  pSampler)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	struct radv_sampler *sampler;

	assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

	sampler = vk_alloc2(&device->alloc, pAllocator, sizeof(*sampler), 8,
			      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (!sampler)
		return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

	radv_init_sampler(device, sampler, pCreateInfo);
	*pSampler = radv_sampler_to_handle(sampler);

	return VK_SUCCESS;
}

void radv_DestroySampler(
	VkDevice                                    _device,
	VkSampler                                   _sampler,
	const VkAllocationCallbacks*                pAllocator)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_sampler, sampler, _sampler);

	if (!sampler)
		return;
	vk_free2(&device->alloc, pAllocator, sampler);
}

/* vk_icd.h does not declare this function, so we declare it here to
 * suppress Wmissing-prototypes.
 */
PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion);

PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion)
{
	/* For the full details on loader interface versioning, see
	* <https://github.com/KhronosGroup/Vulkan-LoaderAndValidationLayers/blob/master/loader/LoaderAndLayerInterface.md>.
	* What follows is a condensed summary, to help you navigate the large and
	* confusing official doc.
	*
	*   - Loader interface v0 is incompatible with later versions. We don't
	*     support it.
	*
	*   - In loader interface v1:
	*       - The first ICD entrypoint called by the loader is
	*         vk_icdGetInstanceProcAddr(). The ICD must statically expose this
	*         entrypoint.
	*       - The ICD must statically expose no other Vulkan symbol unless it is
	*         linked with -Bsymbolic.
	*       - Each dispatchable Vulkan handle created by the ICD must be
	*         a pointer to a struct whose first member is VK_LOADER_DATA. The
	*         ICD must initialize VK_LOADER_DATA.loadMagic to ICD_LOADER_MAGIC.
	*       - The loader implements vkCreate{PLATFORM}SurfaceKHR() and
	*         vkDestroySurfaceKHR(). The ICD must be capable of working with
	*         such loader-managed surfaces.
	*
	*    - Loader interface v2 differs from v1 in:
	*       - The first ICD entrypoint called by the loader is
	*         vk_icdNegotiateLoaderICDInterfaceVersion(). The ICD must
	*         statically expose this entrypoint.
	*
	*    - Loader interface v3 differs from v2 in:
	*        - The ICD must implement vkCreate{PLATFORM}SurfaceKHR(),
	*          vkDestroySurfaceKHR(), and other API which uses VKSurfaceKHR,
	*          because the loader no longer does so.
	*/
	*pSupportedVersion = MIN2(*pSupportedVersion, 3u);
	return VK_SUCCESS;
}

VkResult radv_GetMemoryFdKHR(VkDevice _device,
			     const VkMemoryGetFdInfoKHR *pGetFdInfo,
			     int *pFD)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_device_memory, memory, pGetFdInfo->memory);

	assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);

	/* At the moment, we support only the below handle types. */
	assert(pGetFdInfo->handleType ==
	       VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR ||
	       pGetFdInfo->handleType ==
	       VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

	bool ret = radv_get_memory_fd(device, memory, pFD);
	if (ret == false)
		return vk_error(device->instance, VK_ERROR_OUT_OF_DEVICE_MEMORY);
	return VK_SUCCESS;
}

VkResult radv_GetMemoryFdPropertiesKHR(VkDevice _device,
				       VkExternalMemoryHandleTypeFlagBitsKHR handleType,
				       int fd,
				       VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   RADV_FROM_HANDLE(radv_device, device, _device);

   switch (handleType) {
   case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
      pMemoryFdProperties->memoryTypeBits = (1 << RADV_MEM_TYPE_COUNT) - 1;
      return VK_SUCCESS;

   default:
      /* The valid usage section for this function says:
       *
       *    "handleType must not be one of the handle types defined as
       *    opaque."
       *
       * So opaque handle types fall into the default "unsupported" case.
       */
      return vk_error(device->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR);
   }
}

static VkResult radv_import_opaque_fd(struct radv_device *device,
                                      int fd,
                                      uint32_t *syncobj)
{
	uint32_t syncobj_handle = 0;
	int ret = device->ws->import_syncobj(device->ws, fd, &syncobj_handle);
	if (ret != 0)
		return vk_error(device->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR);

	if (*syncobj)
		device->ws->destroy_syncobj(device->ws, *syncobj);

	*syncobj = syncobj_handle;
	close(fd);

	return VK_SUCCESS;
}

static VkResult radv_import_sync_fd(struct radv_device *device,
                                    int fd,
                                    uint32_t *syncobj)
{
	/* If we create a syncobj we do it locally so that if we have an error, we don't
	 * leave a syncobj in an undetermined state in the fence. */
	uint32_t syncobj_handle =  *syncobj;
	if (!syncobj_handle) {
		int ret = device->ws->create_syncobj(device->ws, &syncobj_handle);
		if (ret) {
			return vk_error(device->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR);
		}
	}

	if (fd == -1) {
		device->ws->signal_syncobj(device->ws, syncobj_handle);
	} else {
		int ret = device->ws->import_syncobj_from_sync_file(device->ws, syncobj_handle, fd);
	if (ret != 0)
		return vk_error(device->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR);
	}

	*syncobj = syncobj_handle;
	if (fd != -1)
		close(fd);

	return VK_SUCCESS;
}

VkResult radv_ImportSemaphoreFdKHR(VkDevice _device,
				   const VkImportSemaphoreFdInfoKHR *pImportSemaphoreFdInfo)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_semaphore, sem, pImportSemaphoreFdInfo->semaphore);
	uint32_t *syncobj_dst = NULL;

	if (pImportSemaphoreFdInfo->flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT_KHR) {
		syncobj_dst = &sem->temp_syncobj;
	} else {
		syncobj_dst = &sem->syncobj;
	}

	switch(pImportSemaphoreFdInfo->handleType) {
		case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR:
			return radv_import_opaque_fd(device, pImportSemaphoreFdInfo->fd, syncobj_dst);
		case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR:
			return radv_import_sync_fd(device, pImportSemaphoreFdInfo->fd, syncobj_dst);
		default:
			unreachable("Unhandled semaphore handle type");
	}
}

VkResult radv_GetSemaphoreFdKHR(VkDevice _device,
				const VkSemaphoreGetFdInfoKHR *pGetFdInfo,
				int *pFd)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_semaphore, sem, pGetFdInfo->semaphore);
	int ret;
	uint32_t syncobj_handle;

	if (sem->temp_syncobj)
		syncobj_handle = sem->temp_syncobj;
	else
		syncobj_handle = sem->syncobj;

	switch(pGetFdInfo->handleType) {
	case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR:
		ret = device->ws->export_syncobj(device->ws, syncobj_handle, pFd);
		break;
	case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR:
		ret = device->ws->export_syncobj_to_sync_file(device->ws, syncobj_handle, pFd);
		if (!ret) {
			if (sem->temp_syncobj) {
				close (sem->temp_syncobj);
				sem->temp_syncobj = 0;
			} else {
				device->ws->reset_syncobj(device->ws, syncobj_handle);
			}
		}
		break;
	default:
		unreachable("Unhandled semaphore handle type");
	}

	if (ret)
		return vk_error(device->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR);
	return VK_SUCCESS;
}

void radv_GetPhysicalDeviceExternalSemaphoreProperties(
	VkPhysicalDevice                            physicalDevice,
	const VkPhysicalDeviceExternalSemaphoreInfoKHR* pExternalSemaphoreInfo,
	VkExternalSemaphorePropertiesKHR*           pExternalSemaphoreProperties)
{
	RADV_FROM_HANDLE(radv_physical_device, pdevice, physicalDevice);

	/* Require has_syncobj_wait_for_submit for the syncobj signal ioctl introduced at virtually the same time */
	if (pdevice->rad_info.has_syncobj_wait_for_submit &&
	    (pExternalSemaphoreInfo->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR || 
	     pExternalSemaphoreInfo->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR)) {
		pExternalSemaphoreProperties->exportFromImportedHandleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR | VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR;
		pExternalSemaphoreProperties->compatibleHandleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR | VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR;
		pExternalSemaphoreProperties->externalSemaphoreFeatures = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT_KHR |
			VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT_KHR;
	} else if (pExternalSemaphoreInfo->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR) {
		pExternalSemaphoreProperties->exportFromImportedHandleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
		pExternalSemaphoreProperties->compatibleHandleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR;
		pExternalSemaphoreProperties->externalSemaphoreFeatures = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT_KHR |
			VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT_KHR;
	} else {
		pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
		pExternalSemaphoreProperties->compatibleHandleTypes = 0;
		pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;
	}
}

VkResult radv_ImportFenceFdKHR(VkDevice _device,
				   const VkImportFenceFdInfoKHR *pImportFenceFdInfo)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_fence, fence, pImportFenceFdInfo->fence);
	uint32_t *syncobj_dst = NULL;


	if (pImportFenceFdInfo->flags & VK_FENCE_IMPORT_TEMPORARY_BIT_KHR) {
		syncobj_dst = &fence->temp_syncobj;
	} else {
		syncobj_dst = &fence->syncobj;
	}

	switch(pImportFenceFdInfo->handleType) {
		case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR:
			return radv_import_opaque_fd(device, pImportFenceFdInfo->fd, syncobj_dst);
		case VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT_KHR:
			return radv_import_sync_fd(device, pImportFenceFdInfo->fd, syncobj_dst);
		default:
			unreachable("Unhandled fence handle type");
	}
}

VkResult radv_GetFenceFdKHR(VkDevice _device,
				const VkFenceGetFdInfoKHR *pGetFdInfo,
				int *pFd)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	RADV_FROM_HANDLE(radv_fence, fence, pGetFdInfo->fence);
	int ret;
	uint32_t syncobj_handle;

	if (fence->temp_syncobj)
		syncobj_handle = fence->temp_syncobj;
	else
		syncobj_handle = fence->syncobj;

	switch(pGetFdInfo->handleType) {
	case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR:
		ret = device->ws->export_syncobj(device->ws, syncobj_handle, pFd);
		break;
	case VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT_KHR:
		ret = device->ws->export_syncobj_to_sync_file(device->ws, syncobj_handle, pFd);
		if (!ret) {
			if (fence->temp_syncobj) {
				close (fence->temp_syncobj);
				fence->temp_syncobj = 0;
			} else {
				device->ws->reset_syncobj(device->ws, syncobj_handle);
			}
		}
		break;
	default:
		unreachable("Unhandled fence handle type");
	}

	if (ret)
		return vk_error(device->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE_KHR);
	return VK_SUCCESS;
}

void radv_GetPhysicalDeviceExternalFenceProperties(
	VkPhysicalDevice                            physicalDevice,
	const VkPhysicalDeviceExternalFenceInfoKHR* pExternalFenceInfo,
	VkExternalFencePropertiesKHR*           pExternalFenceProperties)
{
	RADV_FROM_HANDLE(radv_physical_device, pdevice, physicalDevice);

	if (pdevice->rad_info.has_syncobj_wait_for_submit &&
	    (pExternalFenceInfo->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR || 
	     pExternalFenceInfo->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT_KHR)) {
		pExternalFenceProperties->exportFromImportedHandleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR | VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT_KHR;
		pExternalFenceProperties->compatibleHandleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT_KHR | VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT_KHR;
		pExternalFenceProperties->externalFenceFeatures = VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT_KHR |
			VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT_KHR;
	} else {
		pExternalFenceProperties->exportFromImportedHandleTypes = 0;
		pExternalFenceProperties->compatibleHandleTypes = 0;
		pExternalFenceProperties->externalFenceFeatures = 0;
	}
}

VkResult
radv_CreateDebugReportCallbackEXT(VkInstance _instance,
                                 const VkDebugReportCallbackCreateInfoEXT* pCreateInfo,
                                 const VkAllocationCallbacks* pAllocator,
                                 VkDebugReportCallbackEXT* pCallback)
{
	RADV_FROM_HANDLE(radv_instance, instance, _instance);
	return vk_create_debug_report_callback(&instance->debug_report_callbacks,
	                                       pCreateInfo, pAllocator, &instance->alloc,
	                                       pCallback);
}

void
radv_DestroyDebugReportCallbackEXT(VkInstance _instance,
                                  VkDebugReportCallbackEXT _callback,
                                  const VkAllocationCallbacks* pAllocator)
{
	RADV_FROM_HANDLE(radv_instance, instance, _instance);
	vk_destroy_debug_report_callback(&instance->debug_report_callbacks,
	                                 _callback, pAllocator, &instance->alloc);
}

void
radv_DebugReportMessageEXT(VkInstance _instance,
                          VkDebugReportFlagsEXT flags,
                          VkDebugReportObjectTypeEXT objectType,
                          uint64_t object,
                          size_t location,
                          int32_t messageCode,
                          const char* pLayerPrefix,
                          const char* pMessage)
{
	RADV_FROM_HANDLE(radv_instance, instance, _instance);
	vk_debug_report(&instance->debug_report_callbacks, flags, objectType,
	                object, location, messageCode, pLayerPrefix, pMessage);
}

void
radv_GetDeviceGroupPeerMemoryFeatures(
    VkDevice                                    device,
    uint32_t                                    heapIndex,
    uint32_t                                    localDeviceIndex,
    uint32_t                                    remoteDeviceIndex,
    VkPeerMemoryFeatureFlags*                   pPeerMemoryFeatures)
{
	assert(localDeviceIndex == remoteDeviceIndex);

	*pPeerMemoryFeatures = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT |
	                       VK_PEER_MEMORY_FEATURE_COPY_DST_BIT |
	                       VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT |
	                       VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;
}

static const VkTimeDomainEXT radv_time_domains[] = {
	VK_TIME_DOMAIN_DEVICE_EXT,
	VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT,
	VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT,
};

VkResult radv_GetPhysicalDeviceCalibrateableTimeDomainsEXT(
	VkPhysicalDevice                             physicalDevice,
	uint32_t                                     *pTimeDomainCount,
	VkTimeDomainEXT                              *pTimeDomains)
{
	int d;
	VK_OUTARRAY_MAKE(out, pTimeDomains, pTimeDomainCount);

	for (d = 0; d < ARRAY_SIZE(radv_time_domains); d++) {
		vk_outarray_append(&out, i) {
			*i = radv_time_domains[d];
		}
	}

	return vk_outarray_status(&out);
}

static uint64_t
radv_clock_gettime(clockid_t clock_id)
{
	struct timespec current;
	int ret;

	ret = clock_gettime(clock_id, &current);
	if (ret < 0 && clock_id == CLOCK_MONOTONIC_RAW)
		ret = clock_gettime(CLOCK_MONOTONIC, &current);
	if (ret < 0)
		return 0;

	return (uint64_t) current.tv_sec * 1000000000ULL + current.tv_nsec;
}

VkResult radv_GetCalibratedTimestampsEXT(
	VkDevice                                     _device,
	uint32_t                                     timestampCount,
	const VkCalibratedTimestampInfoEXT           *pTimestampInfos,
	uint64_t                                     *pTimestamps,
	uint64_t                                     *pMaxDeviation)
{
	RADV_FROM_HANDLE(radv_device, device, _device);
	uint32_t clock_crystal_freq = device->physical_device->rad_info.clock_crystal_freq;
	int d;
	uint64_t begin, end;
        uint64_t max_clock_period = 0;

	begin = radv_clock_gettime(CLOCK_MONOTONIC_RAW);

	for (d = 0; d < timestampCount; d++) {
		switch (pTimestampInfos[d].timeDomain) {
		case VK_TIME_DOMAIN_DEVICE_EXT:
			pTimestamps[d] = device->ws->query_value(device->ws,
								 RADEON_TIMESTAMP);
                        uint64_t device_period = DIV_ROUND_UP(1000000, clock_crystal_freq);
                        max_clock_period = MAX2(max_clock_period, device_period);
			break;
		case VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT:
			pTimestamps[d] = radv_clock_gettime(CLOCK_MONOTONIC);
                        max_clock_period = MAX2(max_clock_period, 1);
			break;

		case VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT:
			pTimestamps[d] = begin;
			break;
		default:
			pTimestamps[d] = 0;
			break;
		}
	}

	end = radv_clock_gettime(CLOCK_MONOTONIC_RAW);

        /*
         * The maximum deviation is the sum of the interval over which we
         * perform the sampling and the maximum period of any sampled
         * clock. That's because the maximum skew between any two sampled
         * clock edges is when the sampled clock with the largest period is
         * sampled at the end of that period but right at the beginning of the
         * sampling interval and some other clock is sampled right at the
         * begining of its sampling period and right at the end of the
         * sampling interval. Let's assume the GPU has the longest clock
         * period and that the application is sampling GPU and monotonic:
         *
         *                               s                 e
         *			 w x y z 0 1 2 3 4 5 6 7 8 9 a b c d e f
         *	Raw              -_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-
         *
         *                               g
         *		  0         1         2         3
         *	GPU       -----_____-----_____-----_____-----_____
         *
         *                                                m
         *					    x y z 0 1 2 3 4 5 6 7 8 9 a b c
         *	Monotonic                           -_-_-_-_-_-_-_-_-_-_-_-_-_-_-_-
         *
         *	Interval                     <----------------->
         *	Deviation           <-------------------------->
         *
         *		s  = read(raw)       2
         *		g  = read(GPU)       1
         *		m  = read(monotonic) 2
         *		e  = read(raw)       b
         *
         * We round the sample interval up by one tick to cover sampling error
         * in the interval clock
         */

        uint64_t sample_interval = end - begin + 1;

        *pMaxDeviation = sample_interval + max_clock_period;

	return VK_SUCCESS;
}
