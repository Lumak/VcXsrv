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
 *    Connor Abbott (cwabbott0@gmail.com)
 *
 */

#include "nir.h"
#include "nir_worklist.h"

/* SSA-based mark-and-sweep dead code elimination */

static void
mark_and_push(nir_instr_worklist *wl, nir_instr *instr)
{
   nir_instr_worklist_push_tail(wl, instr);
   instr->pass_flags = 1;
}

static bool
mark_live_cb(nir_src *src, void *_state)
{
   nir_instr_worklist *worklist = (nir_instr_worklist *) _state;

   if (src->is_ssa && !src->ssa->parent_instr->pass_flags)
      mark_and_push(worklist, src->ssa->parent_instr);

   return true;
}

static void
init_instr(nir_instr *instr, nir_instr_worklist *worklist)
{
   nir_alu_instr *alu_instr;
   nir_deref_instr *deref_instr;
   nir_intrinsic_instr *intrin_instr;
   nir_tex_instr *tex_instr;

   /* We use the pass_flags to store the live/dead information.  In DCE, we
    * just treat it as a zero/non-zero boolean for whether or not the
    * instruction is live.
    */
   instr->pass_flags = 0;

   switch (instr->type) {
   case nir_instr_type_call:
   case nir_instr_type_jump:
      mark_and_push(worklist, instr);
      break;

   case nir_instr_type_alu:
      alu_instr = nir_instr_as_alu(instr);
      if (!alu_instr->dest.dest.is_ssa)
         mark_and_push(worklist, instr);
      break;

   case nir_instr_type_deref:
      deref_instr = nir_instr_as_deref(instr);
      if (!deref_instr->dest.is_ssa)
         mark_and_push(worklist, instr);
      break;

   case nir_instr_type_intrinsic:
      intrin_instr = nir_instr_as_intrinsic(instr);
      if (nir_intrinsic_infos[intrin_instr->intrinsic].flags &
          NIR_INTRINSIC_CAN_ELIMINATE) {
         if (nir_intrinsic_infos[intrin_instr->intrinsic].has_dest &&
             !intrin_instr->dest.is_ssa) {
            mark_and_push(worklist, instr);
         }
      } else {
         mark_and_push(worklist, instr);
      }
      break;

   case nir_instr_type_tex:
      tex_instr = nir_instr_as_tex(instr);
      if (!tex_instr->dest.is_ssa)
         mark_and_push(worklist, instr);
      break;

   default:
      break;
   }
}

static bool
init_block(nir_block *block, nir_instr_worklist *worklist)
{
   nir_foreach_instr(instr, block)
      init_instr(instr, worklist);

   nir_if *following_if = nir_block_get_following_if(block);
   if (following_if) {
      if (following_if->condition.is_ssa &&
          !following_if->condition.ssa->parent_instr->pass_flags)
         mark_and_push(worklist, following_if->condition.ssa->parent_instr);
   }

   return true;
}

static bool
nir_opt_dce_impl(nir_function_impl *impl)
{
   nir_instr_worklist *worklist = nir_instr_worklist_create();

   nir_foreach_block(block, impl) {
      init_block(block, worklist);
   }

   nir_foreach_instr_in_worklist(instr, worklist)
      nir_foreach_src(instr, mark_live_cb, worklist);

   nir_instr_worklist_destroy(worklist);

   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (!instr->pass_flags) {
            nir_instr_remove(instr);
            progress = true;
         }
      }
   }

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);

   return progress;
}

bool
nir_opt_dce(nir_shader *shader)
{
   bool progress = false;
   nir_foreach_function(function, shader) {
      if (function->impl && nir_opt_dce_impl(function->impl))
         progress = true;
   }

   return progress;
}
