# Copyright © 2018 Advanced Micro Devices, Inc.
# Copyright © 2018 Mauro Rossi issor.oruam@gmail.com

# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

LOCAL_PATH := $(call my-dir)

# get VULKAN_FILES and VULKAN_GENERATED_FILES
include $(LOCAL_PATH)/Makefile.sources

# The gallium includes are for the util/u_math.h include from main/macros.h

RADV_COMMON_INCLUDES := \
	$(MESA_TOP)/include \
	$(MESA_TOP)/src/ \
	$(MESA_TOP)/src/vulkan/wsi \
	$(MESA_TOP)/src/vulkan/util \
	$(MESA_TOP)/src/amd \
	$(MESA_TOP)/src/amd/common \
	$(MESA_TOP)/src/compiler \
	$(MESA_TOP)/src/mapi \
	$(MESA_TOP)/src/mesa \
	$(MESA_TOP)/src/mesa/drivers/dri/common \
	$(MESA_TOP)/src/gallium/auxiliary \
	$(MESA_TOP)/src/gallium/include \
	frameworks/native/vulkan/include

RADV_SHARED_LIBRARIES := libdrm_amdgpu

ifeq ($(filter $(MESA_ANDROID_MAJOR_VERSION), 4 5 6 7),)
RADV_SHARED_LIBRARIES += libnativewindow
endif

#
# libmesa_radv_common
#

include $(CLEAR_VARS)
LOCAL_MODULE := libmesa_radv_common
LOCAL_MODULE_CLASS := STATIC_LIBRARIES

intermediates := $(call local-generated-sources-dir)

LOCAL_SRC_FILES := \
	$(VULKAN_FILES)

LOCAL_CFLAGS += -DFORCE_BUILD_AMDGPU   # instructs LLVM to declare LLVMInitializeAMDGPU* functions
LOCAL_CFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR

$(call mesa-build-with-llvm)

LOCAL_C_INCLUDES := \
	$(RADV_COMMON_INCLUDES) \
	$(call generated-sources-dir-for,STATIC_LIBRARIES,libmesa_amd_common,,) \
	$(call generated-sources-dir-for,STATIC_LIBRARIES,libmesa_nir,,)/nir \
	$(call generated-sources-dir-for,STATIC_LIBRARIES,libmesa_radv_common,,) \
	$(call generated-sources-dir-for,STATIC_LIBRARIES,libmesa_vulkan_util,,)/util

LOCAL_WHOLE_STATIC_LIBRARIES := \
	libmesa_vulkan_util \
	libmesa_git_sha1

LOCAL_GENERATED_SOURCES += $(intermediates)/radv_entrypoints.c
LOCAL_GENERATED_SOURCES += $(intermediates)/radv_entrypoints.h
LOCAL_GENERATED_SOURCES += $(intermediates)/radv_extensions.c
LOCAL_GENERATED_SOURCES += $(intermediates)/radv_extensions.h
LOCAL_GENERATED_SOURCES += $(intermediates)/vk_format_table.c

RADV_ENTRYPOINTS_SCRIPT := $(MESA_TOP)/src/amd/vulkan/radv_entrypoints_gen.py
RADV_EXTENSIONS_SCRIPT := $(MESA_TOP)/src/amd/vulkan/radv_extensions.py
VK_FORMAT_TABLE_SCRIPT := $(MESA_TOP)/src/amd/vulkan/vk_format_table.py
VK_FORMAT_PARSE_SCRIPT := $(MESA_TOP)/src/amd/vulkan/vk_format_parse.py

vulkan_api_xml = $(MESA_TOP)/src/vulkan/registry/vk.xml
vk_format_layout_csv = $(MESA_TOP)/src/amd/vulkan/vk_format_layout.csv

$(intermediates)/radv_entrypoints.c: $(RADV_ENTRYPOINTS_SCRIPT) \
					$(RADV_EXTENSIONS_SCRIPT) \
					$(vulkan_api_xml)
	@mkdir -p $(dir $@)
	$(MESA_PYTHON2) $(RADV_ENTRYPOINTS_SCRIPT) \
		--xml $(vulkan_api_xml) \
		--outdir $(dir $@)

$(intermediates)/radv_entrypoints.h: $(intermediates)/radv_entrypoints.c

$(intermediates)/radv_extensions.c: $(RADV_EXTENSIONS_SCRIPT) $(vulkan_api_xml)
	@mkdir -p $(dir $@)
	$(MESA_PYTHON2) $(RADV_EXTENSIONS_SCRIPT) \
		--xml $(vulkan_api_xml) \
		--out-c $@ \
		--out-h $(addsuffix .h,$(basename $@))

$(intermediates)/radv_extensions.h: $(intermediates)/radv_extensions.c

$(intermediates)/vk_format_table.c: $(VK_FORMAT_TABLE_SCRIPT) \
					$(VK_FORMAT_PARSE_SCRIPT) \
					$(vk_format_layout_csv)
	@mkdir -p $(dir $@)
	$(MESA_PYTHON2) $(VK_FORMAT_TABLE_SCRIPT) $(vk_format_layout_csv) > $@

LOCAL_SHARED_LIBRARIES += $(RADV_SHARED_LIBRARIES)

LOCAL_EXPORT_C_INCLUDE_DIRS := \
	$(MESA_TOP)/src/amd/vulkan \
	$(intermediates)

include $(MESA_COMMON_MK)
include $(BUILD_STATIC_LIBRARY)

#
# libvulkan_radeon
#

include $(CLEAR_VARS)

LOCAL_MODULE := vulkan.radv
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_PROPRIETARY_MODULE := true
LOCAL_MODULE_RELATIVE_PATH := hw

LOCAL_LDFLAGS += -Wl,--build-id=sha1

LOCAL_SRC_FILES := \
	$(VULKAN_ANDROID_FILES)

LOCAL_CFLAGS += -DFORCE_BUILD_AMDGPU   # instructs LLVM to declare LLVMInitializeAMDGPU* functions
LOCAL_CFLAGS += -DVK_USE_PLATFORM_ANDROID_KHR

$(call mesa-build-with-llvm)

LOCAL_C_INCLUDES := \
	$(RADV_COMMON_INCLUDES) \
	$(call generated-sources-dir-for,STATIC_LIBRARIES,libmesa_radv_common,,)

LOCAL_EXPORT_C_INCLUDE_DIRS := \
	$(MESA_TOP)/src/amd/vulkan \
	$(intermediates)

LOCAL_WHOLE_STATIC_LIBRARIES := \
	libmesa_util \
	libmesa_nir \
	libmesa_glsl \
	libmesa_compiler \
	libmesa_amdgpu_addrlib \
	libmesa_amd_common \
	libmesa_radv_common

LOCAL_SHARED_LIBRARIES += $(RADV_SHARED_LIBRARIES) libz libsync liblog

include $(MESA_COMMON_MK)
include $(BUILD_SHARED_LIBRARY)
