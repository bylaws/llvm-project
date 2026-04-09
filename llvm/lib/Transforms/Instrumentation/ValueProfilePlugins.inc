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
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/InstVisitor.h"

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

/// Check if a callee's formal parameter is used for virtual dispatch.
/// Returns true if the argument has a load (vptr load) whose result feeds
/// into indirect calls through the vtable pattern.
static bool isArgUsedForVirtualDispatch(const Argument *Arg) {
  // Look for: load from the argument (vptr load) -> GEP/bitcast ->
  //           load (function pointer) -> indirect call
  for (const User *U : Arg->users()) {
    const auto *LI = dyn_cast<LoadInst>(U);
    if (!LI)
      continue;
    // This load from the argument could be the vptr load.
    // Check if the loaded value is used in an indirect call pattern.
    for (const User *VU : LI->users()) {
      // The vptr is used in a GEP to index into the vtable, or
      // directly as a function pointer load base.
      const Value *MaybeGEP = VU;
      if (auto *GEP = dyn_cast<GetElementPtrInst>(VU))
        MaybeGEP = GEP;

      for (const User *GU : MaybeGEP->users()) {
        if (const auto *FnLoad = dyn_cast<LoadInst>(GU)) {
          for (const User *FU : FnLoad->users()) {
            if (const auto *CB = dyn_cast<CallBase>(FU)) {
              if (CB->isIndirectCall() &&
                  CB->getCalledOperand() == FnLoad)
                return true;
            }
          }
        }
        // Also handle the case where the load result IS the called operand
        // (single level of indirection).
        if (const auto *CB = dyn_cast<CallBase>(GU)) {
          if (CB->isIndirectCall() && CB->getCalledOperand() == VU)
            return true;
        }
      }
    }
  }
  return false;
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

///----------------------- Registration of the plugins -------------------------
/// For now, registering a plugin with the ValueProfileCollector is done by
/// adding the plugin type to the VP_PLUGIN_LIST macro.
#define VP_PLUGIN_LIST                                                         \
  MemIntrinsicPlugin, IndirectCallPromotionPlugin, VTableProfilingPlugin, ArgumentValueSpecializationPlugin
