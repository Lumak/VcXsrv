/*
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
 *
 * Authors:
 *    Jason Ekstrand (jason@jlekstrand.net)
 *
 */

#include "vtn_private.h"
#include "spirv_info.h"
#include "nir_deref.h"

static struct vtn_access_chain *
vtn_access_chain_create(struct vtn_builder *b, unsigned length)
{
   struct vtn_access_chain *chain;

   /* Subtract 1 from the length since there's already one built in */
   size_t size = sizeof(*chain) +
                 (MAX2(length, 1) - 1) * sizeof(chain->link[0]);
   chain = rzalloc_size(b, size);
   chain->length = length;

   return chain;
}

static struct vtn_access_chain *
vtn_access_chain_extend(struct vtn_builder *b, struct vtn_access_chain *old,
                        unsigned new_ids)
{
   struct vtn_access_chain *chain;

   unsigned old_len = old ? old->length : 0;
   chain = vtn_access_chain_create(b, old_len + new_ids);

   for (unsigned i = 0; i < old_len; i++)
      chain->link[i] = old->link[i];

   return chain;
}

static bool
vtn_pointer_uses_ssa_offset(struct vtn_builder *b,
                            struct vtn_pointer *ptr)
{
   return ptr->mode == vtn_variable_mode_ubo ||
          ptr->mode == vtn_variable_mode_ssbo ||
          ptr->mode == vtn_variable_mode_push_constant ||
          (ptr->mode == vtn_variable_mode_workgroup &&
           b->options->lower_workgroup_access_to_offsets);
}

static bool
vtn_pointer_is_external_block(struct vtn_builder *b,
                              struct vtn_pointer *ptr)
{
   return ptr->mode == vtn_variable_mode_ssbo ||
          ptr->mode == vtn_variable_mode_ubo ||
          ptr->mode == vtn_variable_mode_push_constant ||
          (ptr->mode == vtn_variable_mode_workgroup &&
           b->options->lower_workgroup_access_to_offsets);
}

/* Dereference the given base pointer by the access chain */
static struct vtn_pointer *
vtn_access_chain_pointer_dereference(struct vtn_builder *b,
                                     struct vtn_pointer *base,
                                     struct vtn_access_chain *deref_chain)
{
   struct vtn_access_chain *chain =
      vtn_access_chain_extend(b, base->chain, deref_chain->length);
   struct vtn_type *type = base->type;
   enum gl_access_qualifier access = base->access;

   /* OpPtrAccessChain is only allowed on things which support variable
    * pointers.  For everything else, the client is expected to just pass us
    * the right access chain.
    */
   vtn_assert(!deref_chain->ptr_as_array);

   unsigned start = base->chain ? base->chain->length : 0;
   for (unsigned i = 0; i < deref_chain->length; i++) {
      chain->link[start + i] = deref_chain->link[i];

      if (glsl_type_is_struct(type->type)) {
         vtn_assert(deref_chain->link[i].mode == vtn_access_mode_literal);
         type = type->members[deref_chain->link[i].id];
      } else {
         type = type->array_element;
      }

      access |= type->access;
   }

   struct vtn_pointer *ptr = rzalloc(b, struct vtn_pointer);
   ptr->mode = base->mode;
   ptr->type = type;
   ptr->var = base->var;
   ptr->deref = base->deref;
   ptr->chain = chain;
   ptr->access = access;

   return ptr;
}

static nir_ssa_def *
vtn_access_link_as_ssa(struct vtn_builder *b, struct vtn_access_link link,
                       unsigned stride)
{
   vtn_assert(stride > 0);
   if (link.mode == vtn_access_mode_literal) {
      return nir_imm_int(&b->nb, link.id * stride);
   } else if (stride == 1) {
       nir_ssa_def *ssa = vtn_ssa_value(b, link.id)->def;
       if (ssa->bit_size != 32)
          ssa = nir_u2u32(&b->nb, ssa);
      return ssa;
   } else {
      nir_ssa_def *src0 = vtn_ssa_value(b, link.id)->def;
      if (src0->bit_size != 32)
         src0 = nir_u2u32(&b->nb, src0);
      return nir_imul_imm(&b->nb, src0, stride);
   }
}

static nir_ssa_def *
vtn_variable_resource_index(struct vtn_builder *b, struct vtn_variable *var,
                            nir_ssa_def *desc_array_index)
{
   if (!desc_array_index) {
      vtn_assert(glsl_type_is_struct(var->type->type));
      desc_array_index = nir_imm_int(&b->nb, 0);
   }

   nir_intrinsic_instr *instr =
      nir_intrinsic_instr_create(b->nb.shader,
                                 nir_intrinsic_vulkan_resource_index);
   instr->src[0] = nir_src_for_ssa(desc_array_index);
   nir_intrinsic_set_desc_set(instr, var->descriptor_set);
   nir_intrinsic_set_binding(instr, var->binding);

   nir_ssa_dest_init(&instr->instr, &instr->dest, 1, 32, NULL);
   nir_builder_instr_insert(&b->nb, &instr->instr);

   return &instr->dest.ssa;
}

static nir_ssa_def *
vtn_resource_reindex(struct vtn_builder *b, nir_ssa_def *base_index,
                     nir_ssa_def *offset_index)
{
   nir_intrinsic_instr *instr =
      nir_intrinsic_instr_create(b->nb.shader,
                                 nir_intrinsic_vulkan_resource_reindex);
   instr->src[0] = nir_src_for_ssa(base_index);
   instr->src[1] = nir_src_for_ssa(offset_index);

   nir_ssa_dest_init(&instr->instr, &instr->dest, 1, 32, NULL);
   nir_builder_instr_insert(&b->nb, &instr->instr);

   return &instr->dest.ssa;
}

static struct vtn_pointer *
vtn_ssa_offset_pointer_dereference(struct vtn_builder *b,
                                   struct vtn_pointer *base,
                                   struct vtn_access_chain *deref_chain)
{
   nir_ssa_def *block_index = base->block_index;
   nir_ssa_def *offset = base->offset;
   struct vtn_type *type = base->type;
   enum gl_access_qualifier access = base->access;

   unsigned idx = 0;
   if (base->mode == vtn_variable_mode_ubo ||
       base->mode == vtn_variable_mode_ssbo) {
      if (!block_index) {
         vtn_assert(base->var && base->type);
         nir_ssa_def *desc_arr_idx;
         if (glsl_type_is_array(type->type)) {
            if (deref_chain->length >= 1) {
               desc_arr_idx =
                  vtn_access_link_as_ssa(b, deref_chain->link[0], 1);
               idx++;
               /* This consumes a level of type */
               type = type->array_element;
               access |= type->access;
            } else {
               /* This is annoying.  We've been asked for a pointer to the
                * array of UBOs/SSBOs and not a specifc buffer.  Return a
                * pointer with a descriptor index of 0 and we'll have to do
                * a reindex later to adjust it to the right thing.
                */
               desc_arr_idx = nir_imm_int(&b->nb, 0);
            }
         } else if (deref_chain->ptr_as_array) {
            /* You can't have a zero-length OpPtrAccessChain */
            vtn_assert(deref_chain->length >= 1);
            desc_arr_idx = vtn_access_link_as_ssa(b, deref_chain->link[0], 1);
         } else {
            /* We have a regular non-array SSBO. */
            desc_arr_idx = NULL;
         }
         block_index = vtn_variable_resource_index(b, base->var, desc_arr_idx);
      } else if (deref_chain->ptr_as_array &&
                 type->base_type == vtn_base_type_struct && type->block) {
         /* We are doing an OpPtrAccessChain on a pointer to a struct that is
          * decorated block.  This is an interesting corner in the SPIR-V
          * spec.  One interpretation would be that they client is clearly
          * trying to treat that block as if it's an implicit array of blocks
          * repeated in the buffer.  However, the SPIR-V spec for the
          * OpPtrAccessChain says:
          *
          *    "Base is treated as the address of the first element of an
          *    array, and the Element element’s address is computed to be the
          *    base for the Indexes, as per OpAccessChain."
          *
          * Taken literally, that would mean that your struct type is supposed
          * to be treated as an array of such a struct and, since it's
          * decorated block, that means an array of blocks which corresponds
          * to an array descriptor.  Therefore, we need to do a reindex
          * operation to add the index from the first link in the access chain
          * to the index we recieved.
          *
          * The downside to this interpretation (there always is one) is that
          * this might be somewhat surprising behavior to apps if they expect
          * the implicit array behavior described above.
          */
         vtn_assert(deref_chain->length >= 1);
         nir_ssa_def *offset_index =
            vtn_access_link_as_ssa(b, deref_chain->link[0], 1);
         idx++;

         block_index = vtn_resource_reindex(b, block_index, offset_index);
      }
   }

   if (!offset) {
      if (base->mode == vtn_variable_mode_workgroup) {
         /* SLM doesn't need nor have a block index */
         vtn_assert(!block_index);

         /* We need the variable for the base offset */
         vtn_assert(base->var);

         /* We need ptr_type for size and alignment */
         vtn_assert(base->ptr_type);

         /* Assign location on first use so that we don't end up bloating SLM
          * address space for variables which are never statically used.
          */
         if (base->var->shared_location < 0) {
            vtn_assert(base->ptr_type->length > 0 && base->ptr_type->align > 0);
            b->shader->num_shared = vtn_align_u32(b->shader->num_shared,
                                                  base->ptr_type->align);
            base->var->shared_location = b->shader->num_shared;
            b->shader->num_shared += base->ptr_type->length;
         }

         offset = nir_imm_int(&b->nb, base->var->shared_location);
      } else if (base->mode == vtn_variable_mode_push_constant) {
         /* Push constants neither need nor have a block index */
         vtn_assert(!block_index);

         /* Start off with at the start of the push constant block. */
         offset = nir_imm_int(&b->nb, 0);
      } else {
         /* The code above should have ensured a block_index when needed. */
         vtn_assert(block_index);

         /* Start off with at the start of the buffer. */
         offset = nir_imm_int(&b->nb, 0);
      }
   }

   if (deref_chain->ptr_as_array && idx == 0) {
      /* We need ptr_type for the stride */
      vtn_assert(base->ptr_type);

      /* We need at least one element in the chain */
      vtn_assert(deref_chain->length >= 1);

      nir_ssa_def *elem_offset =
         vtn_access_link_as_ssa(b, deref_chain->link[idx],
                                base->ptr_type->stride);
      offset = nir_iadd(&b->nb, offset, elem_offset);
      idx++;
   }

   for (; idx < deref_chain->length; idx++) {
      switch (glsl_get_base_type(type->type)) {
      case GLSL_TYPE_UINT:
      case GLSL_TYPE_INT:
      case GLSL_TYPE_UINT16:
      case GLSL_TYPE_INT16:
      case GLSL_TYPE_UINT8:
      case GLSL_TYPE_INT8:
      case GLSL_TYPE_UINT64:
      case GLSL_TYPE_INT64:
      case GLSL_TYPE_FLOAT:
      case GLSL_TYPE_FLOAT16:
      case GLSL_TYPE_DOUBLE:
      case GLSL_TYPE_BOOL:
      case GLSL_TYPE_ARRAY: {
         nir_ssa_def *elem_offset =
            vtn_access_link_as_ssa(b, deref_chain->link[idx], type->stride);
         offset = nir_iadd(&b->nb, offset, elem_offset);
         type = type->array_element;
         access |= type->access;
         break;
      }

      case GLSL_TYPE_STRUCT: {
         vtn_assert(deref_chain->link[idx].mode == vtn_access_mode_literal);
         unsigned member = deref_chain->link[idx].id;
         offset = nir_iadd_imm(&b->nb, offset, type->offsets[member]);
         type = type->members[member];
         access |= type->access;
         break;
      }

      default:
         vtn_fail("Invalid type for deref");
      }
   }

   struct vtn_pointer *ptr = rzalloc(b, struct vtn_pointer);
   ptr->mode = base->mode;
   ptr->type = type;
   ptr->block_index = block_index;
   ptr->offset = offset;
   ptr->access = access;

   return ptr;
}

/* Dereference the given base pointer by the access chain */
static struct vtn_pointer *
vtn_pointer_dereference(struct vtn_builder *b,
                        struct vtn_pointer *base,
                        struct vtn_access_chain *deref_chain)
{
   if (vtn_pointer_uses_ssa_offset(b, base)) {
      return vtn_ssa_offset_pointer_dereference(b, base, deref_chain);
   } else {
      return vtn_access_chain_pointer_dereference(b, base, deref_chain);
   }
}

struct vtn_pointer *
vtn_pointer_for_variable(struct vtn_builder *b,
                         struct vtn_variable *var, struct vtn_type *ptr_type)
{
   struct vtn_pointer *pointer = rzalloc(b, struct vtn_pointer);

   pointer->mode = var->mode;
   pointer->type = var->type;
   vtn_assert(ptr_type->base_type == vtn_base_type_pointer);
   vtn_assert(ptr_type->deref->type == var->type->type);
   pointer->ptr_type = ptr_type;
   pointer->var = var;
   pointer->access = var->access | var->type->access;

   return pointer;
}

/* Returns an atomic_uint type based on the original uint type. The returned
 * type will be equivalent to the original one but will have an atomic_uint
 * type as leaf instead of an uint.
 *
 * Manages uint scalars, arrays, and arrays of arrays of any nested depth.
 */
static const struct glsl_type *
repair_atomic_type(const struct glsl_type *type)
{
   assert(glsl_get_base_type(glsl_without_array(type)) == GLSL_TYPE_UINT);
   assert(glsl_type_is_scalar(glsl_without_array(type)));

   if (glsl_type_is_array(type)) {
      const struct glsl_type *atomic =
         repair_atomic_type(glsl_get_array_element(type));

      return glsl_array_type(atomic, glsl_get_length(type));
   } else {
      return glsl_atomic_uint_type();
   }
}

nir_deref_instr *
vtn_pointer_to_deref(struct vtn_builder *b, struct vtn_pointer *ptr)
{
   /* Do on-the-fly copy propagation for samplers. */
   if (ptr->var && ptr->var->copy_prop_sampler)
      return vtn_pointer_to_deref(b, ptr->var->copy_prop_sampler);

   nir_deref_instr *tail;
   if (ptr->deref) {
      tail = ptr->deref;
   } else {
      assert(ptr->var && ptr->var->var);
      tail = nir_build_deref_var(&b->nb, ptr->var->var);
   }

   /* Raw variable access */
   if (!ptr->chain)
      return tail;

   struct vtn_access_chain *chain = ptr->chain;
   vtn_assert(chain);

   for (unsigned i = 0; i < chain->length; i++) {
      if (glsl_type_is_struct(tail->type)) {
         vtn_assert(chain->link[i].mode == vtn_access_mode_literal);
         unsigned idx = chain->link[i].id;
         tail = nir_build_deref_struct(&b->nb, tail, idx);
      } else {
         nir_ssa_def *index;
         if (chain->link[i].mode == vtn_access_mode_literal) {
            index = nir_imm_int(&b->nb, chain->link[i].id);
         } else {
            vtn_assert(chain->link[i].mode == vtn_access_mode_id);
            index = vtn_ssa_value(b, chain->link[i].id)->def;
         }
         tail = nir_build_deref_array(&b->nb, tail, index);
      }
   }

   return tail;
}

static void
_vtn_local_load_store(struct vtn_builder *b, bool load, nir_deref_instr *deref,
                      struct vtn_ssa_value *inout)
{
   if (glsl_type_is_vector_or_scalar(deref->type)) {
      if (load) {
         inout->def = nir_load_deref(&b->nb, deref);
      } else {
         nir_store_deref(&b->nb, deref, inout->def, ~0);
      }
   } else if (glsl_type_is_array(deref->type) ||
              glsl_type_is_matrix(deref->type)) {
      unsigned elems = glsl_get_length(deref->type);
      for (unsigned i = 0; i < elems; i++) {
         nir_deref_instr *child =
            nir_build_deref_array(&b->nb, deref, nir_imm_int(&b->nb, i));
         _vtn_local_load_store(b, load, child, inout->elems[i]);
      }
   } else {
      vtn_assert(glsl_type_is_struct(deref->type));
      unsigned elems = glsl_get_length(deref->type);
      for (unsigned i = 0; i < elems; i++) {
         nir_deref_instr *child = nir_build_deref_struct(&b->nb, deref, i);
         _vtn_local_load_store(b, load, child, inout->elems[i]);
      }
   }
}

nir_deref_instr *
vtn_nir_deref(struct vtn_builder *b, uint32_t id)
{
   struct vtn_pointer *ptr = vtn_value(b, id, vtn_value_type_pointer)->pointer;
   return vtn_pointer_to_deref(b, ptr);
}

/*
 * Gets the NIR-level deref tail, which may have as a child an array deref
 * selecting which component due to OpAccessChain supporting per-component
 * indexing in SPIR-V.
 */
static nir_deref_instr *
get_deref_tail(nir_deref_instr *deref)
{
   if (deref->deref_type != nir_deref_type_array)
      return deref;

   nir_deref_instr *parent =
      nir_instr_as_deref(deref->parent.ssa->parent_instr);

   if (glsl_type_is_vector(parent->type))
      return parent;
   else
      return deref;
}

struct vtn_ssa_value *
vtn_local_load(struct vtn_builder *b, nir_deref_instr *src)
{
   nir_deref_instr *src_tail = get_deref_tail(src);
   struct vtn_ssa_value *val = vtn_create_ssa_value(b, src_tail->type);
   _vtn_local_load_store(b, true, src_tail, val);

   if (src_tail != src) {
      val->type = src->type;
      nir_const_value *const_index = nir_src_as_const_value(src->arr.index);
      if (const_index)
         val->def = vtn_vector_extract(b, val->def, const_index->u32[0]);
      else
         val->def = vtn_vector_extract_dynamic(b, val->def, src->arr.index.ssa);
   }

   return val;
}

void
vtn_local_store(struct vtn_builder *b, struct vtn_ssa_value *src,
                nir_deref_instr *dest)
{
   nir_deref_instr *dest_tail = get_deref_tail(dest);

   if (dest_tail != dest) {
      struct vtn_ssa_value *val = vtn_create_ssa_value(b, dest_tail->type);
      _vtn_local_load_store(b, true, dest_tail, val);

      nir_const_value *const_index = nir_src_as_const_value(dest->arr.index);
      if (const_index)
         val->def = vtn_vector_insert(b, val->def, src->def,
                                      const_index->u32[0]);
      else
         val->def = vtn_vector_insert_dynamic(b, val->def, src->def,
                                              dest->arr.index.ssa);
      _vtn_local_load_store(b, false, dest_tail, val);
   } else {
      _vtn_local_load_store(b, false, dest_tail, src);
   }
}

nir_ssa_def *
vtn_pointer_to_offset(struct vtn_builder *b, struct vtn_pointer *ptr,
                      nir_ssa_def **index_out)
{
   assert(vtn_pointer_uses_ssa_offset(b, ptr));
   if (!ptr->offset) {
      struct vtn_access_chain chain = {
         .length = 0,
      };
      ptr = vtn_ssa_offset_pointer_dereference(b, ptr, &chain);
   }
   *index_out = ptr->block_index;
   return ptr->offset;
}

/* Tries to compute the size of an interface block based on the strides and
 * offsets that are provided to us in the SPIR-V source.
 */
static unsigned
vtn_type_block_size(struct vtn_builder *b, struct vtn_type *type)
{
   enum glsl_base_type base_type = glsl_get_base_type(type->type);
   switch (base_type) {
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
   case GLSL_TYPE_UINT16:
   case GLSL_TYPE_INT16:
   case GLSL_TYPE_UINT8:
   case GLSL_TYPE_INT8:
   case GLSL_TYPE_UINT64:
   case GLSL_TYPE_INT64:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_FLOAT16:
   case GLSL_TYPE_BOOL:
   case GLSL_TYPE_DOUBLE: {
      unsigned cols = type->row_major ? glsl_get_vector_elements(type->type) :
                                        glsl_get_matrix_columns(type->type);
      if (cols > 1) {
         vtn_assert(type->stride > 0);
         return type->stride * cols;
      } else {
         unsigned type_size = glsl_get_bit_size(type->type) / 8;
         return glsl_get_vector_elements(type->type) * type_size;
      }
   }

   case GLSL_TYPE_STRUCT:
   case GLSL_TYPE_INTERFACE: {
      unsigned size = 0;
      unsigned num_fields = glsl_get_length(type->type);
      for (unsigned f = 0; f < num_fields; f++) {
         unsigned field_end = type->offsets[f] +
                              vtn_type_block_size(b, type->members[f]);
         size = MAX2(size, field_end);
      }
      return size;
   }

   case GLSL_TYPE_ARRAY:
      vtn_assert(type->stride > 0);
      vtn_assert(glsl_get_length(type->type) > 0);
      return type->stride * glsl_get_length(type->type);

   default:
      vtn_fail("Invalid block type");
      return 0;
   }
}

static void
_vtn_load_store_tail(struct vtn_builder *b, nir_intrinsic_op op, bool load,
                     nir_ssa_def *index, nir_ssa_def *offset,
                     unsigned access_offset, unsigned access_size,
                     struct vtn_ssa_value **inout, const struct glsl_type *type,
                     enum gl_access_qualifier access)
{
   nir_intrinsic_instr *instr = nir_intrinsic_instr_create(b->nb.shader, op);
   instr->num_components = glsl_get_vector_elements(type);

   /* Booleans usually shouldn't show up in external memory in SPIR-V.
    * However, they do for certain older GLSLang versions and can for shared
    * memory when we lower access chains internally.
    */
   const unsigned data_bit_size = glsl_type_is_boolean(type) ? 32 :
                                  glsl_get_bit_size(type);

   int src = 0;
   if (!load) {
      nir_intrinsic_set_write_mask(instr, (1 << instr->num_components) - 1);
      instr->src[src++] = nir_src_for_ssa((*inout)->def);
   }

   if (op == nir_intrinsic_load_push_constant) {
      nir_intrinsic_set_base(instr, access_offset);
      nir_intrinsic_set_range(instr, access_size);
   }

   if (op == nir_intrinsic_load_ssbo ||
       op == nir_intrinsic_store_ssbo) {
      nir_intrinsic_set_access(instr, access);
   }

   /* With extensions like relaxed_block_layout, we really can't guarantee
    * much more than scalar alignment.
    */
   if (op != nir_intrinsic_load_push_constant)
      nir_intrinsic_set_align(instr, data_bit_size / 8, 0);

   if (index)
      instr->src[src++] = nir_src_for_ssa(index);

   if (op == nir_intrinsic_load_push_constant) {
      /* We need to subtract the offset from where the intrinsic will load the
       * data. */
      instr->src[src++] =
         nir_src_for_ssa(nir_isub(&b->nb, offset,
                                  nir_imm_int(&b->nb, access_offset)));
   } else {
      instr->src[src++] = nir_src_for_ssa(offset);
   }

   if (load) {
      nir_ssa_dest_init(&instr->instr, &instr->dest,
                        instr->num_components, data_bit_size, NULL);
      (*inout)->def = &instr->dest.ssa;
   }

   nir_builder_instr_insert(&b->nb, &instr->instr);

   if (load && glsl_get_base_type(type) == GLSL_TYPE_BOOL)
      (*inout)->def = nir_ine(&b->nb, (*inout)->def, nir_imm_int(&b->nb, 0));
}

static void
_vtn_block_load_store(struct vtn_builder *b, nir_intrinsic_op op, bool load,
                      nir_ssa_def *index, nir_ssa_def *offset,
                      unsigned access_offset, unsigned access_size,
                      struct vtn_type *type, enum gl_access_qualifier access,
                      struct vtn_ssa_value **inout)
{
   if (load && *inout == NULL)
      *inout = vtn_create_ssa_value(b, type->type);

   enum glsl_base_type base_type = glsl_get_base_type(type->type);
   switch (base_type) {
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
   case GLSL_TYPE_UINT16:
   case GLSL_TYPE_INT16:
   case GLSL_TYPE_UINT8:
   case GLSL_TYPE_INT8:
   case GLSL_TYPE_UINT64:
   case GLSL_TYPE_INT64:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_FLOAT16:
   case GLSL_TYPE_DOUBLE:
   case GLSL_TYPE_BOOL:
      /* This is where things get interesting.  At this point, we've hit
       * a vector, a scalar, or a matrix.
       */
      if (glsl_type_is_matrix(type->type)) {
         /* Loading the whole matrix */
         struct vtn_ssa_value *transpose;
         unsigned num_ops, vec_width, col_stride;
         if (type->row_major) {
            num_ops = glsl_get_vector_elements(type->type);
            vec_width = glsl_get_matrix_columns(type->type);
            col_stride = type->array_element->stride;
            if (load) {
               const struct glsl_type *transpose_type =
                  glsl_matrix_type(base_type, vec_width, num_ops);
               *inout = vtn_create_ssa_value(b, transpose_type);
            } else {
               transpose = vtn_ssa_transpose(b, *inout);
               inout = &transpose;
            }
         } else {
            num_ops = glsl_get_matrix_columns(type->type);
            vec_width = glsl_get_vector_elements(type->type);
            col_stride = type->stride;
         }

         for (unsigned i = 0; i < num_ops; i++) {
            nir_ssa_def *elem_offset =
               nir_iadd_imm(&b->nb, offset, i * col_stride);
            _vtn_load_store_tail(b, op, load, index, elem_offset,
                                 access_offset, access_size,
                                 &(*inout)->elems[i],
                                 glsl_vector_type(base_type, vec_width),
                                 type->access | access);
         }

         if (load && type->row_major)
            *inout = vtn_ssa_transpose(b, *inout);
      } else {
         unsigned elems = glsl_get_vector_elements(type->type);
         unsigned type_size = glsl_get_bit_size(type->type) / 8;
         if (elems == 1 || type->stride == type_size) {
            /* This is a tightly-packed normal scalar or vector load */
            vtn_assert(glsl_type_is_vector_or_scalar(type->type));
            _vtn_load_store_tail(b, op, load, index, offset,
                                 access_offset, access_size,
                                 inout, type->type,
                                 type->access | access);
         } else {
            /* This is a strided load.  We have to load N things separately.
             * This is the single column of a row-major matrix case.
             */
            vtn_assert(type->stride > type_size);
            vtn_assert(type->stride % type_size == 0);

            nir_ssa_def *per_comp[4];
            for (unsigned i = 0; i < elems; i++) {
               nir_ssa_def *elem_offset =
                  nir_iadd_imm(&b->nb, offset, i * type->stride);
               struct vtn_ssa_value *comp, temp_val;
               if (!load) {
                  temp_val.def = nir_channel(&b->nb, (*inout)->def, i);
                  temp_val.type = glsl_scalar_type(base_type);
               }
               comp = &temp_val;
               _vtn_load_store_tail(b, op, load, index, elem_offset,
                                    access_offset, access_size,
                                    &comp, glsl_scalar_type(base_type),
                                    type->access | access);
               per_comp[i] = comp->def;
            }

            if (load) {
               if (*inout == NULL)
                  *inout = vtn_create_ssa_value(b, type->type);
               (*inout)->def = nir_vec(&b->nb, per_comp, elems);
            }
         }
      }
      return;

   case GLSL_TYPE_ARRAY: {
      unsigned elems = glsl_get_length(type->type);
      for (unsigned i = 0; i < elems; i++) {
         nir_ssa_def *elem_off =
            nir_iadd_imm(&b->nb, offset, i * type->stride);
         _vtn_block_load_store(b, op, load, index, elem_off,
                               access_offset, access_size,
                               type->array_element,
                               type->array_element->access | access,
                               &(*inout)->elems[i]);
      }
      return;
   }

   case GLSL_TYPE_STRUCT: {
      unsigned elems = glsl_get_length(type->type);
      for (unsigned i = 0; i < elems; i++) {
         nir_ssa_def *elem_off =
            nir_iadd_imm(&b->nb, offset, type->offsets[i]);
         _vtn_block_load_store(b, op, load, index, elem_off,
                               access_offset, access_size,
                               type->members[i],
                               type->members[i]->access | access,
                               &(*inout)->elems[i]);
      }
      return;
   }

   default:
      vtn_fail("Invalid block member type");
   }
}

static struct vtn_ssa_value *
vtn_block_load(struct vtn_builder *b, struct vtn_pointer *src)
{
   nir_intrinsic_op op;
   unsigned access_offset = 0, access_size = 0;
   switch (src->mode) {
   case vtn_variable_mode_ubo:
      op = nir_intrinsic_load_ubo;
      break;
   case vtn_variable_mode_ssbo:
      op = nir_intrinsic_load_ssbo;
      break;
   case vtn_variable_mode_push_constant:
      op = nir_intrinsic_load_push_constant;
      access_size = b->shader->num_uniforms;
      break;
   case vtn_variable_mode_workgroup:
      op = nir_intrinsic_load_shared;
      break;
   default:
      vtn_fail("Invalid block variable mode");
   }

   nir_ssa_def *offset, *index = NULL;
   offset = vtn_pointer_to_offset(b, src, &index);

   struct vtn_ssa_value *value = NULL;
   _vtn_block_load_store(b, op, true, index, offset,
                         access_offset, access_size,
                         src->type, src->access, &value);
   return value;
}

static void
vtn_block_store(struct vtn_builder *b, struct vtn_ssa_value *src,
                struct vtn_pointer *dst)
{
   nir_intrinsic_op op;
   switch (dst->mode) {
   case vtn_variable_mode_ssbo:
      op = nir_intrinsic_store_ssbo;
      break;
   case vtn_variable_mode_workgroup:
      op = nir_intrinsic_store_shared;
      break;
   default:
      vtn_fail("Invalid block variable mode");
   }

   nir_ssa_def *offset, *index = NULL;
   offset = vtn_pointer_to_offset(b, dst, &index);

   _vtn_block_load_store(b, op, false, index, offset,
                         0, 0, dst->type, dst->access, &src);
}

static void
_vtn_variable_load_store(struct vtn_builder *b, bool load,
                         struct vtn_pointer *ptr,
                         struct vtn_ssa_value **inout)
{
   enum glsl_base_type base_type = glsl_get_base_type(ptr->type->type);
   switch (base_type) {
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
   case GLSL_TYPE_UINT16:
   case GLSL_TYPE_INT16:
   case GLSL_TYPE_UINT8:
   case GLSL_TYPE_INT8:
   case GLSL_TYPE_UINT64:
   case GLSL_TYPE_INT64:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_FLOAT16:
   case GLSL_TYPE_BOOL:
   case GLSL_TYPE_DOUBLE:
      /* At this point, we have a scalar, vector, or matrix so we know that
       * there cannot be any structure splitting still in the way.  By
       * stopping at the matrix level rather than the vector level, we
       * ensure that matrices get loaded in the optimal way even if they
       * are storred row-major in a UBO.
       */
      if (load) {
         *inout = vtn_local_load(b, vtn_pointer_to_deref(b, ptr));
      } else {
         vtn_local_store(b, *inout, vtn_pointer_to_deref(b, ptr));
      }
      return;

   case GLSL_TYPE_ARRAY:
   case GLSL_TYPE_STRUCT: {
      unsigned elems = glsl_get_length(ptr->type->type);
      if (load) {
         vtn_assert(*inout == NULL);
         *inout = rzalloc(b, struct vtn_ssa_value);
         (*inout)->type = ptr->type->type;
         (*inout)->elems = rzalloc_array(b, struct vtn_ssa_value *, elems);
      }

      struct vtn_access_chain chain = {
         .length = 1,
         .link = {
            { .mode = vtn_access_mode_literal, },
         }
      };
      for (unsigned i = 0; i < elems; i++) {
         chain.link[0].id = i;
         struct vtn_pointer *elem = vtn_pointer_dereference(b, ptr, &chain);
         _vtn_variable_load_store(b, load, elem, &(*inout)->elems[i]);
      }
      return;
   }

   default:
      vtn_fail("Invalid access chain type");
   }
}

struct vtn_ssa_value *
vtn_variable_load(struct vtn_builder *b, struct vtn_pointer *src)
{
   if (vtn_pointer_is_external_block(b, src)) {
      return vtn_block_load(b, src);
   } else {
      struct vtn_ssa_value *val = NULL;
      _vtn_variable_load_store(b, true, src, &val);
      return val;
   }
}

void
vtn_variable_store(struct vtn_builder *b, struct vtn_ssa_value *src,
                   struct vtn_pointer *dest)
{
   if (vtn_pointer_is_external_block(b, dest)) {
      vtn_assert(dest->mode == vtn_variable_mode_ssbo ||
                 dest->mode == vtn_variable_mode_workgroup);
      vtn_block_store(b, src, dest);
   } else {
      _vtn_variable_load_store(b, false, dest, &src);
   }
}

static void
_vtn_variable_copy(struct vtn_builder *b, struct vtn_pointer *dest,
                   struct vtn_pointer *src)
{
   vtn_assert(src->type->type == dest->type->type);
   enum glsl_base_type base_type = glsl_get_base_type(src->type->type);
   switch (base_type) {
   case GLSL_TYPE_UINT:
   case GLSL_TYPE_INT:
   case GLSL_TYPE_UINT16:
   case GLSL_TYPE_INT16:
   case GLSL_TYPE_UINT8:
   case GLSL_TYPE_INT8:
   case GLSL_TYPE_UINT64:
   case GLSL_TYPE_INT64:
   case GLSL_TYPE_FLOAT:
   case GLSL_TYPE_FLOAT16:
   case GLSL_TYPE_DOUBLE:
   case GLSL_TYPE_BOOL:
      /* At this point, we have a scalar, vector, or matrix so we know that
       * there cannot be any structure splitting still in the way.  By
       * stopping at the matrix level rather than the vector level, we
       * ensure that matrices get loaded in the optimal way even if they
       * are storred row-major in a UBO.
       */
      vtn_variable_store(b, vtn_variable_load(b, src), dest);
      return;

   case GLSL_TYPE_ARRAY:
   case GLSL_TYPE_STRUCT: {
      struct vtn_access_chain chain = {
         .length = 1,
         .link = {
            { .mode = vtn_access_mode_literal, },
         }
      };
      unsigned elems = glsl_get_length(src->type->type);
      for (unsigned i = 0; i < elems; i++) {
         chain.link[0].id = i;
         struct vtn_pointer *src_elem =
            vtn_pointer_dereference(b, src, &chain);
         struct vtn_pointer *dest_elem =
            vtn_pointer_dereference(b, dest, &chain);

         _vtn_variable_copy(b, dest_elem, src_elem);
      }
      return;
   }

   default:
      vtn_fail("Invalid access chain type");
   }
}

static void
vtn_variable_copy(struct vtn_builder *b, struct vtn_pointer *dest,
                  struct vtn_pointer *src)
{
   /* TODO: At some point, we should add a special-case for when we can
    * just emit a copy_var intrinsic.
    */
   _vtn_variable_copy(b, dest, src);
}

static void
set_mode_system_value(struct vtn_builder *b, nir_variable_mode *mode)
{
   vtn_assert(*mode == nir_var_system_value || *mode == nir_var_shader_in);
   *mode = nir_var_system_value;
}

static void
vtn_get_builtin_location(struct vtn_builder *b,
                         SpvBuiltIn builtin, int *location,
                         nir_variable_mode *mode)
{
   switch (builtin) {
   case SpvBuiltInPosition:
      *location = VARYING_SLOT_POS;
      break;
   case SpvBuiltInPointSize:
      *location = VARYING_SLOT_PSIZ;
      break;
   case SpvBuiltInClipDistance:
      *location = VARYING_SLOT_CLIP_DIST0; /* XXX CLIP_DIST1? */
      break;
   case SpvBuiltInCullDistance:
      *location = VARYING_SLOT_CULL_DIST0;
      break;
   case SpvBuiltInVertexId:
   case SpvBuiltInVertexIndex:
      /* The Vulkan spec defines VertexIndex to be non-zero-based and doesn't
       * allow VertexId.  The ARB_gl_spirv spec defines VertexId to be the
       * same as gl_VertexID, which is non-zero-based, and removes
       * VertexIndex.  Since they're both defined to be non-zero-based, we use
       * SYSTEM_VALUE_VERTEX_ID for both.
       */
      *location = SYSTEM_VALUE_VERTEX_ID;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInInstanceIndex:
      *location = SYSTEM_VALUE_INSTANCE_INDEX;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInInstanceId:
      *location = SYSTEM_VALUE_INSTANCE_ID;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInPrimitiveId:
      if (b->shader->info.stage == MESA_SHADER_FRAGMENT) {
         vtn_assert(*mode == nir_var_shader_in);
         *location = VARYING_SLOT_PRIMITIVE_ID;
      } else if (*mode == nir_var_shader_out) {
         *location = VARYING_SLOT_PRIMITIVE_ID;
      } else {
         *location = SYSTEM_VALUE_PRIMITIVE_ID;
         set_mode_system_value(b, mode);
      }
      break;
   case SpvBuiltInInvocationId:
      *location = SYSTEM_VALUE_INVOCATION_ID;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInLayer:
      *location = VARYING_SLOT_LAYER;
      if (b->shader->info.stage == MESA_SHADER_FRAGMENT)
         *mode = nir_var_shader_in;
      else if (b->shader->info.stage == MESA_SHADER_GEOMETRY)
         *mode = nir_var_shader_out;
      else if (b->options && b->options->caps.shader_viewport_index_layer &&
               (b->shader->info.stage == MESA_SHADER_VERTEX ||
                b->shader->info.stage == MESA_SHADER_TESS_EVAL))
         *mode = nir_var_shader_out;
      else
         vtn_fail("invalid stage for SpvBuiltInLayer");
      break;
   case SpvBuiltInViewportIndex:
      *location = VARYING_SLOT_VIEWPORT;
      if (b->shader->info.stage == MESA_SHADER_GEOMETRY)
         *mode = nir_var_shader_out;
      else if (b->options && b->options->caps.shader_viewport_index_layer &&
               (b->shader->info.stage == MESA_SHADER_VERTEX ||
                b->shader->info.stage == MESA_SHADER_TESS_EVAL))
         *mode = nir_var_shader_out;
      else if (b->shader->info.stage == MESA_SHADER_FRAGMENT)
         *mode = nir_var_shader_in;
      else
         vtn_fail("invalid stage for SpvBuiltInViewportIndex");
      break;
   case SpvBuiltInTessLevelOuter:
      *location = VARYING_SLOT_TESS_LEVEL_OUTER;
      break;
   case SpvBuiltInTessLevelInner:
      *location = VARYING_SLOT_TESS_LEVEL_INNER;
      break;
   case SpvBuiltInTessCoord:
      *location = SYSTEM_VALUE_TESS_COORD;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInPatchVertices:
      *location = SYSTEM_VALUE_VERTICES_IN;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInFragCoord:
      *location = VARYING_SLOT_POS;
      vtn_assert(*mode == nir_var_shader_in);
      break;
   case SpvBuiltInPointCoord:
      *location = VARYING_SLOT_PNTC;
      vtn_assert(*mode == nir_var_shader_in);
      break;
   case SpvBuiltInFrontFacing:
      *location = SYSTEM_VALUE_FRONT_FACE;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInSampleId:
      *location = SYSTEM_VALUE_SAMPLE_ID;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInSamplePosition:
      *location = SYSTEM_VALUE_SAMPLE_POS;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInSampleMask:
      if (*mode == nir_var_shader_out) {
         *location = FRAG_RESULT_SAMPLE_MASK;
      } else {
         *location = SYSTEM_VALUE_SAMPLE_MASK_IN;
         set_mode_system_value(b, mode);
      }
      break;
   case SpvBuiltInFragDepth:
      *location = FRAG_RESULT_DEPTH;
      vtn_assert(*mode == nir_var_shader_out);
      break;
   case SpvBuiltInHelperInvocation:
      *location = SYSTEM_VALUE_HELPER_INVOCATION;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInNumWorkgroups:
      *location = SYSTEM_VALUE_NUM_WORK_GROUPS;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInWorkgroupSize:
      *location = SYSTEM_VALUE_LOCAL_GROUP_SIZE;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInWorkgroupId:
      *location = SYSTEM_VALUE_WORK_GROUP_ID;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInLocalInvocationId:
      *location = SYSTEM_VALUE_LOCAL_INVOCATION_ID;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInLocalInvocationIndex:
      *location = SYSTEM_VALUE_LOCAL_INVOCATION_INDEX;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInGlobalInvocationId:
      *location = SYSTEM_VALUE_GLOBAL_INVOCATION_ID;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInBaseVertex:
      /* OpenGL gl_BaseVertex (SYSTEM_VALUE_BASE_VERTEX) is not the same
       * semantic as SPIR-V BaseVertex (SYSTEM_VALUE_FIRST_VERTEX).
       */
      *location = SYSTEM_VALUE_FIRST_VERTEX;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInBaseInstance:
      *location = SYSTEM_VALUE_BASE_INSTANCE;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInDrawIndex:
      *location = SYSTEM_VALUE_DRAW_ID;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInSubgroupSize:
      *location = SYSTEM_VALUE_SUBGROUP_SIZE;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInSubgroupId:
      *location = SYSTEM_VALUE_SUBGROUP_ID;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInSubgroupLocalInvocationId:
      *location = SYSTEM_VALUE_SUBGROUP_INVOCATION;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInNumSubgroups:
      *location = SYSTEM_VALUE_NUM_SUBGROUPS;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInDeviceIndex:
      *location = SYSTEM_VALUE_DEVICE_INDEX;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInViewIndex:
      *location = SYSTEM_VALUE_VIEW_INDEX;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInSubgroupEqMask:
      *location = SYSTEM_VALUE_SUBGROUP_EQ_MASK,
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInSubgroupGeMask:
      *location = SYSTEM_VALUE_SUBGROUP_GE_MASK,
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInSubgroupGtMask:
      *location = SYSTEM_VALUE_SUBGROUP_GT_MASK,
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInSubgroupLeMask:
      *location = SYSTEM_VALUE_SUBGROUP_LE_MASK,
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInSubgroupLtMask:
      *location = SYSTEM_VALUE_SUBGROUP_LT_MASK,
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInFragStencilRefEXT:
      *location = FRAG_RESULT_STENCIL;
      vtn_assert(*mode == nir_var_shader_out);
      break;
   case SpvBuiltInWorkDim:
      *location = SYSTEM_VALUE_WORK_DIM;
      set_mode_system_value(b, mode);
      break;
   case SpvBuiltInGlobalSize:
      *location = SYSTEM_VALUE_GLOBAL_GROUP_SIZE;
      set_mode_system_value(b, mode);
      break;
   default:
      vtn_fail("unsupported builtin: %u", builtin);
   }
}

static void
apply_var_decoration(struct vtn_builder *b,
                     struct nir_variable_data *var_data,
                     const struct vtn_decoration *dec)
{
   switch (dec->decoration) {
   case SpvDecorationRelaxedPrecision:
      break; /* FIXME: Do nothing with this for now. */
   case SpvDecorationNoPerspective:
      var_data->interpolation = INTERP_MODE_NOPERSPECTIVE;
      break;
   case SpvDecorationFlat:
      var_data->interpolation = INTERP_MODE_FLAT;
      break;
   case SpvDecorationCentroid:
      var_data->centroid = true;
      break;
   case SpvDecorationSample:
      var_data->sample = true;
      break;
   case SpvDecorationInvariant:
      var_data->invariant = true;
      break;
   case SpvDecorationConstant:
      var_data->read_only = true;
      break;
   case SpvDecorationNonReadable:
      var_data->image.access |= ACCESS_NON_READABLE;
      break;
   case SpvDecorationNonWritable:
      var_data->read_only = true;
      var_data->image.access |= ACCESS_NON_WRITEABLE;
      break;
   case SpvDecorationRestrict:
      var_data->image.access |= ACCESS_RESTRICT;
      break;
   case SpvDecorationVolatile:
      var_data->image.access |= ACCESS_VOLATILE;
      break;
   case SpvDecorationCoherent:
      var_data->image.access |= ACCESS_COHERENT;
      break;
   case SpvDecorationComponent:
      var_data->location_frac = dec->literals[0];
      break;
   case SpvDecorationIndex:
      var_data->index = dec->literals[0];
      break;
   case SpvDecorationBuiltIn: {
      SpvBuiltIn builtin = dec->literals[0];

      nir_variable_mode mode = var_data->mode;
      vtn_get_builtin_location(b, builtin, &var_data->location, &mode);
      var_data->mode = mode;

      switch (builtin) {
      case SpvBuiltInTessLevelOuter:
      case SpvBuiltInTessLevelInner:
         var_data->compact = true;
         break;
      case SpvBuiltInFragCoord:
         var_data->pixel_center_integer = b->pixel_center_integer;
         /* fallthrough */
      case SpvBuiltInSamplePosition:
         var_data->origin_upper_left = b->origin_upper_left;
         break;
      default:
         break;
      }
   }

   case SpvDecorationSpecId:
   case SpvDecorationRowMajor:
   case SpvDecorationColMajor:
   case SpvDecorationMatrixStride:
   case SpvDecorationAliased:
   case SpvDecorationUniform:
   case SpvDecorationLinkageAttributes:
      break; /* Do nothing with these here */

   case SpvDecorationPatch:
      var_data->patch = true;
      break;

   case SpvDecorationLocation:
      vtn_fail("Handled above");

   case SpvDecorationBlock:
   case SpvDecorationBufferBlock:
   case SpvDecorationArrayStride:
   case SpvDecorationGLSLShared:
   case SpvDecorationGLSLPacked:
      break; /* These can apply to a type but we don't care about them */

   case SpvDecorationBinding:
   case SpvDecorationDescriptorSet:
   case SpvDecorationNoContraction:
   case SpvDecorationInputAttachmentIndex:
      vtn_warn("Decoration not allowed for variable or structure member: %s",
               spirv_decoration_to_string(dec->decoration));
      break;

   case SpvDecorationXfbBuffer:
      var_data->explicit_xfb_buffer = true;
      var_data->xfb_buffer = dec->literals[0];
      var_data->always_active_io = true;
      break;
   case SpvDecorationXfbStride:
      var_data->explicit_xfb_stride = true;
      var_data->xfb_stride = dec->literals[0];
      break;
   case SpvDecorationOffset:
      var_data->explicit_offset = true;
      var_data->offset = dec->literals[0];
      break;

   case SpvDecorationStream:
      var_data->stream = dec->literals[0];
      break;

   case SpvDecorationCPacked:
   case SpvDecorationSaturatedConversion:
   case SpvDecorationFuncParamAttr:
   case SpvDecorationFPRoundingMode:
   case SpvDecorationFPFastMathMode:
   case SpvDecorationAlignment:
      vtn_warn("Decoration only allowed for CL-style kernels: %s",
               spirv_decoration_to_string(dec->decoration));
      break;

   case SpvDecorationHlslSemanticGOOGLE:
      /* HLSL semantic decorations can safely be ignored by the driver. */
      break;

   default:
      vtn_fail("Unhandled decoration");
   }
}

static void
var_is_patch_cb(struct vtn_builder *b, struct vtn_value *val, int member,
                const struct vtn_decoration *dec, void *out_is_patch)
{
   if (dec->decoration == SpvDecorationPatch) {
      *((bool *) out_is_patch) = true;
   }
}

static void
var_decoration_cb(struct vtn_builder *b, struct vtn_value *val, int member,
                  const struct vtn_decoration *dec, void *void_var)
{
   struct vtn_variable *vtn_var = void_var;

   /* Handle decorations that apply to a vtn_variable as a whole */
   switch (dec->decoration) {
   case SpvDecorationBinding:
      vtn_var->binding = dec->literals[0];
      vtn_var->explicit_binding = true;
      return;
   case SpvDecorationDescriptorSet:
      vtn_var->descriptor_set = dec->literals[0];
      return;
   case SpvDecorationInputAttachmentIndex:
      vtn_var->input_attachment_index = dec->literals[0];
      return;
   case SpvDecorationPatch:
      vtn_var->patch = true;
      break;
   case SpvDecorationOffset:
      vtn_var->offset = dec->literals[0];
      break;
   case SpvDecorationNonWritable:
      vtn_var->access |= ACCESS_NON_WRITEABLE;
      break;
   case SpvDecorationNonReadable:
      vtn_var->access |= ACCESS_NON_READABLE;
      break;
   case SpvDecorationVolatile:
      vtn_var->access |= ACCESS_VOLATILE;
      break;
   case SpvDecorationCoherent:
      vtn_var->access |= ACCESS_COHERENT;
      break;
   case SpvDecorationHlslCounterBufferGOOGLE:
      /* HLSL semantic decorations can safely be ignored by the driver. */
      break;
   default:
      break;
   }

   if (val->value_type == vtn_value_type_pointer) {
      assert(val->pointer->var == void_var);
      assert(val->pointer->chain == NULL);
      assert(member == -1);
   } else {
      assert(val->value_type == vtn_value_type_type);
   }

   /* Location is odd.  If applied to a split structure, we have to walk the
    * whole thing and accumulate the location.  It's easier to handle as a
    * special case.
    */
   if (dec->decoration == SpvDecorationLocation) {
      unsigned location = dec->literals[0];
      bool is_vertex_input = false;
      if (b->shader->info.stage == MESA_SHADER_FRAGMENT &&
          vtn_var->mode == vtn_variable_mode_output) {
         location += FRAG_RESULT_DATA0;
      } else if (b->shader->info.stage == MESA_SHADER_VERTEX &&
                 vtn_var->mode == vtn_variable_mode_input) {
         is_vertex_input = true;
         location += VERT_ATTRIB_GENERIC0;
      } else if (vtn_var->mode == vtn_variable_mode_input ||
                 vtn_var->mode == vtn_variable_mode_output) {
         location += vtn_var->patch ? VARYING_SLOT_PATCH0 : VARYING_SLOT_VAR0;
      } else if (vtn_var->mode != vtn_variable_mode_uniform) {
         vtn_warn("Location must be on input, output, uniform, sampler or "
                  "image variable");
         return;
      }

      if (vtn_var->var->num_members == 0) {
         /* This handles the member and lone variable cases */
         vtn_var->var->data.location = location;
      } else {
         /* This handles the structure member case */
         assert(vtn_var->var->members);
         for (unsigned i = 0; i < vtn_var->var->num_members; i++) {
            vtn_var->var->members[i].location = location;
            const struct glsl_type *member_type =
               glsl_get_struct_field(vtn_var->var->interface_type, i);
            location += glsl_count_attribute_slots(member_type,
                                                   is_vertex_input);
         }
      }
      return;
   } else {
      if (vtn_var->var) {
         if (vtn_var->var->num_members == 0) {
            assert(member == -1);
            apply_var_decoration(b, &vtn_var->var->data, dec);
         } else if (member >= 0) {
            /* Member decorations must come from a type */
            assert(val->value_type == vtn_value_type_type);
            apply_var_decoration(b, &vtn_var->var->members[member], dec);
         } else {
            unsigned length =
               glsl_get_length(glsl_without_array(vtn_var->type->type));
            for (unsigned i = 0; i < length; i++)
               apply_var_decoration(b, &vtn_var->var->members[i], dec);
         }
      } else {
         /* A few variables, those with external storage, have no actual
          * nir_variables associated with them.  Fortunately, all decorations
          * we care about for those variables are on the type only.
          */
         vtn_assert(vtn_var->mode == vtn_variable_mode_ubo ||
                    vtn_var->mode == vtn_variable_mode_ssbo ||
                    vtn_var->mode == vtn_variable_mode_push_constant ||
                    (vtn_var->mode == vtn_variable_mode_workgroup &&
                     b->options->lower_workgroup_access_to_offsets));
      }
   }
}

static enum vtn_variable_mode
vtn_storage_class_to_mode(struct vtn_builder *b,
                          SpvStorageClass class,
                          struct vtn_type *interface_type,
                          nir_variable_mode *nir_mode_out)
{
   enum vtn_variable_mode mode;
   nir_variable_mode nir_mode;
   switch (class) {
   case SpvStorageClassUniform:
      if (interface_type->block) {
         mode = vtn_variable_mode_ubo;
         nir_mode = 0;
      } else if (interface_type->buffer_block) {
         mode = vtn_variable_mode_ssbo;
         nir_mode = 0;
      } else {
         /* Default-block uniforms, coming from gl_spirv */
         mode = vtn_variable_mode_uniform;
         nir_mode = nir_var_uniform;
      }
      break;
   case SpvStorageClassStorageBuffer:
      mode = vtn_variable_mode_ssbo;
      nir_mode = 0;
      break;
   case SpvStorageClassUniformConstant:
      mode = vtn_variable_mode_uniform;
      nir_mode = nir_var_uniform;
      break;
   case SpvStorageClassPushConstant:
      mode = vtn_variable_mode_push_constant;
      nir_mode = nir_var_uniform;
      break;
   case SpvStorageClassInput:
      mode = vtn_variable_mode_input;
      nir_mode = nir_var_shader_in;
      break;
   case SpvStorageClassOutput:
      mode = vtn_variable_mode_output;
      nir_mode = nir_var_shader_out;
      break;
   case SpvStorageClassPrivate:
      mode = vtn_variable_mode_global;
      nir_mode = nir_var_global;
      break;
   case SpvStorageClassFunction:
      mode = vtn_variable_mode_local;
      nir_mode = nir_var_local;
      break;
   case SpvStorageClassWorkgroup:
      mode = vtn_variable_mode_workgroup;
      nir_mode = nir_var_shared;
      break;
   case SpvStorageClassAtomicCounter:
      mode = vtn_variable_mode_uniform;
      nir_mode = nir_var_uniform;
      break;
   case SpvStorageClassCrossWorkgroup:
   case SpvStorageClassGeneric:
   default:
      vtn_fail("Unhandled variable storage class");
   }

   if (nir_mode_out)
      *nir_mode_out = nir_mode;

   return mode;
}

nir_ssa_def *
vtn_pointer_to_ssa(struct vtn_builder *b, struct vtn_pointer *ptr)
{
   if (vtn_pointer_uses_ssa_offset(b, ptr)) {
      /* This pointer needs to have a pointer type with actual storage */
      vtn_assert(ptr->ptr_type);
      vtn_assert(ptr->ptr_type->type);

      if (!ptr->offset) {
         /* If we don't have an offset then we must be a pointer to the variable
          * itself.
          */
         vtn_assert(!ptr->offset && !ptr->block_index);

         struct vtn_access_chain chain = {
            .length = 0,
         };
         ptr = vtn_ssa_offset_pointer_dereference(b, ptr, &chain);
      }

      vtn_assert(ptr->offset);
      if (ptr->block_index) {
         vtn_assert(ptr->mode == vtn_variable_mode_ubo ||
                    ptr->mode == vtn_variable_mode_ssbo);
         return nir_vec2(&b->nb, ptr->block_index, ptr->offset);
      } else {
         vtn_assert(ptr->mode == vtn_variable_mode_workgroup);
         return ptr->offset;
      }
   } else {
      return &vtn_pointer_to_deref(b, ptr)->dest.ssa;
   }
}

struct vtn_pointer *
vtn_pointer_from_ssa(struct vtn_builder *b, nir_ssa_def *ssa,
                     struct vtn_type *ptr_type)
{
   vtn_assert(ssa->num_components <= 2 && ssa->bit_size == 32);
   vtn_assert(ptr_type->base_type == vtn_base_type_pointer);

   struct vtn_type *interface_type = ptr_type->deref;
   while (interface_type->base_type == vtn_base_type_array)
      interface_type = interface_type->array_element;

   struct vtn_pointer *ptr = rzalloc(b, struct vtn_pointer);
   nir_variable_mode nir_mode;
   ptr->mode = vtn_storage_class_to_mode(b, ptr_type->storage_class,
                                         interface_type, &nir_mode);
   ptr->type = ptr_type->deref;
   ptr->ptr_type = ptr_type;

   if (ptr->mode == vtn_variable_mode_ubo ||
       ptr->mode == vtn_variable_mode_ssbo) {
      /* This pointer type needs to have actual storage */
      vtn_assert(ptr_type->type);
      vtn_assert(ssa->num_components == 2);
      ptr->block_index = nir_channel(&b->nb, ssa, 0);
      ptr->offset = nir_channel(&b->nb, ssa, 1);
   } else if (ptr->mode == vtn_variable_mode_workgroup ||
              ptr->mode == vtn_variable_mode_push_constant) {
      /* This pointer type needs to have actual storage */
      vtn_assert(ptr_type->type);
      vtn_assert(ssa->num_components == 1);
      ptr->block_index = NULL;
      ptr->offset = ssa;
   } else {
      ptr->deref = nir_build_deref_cast(&b->nb, ssa, nir_mode,
                                        ptr_type->deref->type);
   }

   return ptr;
}

static bool
is_per_vertex_inout(const struct vtn_variable *var, gl_shader_stage stage)
{
   if (var->patch || !glsl_type_is_array(var->type->type))
      return false;

   if (var->mode == vtn_variable_mode_input) {
      return stage == MESA_SHADER_TESS_CTRL ||
             stage == MESA_SHADER_TESS_EVAL ||
             stage == MESA_SHADER_GEOMETRY;
   }

   if (var->mode == vtn_variable_mode_output)
      return stage == MESA_SHADER_TESS_CTRL;

   return false;
}

static void
vtn_create_variable(struct vtn_builder *b, struct vtn_value *val,
                    struct vtn_type *ptr_type, SpvStorageClass storage_class,
                    nir_constant *initializer)
{
   vtn_assert(ptr_type->base_type == vtn_base_type_pointer);
   struct vtn_type *type = ptr_type->deref;

   struct vtn_type *without_array = type;
   while(glsl_type_is_array(without_array->type))
      without_array = without_array->array_element;

   enum vtn_variable_mode mode;
   nir_variable_mode nir_mode;
   mode = vtn_storage_class_to_mode(b, storage_class, without_array, &nir_mode);

   switch (mode) {
   case vtn_variable_mode_ubo:
      b->shader->info.num_ubos++;
      break;
   case vtn_variable_mode_ssbo:
      b->shader->info.num_ssbos++;
      break;
   case vtn_variable_mode_uniform:
      if (glsl_type_is_image(without_array->type))
         b->shader->info.num_images++;
      else if (glsl_type_is_sampler(without_array->type))
         b->shader->info.num_textures++;
      break;
   case vtn_variable_mode_push_constant:
      b->shader->num_uniforms = vtn_type_block_size(b, type);
      break;
   default:
      /* No tallying is needed */
      break;
   }

   struct vtn_variable *var = rzalloc(b, struct vtn_variable);
   var->type = type;
   var->mode = mode;

   vtn_assert(val->value_type == vtn_value_type_pointer);
   val->pointer = vtn_pointer_for_variable(b, var, ptr_type);

   switch (var->mode) {
   case vtn_variable_mode_local:
   case vtn_variable_mode_global:
   case vtn_variable_mode_uniform:
      /* For these, we create the variable normally */
      var->var = rzalloc(b->shader, nir_variable);
      var->var->name = ralloc_strdup(var->var, val->name);

      /* Need to tweak the nir type here as at vtn_handle_type we don't have
       * the access to storage_class, that is the one that points us that is
       * an atomic uint.
       */
      if (storage_class == SpvStorageClassAtomicCounter) {
         var->var->type = repair_atomic_type(var->type->type);
      } else {
         var->var->type = var->type->type;
      }
      var->var->data.mode = nir_mode;
      var->var->data.location = -1;
      var->var->interface_type = NULL;
      break;

   case vtn_variable_mode_workgroup:
      if (b->options->lower_workgroup_access_to_offsets) {
         var->shared_location = -1;
      } else {
         /* Create the variable normally */
         var->var = rzalloc(b->shader, nir_variable);
         var->var->name = ralloc_strdup(var->var, val->name);
         var->var->type = var->type->type;
         var->var->data.mode = nir_var_shared;
      }
      break;

   case vtn_variable_mode_input:
   case vtn_variable_mode_output: {
      /* In order to know whether or not we're a per-vertex inout, we need
       * the patch qualifier.  This means walking the variable decorations
       * early before we actually create any variables.  Not a big deal.
       *
       * GLSLang really likes to place decorations in the most interior
       * thing it possibly can.  In particular, if you have a struct, it
       * will place the patch decorations on the struct members.  This
       * should be handled by the variable splitting below just fine.
       *
       * If you have an array-of-struct, things get even more weird as it
       * will place the patch decorations on the struct even though it's
       * inside an array and some of the members being patch and others not
       * makes no sense whatsoever.  Since the only sensible thing is for
       * it to be all or nothing, we'll call it patch if any of the members
       * are declared patch.
       */
      var->patch = false;
      vtn_foreach_decoration(b, val, var_is_patch_cb, &var->patch);
      if (glsl_type_is_array(var->type->type) &&
          glsl_type_is_struct(without_array->type)) {
         vtn_foreach_decoration(b, vtn_value(b, without_array->id,
                                             vtn_value_type_type),
                                var_is_patch_cb, &var->patch);
      }

      /* For inputs and outputs, we immediately split structures.  This
       * is for a couple of reasons.  For one, builtins may all come in
       * a struct and we really want those split out into separate
       * variables.  For another, interpolation qualifiers can be
       * applied to members of the top-level struct ane we need to be
       * able to preserve that information.
       */

      struct vtn_type *interface_type = var->type;
      if (is_per_vertex_inout(var, b->shader->info.stage)) {
         /* In Geometry shaders (and some tessellation), inputs come
          * in per-vertex arrays.  However, some builtins come in
          * non-per-vertex, hence the need for the is_array check.  In
          * any case, there are no non-builtin arrays allowed so this
          * check should be sufficient.
          */
         interface_type = var->type->array_element;
      }

      var->var = rzalloc(b->shader, nir_variable);
      var->var->name = ralloc_strdup(var->var, val->name);
      var->var->type = var->type->type;
      var->var->interface_type = interface_type->type;
      var->var->data.mode = nir_mode;
      var->var->data.patch = var->patch;

      if (glsl_type_is_struct(interface_type->type)) {
         /* It's a struct.  Set it up as per-member. */
         var->var->num_members = glsl_get_length(interface_type->type);
         var->var->members = rzalloc_array(var->var, struct nir_variable_data,
                                           var->var->num_members);

         for (unsigned i = 0; i < var->var->num_members; i++) {
            var->var->members[i].mode = nir_mode;
            var->var->members[i].patch = var->patch;
         }
      }

      /* For inputs and outputs, we need to grab locations and builtin
       * information from the interface type.
       */
      vtn_foreach_decoration(b, vtn_value(b, interface_type->id,
                                          vtn_value_type_type),
                             var_decoration_cb, var);
      break;
   }

   case vtn_variable_mode_ubo:
   case vtn_variable_mode_ssbo:
   case vtn_variable_mode_push_constant:
      /* These don't need actual variables. */
      break;
   }

   if (initializer) {
      var->var->constant_initializer =
         nir_constant_clone(initializer, var->var);
   }

   vtn_foreach_decoration(b, val, var_decoration_cb, var);

   if (var->mode == vtn_variable_mode_uniform) {
      /* XXX: We still need the binding information in the nir_variable
       * for these. We should fix that.
       */
      var->var->data.binding = var->binding;
      var->var->data.explicit_binding = var->explicit_binding;
      var->var->data.descriptor_set = var->descriptor_set;
      var->var->data.index = var->input_attachment_index;
      var->var->data.offset = var->offset;

      if (glsl_type_is_image(without_array->type))
         var->var->data.image.format = without_array->image_format;
   }

   if (var->mode == vtn_variable_mode_local) {
      vtn_assert(var->var != NULL && var->var->members == NULL);
      nir_function_impl_add_variable(b->nb.impl, var->var);
   } else if (var->var) {
      nir_shader_add_variable(b->shader, var->var);
   } else {
      vtn_assert(vtn_pointer_is_external_block(b, val->pointer));
   }
}

static void
vtn_assert_types_equal(struct vtn_builder *b, SpvOp opcode,
                       struct vtn_type *dst_type,
                       struct vtn_type *src_type)
{
   if (dst_type->id == src_type->id)
      return;

   if (vtn_types_compatible(b, dst_type, src_type)) {
      /* Early versions of GLSLang would re-emit types unnecessarily and you
       * would end up with OpLoad, OpStore, or OpCopyMemory opcodes which have
       * mismatched source and destination types.
       *
       * https://github.com/KhronosGroup/glslang/issues/304
       * https://github.com/KhronosGroup/glslang/issues/307
       * https://bugs.freedesktop.org/show_bug.cgi?id=104338
       * https://bugs.freedesktop.org/show_bug.cgi?id=104424
       */
      vtn_warn("Source and destination types of %s do not have the same "
               "ID (but are compatible): %u vs %u",
                spirv_op_to_string(opcode), dst_type->id, src_type->id);
      return;
   }

   vtn_fail("Source and destination types of %s do not match: %s vs. %s",
            spirv_op_to_string(opcode),
            glsl_get_type_name(dst_type->type),
            glsl_get_type_name(src_type->type));
}

void
vtn_handle_variables(struct vtn_builder *b, SpvOp opcode,
                     const uint32_t *w, unsigned count)
{
   switch (opcode) {
   case SpvOpUndef: {
      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_undef);
      val->type = vtn_value(b, w[1], vtn_value_type_type)->type;
      break;
   }

   case SpvOpVariable: {
      struct vtn_type *ptr_type = vtn_value(b, w[1], vtn_value_type_type)->type;

      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_pointer);

      SpvStorageClass storage_class = w[3];
      nir_constant *initializer = NULL;
      if (count > 4)
         initializer = vtn_value(b, w[4], vtn_value_type_constant)->constant;

      vtn_create_variable(b, val, ptr_type, storage_class, initializer);
      break;
   }

   case SpvOpAccessChain:
   case SpvOpPtrAccessChain:
   case SpvOpInBoundsAccessChain: {
      struct vtn_access_chain *chain = vtn_access_chain_create(b, count - 4);
      chain->ptr_as_array = (opcode == SpvOpPtrAccessChain);

      unsigned idx = 0;
      for (int i = 4; i < count; i++) {
         struct vtn_value *link_val = vtn_untyped_value(b, w[i]);
         if (link_val->value_type == vtn_value_type_constant) {
            chain->link[idx].mode = vtn_access_mode_literal;
            chain->link[idx].id = link_val->constant->values[0].u32[0];
         } else {
            chain->link[idx].mode = vtn_access_mode_id;
            chain->link[idx].id = w[i];

         }
         idx++;
      }

      struct vtn_type *ptr_type = vtn_value(b, w[1], vtn_value_type_type)->type;
      struct vtn_value *base_val = vtn_untyped_value(b, w[3]);
      if (base_val->value_type == vtn_value_type_sampled_image) {
         /* This is rather insane.  SPIR-V allows you to use OpSampledImage
          * to combine an array of images with a single sampler to get an
          * array of sampled images that all share the same sampler.
          * Fortunately, this means that we can more-or-less ignore the
          * sampler when crawling the access chain, but it does leave us
          * with this rather awkward little special-case.
          */
         struct vtn_value *val =
            vtn_push_value(b, w[2], vtn_value_type_sampled_image);
         val->sampled_image = ralloc(b, struct vtn_sampled_image);
         val->sampled_image->type = base_val->sampled_image->type;
         val->sampled_image->image =
            vtn_pointer_dereference(b, base_val->sampled_image->image, chain);
         val->sampled_image->sampler = base_val->sampled_image->sampler;
      } else {
         vtn_assert(base_val->value_type == vtn_value_type_pointer);
         struct vtn_value *val =
            vtn_push_value(b, w[2], vtn_value_type_pointer);
         val->pointer = vtn_pointer_dereference(b, base_val->pointer, chain);
         val->pointer->ptr_type = ptr_type;
      }
      break;
   }

   case SpvOpCopyMemory: {
      struct vtn_value *dest = vtn_value(b, w[1], vtn_value_type_pointer);
      struct vtn_value *src = vtn_value(b, w[2], vtn_value_type_pointer);

      vtn_assert_types_equal(b, opcode, dest->type->deref, src->type->deref);

      vtn_variable_copy(b, dest->pointer, src->pointer);
      break;
   }

   case SpvOpLoad: {
      struct vtn_type *res_type =
         vtn_value(b, w[1], vtn_value_type_type)->type;
      struct vtn_value *src_val = vtn_value(b, w[3], vtn_value_type_pointer);
      struct vtn_pointer *src = src_val->pointer;

      vtn_assert_types_equal(b, opcode, res_type, src_val->type->deref);

      if (glsl_type_is_image(res_type->type) ||
          glsl_type_is_sampler(res_type->type)) {
         vtn_push_value(b, w[2], vtn_value_type_pointer)->pointer = src;
         return;
      }

      vtn_push_ssa(b, w[2], res_type, vtn_variable_load(b, src));
      break;
   }

   case SpvOpStore: {
      struct vtn_value *dest_val = vtn_value(b, w[1], vtn_value_type_pointer);
      struct vtn_pointer *dest = dest_val->pointer;
      struct vtn_value *src_val = vtn_untyped_value(b, w[2]);

      /* OpStore requires us to actually have a storage type */
      vtn_fail_if(dest->type->type == NULL,
                  "Invalid destination type for OpStore");

      if (glsl_get_base_type(dest->type->type) == GLSL_TYPE_BOOL &&
          glsl_get_base_type(src_val->type->type) == GLSL_TYPE_UINT) {
         /* Early versions of GLSLang would use uint types for UBOs/SSBOs but
          * would then store them to a local variable as bool.  Work around
          * the issue by doing an implicit conversion.
          *
          * https://github.com/KhronosGroup/glslang/issues/170
          * https://bugs.freedesktop.org/show_bug.cgi?id=104424
          */
         vtn_warn("OpStore of value of type OpTypeInt to a pointer to type "
                  "OpTypeBool.  Doing an implicit conversion to work around "
                  "the problem.");
         struct vtn_ssa_value *bool_ssa =
            vtn_create_ssa_value(b, dest->type->type);
         bool_ssa->def = nir_i2b(&b->nb, vtn_ssa_value(b, w[2])->def);
         vtn_variable_store(b, bool_ssa, dest);
         break;
      }

      vtn_assert_types_equal(b, opcode, dest_val->type->deref, src_val->type);

      if (glsl_type_is_sampler(dest->type->type)) {
         vtn_warn("OpStore of a sampler detected.  Doing on-the-fly copy "
                  "propagation to workaround the problem.");
         vtn_assert(dest->var->copy_prop_sampler == NULL);
         dest->var->copy_prop_sampler =
            vtn_value(b, w[2], vtn_value_type_pointer)->pointer;
         break;
      }

      struct vtn_ssa_value *src = vtn_ssa_value(b, w[2]);
      vtn_variable_store(b, src, dest);
      break;
   }

   case SpvOpArrayLength: {
      struct vtn_pointer *ptr =
         vtn_value(b, w[3], vtn_value_type_pointer)->pointer;

      const uint32_t offset = ptr->var->type->offsets[w[4]];
      const uint32_t stride = ptr->var->type->members[w[4]]->stride;

      if (!ptr->block_index) {
         struct vtn_access_chain chain = {
            .length = 0,
         };
         ptr = vtn_ssa_offset_pointer_dereference(b, ptr, &chain);
         vtn_assert(ptr->block_index);
      }

      nir_intrinsic_instr *instr =
         nir_intrinsic_instr_create(b->nb.shader,
                                    nir_intrinsic_get_buffer_size);
      instr->src[0] = nir_src_for_ssa(ptr->block_index);
      nir_ssa_dest_init(&instr->instr, &instr->dest, 1, 32, NULL);
      nir_builder_instr_insert(&b->nb, &instr->instr);
      nir_ssa_def *buf_size = &instr->dest.ssa;

      /* array_length = max(buffer_size - offset, 0) / stride */
      nir_ssa_def *array_length =
         nir_idiv(&b->nb,
                  nir_imax(&b->nb,
                           nir_isub(&b->nb,
                                    buf_size,
                                    nir_imm_int(&b->nb, offset)),
                           nir_imm_int(&b->nb, 0u)),
                  nir_imm_int(&b->nb, stride));

      struct vtn_value *val = vtn_push_value(b, w[2], vtn_value_type_ssa);
      val->ssa = vtn_create_ssa_value(b, glsl_uint_type());
      val->ssa->def = array_length;
      break;
   }

   case SpvOpCopyMemorySized:
   default:
      vtn_fail("Unhandled opcode");
   }
}
