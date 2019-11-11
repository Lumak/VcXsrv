/*
 * Copyright © 2017 Timothy Arceri
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

#include "nir.h"
#include "nir_builder.h"
#include "nir_deref.h"

/** @file nir_lower_io_arrays_to_elements.c
 *
 * Split arrays/matrices with direct indexing into individual elements. This
 * will allow optimisation passes to better clean up unused elements.
 *
 */

static unsigned
get_io_offset(nir_builder *b, nir_deref_instr *deref, nir_variable *var,
              unsigned *element_index, nir_ssa_def **vertex_index)
{
   nir_deref_path path;
   nir_deref_path_init(&path, deref, NULL);

   assert(path.path[0]->deref_type == nir_deref_type_var);
   nir_deref_instr **p = &path.path[1];

   /* For per-vertex input arrays (i.e. geometry shader inputs), skip the
    * outermost array index.  Process the rest normally.
    */
   if (nir_is_per_vertex_io(var, b->shader->info.stage)) {
      *vertex_index = nir_ssa_for_src(b, (*p)->arr.index, 1);
      p++;
   }

   unsigned offset = 0;
   for (; *p; p++) {
      if ((*p)->deref_type == nir_deref_type_array) {
         /* must not be indirect dereference */
         unsigned index = nir_src_as_uint((*p)->arr.index);

         unsigned size = glsl_count_attribute_slots((*p)->type, false);
         offset += size * index;

         unsigned num_elements = glsl_type_is_array((*p)->type) ?
            glsl_get_aoa_size((*p)->type) : 1;

         num_elements *= glsl_type_is_matrix(glsl_without_array((*p)->type)) ?
            glsl_get_matrix_columns(glsl_without_array((*p)->type)) : 1;

         *element_index += num_elements * index;
      } else if ((*p)->deref_type == nir_deref_type_struct) {
         /* TODO: we could also add struct splitting support to this pass */
         break;
      }
   }

   nir_deref_path_finish(&path);

   return offset;
}

static nir_variable **
get_array_elements(struct hash_table *ht, nir_variable *var,
                   gl_shader_stage stage)
{
   nir_variable **elements;
   struct hash_entry *entry = _mesa_hash_table_search(ht, var);
   if (!entry) {
      const struct glsl_type *type = var->type;
      if (nir_is_per_vertex_io(var, stage)) {
         assert(glsl_type_is_array(type));
         type = glsl_get_array_element(type);
      }

      unsigned num_elements = glsl_type_is_array(type) ?
         glsl_get_aoa_size(type) : 1;

      num_elements *= glsl_type_is_matrix(glsl_without_array(type)) ?
         glsl_get_matrix_columns(glsl_without_array(type)) : 1;

      elements = (nir_variable **) calloc(num_elements, sizeof(nir_variable *));
      _mesa_hash_table_insert(ht, var, elements);
   } else {
      elements = (nir_variable **) entry->data;
   }

   return elements;
}

static void
lower_array(nir_builder *b, nir_intrinsic_instr *intr, nir_variable *var,
            struct hash_table *varyings)
{
   b->cursor = nir_before_instr(&intr->instr);

   nir_variable **elements =
      get_array_elements(varyings, var, b->shader->info.stage);

   nir_ssa_def *vertex_index = NULL;
   unsigned elements_index = 0;
   unsigned io_offset = get_io_offset(b, nir_src_as_deref(intr->src[0]),
                                      var, &elements_index, &vertex_index);

   nir_variable *element = elements[elements_index];
   if (!element) {
         element = nir_variable_clone(var, b->shader);
         element->data.location =  var->data.location + io_offset;

         const struct glsl_type *type = glsl_without_array(element->type);

         /* This pass also splits matrices so we need give them a new type. */
         if (glsl_type_is_matrix(type)) {
            type = glsl_vector_type(glsl_get_base_type(type),
                                    glsl_get_vector_elements(type));
         }

         if (nir_is_per_vertex_io(var, b->shader->info.stage)) {
            type = glsl_get_array_instance(type,
                                           glsl_get_length(element->type));
         }

         element->type = type;
         elements[elements_index] = element;

         nir_shader_add_variable(b->shader, element);
   }

   nir_deref_instr *element_deref = nir_build_deref_var(b, element);

   if (nir_is_per_vertex_io(var, b->shader->info.stage)) {
      assert(vertex_index);
      element_deref = nir_build_deref_array(b, element_deref, vertex_index);
   }

   nir_intrinsic_instr *element_intr =
      nir_intrinsic_instr_create(b->shader, intr->intrinsic);
   element_intr->num_components = intr->num_components;
   element_intr->src[0] = nir_src_for_ssa(&element_deref->dest.ssa);

   if (intr->intrinsic != nir_intrinsic_store_deref) {
      nir_ssa_dest_init(&element_intr->instr, &element_intr->dest,
                        intr->num_components, intr->dest.ssa.bit_size, NULL);

      if (intr->intrinsic == nir_intrinsic_interp_deref_at_offset ||
          intr->intrinsic == nir_intrinsic_interp_deref_at_sample) {
         nir_src_copy(&element_intr->src[1], &intr->src[1],
                      &element_intr->instr);
      }

      nir_ssa_def_rewrite_uses(&intr->dest.ssa,
                               nir_src_for_ssa(&element_intr->dest.ssa));
   } else {
      nir_intrinsic_set_write_mask(element_intr,
                                   nir_intrinsic_write_mask(intr));
      nir_src_copy(&element_intr->src[1], &intr->src[1],
                   &element_intr->instr);
   }

   nir_builder_instr_insert(b, &element_intr->instr);

   /* Remove the old load intrinsic */
   nir_instr_remove(&intr->instr);
}

static bool
deref_has_indirect(nir_builder *b, nir_variable *var, nir_deref_path *path)
{
   assert(path->path[0]->deref_type == nir_deref_type_var);
   nir_deref_instr **p = &path->path[1];

   if (nir_is_per_vertex_io(var, b->shader->info.stage)) {
      p++;
   }

   for (; *p; p++) {
      if ((*p)->deref_type != nir_deref_type_array)
         continue;

      if (!nir_src_is_const((*p)->arr.index))
         return true;
   }

   return false;
}

/* Creates a mask of locations that contains arrays that are indexed via
 * indirect indexing.
 */
static void
create_indirects_mask(nir_shader *shader, uint64_t *indirects,
                      uint64_t *patch_indirects, nir_variable_mode mode)
{
   nir_foreach_function(function, shader) {
      if (function->impl) {
         nir_builder b;
         nir_builder_init(&b, function->impl);

         nir_foreach_block(block, function->impl) {
            nir_foreach_instr_safe(instr, block) {

               if (instr->type != nir_instr_type_intrinsic)
                  continue;

               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

               if (intr->intrinsic != nir_intrinsic_load_deref &&
                   intr->intrinsic != nir_intrinsic_store_deref &&
                   intr->intrinsic != nir_intrinsic_interp_deref_at_centroid &&
                   intr->intrinsic != nir_intrinsic_interp_deref_at_sample &&
                   intr->intrinsic != nir_intrinsic_interp_deref_at_offset)
                  continue;

               nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
               nir_variable *var = nir_deref_instr_get_variable(deref);

               if (var->data.mode != mode)
                  continue;

               nir_deref_path path;
               nir_deref_path_init(&path, deref, NULL);

               uint64_t loc_mask = ((uint64_t)1) << var->data.location;
               if (var->data.patch) {
                  if (deref_has_indirect(&b, var, &path))
                     patch_indirects[var->data.location_frac] |= loc_mask;
               } else {
                  if (deref_has_indirect(&b, var, &path))
                     indirects[var->data.location_frac] |= loc_mask;
               }

               nir_deref_path_finish(&path);
            }
         }
      }
   }
}

static void
lower_io_arrays_to_elements(nir_shader *shader, nir_variable_mode mask,
                            uint64_t *indirects, uint64_t *patch_indirects,
                            struct hash_table *varyings,
                            bool after_cross_stage_opts)
{
   nir_foreach_function(function, shader) {
      if (function->impl) {
         nir_builder b;
         nir_builder_init(&b, function->impl);

         nir_foreach_block(block, function->impl) {
            nir_foreach_instr_safe(instr, block) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;

               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

               if (intr->intrinsic != nir_intrinsic_load_deref &&
                   intr->intrinsic != nir_intrinsic_store_deref &&
                   intr->intrinsic != nir_intrinsic_interp_deref_at_centroid &&
                   intr->intrinsic != nir_intrinsic_interp_deref_at_sample &&
                   intr->intrinsic != nir_intrinsic_interp_deref_at_offset)
                  continue;

               nir_variable *var =
                  nir_deref_instr_get_variable(nir_src_as_deref(intr->src[0]));

               /* Skip indirects */
               uint64_t loc_mask = ((uint64_t)1) << var->data.location;
               if (var->data.patch) {
                  if (patch_indirects[var->data.location_frac] & loc_mask)
                     continue;
               } else {
                  if (indirects[var->data.location_frac] & loc_mask)
                     continue;
               }

               nir_variable_mode mode = var->data.mode;

               const struct glsl_type *type = var->type;
               if (nir_is_per_vertex_io(var, b.shader->info.stage)) {
                  assert(glsl_type_is_array(type));
                  type = glsl_get_array_element(type);
               }

               /* Skip types we cannot split.
                *
                * TODO: Add support for struct splitting.
                */
               if ((!glsl_type_is_array(type) && !glsl_type_is_matrix(type))||
                   glsl_type_is_struct(glsl_without_array(type)))
                  continue;

               /* Skip builtins */
               if (!after_cross_stage_opts &&
                   var->data.location < VARYING_SLOT_VAR0 &&
                   var->data.location >= 0)
                  continue;

               /* Don't bother splitting if we can't opt away any unused
                * elements.
                */
               if (!after_cross_stage_opts && var->data.always_active_io)
                  continue;

               switch (intr->intrinsic) {
               case nir_intrinsic_interp_deref_at_centroid:
               case nir_intrinsic_interp_deref_at_sample:
               case nir_intrinsic_interp_deref_at_offset:
               case nir_intrinsic_load_deref:
               case nir_intrinsic_store_deref:
                  if ((mask & nir_var_shader_in && mode == nir_var_shader_in) ||
                      (mask & nir_var_shader_out && mode == nir_var_shader_out))
                     lower_array(&b, intr, var, varyings);
                  break;
               default:
                  break;
               }
            }
         }
      }
   }
}

void
nir_lower_io_arrays_to_elements_no_indirects(nir_shader *shader,
                                             bool outputs_only)
{
   struct hash_table *split_inputs =
      _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                              _mesa_key_pointer_equal);
   struct hash_table *split_outputs =
      _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                              _mesa_key_pointer_equal);

   uint64_t indirects[4] = {0}, patch_indirects[4] = {0};

   lower_io_arrays_to_elements(shader, nir_var_shader_out, indirects,
                               patch_indirects, split_outputs, true);

   if (!outputs_only) {
      lower_io_arrays_to_elements(shader, nir_var_shader_in, indirects,
                                  patch_indirects, split_inputs, true);

      /* Remove old input from the shaders inputs list */
      hash_table_foreach(split_inputs, entry) {
         nir_variable *var = (nir_variable *) entry->key;
         exec_node_remove(&var->node);

         free(entry->data);
      }
   }

   /* Remove old output from the shaders outputs list */
   hash_table_foreach(split_outputs, entry) {
      nir_variable *var = (nir_variable *) entry->key;
      exec_node_remove(&var->node);

      free(entry->data);
   }

   _mesa_hash_table_destroy(split_inputs, NULL);
   _mesa_hash_table_destroy(split_outputs, NULL);

   nir_remove_dead_derefs(shader);
}

void
nir_lower_io_arrays_to_elements(nir_shader *producer, nir_shader *consumer)
{
   struct hash_table *split_inputs =
      _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                              _mesa_key_pointer_equal);
   struct hash_table *split_outputs =
      _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                              _mesa_key_pointer_equal);

   uint64_t indirects[4] = {0}, patch_indirects[4] = {0};
   create_indirects_mask(producer, indirects, patch_indirects,
                         nir_var_shader_out);
   create_indirects_mask(consumer, indirects, patch_indirects,
                         nir_var_shader_in);

   lower_io_arrays_to_elements(producer, nir_var_shader_out, indirects,
                               patch_indirects, split_outputs, false);

   lower_io_arrays_to_elements(consumer, nir_var_shader_in, indirects,
                               patch_indirects, split_inputs, false);

   /* Remove old input from the shaders inputs list */
   hash_table_foreach(split_inputs, entry) {
      nir_variable *var = (nir_variable *) entry->key;
      exec_node_remove(&var->node);

      free(entry->data);
   }

   /* Remove old output from the shaders outputs list */
   hash_table_foreach(split_outputs, entry) {
      nir_variable *var = (nir_variable *) entry->key;
      exec_node_remove(&var->node);

      free(entry->data);
   }

   _mesa_hash_table_destroy(split_inputs, NULL);
   _mesa_hash_table_destroy(split_outputs, NULL);

   nir_remove_dead_derefs(producer);
   nir_remove_dead_derefs(consumer);
}
