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

#include <llvm-c/Core.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Scalar.h>

#include <cstring>

/* DO NOT REORDER THE HEADERS
 * The LLVM headers need to all be included before any Mesa header,
 * as they use the `restrict` keyword in ways that are incompatible
 * with our #define in include/c99_compat.h
 */

#include "ac_binary.h"
#include "ac_llvm_util.h"
#include "ac_llvm_build.h"
#include "util/macros.h"

using namespace llvm;

bool ac_is_llvm_processor_supported(LLVMTargetMachineRef tm, const char *processor)
{
   TargetMachine *TM = reinterpret_cast<TargetMachine *>(tm);
   return TM->getMCSubtargetInfo()->isCPUStringValid(processor);
}

void ac_reset_llvm_all_options_occurences()
{
   cl::ResetAllOptionOccurrences();
}

void ac_add_attr_dereferenceable(LLVMValueRef val, uint64_t bytes)
{
   Argument *A = unwrap<Argument>(val);
   A->addAttr(Attribute::getWithDereferenceableBytes(A->getContext(), bytes));
}

void ac_add_attr_alignment(LLVMValueRef val, uint64_t bytes)
{
   Argument *A = unwrap<Argument>(val);
   A->addAttr(Attribute::getWithAlignment(A->getContext(), Align(bytes)));
}

bool ac_is_sgpr_param(LLVMValueRef arg)
{
   Argument *A = unwrap<Argument>(arg);
   AttributeList AS = A->getParent()->getAttributes();
   unsigned ArgNo = A->getArgNo();
   return AS.hasParamAttr(ArgNo, Attribute::InReg);
}

LLVMModuleRef ac_create_module(LLVMTargetMachineRef tm, LLVMContextRef ctx)
{
   TargetMachine *TM = reinterpret_cast<TargetMachine *>(tm);
   LLVMModuleRef module = LLVMModuleCreateWithNameInContext("mesa-shader", ctx);

   unwrap(module)->setTargetTriple(TM->getTargetTriple().getTriple());
   unwrap(module)->setDataLayout(TM->createDataLayout());
   return module;
}

LLVMBuilderRef ac_create_builder(LLVMContextRef ctx, enum ac_float_mode float_mode)
{
   LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);

   FastMathFlags flags;

   switch (float_mode) {
   case AC_FLOAT_MODE_DEFAULT:
   case AC_FLOAT_MODE_DENORM_FLUSH_TO_ZERO:
      break;

   case AC_FLOAT_MODE_DEFAULT_OPENGL:
      /* Allow optimizations to treat the sign of a zero argument or
       * result as insignificant.
       */
      flags.setNoSignedZeros(); /* nsz */

      /* Allow optimizations to use the reciprocal of an argument
       * rather than perform division.
       */
      flags.setAllowReciprocal(); /* arcp */

      unwrap(builder)->setFastMathFlags(flags);
      break;
   }

   return builder;
}

void ac_enable_signed_zeros(struct ac_llvm_context *ctx)
{
   if (ctx->float_mode == AC_FLOAT_MODE_DEFAULT_OPENGL) {
      auto *b = unwrap(ctx->builder);
      FastMathFlags flags = b->getFastMathFlags();

      /* This disables the optimization of (x + 0), which is used
       * to convert negative zero to positive zero.
       */
      flags.setNoSignedZeros(false);
      b->setFastMathFlags(flags);
   }
}

void ac_disable_signed_zeros(struct ac_llvm_context *ctx)
{
   if (ctx->float_mode == AC_FLOAT_MODE_DEFAULT_OPENGL) {
      auto *b = unwrap(ctx->builder);
      FastMathFlags flags = b->getFastMathFlags();

      flags.setNoSignedZeros();
      b->setFastMathFlags(flags);
   }
}

LLVMTargetLibraryInfoRef ac_create_target_library_info(const char *triple)
{
   return reinterpret_cast<LLVMTargetLibraryInfoRef>(
      new TargetLibraryInfoImpl(Triple(triple)));
}

void ac_dispose_target_library_info(LLVMTargetLibraryInfoRef library_info)
{
   delete reinterpret_cast<TargetLibraryInfoImpl *>(library_info);
}

/* Implementation of raw_pwrite_stream that works on malloc()ed memory for
 * better compatibility with C code. */
struct raw_memory_ostream : public raw_pwrite_stream {
   char *buffer;
   size_t written;
   size_t bufsize;

   raw_memory_ostream()
   {
      buffer = NULL;
      written = 0;
      bufsize = 0;
      SetUnbuffered();
   }

   ~raw_memory_ostream()
   {
      free(buffer);
   }

   void clear()
   {
      written = 0;
   }

   void take(char *&out_buffer, size_t &out_size)
   {
      out_buffer = buffer;
      out_size = written;
      buffer = NULL;
      written = 0;
      bufsize = 0;
   }

   void flush() = delete;

   void write_impl(const char *ptr, size_t size) override
   {
      if (unlikely(written + size < written))
         abort();
      if (written + size > bufsize) {
         bufsize = MAX3(1024, written + size, bufsize / 3 * 4);
         buffer = (char *)realloc(buffer, bufsize);
         if (!buffer) {
            fprintf(stderr, "amd: out of memory allocating ELF buffer\n");
            abort();
         }
      }
      memcpy(buffer + written, ptr, size);
      written += size;
   }

   void pwrite_impl(const char *ptr, size_t size, uint64_t offset) override
   {
      assert(offset == (size_t)offset && offset + size >= offset && offset + size <= written);
      memcpy(buffer + offset, ptr, size);
   }

   uint64_t current_pos() const override
   {
      return written;
   }
};

/* The LLVM compiler is represented as a pass manager containing passes for
 * optimizations, instruction selection, and code generation.
 */
struct ac_compiler_passes {
   raw_memory_ostream ostream;        /* ELF shader binary stream */
   legacy::PassManager passmgr; /* list of passes */
};

struct ac_compiler_passes *ac_create_llvm_passes(LLVMTargetMachineRef tm)
{
   struct ac_compiler_passes *p = new ac_compiler_passes();
   if (!p)
      return NULL;

   TargetMachine *TM = reinterpret_cast<TargetMachine *>(tm);

   if (TM->addPassesToEmitFile(p->passmgr, p->ostream, nullptr,
                               CGFT_ObjectFile)) {
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
bool ac_compile_module_to_elf(struct ac_compiler_passes *p, LLVMModuleRef module,
                              char **pelf_buffer, size_t *pelf_size)
{
   p->passmgr.run(*unwrap(module));
   p->ostream.take(*pelf_buffer, *pelf_size);
   return true;
}

void ac_llvm_add_barrier_noop_pass(LLVMPassManagerRef passmgr)
{
   unwrap(passmgr)->add(createBarrierNoopPass());
}

LLVMValueRef ac_build_atomic_rmw(struct ac_llvm_context *ctx, LLVMAtomicRMWBinOp op,
                                 LLVMValueRef ptr, LLVMValueRef val, const char *sync_scope)
{
   AtomicRMWInst::BinOp binop;
   switch (op) {
   case LLVMAtomicRMWBinOpXchg:
      binop = AtomicRMWInst::Xchg;
      break;
   case LLVMAtomicRMWBinOpAdd:
      binop = AtomicRMWInst::Add;
      break;
   case LLVMAtomicRMWBinOpSub:
      binop = AtomicRMWInst::Sub;
      break;
   case LLVMAtomicRMWBinOpAnd:
      binop = AtomicRMWInst::And;
      break;
   case LLVMAtomicRMWBinOpNand:
      binop = AtomicRMWInst::Nand;
      break;
   case LLVMAtomicRMWBinOpOr:
      binop = AtomicRMWInst::Or;
      break;
   case LLVMAtomicRMWBinOpXor:
      binop = AtomicRMWInst::Xor;
      break;
   case LLVMAtomicRMWBinOpMax:
      binop = AtomicRMWInst::Max;
      break;
   case LLVMAtomicRMWBinOpMin:
      binop = AtomicRMWInst::Min;
      break;
   case LLVMAtomicRMWBinOpUMax:
      binop = AtomicRMWInst::UMax;
      break;
   case LLVMAtomicRMWBinOpUMin:
      binop = AtomicRMWInst::UMin;
      break;
   case LLVMAtomicRMWBinOpFAdd:
      binop = AtomicRMWInst::FAdd;
      break;
   default:
      unreachable("invalid LLVMAtomicRMWBinOp");
      break;
   }
   unsigned SSID = unwrap(ctx->context)->getOrInsertSyncScopeID(sync_scope);
   return wrap(unwrap(ctx->builder)
                        ->CreateAtomicRMW(binop, unwrap(ptr), unwrap(val),
#if LLVM_VERSION_MAJOR >= 13
                                          MaybeAlign(0),
#endif
                                          AtomicOrdering::SequentiallyConsistent, SSID));
}

LLVMValueRef ac_build_atomic_cmp_xchg(struct ac_llvm_context *ctx, LLVMValueRef ptr,
                                      LLVMValueRef cmp, LLVMValueRef val, const char *sync_scope)
{
   unsigned SSID = unwrap(ctx->context)->getOrInsertSyncScopeID(sync_scope);
   return wrap(unwrap(ctx->builder)
                        ->CreateAtomicCmpXchg(unwrap(ptr), unwrap(cmp),
                                              unwrap(val),
#if LLVM_VERSION_MAJOR >= 13
                                              MaybeAlign(0),
#endif
                                              AtomicOrdering::SequentiallyConsistent,
                                              AtomicOrdering::SequentiallyConsistent, SSID));
}

void ac_add_sinking_pass(LLVMPassManagerRef PM)
{
  unwrap(PM)->add(createLoopSinkPass());
}
