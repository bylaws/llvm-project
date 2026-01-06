//===- PGOFunctionSpecialization.h - PGO Function Specialization ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_PGOFUNCTIONSPECIALIZATION_H
#define LLVM_TRANSFORMS_IPO_PGOFUNCTIONSPECIALIZATION_H

#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/SCCPSolver.h"
#include "llvm/Transforms/Utils/SizeOpts.h"
#include <map>

namespace llvm {
struct PGOSpec {
  Function *F;
  Function *Clone = nullptr;
  Function *ExistingFunc = nullptr; // Pre-existing specialization to reuse
  ArgInfo Arg;
  unsigned OriginalCodeSize;
  unsigned OriginalLatency;
  unsigned SpecializedCodeSize;
  unsigned SpecializedLatency;
  uint64_t Count;
  SmallVector<CallBase *> CallSites;

  PGOSpec(Function *F, Function *Clone, const ArgInfo &A, unsigned OrigCodeSize,
          unsigned OrigLatency, unsigned SpecCodeSize, unsigned SpecLatency,
          uint64_t Cnt)
      : F(F), Clone(Clone), Arg(A), OriginalCodeSize(OrigCodeSize),
        OriginalLatency(OrigLatency), SpecializedCodeSize(SpecCodeSize),
        SpecializedLatency(SpecLatency), Count(Cnt) {}
  PGOSpec(Function *F, Function *Clone, const ArgInfo &&A,
          unsigned OrigCodeSize, unsigned OrigLatency, unsigned SpecCodeSize,
          unsigned SpecLatency, uint64_t Cnt)
      : F(F), Clone(Clone), Arg(A), OriginalCodeSize(OrigCodeSize),
        OriginalLatency(OrigLatency), SpecializedCodeSize(SpecCodeSize),
        SpecializedLatency(SpecLatency), Count(Cnt) {}
};

class PGOFunctionSpecializer {
  Module &M;
  FunctionAnalysisManager *FAM;
  std::function<BlockFrequencyInfo &(Function &)> GetBFI;
  std::function<const TargetLibraryInfo &(Function &)> GetTLI;
  std::function<TargetTransformInfo &(Function &)> GetTTI;
  std::function<AssumptionCache &(Function &)> GetAC;
  std::function<DominatorTree &(Function &)> GetDT;
  int OptLevel;

  DenseMap<Function *, CodeMetrics> FunctionMetrics;

public:
  PGOFunctionSpecializer(
      Module &M, FunctionAnalysisManager *FAM,
      std::function<BlockFrequencyInfo &(Function &)> GetBFI,
      std::function<const TargetLibraryInfo &(Function &)> GetTLI,
      std::function<TargetTransformInfo &(Function &)> GetTTI,
      std::function<AssumptionCache &(Function &)> GetAC,
      std::function<DominatorTree &(Function &)> GetDT, int OptLevel)
      : M(M), FAM(FAM), GetBFI(GetBFI), GetTLI(GetTLI), GetTTI(GetTTI),
        GetAC(GetAC), GetDT(GetDT), OptLevel(OptLevel) {}

  LLVM_ABI ~PGOFunctionSpecializer();

  LLVM_ABI bool run();

private:
  std::pair<Function *, Function *>
  cloneFunctionSpecialized(Function *F, Argument *Arg, Constant *C, uint64_t V);

  std::pair<unsigned, unsigned>
  calculateFunctionSizeLatency(std::map<unsigned, uint64_t> Tags, Function *F,
                               FunctionCallee &BlockTag);

  bool findSpecializations(Function *F, unsigned FuncSize,
                           SmallVectorImpl<PGOSpec> &AllSpecs,
                           FunctionCallee &BlockTag);

  bool isCandidateFunction(Function *F);
};

class PGOFunctionSpecializationPass
    : public PassInfoMixin<PGOFunctionSpecializationPass> {
  int OptLevel;

public:
  PGOFunctionSpecializationPass(int OptLevel = 2) : OptLevel(OptLevel) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_IPO_PGOFUNCTIONSPECIALIZATION_H
