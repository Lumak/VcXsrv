/*
 * Copyright © 2017 Intel Corporation
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

#ifndef GL_NIR_LINKER_H
#define GL_NIR_LINKER_H

#ifdef __cplusplus
extern "C" {
#endif

struct gl_context;
struct gl_shader_program;

bool gl_nir_link_uniforms(struct gl_context *ctx,
                          struct gl_shader_program *prog);

void gl_nir_set_uniform_initializers(struct gl_context *ctx,
                                     struct gl_shader_program *prog);

void nir_build_program_resource_list(struct gl_context *ctx,
                                     struct gl_shader_program *prog);

void gl_nir_link_assign_atomic_counter_resources(struct gl_context *ctx,
                                                 struct gl_shader_program *prog);

void gl_nir_link_assign_xfb_resources(struct gl_context *ctx,
                                      struct gl_shader_program *prog);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GL_NIR_LINKER_H */
