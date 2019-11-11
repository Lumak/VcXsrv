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

#include <inttypes.h>
#include "nir_search.h"
#include "nir_builder.h"
#include "util/half_float.h"

struct match_state {
   bool inexact_match;
   bool has_exact_alu;
   unsigned variables_seen;
   nir_alu_src variables[NIR_SEARCH_MAX_VARIABLES];
};

static bool
match_expression(const nir_search_expression *expr, nir_alu_instr *instr,
                 unsigned num_components, const uint8_t *swizzle,
                 struct match_state *state);

static const uint8_t identity_swizzle[NIR_MAX_VEC_COMPONENTS] = { 0, 1, 2, 3 };

/**
 * Check if a source produces a value of the given type.
 *
 * Used for satisfying 'a@type' constraints.
 */
static bool
src_is_type(nir_src src, nir_alu_type type)
{
   assert(type != nir_type_invalid);

   if (!src.is_ssa)
      return false;

   if (src.ssa->parent_instr->type == nir_instr_type_alu) {
      nir_alu_instr *src_alu = nir_instr_as_alu(src.ssa->parent_instr);
      nir_alu_type output_type = nir_op_infos[src_alu->op].output_type;

      if (type == nir_type_bool) {
         switch (src_alu->op) {
         case nir_op_iand:
         case nir_op_ior:
         case nir_op_ixor:
            return src_is_type(src_alu->src[0].src, nir_type_bool) &&
                   src_is_type(src_alu->src[1].src, nir_type_bool);
         case nir_op_inot:
            return src_is_type(src_alu->src[0].src, nir_type_bool);
         default:
            break;
         }
      }

      return nir_alu_type_get_base_type(output_type) == type;
   } else if (src.ssa->parent_instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(src.ssa->parent_instr);

      if (type == nir_type_bool) {
         return intr->intrinsic == nir_intrinsic_load_front_face ||
                intr->intrinsic == nir_intrinsic_load_helper_invocation;
      }
   }

   /* don't know */
   return false;
}

static bool
match_value(const nir_search_value *value, nir_alu_instr *instr, unsigned src,
            unsigned num_components, const uint8_t *swizzle,
            struct match_state *state)
{
   uint8_t new_swizzle[NIR_MAX_VEC_COMPONENTS];

   /* Searching only works on SSA values because, if it's not SSA, we can't
    * know if the value changed between one instance of that value in the
    * expression and another.  Also, the replace operation will place reads of
    * that value right before the last instruction in the expression we're
    * replacing so those reads will happen after the original reads and may
    * not be valid if they're register reads.
    */
   if (!instr->src[src].src.is_ssa)
      return false;

   /* If the source is an explicitly sized source, then we need to reset
    * both the number of components and the swizzle.
    */
   if (nir_op_infos[instr->op].input_sizes[src] != 0) {
      num_components = nir_op_infos[instr->op].input_sizes[src];
      swizzle = identity_swizzle;
   }

   for (unsigned i = 0; i < num_components; ++i)
      new_swizzle[i] = instr->src[src].swizzle[swizzle[i]];

   /* If the value has a specific bit size and it doesn't match, bail */
   if (value->bit_size &&
       nir_src_bit_size(instr->src[src].src) != value->bit_size)
      return false;

   switch (value->type) {
   case nir_search_value_expression:
      if (instr->src[src].src.ssa->parent_instr->type != nir_instr_type_alu)
         return false;

      return match_expression(nir_search_value_as_expression(value),
                              nir_instr_as_alu(instr->src[src].src.ssa->parent_instr),
                              num_components, new_swizzle, state);

   case nir_search_value_variable: {
      nir_search_variable *var = nir_search_value_as_variable(value);
      assert(var->variable < NIR_SEARCH_MAX_VARIABLES);

      if (state->variables_seen & (1 << var->variable)) {
         if (state->variables[var->variable].src.ssa != instr->src[src].src.ssa)
            return false;

         assert(!instr->src[src].abs && !instr->src[src].negate);

         for (unsigned i = 0; i < num_components; ++i) {
            if (state->variables[var->variable].swizzle[i] != new_swizzle[i])
               return false;
         }

         return true;
      } else {
         if (var->is_constant &&
             instr->src[src].src.ssa->parent_instr->type != nir_instr_type_load_const)
            return false;

         if (var->cond && !var->cond(instr, src, num_components, new_swizzle))
            return false;

         if (var->type != nir_type_invalid &&
             !src_is_type(instr->src[src].src, var->type))
            return false;

         state->variables_seen |= (1 << var->variable);
         state->variables[var->variable].src = instr->src[src].src;
         state->variables[var->variable].abs = false;
         state->variables[var->variable].negate = false;

         for (unsigned i = 0; i < NIR_MAX_VEC_COMPONENTS; ++i) {
            if (i < num_components)
               state->variables[var->variable].swizzle[i] = new_swizzle[i];
            else
               state->variables[var->variable].swizzle[i] = 0;
         }

         return true;
      }
   }

   case nir_search_value_constant: {
      nir_search_constant *const_val = nir_search_value_as_constant(value);

      if (!nir_src_is_const(instr->src[src].src))
         return false;

      switch (const_val->type) {
      case nir_type_float:
         for (unsigned i = 0; i < num_components; ++i) {
            double val = nir_src_comp_as_float(instr->src[src].src,
                                               new_swizzle[i]);
            if (val != const_val->data.d)
               return false;
         }
         return true;

      case nir_type_int:
      case nir_type_uint:
      case nir_type_bool: {
         unsigned bit_size = nir_src_bit_size(instr->src[src].src);
         uint64_t mask = bit_size == 64 ? UINT64_MAX : (1ull << bit_size) - 1;
         for (unsigned i = 0; i < num_components; ++i) {
            uint64_t val = nir_src_comp_as_uint(instr->src[src].src,
                                                new_swizzle[i]);
            if ((val & mask) != (const_val->data.u & mask))
               return false;
         }
         return true;
      }

      default:
         unreachable("Invalid alu source type");
      }
   }

   default:
      unreachable("Invalid search value type");
   }
}

static bool
match_expression(const nir_search_expression *expr, nir_alu_instr *instr,
                 unsigned num_components, const uint8_t *swizzle,
                 struct match_state *state)
{
   if (expr->cond && !expr->cond(instr))
      return false;

   if (instr->op != expr->opcode)
      return false;

   assert(instr->dest.dest.is_ssa);

   if (expr->value.bit_size &&
       instr->dest.dest.ssa.bit_size != expr->value.bit_size)
      return false;

   state->inexact_match = expr->inexact || state->inexact_match;
   state->has_exact_alu = instr->exact || state->has_exact_alu;
   if (state->inexact_match && state->has_exact_alu)
      return false;

   assert(!instr->dest.saturate);
   assert(nir_op_infos[instr->op].num_inputs > 0);

   /* If we have an explicitly sized destination, we can only handle the
    * identity swizzle.  While dot(vec3(a, b, c).zxy) is a valid
    * expression, we don't have the information right now to propagate that
    * swizzle through.  We can only properly propagate swizzles if the
    * instruction is vectorized.
    */
   if (nir_op_infos[instr->op].output_size != 0) {
      for (unsigned i = 0; i < num_components; i++) {
         if (swizzle[i] != i)
            return false;
      }
   }

   /* Stash off the current variables_seen bitmask.  This way we can
    * restore it prior to matching in the commutative case below.
    */
   unsigned variables_seen_stash = state->variables_seen;

   bool matched = true;
   for (unsigned i = 0; i < nir_op_infos[instr->op].num_inputs; i++) {
      if (!match_value(expr->srcs[i], instr, i, num_components,
                       swizzle, state)) {
         matched = false;
         break;
      }
   }

   if (matched)
      return true;

   if (nir_op_infos[instr->op].algebraic_properties & NIR_OP_IS_COMMUTATIVE) {
      assert(nir_op_infos[instr->op].num_inputs == 2);

      /* Restore the variables_seen bitmask.  If we don't do this, then we
       * could end up with an erroneous failure due to variables found in the
       * first match attempt above not matching those in the second.
       */
      state->variables_seen = variables_seen_stash;

      if (!match_value(expr->srcs[0], instr, 1, num_components,
                       swizzle, state))
         return false;

      return match_value(expr->srcs[1], instr, 0, num_components,
                         swizzle, state);
   } else {
      return false;
   }
}

typedef struct bitsize_tree {
   unsigned num_srcs;
   struct bitsize_tree *srcs[4];

   unsigned common_size;
   bool is_src_sized[4];
   bool is_dest_sized;

   unsigned dest_size;
   unsigned src_size[4];
} bitsize_tree;

static bitsize_tree *
build_bitsize_tree(void *mem_ctx, struct match_state *state,
                   const nir_search_value *value)
{
   bitsize_tree *tree = rzalloc(mem_ctx, bitsize_tree);

   switch (value->type) {
   case nir_search_value_expression: {
      nir_search_expression *expr = nir_search_value_as_expression(value);
      nir_op_info info = nir_op_infos[expr->opcode];
      tree->num_srcs = info.num_inputs;
      tree->common_size = 0;
      for (unsigned i = 0; i < info.num_inputs; i++) {
         tree->is_src_sized[i] = !!nir_alu_type_get_type_size(info.input_types[i]);
         if (tree->is_src_sized[i])
            tree->src_size[i] = nir_alu_type_get_type_size(info.input_types[i]);
         tree->srcs[i] = build_bitsize_tree(mem_ctx, state, expr->srcs[i]);
      }
      tree->is_dest_sized = !!nir_alu_type_get_type_size(info.output_type);
      if (tree->is_dest_sized)
         tree->dest_size = nir_alu_type_get_type_size(info.output_type);
      break;
   }

   case nir_search_value_variable: {
      nir_search_variable *var = nir_search_value_as_variable(value);
      tree->num_srcs = 0;
      tree->is_dest_sized = true;
      tree->dest_size = nir_src_bit_size(state->variables[var->variable].src);
      break;
   }

   case nir_search_value_constant: {
      tree->num_srcs = 0;
      tree->is_dest_sized = false;
      tree->common_size = 0;
      break;
   }
   }

   if (value->bit_size) {
      assert(!tree->is_dest_sized || tree->dest_size == value->bit_size);
      tree->common_size = value->bit_size;
   }

   return tree;
}

static unsigned
bitsize_tree_filter_up(bitsize_tree *tree)
{
   for (unsigned i = 0; i < tree->num_srcs; i++) {
      unsigned src_size = bitsize_tree_filter_up(tree->srcs[i]);
      if (src_size == 0)
         continue;

      if (tree->is_src_sized[i]) {
         assert(src_size == tree->src_size[i]);
      } else if (tree->common_size != 0) {
         assert(src_size == tree->common_size);
         tree->src_size[i] = src_size;
      } else {
         tree->common_size = src_size;
         tree->src_size[i] = src_size;
      }
   }

   if (tree->num_srcs && tree->common_size) {
      if (tree->dest_size == 0)
         tree->dest_size = tree->common_size;
      else if (!tree->is_dest_sized)
         assert(tree->dest_size == tree->common_size);

      for (unsigned i = 0; i < tree->num_srcs; i++) {
         if (!tree->src_size[i])
            tree->src_size[i] = tree->common_size;
      }
   }

   return tree->dest_size;
}

static void
bitsize_tree_filter_down(bitsize_tree *tree, unsigned size)
{
   if (tree->dest_size)
      assert(tree->dest_size == size);
   else
      tree->dest_size = size;

   if (!tree->is_dest_sized) {
      if (tree->common_size)
         assert(tree->common_size == size);
      else
         tree->common_size = size;
   }

   for (unsigned i = 0; i < tree->num_srcs; i++) {
      if (!tree->src_size[i]) {
         assert(tree->common_size);
         tree->src_size[i] = tree->common_size;
      }
      bitsize_tree_filter_down(tree->srcs[i], tree->src_size[i]);
   }
}

static nir_alu_src
construct_value(nir_builder *build,
                const nir_search_value *value,
                unsigned num_components, bitsize_tree *bitsize,
                struct match_state *state,
                nir_instr *instr)
{
   switch (value->type) {
   case nir_search_value_expression: {
      const nir_search_expression *expr = nir_search_value_as_expression(value);

      if (nir_op_infos[expr->opcode].output_size != 0)
         num_components = nir_op_infos[expr->opcode].output_size;

      nir_alu_instr *alu = nir_alu_instr_create(build->shader, expr->opcode);
      nir_ssa_dest_init(&alu->instr, &alu->dest.dest, num_components,
                        bitsize->dest_size, NULL);
      alu->dest.write_mask = (1 << num_components) - 1;
      alu->dest.saturate = false;

      /* We have no way of knowing what values in a given search expression
       * map to a particular replacement value.  Therefore, if the
       * expression we are replacing has any exact values, the entire
       * replacement should be exact.
       */
      alu->exact = state->has_exact_alu;

      for (unsigned i = 0; i < nir_op_infos[expr->opcode].num_inputs; i++) {
         /* If the source is an explicitly sized source, then we need to reset
          * the number of components to match.
          */
         if (nir_op_infos[alu->op].input_sizes[i] != 0)
            num_components = nir_op_infos[alu->op].input_sizes[i];

         alu->src[i] = construct_value(build, expr->srcs[i],
                                       num_components, bitsize->srcs[i],
                                       state, instr);
      }

      nir_builder_instr_insert(build, &alu->instr);

      nir_alu_src val;
      val.src = nir_src_for_ssa(&alu->dest.dest.ssa);
      val.negate = false;
      val.abs = false,
      memcpy(val.swizzle, identity_swizzle, sizeof val.swizzle);

      return val;
   }

   case nir_search_value_variable: {
      const nir_search_variable *var = nir_search_value_as_variable(value);
      assert(state->variables_seen & (1 << var->variable));

      nir_alu_src val = { NIR_SRC_INIT };
      nir_alu_src_copy(&val, &state->variables[var->variable],
                       (void *)build->shader);
      assert(!var->is_constant);

      return val;
   }

   case nir_search_value_constant: {
      const nir_search_constant *c = nir_search_value_as_constant(value);

      nir_ssa_def *cval;
      switch (c->type) {
      case nir_type_float:
         cval = nir_imm_floatN_t(build, c->data.d, bitsize->dest_size);
         break;

      case nir_type_int:
      case nir_type_uint:
         cval = nir_imm_intN_t(build, c->data.i, bitsize->dest_size);
         break;

      case nir_type_bool:
         cval = nir_imm_bool(build, c->data.u);
         break;
      default:
         unreachable("Invalid alu source type");
      }

      nir_alu_src val;
      val.src = nir_src_for_ssa(cval);
      val.negate = false;
      val.abs = false,
      memset(val.swizzle, 0, sizeof val.swizzle);

      return val;
   }

   default:
      unreachable("Invalid search value type");
   }
}

nir_ssa_def *
nir_replace_instr(nir_builder *build, nir_alu_instr *instr,
                  const nir_search_expression *search,
                  const nir_search_value *replace)
{
   uint8_t swizzle[NIR_MAX_VEC_COMPONENTS] = { 0 };

   for (unsigned i = 0; i < instr->dest.dest.ssa.num_components; ++i)
      swizzle[i] = i;

   assert(instr->dest.dest.is_ssa);

   struct match_state state;
   state.inexact_match = false;
   state.has_exact_alu = false;
   state.variables_seen = 0;

   if (!match_expression(search, instr, instr->dest.dest.ssa.num_components,
                         swizzle, &state))
      return NULL;

   void *bitsize_ctx = ralloc_context(NULL);
   bitsize_tree *tree = build_bitsize_tree(bitsize_ctx, &state, replace);
   bitsize_tree_filter_up(tree);
   bitsize_tree_filter_down(tree, instr->dest.dest.ssa.bit_size);

   build->cursor = nir_before_instr(&instr->instr);

   nir_alu_src val = construct_value(build, replace,
                                     instr->dest.dest.ssa.num_components,
                                     tree, &state, &instr->instr);

   /* Inserting a mov may be unnecessary.  However, it's much easier to
    * simply let copy propagation clean this up than to try to go through
    * and rewrite swizzles ourselves.
    */
   nir_ssa_def *ssa_val =
      nir_imov_alu(build, val, instr->dest.dest.ssa.num_components);
   nir_ssa_def_rewrite_uses(&instr->dest.dest.ssa, nir_src_for_ssa(ssa_val));

   /* We know this one has no more uses because we just rewrote them all,
    * so we can remove it.  The rest of the matched expression, however, we
    * don't know so much about.  We'll just let dead code clean them up.
    */
   nir_instr_remove(&instr->instr);

   ralloc_free(bitsize_ctx);

   return ssa_val;
}
