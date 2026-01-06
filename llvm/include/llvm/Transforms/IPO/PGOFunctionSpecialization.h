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

namespace llvm {
using PGOSpecMap = DenseMap<Function *, std::pair<unsigned, unsigned>>;

struct PGOSpecSig {
  unsigned Key = 0;
  SmallVector<ArgInfo, 4> Args;

  bool operator==(const PGOSpecSig &Other) const {
    if (Key != Other.Key)
      return false;
    return Args == Other.Args;
  }

  friend hash_code hash_value(const PGOSpecSig &S) {
    return hash_combine(hash_value(S.Key), hash_combine_range(S.Args));
  }
};

struct PGOSpec {
  Function *F;
  Function *Clone = nullptr;
  PGOSpecSig Sig;
  Function *ExistingFunc = nullptr; // Pre-existing specialization to reuse
  unsigned OriginalCodeSize;
  unsigned OriginalLatency;
  unsigned SpecializedCodeSize;
  unsigned SpecializedLatency;
  unsigned Count;
  SmallVector<CallBase *> CallSites;

  PGOSpec(Function *F, Function *Clone, const PGOSpecSig &S, unsigned OrigCodeSize, unsigned OrigLatency,
          unsigned SpecCodeSize, unsigned SpecLatency, unsigned Cnt)
      : F(F), Clone(Clone), Sig(S), OriginalCodeSize(OrigCodeSize), OriginalLatency(OrigLatency),
        SpecializedCodeSize(SpecCodeSize), SpecializedLatency(SpecLatency), Count(Cnt) {}
  PGOSpec(Function *F, Function *Clone, const PGOSpecSig &&S, unsigned OrigCodeSize, unsigned OrigLatency,
          unsigned SpecCodeSize, unsigned SpecLatency, unsigned Cnt)
      : F(F), Clone(Clone), Sig(S), OriginalCodeSize(OrigCodeSize), OriginalLatency(OrigLatency),
        SpecializedCodeSize(SpecCodeSize), SpecializedLatency(SpecLatency), Count(Cnt) {}
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

  SmallPtrSet<Function *, 32> Specializations;
  SmallPtrSet<Function *, 32> FullySpecialized;
  DenseMap<Function *, CodeMetrics> FunctionMetrics;
  unsigned NGlobals = 0;

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

  std::pair<unsigned, unsigned> calculateFunctionSizeLatency(Function *F);

  bool findSpecializations(Function *F, unsigned FuncSize,
                           SmallVectorImpl<PGOSpec> &AllSpecs, PGOSpecMap &SM);

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
