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

#include "nir_xfb_info.h"

#include <util/u_math.h>

static void
add_var_xfb_outputs(nir_xfb_info *xfb,
                    nir_variable *var,
                    unsigned *location,
                    unsigned *offset,
                    const struct glsl_type *type)
{
   if (glsl_type_is_array(type) || glsl_type_is_matrix(type)) {
      unsigned length = glsl_get_length(type);
      const struct glsl_type *child_type = glsl_get_array_element(type);
      for (unsigned i = 0; i < length; i++)
         add_var_xfb_outputs(xfb, var, location, offset, child_type);
   } else if (glsl_type_is_struct(type)) {
      unsigned length = glsl_get_length(type);
      for (unsigned i = 0; i < length; i++) {
         const struct glsl_type *child_type = glsl_get_struct_field(type, i);
         add_var_xfb_outputs(xfb, var, location, offset, child_type);
      }
   } else {
      assert(var->data.xfb_buffer < NIR_MAX_XFB_BUFFERS);
      if (xfb->buffers_written & (1 << var->data.xfb_buffer)) {
         assert(xfb->strides[var->data.xfb_buffer] == var->data.xfb_stride);
         assert(xfb->buffer_to_stream[var->data.xfb_buffer] == var->data.stream);
      } else {
         xfb->buffers_written |= (1 << var->data.xfb_buffer);
         xfb->strides[var->data.xfb_buffer] = var->data.xfb_stride;
         xfb->buffer_to_stream[var->data.xfb_buffer] = var->data.stream;
      }

      assert(var->data.stream < NIR_MAX_XFB_STREAMS);
      xfb->streams_written |= (1 << var->data.stream);

      unsigned comp_slots = glsl_get_component_slots(type);
      unsigned attrib_slots = DIV_ROUND_UP(comp_slots, 4);
      assert(attrib_slots == glsl_count_attribute_slots(type, false));

      /* Ensure that we don't have, for instance, a dvec2 with a location_frac
       * of 2 which would make it crass a location boundary even though it
       * fits in a single slot.  However, you can have a dvec3 which crosses
       * the slot boundary with a location_frac of 2.
       */
      assert(DIV_ROUND_UP(var->data.location_frac + comp_slots, 4) == attrib_slots);

      assert(var->data.location_frac + comp_slots <= 8);
      uint8_t comp_mask = ((1 << comp_slots) - 1) << var->data.location_frac;

      assert(attrib_slots <= 2);
      for (unsigned s = 0; s < attrib_slots; s++) {
         nir_xfb_output_info *output = &xfb->outputs[xfb->output_count++];

         output->buffer = var->data.xfb_buffer;
         output->offset = *offset;
         output->location = *location;
         output->component_mask = (comp_mask >> (s * 4)) & 0xf;

         (*location)++;
         *offset += comp_slots * 4;
      }
   }
}

static int
compare_xfb_output_offsets(const void *_a, const void *_b)
{
   const nir_xfb_output_info *a = _a, *b = _b;
   return a->offset - b->offset;
}

nir_xfb_info *
nir_gather_xfb_info(const nir_shader *shader, void *mem_ctx)
{
   assert(shader->info.stage == MESA_SHADER_VERTEX ||
          shader->info.stage == MESA_SHADER_TESS_EVAL ||
          shader->info.stage == MESA_SHADER_GEOMETRY);

   /* Compute the number of outputs we have.  This is simply the number of
    * cumulative locations consumed by all the variables.  If a location is
    * represented by multiple variables, then they each count separately in
    * number of outputs.
    */
   unsigned num_outputs = 0;
   nir_foreach_variable(var, &shader->outputs) {
      if (var->data.explicit_xfb_buffer ||
          var->data.explicit_xfb_stride) {
         assert(var->data.explicit_xfb_buffer &&
                var->data.explicit_xfb_stride &&
                var->data.explicit_offset);
         num_outputs += glsl_count_attribute_slots(var->type, false);
      }
   }
   if (num_outputs == 0)
      return NULL;

   nir_xfb_info *xfb = rzalloc_size(mem_ctx, nir_xfb_info_size(num_outputs));

   /* Walk the list of outputs and add them to the array */
   nir_foreach_variable(var, &shader->outputs) {
      if (var->data.explicit_xfb_buffer ||
          var->data.explicit_xfb_stride) {
         unsigned location = var->data.location;
         unsigned offset = var->data.offset;
         add_var_xfb_outputs(xfb, var, &location, &offset, var->type);
      }
   }
   assert(xfb->output_count == num_outputs);

   /* Everything is easier in the state setup code if the list is sorted in
    * order of output offset.
    */
   qsort(xfb->outputs, xfb->output_count, sizeof(xfb->outputs[0]),
         compare_xfb_output_offsets);

   /* Finally, do a sanity check */
   unsigned max_offset[NIR_MAX_XFB_BUFFERS] = {0};
   for (unsigned i = 0; i < xfb->output_count; i++) {
      assert(xfb->outputs[i].offset >= max_offset[xfb->outputs[i].buffer]);
      assert(xfb->outputs[i].component_mask != 0);
      unsigned slots = util_bitcount(xfb->outputs[i].component_mask);
      max_offset[xfb->outputs[i].buffer] = xfb->outputs[i].offset + slots * 4;
   }

   return xfb;
}
