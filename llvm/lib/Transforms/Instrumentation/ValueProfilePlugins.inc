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

inline bool isArgumentUsedAsCallTarget(Function *Callee, unsigned ArgNo) {
  if (!Callee || Callee->isDeclaration() || ArgNo >= Callee->arg_size())
    return false;
  Argument *Arg = Callee->getArg(ArgNo);
  for (Use &U : Arg->uses()) {
    if (auto *CB = dyn_cast<CallBase>(U.getUser())) {
      if (CB->isCallee(&U))
        return true;
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

    for (CallBase *I : Result) {
      Instruction *InsertPt = I;
      Instruction *AnnotatedInst = I;

      unsigned ArgIdx = 0;
      for (Value *V : I->args()) {
        Type *T = V->getType();
        if (T->isIntegerTy() || T->isFloatTy() || T->isDoubleTy()) {
          Candidates.emplace_back(CandidateInfo{V, InsertPt, AnnotatedInst});
        } else if (T->isPointerTy() &&
                   isArgumentUsedAsCallTarget(I->getCalledFunction(), ArgIdx)) {
          Candidates.emplace_back(CandidateInfo{V, InsertPt, AnnotatedInst});
        }
        ++ArgIdx;
      }
    }
  }
};

///----------------------- Registration of the plugins -------------------------
/// For now, registering a plugin with the ValueProfileCollector is done by
/// adding the plugin type to the VP_PLUGIN_LIST macro.
#define VP_PLUGIN_LIST                                                         \
  MemIntrinsicPlugin, IndirectCallPromotionPlugin, VTableProfilingPlugin, ArgumentValueSpecializationPlugin
