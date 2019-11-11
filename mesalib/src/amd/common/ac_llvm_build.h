/*
 * Copyright 2016 Bas Nieuwenhuizen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */
#ifndef AC_LLVM_BUILD_H
#define AC_LLVM_BUILD_H

#include <stdbool.h>
#include <llvm-c/TargetMachine.h>
#include "compiler/nir/nir.h"
#include "amd_family.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAVE_32BIT_POINTERS (HAVE_LLVM >= 0x0700)

enum {
	AC_ADDR_SPACE_FLAT = HAVE_LLVM >= 0x0700 ? 0 : 4, /* Slower than global. */
	AC_ADDR_SPACE_GLOBAL = 1,
	AC_ADDR_SPACE_GDS = HAVE_LLVM >= 0x0700 ? 2 : 5,
	AC_ADDR_SPACE_LDS = 3,
	AC_ADDR_SPACE_CONST = HAVE_LLVM >= 0x0700 ? 4 : 2, /* Global allowing SMEM. */
	AC_ADDR_SPACE_CONST_32BIT = 6, /* same as CONST, but the pointer type has 32 bits */
};

/* Combine these with & instead of |. */
#define NOOP_WAITCNT	0xcf7f
#define LGKM_CNT	0xc07f
#define EXP_CNT		0xcf0f
#define VM_CNT		0x0f70 /* On GFX9, vmcnt has 6 bits in [0:3] and [14:15] */

struct ac_llvm_flow;

struct ac_llvm_context {
	LLVMContextRef context;
	LLVMModuleRef module;
	LLVMBuilderRef builder;

	LLVMTypeRef voidt;
	LLVMTypeRef i1;
	LLVMTypeRef i8;
	LLVMTypeRef i16;
	LLVMTypeRef i32;
	LLVMTypeRef i64;
	LLVMTypeRef intptr;
	LLVMTypeRef f16;
	LLVMTypeRef f32;
	LLVMTypeRef f64;
	LLVMTypeRef v2i16;
	LLVMTypeRef v2i32;
	LLVMTypeRef v3i32;
	LLVMTypeRef v4i32;
	LLVMTypeRef v2f32;
	LLVMTypeRef v4f32;
	LLVMTypeRef v8i32;

	LLVMValueRef i16_0;
	LLVMValueRef i16_1;
	LLVMValueRef i32_0;
	LLVMValueRef i32_1;
	LLVMValueRef i64_0;
	LLVMValueRef i64_1;
	LLVMValueRef f32_0;
	LLVMValueRef f32_1;
	LLVMValueRef f64_0;
	LLVMValueRef f64_1;
	LLVMValueRef i1true;
	LLVMValueRef i1false;

	struct ac_llvm_flow *flow;
	unsigned flow_depth;
	unsigned flow_depth_max;

	unsigned range_md_kind;
	unsigned invariant_load_md_kind;
	unsigned uniform_md_kind;
	unsigned fpmath_md_kind;
	LLVMValueRef fpmath_md_2p5_ulp;
	LLVMValueRef empty_md;

	enum chip_class chip_class;
	enum radeon_family family;

	LLVMValueRef lds;
};

void
ac_llvm_context_init(struct ac_llvm_context *ctx,
		     enum chip_class chip_class, enum radeon_family family);

void
ac_llvm_context_dispose(struct ac_llvm_context *ctx);

int
ac_get_llvm_num_components(LLVMValueRef value);

int
ac_get_elem_bits(struct ac_llvm_context *ctx, LLVMTypeRef type);

LLVMValueRef
ac_llvm_extract_elem(struct ac_llvm_context *ac,
		     LLVMValueRef value,
		     int index);

unsigned ac_get_type_size(LLVMTypeRef type);

LLVMTypeRef ac_to_integer_type(struct ac_llvm_context *ctx, LLVMTypeRef t);
LLVMValueRef ac_to_integer(struct ac_llvm_context *ctx, LLVMValueRef v);
LLVMTypeRef ac_to_float_type(struct ac_llvm_context *ctx, LLVMTypeRef t);
LLVMValueRef ac_to_float(struct ac_llvm_context *ctx, LLVMValueRef v);

LLVMValueRef
ac_build_intrinsic(struct ac_llvm_context *ctx, const char *name,
		   LLVMTypeRef return_type, LLVMValueRef *params,
		   unsigned param_count, unsigned attrib_mask);

void ac_build_type_name_for_intr(LLVMTypeRef type, char *buf, unsigned bufsize);

LLVMValueRef
ac_build_phi(struct ac_llvm_context *ctx, LLVMTypeRef type,
	     unsigned count_incoming, LLVMValueRef *values,
	     LLVMBasicBlockRef *blocks);

void ac_build_s_barrier(struct ac_llvm_context *ctx);
void ac_build_optimization_barrier(struct ac_llvm_context *ctx,
				   LLVMValueRef *pvgpr);

LLVMValueRef ac_build_shader_clock(struct ac_llvm_context *ctx);

LLVMValueRef ac_build_ballot(struct ac_llvm_context *ctx, LLVMValueRef value);

LLVMValueRef ac_build_vote_all(struct ac_llvm_context *ctx, LLVMValueRef value);

LLVMValueRef ac_build_vote_any(struct ac_llvm_context *ctx, LLVMValueRef value);

LLVMValueRef ac_build_vote_eq(struct ac_llvm_context *ctx, LLVMValueRef value);

LLVMValueRef
ac_build_varying_gather_values(struct ac_llvm_context *ctx, LLVMValueRef *values,
			       unsigned value_count, unsigned component);

LLVMValueRef
ac_build_gather_values_extended(struct ac_llvm_context *ctx,
				LLVMValueRef *values,
				unsigned value_count,
				unsigned value_stride,
				bool load,
				bool always_vector);
LLVMValueRef
ac_build_gather_values(struct ac_llvm_context *ctx,
		       LLVMValueRef *values,
		       unsigned value_count);
LLVMValueRef ac_build_expand(struct ac_llvm_context *ctx,
			     LLVMValueRef value,
			     unsigned src_channels, unsigned dst_channels);
LLVMValueRef ac_build_expand_to_vec4(struct ac_llvm_context *ctx,
				     LLVMValueRef value,
				     unsigned num_channels);
LLVMValueRef ac_build_round(struct ac_llvm_context *ctx, LLVMValueRef value);

LLVMValueRef
ac_build_fdiv(struct ac_llvm_context *ctx,
	      LLVMValueRef num,
	      LLVMValueRef den);

LLVMValueRef ac_build_fast_udiv(struct ac_llvm_context *ctx,
				LLVMValueRef num,
				LLVMValueRef multiplier,
				LLVMValueRef pre_shift,
				LLVMValueRef post_shift,
				LLVMValueRef increment);
LLVMValueRef ac_build_fast_udiv_nuw(struct ac_llvm_context *ctx,
				    LLVMValueRef num,
				    LLVMValueRef multiplier,
				    LLVMValueRef pre_shift,
				    LLVMValueRef post_shift,
				    LLVMValueRef increment);
LLVMValueRef ac_build_fast_udiv_u31_d_not_one(struct ac_llvm_context *ctx,
					      LLVMValueRef num,
					      LLVMValueRef multiplier,
					      LLVMValueRef post_shift);

void
ac_prepare_cube_coords(struct ac_llvm_context *ctx,
		       bool is_deriv, bool is_array, bool is_lod,
		       LLVMValueRef *coords_arg,
		       LLVMValueRef *derivs_arg);


LLVMValueRef
ac_build_fs_interp(struct ac_llvm_context *ctx,
		   LLVMValueRef llvm_chan,
		   LLVMValueRef attr_number,
		   LLVMValueRef params,
		   LLVMValueRef i,
		   LLVMValueRef j);

LLVMValueRef
ac_build_fs_interp_mov(struct ac_llvm_context *ctx,
		       LLVMValueRef parameter,
		       LLVMValueRef llvm_chan,
		       LLVMValueRef attr_number,
		       LLVMValueRef params);

LLVMValueRef
ac_build_gep0(struct ac_llvm_context *ctx,
	      LLVMValueRef base_ptr,
	      LLVMValueRef index);
LLVMValueRef ac_build_pointer_add(struct ac_llvm_context *ctx, LLVMValueRef ptr,
				  LLVMValueRef index);

void
ac_build_indexed_store(struct ac_llvm_context *ctx,
		       LLVMValueRef base_ptr, LLVMValueRef index,
		       LLVMValueRef value);

LLVMValueRef ac_build_load(struct ac_llvm_context *ctx, LLVMValueRef base_ptr,
			   LLVMValueRef index);
LLVMValueRef ac_build_load_invariant(struct ac_llvm_context *ctx,
				     LLVMValueRef base_ptr, LLVMValueRef index);
LLVMValueRef ac_build_load_to_sgpr(struct ac_llvm_context *ctx,
				   LLVMValueRef base_ptr, LLVMValueRef index);
LLVMValueRef ac_build_load_to_sgpr_uint_wraparound(struct ac_llvm_context *ctx,
				   LLVMValueRef base_ptr, LLVMValueRef index);

void
ac_build_buffer_store_dword(struct ac_llvm_context *ctx,
			    LLVMValueRef rsrc,
			    LLVMValueRef vdata,
			    unsigned num_channels,
			    LLVMValueRef voffset,
			    LLVMValueRef soffset,
			    unsigned inst_offset,
		            bool glc,
		            bool slc,
			    bool writeonly_memory,
			    bool swizzle_enable_hint);
LLVMValueRef
ac_build_buffer_load(struct ac_llvm_context *ctx,
		     LLVMValueRef rsrc,
		     int num_channels,
		     LLVMValueRef vindex,
		     LLVMValueRef voffset,
		     LLVMValueRef soffset,
		     unsigned inst_offset,
		     unsigned glc,
		     unsigned slc,
		     bool can_speculate,
		     bool allow_smem);

LLVMValueRef ac_build_buffer_load_format(struct ac_llvm_context *ctx,
					 LLVMValueRef rsrc,
					 LLVMValueRef vindex,
					 LLVMValueRef voffset,
					 unsigned num_channels,
					 bool glc,
					 bool can_speculate);

/* load_format that handles the stride & element count better if idxen is
 * disabled by LLVM. */
LLVMValueRef ac_build_buffer_load_format_gfx9_safe(struct ac_llvm_context *ctx,
                                                  LLVMValueRef rsrc,
                                                  LLVMValueRef vindex,
                                                  LLVMValueRef voffset,
                                                  unsigned num_channels,
                                                  bool glc,
                                                  bool can_speculate);

LLVMValueRef
ac_build_tbuffer_load_short(struct ac_llvm_context *ctx,
			    LLVMValueRef rsrc,
			    LLVMValueRef vindex,
			    LLVMValueRef voffset,
				LLVMValueRef soffset,
				LLVMValueRef immoffset,
				LLVMValueRef glc);

LLVMValueRef
ac_get_thread_id(struct ac_llvm_context *ctx);

#define AC_TID_MASK_TOP_LEFT 0xfffffffc
#define AC_TID_MASK_TOP      0xfffffffd
#define AC_TID_MASK_LEFT     0xfffffffe

LLVMValueRef
ac_build_ddxy(struct ac_llvm_context *ctx,
	      uint32_t mask,
	      int idx,
	      LLVMValueRef val);

#define AC_SENDMSG_GS 2
#define AC_SENDMSG_GS_DONE 3

#define AC_SENDMSG_GS_OP_NOP      (0 << 4)
#define AC_SENDMSG_GS_OP_CUT      (1 << 4)
#define AC_SENDMSG_GS_OP_EMIT     (2 << 4)
#define AC_SENDMSG_GS_OP_EMIT_CUT (3 << 4)

void ac_build_sendmsg(struct ac_llvm_context *ctx,
		      uint32_t msg,
		      LLVMValueRef wave_id);

LLVMValueRef ac_build_imsb(struct ac_llvm_context *ctx,
			   LLVMValueRef arg,
			   LLVMTypeRef dst_type);

LLVMValueRef ac_build_umsb(struct ac_llvm_context *ctx,
			  LLVMValueRef arg,
			  LLVMTypeRef dst_type);
LLVMValueRef ac_build_fmin(struct ac_llvm_context *ctx, LLVMValueRef a,
			   LLVMValueRef b);
LLVMValueRef ac_build_fmax(struct ac_llvm_context *ctx, LLVMValueRef a,
			   LLVMValueRef b);
LLVMValueRef ac_build_imin(struct ac_llvm_context *ctx, LLVMValueRef a,
			   LLVMValueRef b);
LLVMValueRef ac_build_imax(struct ac_llvm_context *ctx, LLVMValueRef a,
			   LLVMValueRef b);
LLVMValueRef ac_build_umin(struct ac_llvm_context *ctx, LLVMValueRef a, LLVMValueRef b);
LLVMValueRef ac_build_clamp(struct ac_llvm_context *ctx, LLVMValueRef value);

struct ac_export_args {
	LLVMValueRef out[4];
        unsigned target;
        unsigned enabled_channels;
        bool compr;
        bool done;
        bool valid_mask;
};

void ac_build_export(struct ac_llvm_context *ctx, struct ac_export_args *a);

void ac_build_export_null(struct ac_llvm_context *ctx);

enum ac_image_opcode {
	ac_image_sample,
	ac_image_gather4,
	ac_image_load,
	ac_image_load_mip,
	ac_image_store,
	ac_image_store_mip,
	ac_image_get_lod,
	ac_image_get_resinfo,
	ac_image_atomic,
	ac_image_atomic_cmpswap,
};

enum ac_atomic_op {
	ac_atomic_swap,
	ac_atomic_add,
	ac_atomic_sub,
	ac_atomic_smin,
	ac_atomic_umin,
	ac_atomic_smax,
	ac_atomic_umax,
	ac_atomic_and,
	ac_atomic_or,
	ac_atomic_xor,
};

enum ac_image_dim {
	ac_image_1d,
	ac_image_2d,
	ac_image_3d,
	ac_image_cube, // includes cube arrays
	ac_image_1darray,
	ac_image_2darray,
	ac_image_2dmsaa,
	ac_image_2darraymsaa,
};

/* These cache policy bits match the definitions used by the LLVM intrinsics. */
enum ac_image_cache_policy {
	ac_glc = 1 << 0,
	ac_slc = 1 << 1,
};

struct ac_image_args {
	enum ac_image_opcode opcode : 4;
	enum ac_atomic_op atomic : 4; /* for the ac_image_atomic opcode */
	enum ac_image_dim dim : 3;
	unsigned dmask : 4;
	unsigned cache_policy : 2;
	bool unorm : 1;
	bool level_zero : 1;
	unsigned attributes; /* additional call-site specific AC_FUNC_ATTRs */

	LLVMValueRef resource;
	LLVMValueRef sampler;
	LLVMValueRef data[2]; /* data[0] is source data (vector); data[1] is cmp for cmpswap */
	LLVMValueRef offset;
	LLVMValueRef bias;
	LLVMValueRef compare;
	LLVMValueRef derivs[6];
	LLVMValueRef coords[4];
	LLVMValueRef lod; // also used by ac_image_get_resinfo
};

LLVMValueRef ac_build_image_opcode(struct ac_llvm_context *ctx,
				   struct ac_image_args *a);
LLVMValueRef ac_build_cvt_pkrtz_f16(struct ac_llvm_context *ctx,
				    LLVMValueRef args[2]);
LLVMValueRef ac_build_cvt_pknorm_i16(struct ac_llvm_context *ctx,
				     LLVMValueRef args[2]);
LLVMValueRef ac_build_cvt_pknorm_u16(struct ac_llvm_context *ctx,
				     LLVMValueRef args[2]);
LLVMValueRef ac_build_cvt_pk_i16(struct ac_llvm_context *ctx,
				 LLVMValueRef args[2], unsigned bits, bool hi);
LLVMValueRef ac_build_cvt_pk_u16(struct ac_llvm_context *ctx,
				 LLVMValueRef args[2], unsigned bits, bool hi);
LLVMValueRef ac_build_wqm_vote(struct ac_llvm_context *ctx, LLVMValueRef i1);
void ac_build_kill_if_false(struct ac_llvm_context *ctx, LLVMValueRef i1);
LLVMValueRef ac_build_bfe(struct ac_llvm_context *ctx, LLVMValueRef input,
			  LLVMValueRef offset, LLVMValueRef width,
			  bool is_signed);
LLVMValueRef ac_build_imad(struct ac_llvm_context *ctx, LLVMValueRef s0,
			   LLVMValueRef s1, LLVMValueRef s2);
LLVMValueRef ac_build_fmad(struct ac_llvm_context *ctx, LLVMValueRef s0,
			   LLVMValueRef s1, LLVMValueRef s2);

void ac_build_waitcnt(struct ac_llvm_context *ctx, unsigned simm16);

LLVMValueRef ac_build_fract(struct ac_llvm_context *ctx, LLVMValueRef src0,
			   unsigned bitsize);

LLVMValueRef ac_build_isign(struct ac_llvm_context *ctx, LLVMValueRef src0,
			    unsigned bitsize);

LLVMValueRef ac_build_fsign(struct ac_llvm_context *ctx, LLVMValueRef src0,
			    unsigned bitsize);

LLVMValueRef ac_build_bit_count(struct ac_llvm_context *ctx, LLVMValueRef src0);

LLVMValueRef ac_build_bitfield_reverse(struct ac_llvm_context *ctx,
				       LLVMValueRef src0);

void ac_optimize_vs_outputs(struct ac_llvm_context *ac,
			    LLVMValueRef main_fn,
			    uint8_t *vs_output_param_offset,
			    uint32_t num_outputs,
			    uint8_t *num_param_exports);
void ac_init_exec_full_mask(struct ac_llvm_context *ctx);

void ac_declare_lds_as_pointer(struct ac_llvm_context *ac);
LLVMValueRef ac_lds_load(struct ac_llvm_context *ctx,
			 LLVMValueRef dw_addr);
void ac_lds_store(struct ac_llvm_context *ctx,
		  LLVMValueRef dw_addr, LLVMValueRef value);

LLVMValueRef ac_find_lsb(struct ac_llvm_context *ctx,
			 LLVMTypeRef dst_type,
			 LLVMValueRef src0);

LLVMTypeRef ac_array_in_const_addr_space(LLVMTypeRef elem_type);
LLVMTypeRef ac_array_in_const32_addr_space(LLVMTypeRef elem_type);

void ac_build_bgnloop(struct ac_llvm_context *ctx, int lable_id);
void ac_build_break(struct ac_llvm_context *ctx);
void ac_build_continue(struct ac_llvm_context *ctx);
void ac_build_else(struct ac_llvm_context *ctx, int lable_id);
void ac_build_endif(struct ac_llvm_context *ctx, int lable_id);
void ac_build_endloop(struct ac_llvm_context *ctx, int lable_id);
void ac_build_if(struct ac_llvm_context *ctx, LLVMValueRef value,
		 int lable_id);
void ac_build_uif(struct ac_llvm_context *ctx, LLVMValueRef value,
		  int lable_id);

LLVMValueRef ac_build_alloca(struct ac_llvm_context *ac, LLVMTypeRef type,
			     const char *name);
LLVMValueRef ac_build_alloca_undef(struct ac_llvm_context *ac, LLVMTypeRef type,
				   const char *name);

LLVMValueRef ac_cast_ptr(struct ac_llvm_context *ctx, LLVMValueRef ptr,
			 LLVMTypeRef type);

LLVMValueRef ac_trim_vector(struct ac_llvm_context *ctx, LLVMValueRef value,
			    unsigned count);

LLVMValueRef ac_unpack_param(struct ac_llvm_context *ctx, LLVMValueRef param,
			     unsigned rshift, unsigned bitwidth);

void ac_apply_fmask_to_sample(struct ac_llvm_context *ac, LLVMValueRef fmask,
			      LLVMValueRef *addr, bool is_array_tex);

LLVMValueRef
ac_build_ds_swizzle(struct ac_llvm_context *ctx, LLVMValueRef src, unsigned mask);

LLVMValueRef
ac_build_readlane(struct ac_llvm_context *ctx, LLVMValueRef src, LLVMValueRef lane);

LLVMValueRef
ac_build_writelane(struct ac_llvm_context *ctx, LLVMValueRef src, LLVMValueRef value, LLVMValueRef lane);

LLVMValueRef
ac_build_mbcnt(struct ac_llvm_context *ctx, LLVMValueRef mask);

LLVMValueRef
ac_build_inclusive_scan(struct ac_llvm_context *ctx, LLVMValueRef src, nir_op op);

LLVMValueRef
ac_build_exclusive_scan(struct ac_llvm_context *ctx, LLVMValueRef src, nir_op op);

LLVMValueRef
ac_build_reduce(struct ac_llvm_context *ctx, LLVMValueRef src, nir_op op, unsigned cluster_size);

LLVMValueRef
ac_build_quad_swizzle(struct ac_llvm_context *ctx, LLVMValueRef src,
		unsigned lane0, unsigned lane1, unsigned lane2, unsigned lane3);

LLVMValueRef
ac_build_shuffle(struct ac_llvm_context *ctx, LLVMValueRef src, LLVMValueRef index);

#ifdef __cplusplus
}
#endif

#endif
