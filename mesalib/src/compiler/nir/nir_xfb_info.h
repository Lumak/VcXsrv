/*
 * Copyright © 2018 Intel Corporation
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

#ifndef NIR_XFB_INFO_H
#define NIR_XFB_INFO_H

#include "nir.h"

#define NIR_MAX_XFB_BUFFERS 4
#define NIR_MAX_XFB_STREAMS 4

typedef struct {
   uint8_t buffer;
   uint16_t offset;
   uint8_t location;
   uint8_t component_mask;
} nir_xfb_output_info;

typedef struct {
   uint8_t buffers_written;
   uint8_t streams_written;

   uint16_t strides[NIR_MAX_XFB_BUFFERS];
   uint8_t buffer_to_stream[NIR_MAX_XFB_STREAMS];

   uint16_t output_count;
   nir_xfb_output_info outputs[0];
} nir_xfb_info;

static inline size_t
nir_xfb_info_size(uint16_t output_count)
{
   return sizeof(nir_xfb_info) + sizeof(nir_xfb_output_info) * output_count;
}

nir_xfb_info *
nir_gather_xfb_info(const nir_shader *shader, void *mem_ctx);

#endif /* NIR_XFB_INFO_H */
