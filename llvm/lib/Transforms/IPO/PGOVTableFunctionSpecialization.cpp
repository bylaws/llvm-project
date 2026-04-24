//===- PGOVTableFunctionSpecialization.cpp - VTable Function Spec
//----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/PGOVTableFunctionSpecialization.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/IndirectCallVisitor.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/TypeMetadataUtils.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Instrumentation/PGOInstrumentation.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

#define DEBUG_TYPE "pgo-vtable-func-spec"

static cl::opt<bool> EnablePGOVTableFuncSpec("enable-pgo-vtable-func-spec",
                                             cl::init(false), cl::Hidden,
                                             cl::desc(""));

static cl::opt<unsigned> VTableDominanceThreshold(
    "pgo-vtable-func-spec-dominance-thresh", cl::init(90), cl::Hidden,
    cl::desc("Minimum percentage of calls a vtable must account for to "
             "trigger specialization"));

STATISTIC(NumSpecialized, "Number of functions specialized on vtable");
STATISTIC(NumCallSitesRewritten,
          "Number of call sites rewritten for vtable specialization");

namespace {
// LLVM will generate type.test intrinsics before any virtual calls to verify
// the type with the given vtable pointer is compatible with the expected type.
// We need this to recover the address point as it is lost by llvm-profdata when
// it remaps the specific offset vtable address point to the root vtable MD5
// hash.
StringRef findCompatibleType(LoadInst *VPtrLoad, Function &F) {
  for (const User *U : VPtrLoad->users()) {
    const auto *CI = dyn_cast<CallInst>(U);
    if (!CI)
      continue;
    auto *CalleeF = CI->getCalledFunction();
    if (!CalleeF)
      continue;
    if (CalleeF->getIntrinsicID() != Intrinsic::type_test &&
        CalleeF->getIntrinsicID() != Intrinsic::public_type_test)
      continue;
    auto *MDV = dyn_cast<MetadataAsValue>(CI->getArgOperand(1));
    if (!MDV)
      continue;
    if (auto *MDS = dyn_cast<MDString>(MDV->getMetadata()))
      return MDS->getString();
  }
  return {};
}

struct VTableArgInfo {
  Argument *PtrArg;
  LoadInst *VPtrLoad;
  StringRef CompatType;
  uint32_t VPArgIdx;
};

struct MonomorphicCallSite {
  CallBase *CS;
  uint64_t VTableMD5;
  uint64_t Count;
  uint64_t TotalCount;
};
} // anonymous namespace

static Function *cloneFunctionSpecialized(Function *F, Argument *Arg,
                                          Constant *AddrPoint,
                                          uint64_t VTableMD5, Module &M) {
  ValueToValueMapTy VMap;
  Function *Clone = CloneFunction(F, VMap);

  std::string NewName =
      (F->getName() + ".spec.vtable." + Twine(VTableMD5)).str();
  Clone->setName(NewName);

  Clone->addFnAttr("pgo.specialization");
  if (F->hasLocalLinkage()) {
    Clone->setLinkage(GlobalValue::InternalLinkage);
  } else {
    Clone->setLinkage(GlobalValue::LinkOnceODRLinkage);
    Clone->setVisibility(GlobalValue::HiddenVisibility);
    Comdat *CD = M.getOrInsertComdat(NewName);
    CD->setSelectionKind(Comdat::Any);
    Clone->setComdat(CD);
  }

  Argument *ClonedArg = cast<Argument>(VMap[Arg]);

  // Accumulate into list to avoid concurrent modification while iterating
  SmallVector<LoadInst *> VPtrLoads;
  for (User *U : ClonedArg->users()) {
    if (auto *LI = dyn_cast<LoadInst>(U)) {
      if (LI->getPointerOperand()->stripPointerCasts() == ClonedArg)
        VPtrLoads.push_back(LI);
    }
  }
  for (LoadInst *LI : VPtrLoads) {
    LI->replaceAllUsesWith(AddrPoint);
    LI->eraseFromParent();
  }

  return Clone;
}

/// Rewrite a call site to dispatch to the specialized clone when the runtime
/// vptr matches the expected one, very close to PGOFunctionSpecialization
static void rewriteCallSite(CallBase *CS, Function *Clone, unsigned ArgNo,
                            Constant *AddrPoint, uint64_t SpecCount,
                            uint64_t TotalCount) {
  Function *Func = CS->getFunction();
  auto &Ctx = Func->getContext();
  Value *Arg = CS->getArgOperand(ArgNo);

  BasicBlock *BB = CS->getParent();

  CS->setMetadata(LLVMContext::MD_prof, nullptr);

  BasicBlock *DefaultBB = nullptr;
  BasicBlock *MergeBB = nullptr;
  BasicBlock *UnwindBB = nullptr;

  if (auto *Invoke = dyn_cast<InvokeInst>(CS)) {
    DefaultBB = SplitBlock(BB, CS);

    BasicBlock *OrigNormalDest = Invoke->getNormalDest();
    UnwindBB = Invoke->getUnwindDest();

    MergeBB = BasicBlock::Create(Ctx, "", Func, OrigNormalDest);
    BranchInst::Create(OrigNormalDest, MergeBB);

    Invoke->setNormalDest(MergeBB);

    for (PHINode &PN : OrigNormalDest->phis()) {
      int Idx = PN.getBasicBlockIndex(DefaultBB);
      if (Idx >= 0)
        PN.setIncomingBlock(Idx, MergeBB);
    }
  } else {
    DefaultBB = SplitBlock(BB, CS);
    BasicBlock::iterator AfterCS(CS);
    ++AfterCS;
    assert(AfterCS != DefaultBB->end());
    MergeBB = SplitBlock(DefaultBB, &(*AfterCS));
  }

  MergeBB->setName("CS.Merge");
  DefaultBB->setName("CS.Default");

  BB->getTerminator()->eraseFromParent();

  IRBuilder<> IRB(BB);

  Value *VPtr = IRB.CreateLoad(PointerType::getUnqual(Ctx), Arg, "CS.vptr");
  Value *Cmp = IRB.CreateICmpEQ(VPtr, AddrPoint, "CS.cmp");

  BasicBlock *CaseBB = BasicBlock::Create(Ctx, "CS.Case", Func, DefaultBB);

  uint64_t DefaultCount =
      (TotalCount > SpecCount) ? (TotalCount - SpecCount) : 1;
  MDBuilder MDB(Ctx);
  BranchInst *CondBr = IRB.CreateCondBr(Cmp, CaseBB, DefaultBB);
  CondBr->setMetadata(LLVMContext::MD_prof,
                      MDB.createBranchWeights(SpecCount, DefaultCount));

  CallBase *SpecCall = cast<CallBase>(CS->clone());
  SpecCall->setCalledFunction(Clone);
  SpecCall->insertInto(CaseBB, CaseBB->end());

  if (UnwindBB) {
    // Update phis for the new predecessor
    for (PHINode &PN : UnwindBB->phis()) {
      Value *Val = PN.getIncomingValueForBlock(DefaultBB);
      PN.addIncoming(Val, CaseBB);
    }
  } else {
    IRBuilder<> CaseIRB(CaseBB);
    CaseIRB.CreateBr(MergeBB);
  }

  Type *CSTy = CS->getType();
  if (!CSTy->isVoidTy()) {
    // Insert a phi for the return values at the merge block.
    IRBuilder<> IRBM(MergeBB, MergeBB->getFirstNonPHIIt());
    PHINode *PHI = IRBM.CreatePHI(CSTy, 2, "CS.RVMerge");
    CS->replaceAllUsesWith(PHI);
    PHI->addIncoming(CS, DefaultBB);
    PHI->addIncoming(SpecCall, CaseBB);
  }

  ++NumCallSitesRewritten;
}

PreservedAnalyses
PGOVTableFunctionSpecializationPass::run(Module &M, ModuleAnalysisManager &AM) {
  if (!EnablePGOVTableFuncSpec)
    return PreservedAnalyses::all();

  InstrProfSymtab Symtab;
  if (Error E = Symtab.create(M, /*InLTO=*/true)) {
    consumeError(std::move(E));
    return PreservedAnalyses::all();
  }

  bool Changed = false;

  struct SpecTarget {
    Function *F;
    VTableArgInfo ArgInfo;
    // vtable MD5: clone + call sites.
    DenseMap<uint64_t, SmallVector<MonomorphicCallSite>> VTableCallSites;
  };
  SmallVector<SpecTarget> Targets;

  for (Function &F : M) {
    if (F.isDeclaration() || F.arg_empty())
      continue;
    if (F.hasFnAttribute(Attribute::NoDuplicate))
      continue;
    if (F.hasFnAttribute("pgo.specialization"))
      continue;

    for (auto &PA : getProfiledArgs(F)) {
      if (PA.Kind != ProfiledArgKind::VDispatchPtr)
        continue;
      Argument &Arg = *PA.Arg;

      LoadInst *VPtrLoad = nullptr;
      StringRef CompatType;
      for (User *U : Arg.users()) {
        auto *LI = dyn_cast<LoadInst>(U);
        if (!LI || LI->getPointerOperand()->stripPointerCasts() != &Arg)
          continue;
        CompatType = findCompatibleType(LI, F);
        if (!CompatType.empty()) {
          VPtrLoad = LI;
          break;
        }
      }
      if (!VPtrLoad)
        continue;

      VTableArgInfo Info = {&Arg, VPtrLoad, CompatType, PA.VPArgIdx};

      // Scan all call sites to F and check per-call-site argument value data.
      DenseMap<uint64_t, SmallVector<MonomorphicCallSite>> VTableCallSites;

      for (User *U : F.users()) {
        auto *CS = dyn_cast<CallBase>(U);
        if (!CS || CS->getCalledFunction() != &F)
          continue;

        uint64_t TotalCount = 0;
        auto VPD = getValueProfDataFromInst(*CS, IPVK_ArgumentValue, 1,
                                            TotalCount, false, PA.VPArgIdx);
        if (VPD.empty() || TotalCount == 0)
          continue;

        // We only care about the most common value
        if (VPD[0].Count * 100 < TotalCount * VTableDominanceThreshold)
          continue;

        uint64_t VTableMD5 = VPD[0].Value;

        LLVM_DEBUG(dbgs() << "PGOVTableSpec: monomorphic call site in "
                          << CS->getFunction()->getName()
                          << " vtable MD5=" << VTableMD5 << "\n");

        VTableCallSites[VTableMD5].push_back(
            {CS, VTableMD5, VPD[0].Count, TotalCount});
      }

      if (!VTableCallSites.empty()) {
        Targets.push_back({&F, Info, std::move(VTableCallSites)});
        break; // One arg per function for now.
      }
    }
  }

  // Now create clones and rewrite call sites.
  for (auto &Target : Targets) {
    Function *F = Target.F;
    VTableArgInfo &ArgInfo = Target.ArgInfo;

    for (auto &[VTableMD5, CallSites] : Target.VTableCallSites) {
      // Resolve vtable GV from MD5 hash.
      GlobalVariable *VTableGV = Symtab.getGlobalVariable(VTableMD5);
      if (!VTableGV || !VTableGV->hasDefinitiveInitializer()) {
        LLVM_DEBUG(dbgs() << "PGOVTableSpec: cannot resolve vtable GV for MD5="
                          << VTableMD5 << "\n");
        continue;
      }

      // Resolve the address point offset for the callsite type for
      // multiple-virtual-inheritance, 0 otherwise
      auto AddrPointOffset =
          getAddressPointOffset(*VTableGV, ArgInfo.CompatType);
      if (!AddrPointOffset) {
        LLVM_DEBUG(dbgs() << "PGOVTableSpec: no address point offset for "
                          << VTableGV->getName() << " with type "
                          << ArgInfo.CompatType << "\n");
        continue;
      }

      // Resolve the called type vtable base into a concrete constant to
      // specialize on.
      Constant *AddrPointConst =
          getVTableAddressPointOffset(VTableGV, *AddrPointOffset);

      LLVM_DEBUG(dbgs() << "PGOVTableSpec: specializing " << F->getName()
                        << " arg " << ArgInfo.PtrArg->getArgNo()
                        << " on vtable " << VTableGV->getName()
                        << " (MD5=" << VTableMD5 << ", " << CallSites.size()
                        << " call sites)\n");

      Function *Clone = cloneFunctionSpecialized(
          F, ArgInfo.PtrArg, AddrPointConst, VTableMD5, *F->getParent());

      for (auto &MCS : CallSites) {
        LLVM_DEBUG(dbgs() << "PGOVTableSpec: rewriting call site in "
                          << MCS.CS->getFunction()->getName() << "\n");
        rewriteCallSite(MCS.CS, Clone, ArgInfo.PtrArg->getArgNo(),
                        AddrPointConst, MCS.Count, MCS.TotalCount);
      }

      Changed = true;
      ++NumSpecialized;
    }
  }

  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserve<FunctionAnalysisManagerModuleProxy>();
  return PA;
}
