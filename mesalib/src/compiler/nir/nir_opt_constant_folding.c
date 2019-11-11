/*
 * Copyright © 2014 Intel Corporation
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

#include "nir_constant_expressions.h"
#include <math.h>

/*
 * Implements SSA-based constant folding.
 */

struct constant_fold_state {
   void *mem_ctx;
   nir_function_impl *impl;
   bool progress;
};

static bool
constant_fold_alu_instr(nir_alu_instr *instr, void *mem_ctx)
{
   nir_const_value src[NIR_MAX_VEC_COMPONENTS];

   if (!instr->dest.dest.is_ssa)
      return false;

   /* In the case that any outputs/inputs have unsized types, then we need to
    * guess the bit-size. In this case, the validator ensures that all
    * bit-sizes match so we can just take the bit-size from first
    * output/input with an unsized type. If all the outputs/inputs are sized
    * then we don't need to guess the bit-size at all because the code we
    * generate for constant opcodes in this case already knows the sizes of
    * the types involved and does not need the provided bit-size for anything
    * (although it still requires to receive a valid bit-size).
    */
   unsigned bit_size = 0;
   if (!nir_alu_type_get_type_size(nir_op_infos[instr->op].output_type))
      bit_size = instr->dest.dest.ssa.bit_size;

   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
      if (!instr->src[i].src.is_ssa)
         return false;

      if (bit_size == 0 && nir_op_infos[instr->op].input_sizes[i] == 0)
         bit_size = instr->src[i].src.ssa->bit_size;

      nir_instr *src_instr = instr->src[i].src.ssa->parent_instr;

      if (src_instr->type != nir_instr_type_load_const)
         return false;
      nir_load_const_instr* load_const = nir_instr_as_load_const(src_instr);

      for (unsigned j = 0; j < nir_ssa_alu_instr_src_components(instr, i);
           j++) {
         switch(load_const->def.bit_size) {
         case 64:
            src[i].u64[j] = load_const->value.u64[instr->src[i].swizzle[j]];
            break;
         case 32:
            src[i].u32[j] = load_const->value.u32[instr->src[i].swizzle[j]];
            break;
         case 16:
            src[i].u16[j] = load_const->value.u16[instr->src[i].swizzle[j]];
            break;
         case 8:
            src[i].u8[j] = load_const->value.u8[instr->src[i].swizzle[j]];
            break;
         default:
            unreachable("Invalid bit size");
         }
      }

      /* We shouldn't have any source modifiers in the optimization loop. */
      assert(!instr->src[i].abs && !instr->src[i].negate);
   }

   if (bit_size == 0)
      bit_size = 32;

   /* We shouldn't have any saturate modifiers in the optimization loop. */
   assert(!instr->dest.saturate);

   nir_const_value dest =
      nir_eval_const_opcode(instr->op, instr->dest.dest.ssa.num_components,
                            bit_size, src);

   nir_load_const_instr *new_instr =
      nir_load_const_instr_create(mem_ctx,
                                  instr->dest.dest.ssa.num_components,
                                  instr->dest.dest.ssa.bit_size);

   new_instr->value = dest;

   nir_instr_insert_before(&instr->instr, &new_instr->instr);

   nir_ssa_def_rewrite_uses(&instr->dest.dest.ssa,
                            nir_src_for_ssa(&new_instr->def));

   nir_instr_remove(&instr->instr);
   ralloc_free(instr);

   return true;
}

static bool
constant_fold_intrinsic_instr(nir_intrinsic_instr *instr)
{
   bool progress = false;

   if (instr->intrinsic == nir_intrinsic_discard_if &&
       nir_src_is_const(instr->src[0])) {
      if (nir_src_as_bool(instr->src[0])) {
         /* This method of getting a nir_shader * from a nir_instr is
          * admittedly gross, but given the rarity of hitting this case I think
          * it's preferable to plumbing an otherwise unused nir_shader *
          * parameter through four functions to get here.
          */
         nir_cf_node *cf_node = &instr->instr.block->cf_node;
         nir_function_impl *impl = nir_cf_node_get_function(cf_node);
         nir_shader *shader = impl->function->shader;

         nir_intrinsic_instr *discard =
            nir_intrinsic_instr_create(shader, nir_intrinsic_discard);
         nir_instr_insert_before(&instr->instr, &discard->instr);
         nir_instr_remove(&instr->instr);
         progress = true;
      } else {
         /* We're not discarding, just delete the instruction */
         nir_instr_remove(&instr->instr);
         progress = true;
      }
   }

   return progress;
}

static bool
constant_fold_block(nir_block *block, void *mem_ctx)
{
   bool progress = false;

   nir_foreach_instr_safe(instr, block) {
      switch (instr->type) {
      case nir_instr_type_alu:
         progress |= constant_fold_alu_instr(nir_instr_as_alu(instr), mem_ctx);
         break;
      case nir_instr_type_intrinsic:
         progress |=
            constant_fold_intrinsic_instr(nir_instr_as_intrinsic(instr));
         break;
      default:
         /* Don't know how to constant fold */
         break;
      }
   }

   return progress;
}

static bool
nir_opt_constant_folding_impl(nir_function_impl *impl)
{
   void *mem_ctx = ralloc_parent(impl);
   bool progress = false;

   nir_foreach_block(block, impl) {
      progress |= constant_fold_block(block, mem_ctx);
   }

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);

   return progress;
}

bool
nir_opt_constant_folding(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function(function, shader) {
      if (function->impl)
         progress |= nir_opt_constant_folding_impl(function->impl);
   }

   return progress;
}
