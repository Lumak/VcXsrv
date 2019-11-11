/*
 * Copyright 2014 Advanced Micro Devices, Inc.
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

/* based on Marek's patch to lp_bld_misc.cpp */

// Workaround http://llvm.org/PR23628
#pragma push_macro("DEBUG")
#undef DEBUG

#include "ac_binary.h"
#include "ac_llvm_util.h"

#include <llvm-c/Core.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Transforms/IPO.h>

#include <llvm/IR/LegacyPassManager.h>
#if HAVE_LLVM < 0x0700
#include "llvm/Support/raw_ostream.h"
#endif

void ac_add_attr_dereferenceable(LLVMValueRef val, uint64_t bytes)
{
   llvm::Argument *A = llvm::unwrap<llvm::Argument>(val);
   A->addAttr(llvm::Attribute::getWithDereferenceableBytes(A->getContext(), bytes));
}

bool ac_is_sgpr_param(LLVMValueRef arg)
{
	llvm::Argument *A = llvm::unwrap<llvm::Argument>(arg);
	llvm::AttributeList AS = A->getParent()->getAttributes();
	unsigned ArgNo = A->getArgNo();
	return AS.hasAttribute(ArgNo + 1, llvm::Attribute::InReg);
}

LLVMValueRef ac_llvm_get_called_value(LLVMValueRef call)
{
	return LLVMGetCalledValue(call);
}

bool ac_llvm_is_function(LLVMValueRef v)
{
	return LLVMGetValueKind(v) == LLVMFunctionValueKind;
}

LLVMModuleRef ac_create_module(LLVMTargetMachineRef tm, LLVMContextRef ctx)
{
   llvm::TargetMachine *TM = reinterpret_cast<llvm::TargetMachine*>(tm);
   LLVMModuleRef module = LLVMModuleCreateWithNameInContext("mesa-shader", ctx);

   llvm::unwrap(module)->setTargetTriple(TM->getTargetTriple().getTriple());
   llvm::unwrap(module)->setDataLayout(TM->createDataLayout());
   return module;
}

LLVMBuilderRef ac_create_builder(LLVMContextRef ctx,
				 enum ac_float_mode float_mode)
{
	LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);

	llvm::FastMathFlags flags;

	switch (float_mode) {
	case AC_FLOAT_MODE_DEFAULT:
		break;
	case AC_FLOAT_MODE_NO_SIGNED_ZEROS_FP_MATH:
		flags.setNoSignedZeros();
		llvm::unwrap(builder)->setFastMathFlags(flags);
		break;
	case AC_FLOAT_MODE_UNSAFE_FP_MATH:
		flags.setFast();
		llvm::unwrap(builder)->setFastMathFlags(flags);
		break;
	}

	return builder;
}

LLVMTargetLibraryInfoRef
ac_create_target_library_info(const char *triple)
{
	return reinterpret_cast<LLVMTargetLibraryInfoRef>(new llvm::TargetLibraryInfoImpl(llvm::Triple(triple)));
}

void
ac_dispose_target_library_info(LLVMTargetLibraryInfoRef library_info)
{
	delete reinterpret_cast<llvm::TargetLibraryInfoImpl *>(library_info);
}

/* The LLVM compiler is represented as a pass manager containing passes for
 * optimizations, instruction selection, and code generation.
 */
struct ac_compiler_passes {
	ac_compiler_passes(): ostream(code_string) {}

	llvm::SmallString<0> code_string;  /* ELF shader binary */
	llvm::raw_svector_ostream ostream; /* stream for appending data to the binary */
	llvm::legacy::PassManager passmgr; /* list of passes */
};

struct ac_compiler_passes *ac_create_llvm_passes(LLVMTargetMachineRef tm)
{
	struct ac_compiler_passes *p = new ac_compiler_passes();
	if (!p)
		return NULL;

	llvm::TargetMachine *TM = reinterpret_cast<llvm::TargetMachine*>(tm);

	if (TM->addPassesToEmitFile(p->passmgr, p->ostream,
#if HAVE_LLVM >= 0x0700
				    nullptr,
#endif
				    llvm::TargetMachine::CGFT_ObjectFile)) {
		fprintf(stderr, "amd: TargetMachine can't emit a file of this type!\n");
		delete p;
		return NULL;
	}
	return p;
}

void ac_destroy_llvm_passes(struct ac_compiler_passes *p)
{
	delete p;
}

/* This returns false on failure. */
bool ac_compile_module_to_binary(struct ac_compiler_passes *p, LLVMModuleRef module,
				 struct ac_shader_binary *binary)
{
	p->passmgr.run(*llvm::unwrap(module));

	llvm::StringRef data = p->ostream.str();
	bool success = ac_elf_read(data.data(), data.size(), binary);
	p->code_string = ""; /* release the ELF shader binary */

	if (!success)
		fprintf(stderr, "amd: cannot read an ELF shader binary\n");
	return success;
}

void ac_llvm_add_barrier_noop_pass(LLVMPassManagerRef passmgr)
{
	llvm::unwrap(passmgr)->add(llvm::createBarrierNoopPass());
}

void ac_enable_global_isel(LLVMTargetMachineRef tm)
{
#if HAVE_LLVM >= 0x0700
  reinterpret_cast<llvm::TargetMachine*>(tm)->setGlobalISel(true);
#endif
}
