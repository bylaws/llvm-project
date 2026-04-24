//=== ValueProfilePlugins.inc - set of plugins used by ValueProfileCollector =//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains a set of plugin classes used in ValueProfileCollectorImpl.
// Each plugin is responsible for collecting Value Profiling candidates for a
// particular optimization.
// Each plugin must satisfy the interface described in ValueProfileCollector.cpp
//
//===----------------------------------------------------------------------===//

#include "ValueProfileCollector.h"
#include "llvm/Analysis/IndirectCallVisitor.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/Transforms/Instrumentation/PGOInstrumentation.h"

using namespace llvm;
using CandidateInfo = ValueProfileCollector::CandidateInfo;

extern cl::opt<bool> MemOPOptMemcmpBcmp;

///--------------------------- MemIntrinsicPlugin ------------------------------
class MemIntrinsicPlugin : public InstVisitor<MemIntrinsicPlugin> {
  Function &F;
  TargetLibraryInfo &TLI;
  std::vector<CandidateInfo> *Candidates;

public:
  static constexpr InstrProfValueKind Kind = IPVK_MemOPSize;

  MemIntrinsicPlugin(Function &Fn, TargetLibraryInfo &TLI)
      : F(Fn), TLI(TLI), Candidates(nullptr) {}

  void run(std::vector<CandidateInfo> &Cs) {
    Candidates = &Cs;
    visit(F);
    Candidates = nullptr;
  }
  void visitMemIntrinsic(MemIntrinsic &MI) {
    Value *Length = MI.getLength();
    // Not instrument constant length calls.
    if (isa<ConstantInt>(Length))
      return;

    Instruction *InsertPt = &MI;
    Instruction *AnnotatedInst = &MI;
    Candidates->emplace_back(CandidateInfo{Length, InsertPt, AnnotatedInst});
  }
  void visitCallInst(CallInst &CI) {
    if (!MemOPOptMemcmpBcmp)
      return;
    auto *F = CI.getCalledFunction();
    if (!F)
      return;
    LibFunc Func;
    if (TLI.getLibFunc(CI, Func) &&
        (Func == LibFunc_memcmp || Func == LibFunc_bcmp)) {
      Value *Length = CI.getArgOperand(2);
      // Not instrument constant length calls.
      if (isa<ConstantInt>(Length))
        return;
      Instruction *InsertPt = &CI;
      Instruction *AnnotatedInst = &CI;
      Candidates->emplace_back(CandidateInfo{Length, InsertPt, AnnotatedInst});
    }
  }
};

///------------------------ IndirectCallPromotionPlugin ------------------------
class IndirectCallPromotionPlugin {
  Function &F;

public:
  static constexpr InstrProfValueKind Kind = IPVK_IndirectCallTarget;

  IndirectCallPromotionPlugin(Function &Fn, TargetLibraryInfo &TLI) : F(Fn) {}

  void run(std::vector<CandidateInfo> &Candidates) {
    std::vector<CallBase *> Result = findIndirectCalls(F);
    for (Instruction *I : Result) {
      Value *Callee = cast<CallBase>(I)->getCalledOperand();
      Instruction *InsertPt = I;
      Instruction *AnnotatedInst = I;
      Candidates.emplace_back(CandidateInfo{Callee, InsertPt, AnnotatedInst});
    }
  }
};

///--------------------- VirtualTableValueProfilingPlugin --------------------
class VTableProfilingPlugin {
  Function &F;

public:
  static constexpr InstrProfValueKind Kind = IPVK_VTableTarget;

  VTableProfilingPlugin(Function &Fn, TargetLibraryInfo &TLI) : F(Fn) {}

  void run(std::vector<CandidateInfo> &Candidates) {
    std::vector<Instruction *> Result = findVTableAddrs(F);
    for (Instruction *I : Result) {
      Instruction *InsertPt = I->getNextNonDebugInstruction();
      // When finding an insertion point, keep PHI and EH pad instructions
      // before vp intrinsics. This is similar to
      // `BasicBlock::getFirstInsertionPt`.
      while (InsertPt && (dyn_cast<PHINode>(InsertPt) || InsertPt->isEHPad()))
        InsertPt = InsertPt->getNextNonDebugInstruction();
      // Skip instrumentating the value if InsertPt is the last instruction.
      // FIXME: Set InsertPt to the end of basic block to instrument the value
      // if InsertPt is the last instruction.
      if (InsertPt == nullptr)
        continue;

      Instruction *AnnotatedInst = I;
      Candidates.emplace_back(CandidateInfo{I, InsertPt, AnnotatedInst});
    }
  }
};

///------------------------ ArgumentValueSpecializationPlugin ------------------------
struct PGODirectCallSiteVisitor : public InstVisitor<PGODirectCallSiteVisitor> {
  std::vector<CallBase *> Calls;

  void visitCallInst(CallInst &Call) {
    if (Call.isIndirectCall()) {
      return;
    }
    Calls.push_back(&Call);
  }

  void visitInvokeInst(InvokeInst &Invoke) {
    if (Invoke.isIndirectCall()) {
      return;
    }
    Calls.push_back(&Invoke);
  }
};

inline std::vector<CallBase *> findDirectCallSites(Function &F) {
  PGODirectCallSiteVisitor CV;
  CV.visit(F);
  return CV.Calls;
}

class ArgumentValueSpecializationPlugin {
  Function &F;

public:
  static constexpr InstrProfValueKind Kind = IPVK_ArgumentValue;

  ArgumentValueSpecializationPlugin(Function &Fn, TargetLibraryInfo &TLI) : F(Fn) {}

  void run(std::vector<CandidateInfo> &Candidates) {
    std::vector<CallBase *> Result = findDirectCallSites(F);

    for (CallBase *CB : Result) {
      Instruction *InsertPt = CB;
      Instruction *AnnotatedInst = CB;

      Function *Callee = CB->getCalledFunction();

      for (unsigned ArgIdx = 0; ArgIdx < CB->arg_size(); ++ArgIdx) {
        Value *V = CB->getArgOperand(ArgIdx);
        Type *T = V->getType();
        if (T->isIntegerTy() || T->isFloatTy() || T->isDoubleTy()) {
          Candidates.emplace_back(CandidateInfo{V, InsertPt, AnnotatedInst});
        } else if (T->isPointerTy() && Callee && !Callee->isDeclaration() &&
                   ArgIdx < Callee->arg_size()) {
          // For pointer arguments that are nonnull and used for virtual
          // dispatch in the callee, load the vptr and profile it — same
          // approach as VTableProfilingPlugin. Check the callee's formal
          // parameter (not call-site attrs) so VPArgIdx is consistent
          // across all callers.
          const Argument *FormalArg = Callee->getArg(ArgIdx);
          if (!FormalArg->hasNonNullAttr())
            continue;

          if (!isArgUsedForVirtualDispatch(FormalArg))
            continue;

          auto *VPtrLoad = new LoadInst(
              PointerType::getUnqual(F.getContext()), V, "vptr.arg.prof",
              CB->getIterator());
          Candidates.emplace_back(
              CandidateInfo{VPtrLoad, InsertPt, AnnotatedInst});
        }
      }
    }
  }
};

///------------------------ LoopTripCountProfilingPlugin ------------------------
///
/// Profiles the trip count bound of loops whose bound is loaded from memory
/// or otherwise not a compile-time constant. Knowing the dominant trip count
/// enables full unrolling, exact vectorization, and other loop transforms.
class LoopTripCountProfilingPlugin {
  Function &F;

public:
  static constexpr InstrProfValueKind Kind = IPVK_LoopTripCount;

  LoopTripCountProfilingPlugin(Function &Fn, TargetLibraryInfo &TLI) : F(Fn) {}

  /// Given a loop exit condition icmp, return the loop-invariant bound operand
  /// (i.e. the one that is NOT the induction variable).
  /// Since PGO instrumentation runs before LICM, the bound may still be inside
  /// the loop as a load from a loop-invariant address. In that case, create a
  /// hoisted copy in the preheader for profiling.
  static Value *findLoopBound(Loop *L, ICmpInst *Cmp, BasicBlock *Preheader) {
    Value *Op0 = Cmp->getOperand(0);
    Value *Op1 = Cmp->getOperand(1);
    bool Op0Invariant = L->isLoopInvariant(Op0);
    bool Op1Invariant = L->isLoopInvariant(Op1);
    // Exactly one should be loop-invariant (the bound).
    if (Op0Invariant && !Op1Invariant)
      return Op0;
    if (!Op0Invariant && Op1Invariant)
      return Op1;

    // Neither operand is loop-invariant. This happens when LICM hasn't run
    // yet. Look through sext/zext for a load from a loop-invariant address
    // and hoist it to the preheader.
    auto tryHoistBound = [&](Value *Op) -> Value * {
      Value *V = Op;
      SmallVector<CastInst *, 2> CastChain;
      while (auto *Cast = dyn_cast<CastInst>(V)) {
        if (Cast->getOpcode() == Instruction::SExt ||
            Cast->getOpcode() == Instruction::ZExt) {
          CastChain.push_back(Cast);
          V = Cast->getOperand(0);
        } else
          break;
      }
      auto *LI = dyn_cast<LoadInst>(V);
      if (!LI || LI->isVolatile())
        return nullptr;
      if (!L->isLoopInvariant(LI->getPointerOperand()))
        return nullptr;

      // The load is from a loop-invariant address. Hoist a copy to the
      // preheader so we can profile the value once per loop entry.
      IRBuilder<> Builder(Preheader->getTerminator());
      Value *Result = Builder.CreateAlignedLoad(
          LI->getType(), LI->getPointerOperand(), LI->getAlign(),
          "ltc.bound");
      // Replay sext/zext chain (innermost cast first).
      for (auto It = CastChain.rbegin(); It != CastChain.rend(); ++It) {
        Result = Builder.CreateCast((*It)->getOpcode(), Result,
                                    (*It)->getType());
      }
      return Result;
    };

    if (Value *Hoisted = tryHoistBound(Op0))
      return Hoisted;
    if (Value *Hoisted = tryHoistBound(Op1))
      return Hoisted;
    return nullptr;
  }

  void run(std::vector<CandidateInfo> &Candidates) {
    DominatorTree DT(F);
    LoopInfo LI(DT);

    for (Loop *L : LI.getLoopsInPreorder()) {
      if (!L->isLoopSimplifyForm())
        continue;

      BasicBlock *ExitingBB = L->getExitingBlock();
      if (!ExitingBB)
        continue;

      auto *BI = dyn_cast<BranchInst>(ExitingBB->getTerminator());
      if (!BI || !BI->isConditional())
        continue;

      auto *Cmp = dyn_cast<ICmpInst>(BI->getCondition());
      if (!Cmp)
        continue;

      BasicBlock *Preheader = L->getLoopPreheader();
      if (!Preheader)
        continue;

      Value *Bound = findLoopBound(L, Cmp, Preheader);
      if (!Bound || isa<ConstantInt>(Bound))
        continue;

      if (!Bound->getType()->isIntegerTy())
        continue;

      // Profile the bound value in the preheader (once per loop entry).
      // Annotate the icmp — it survives LoopRotate and other loop transforms,
      // unlike the preheader terminator which gets replaced.
      Instruction *InsertPt = Preheader->getTerminator();
      Instruction *AnnotatedInst = Cmp;
      Candidates.emplace_back(CandidateInfo{Bound, InsertPt, AnnotatedInst});
    }
  }
};

///----------------------- Registration of the plugins -------------------------
/// For now, registering a plugin with the ValueProfileCollector is done by
/// adding the plugin type to the VP_PLUGIN_LIST macro.
#define VP_PLUGIN_LIST                                                         \
  MemIntrinsicPlugin, IndirectCallPromotionPlugin, VTableProfilingPlugin,      \
      ArgumentValueSpecializationPlugin, LoopTripCountProfilingPlugin
