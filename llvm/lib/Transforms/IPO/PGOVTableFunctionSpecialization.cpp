//===- PGOVTableFunctionSpecialization.cpp - VTable Function Spec ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass specializes functions on monomorphic vtable arguments using
// per-call-site IPVK_ArgumentValue profile data. For a function with a
// polymorphic pointer argument, if callers' profiles show 100% one vtable:
//
//   1. Clone the function, replacing the vptr load with the constant vtable
//      address point. Downstream passes (InstCombine, inliner) resolve
//      load(gep(vtable, offset)) -> Function*, devirtualizing all calls.
//
//   2. At each monomorphic call site, insert a vptr guard dispatching to the
//      specialized clone.
//
// This runs before ICP. Where ICP adds one guard per indirect call, this pass
// adds one guard at the call site for all virtual calls through the pointer.
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
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

using namespace llvm;

#define DEBUG_TYPE "pgo-vtable-func-spec"

static cl::opt<bool> EnablePGOVTableFuncSpec(
    "enable-pgo-vtable-func-spec", cl::init(false), cl::Hidden,
    cl::desc("Enable PGO-driven vtable function specialization"));

STATISTIC(NumSpecialized, "Number of functions specialized on vtable");
STATISTIC(NumCallSitesRewritten,
          "Number of call sites rewritten for vtable specialization");

// Copied from IndirectCallPromotion.cpp (static there).
static std::optional<uint64_t>
getAddressPointOffset(const GlobalVariable &VTableVar,
                      StringRef CompatibleType) {
  SmallVector<MDNode *> Types;
  VTableVar.getMetadata(LLVMContext::MD_type, Types);

  for (MDNode *Type : Types)
    if (auto *TypeId = dyn_cast<MDString>(Type->getOperand(1).get());
        TypeId && TypeId->getString() == CompatibleType)
      return cast<ConstantInt>(
                 cast<ConstantAsMetadata>(Type->getOperand(0))->getValue())
          ->getZExtValue();

  return std::nullopt;
}

// Copied from IndirectCallPromotion.cpp (static there).
static Constant *getVTableAddressPointOffset(GlobalVariable *VTable,
                                             uint32_t AddressPointOffset) {
  Module &M = *VTable->getParent();
  LLVMContext &Context = M.getContext();
  assert(AddressPointOffset <
             M.getDataLayout().getTypeAllocSize(VTable->getValueType()) &&
         "Out-of-bound access");

  return ConstantExpr::getInBoundsGetElementPtr(
      Type::getInt8Ty(Context), VTable,
      llvm::ConstantInt::get(Type::getInt32Ty(Context), AddressPointOffset));
}

/// Check if a formal parameter is used for virtual dispatch.
/// Must be kept in sync with the same check in ValueProfilePlugins.inc.
static bool isArgUsedForVirtualDispatch(const Argument *Arg) {
  for (const User *U : Arg->users()) {
    const auto *LI = dyn_cast<LoadInst>(U);
    if (!LI)
      continue;
    for (const User *VU : LI->users()) {
      const Value *MaybeGEP = VU;
      if (auto *GEP = dyn_cast<GetElementPtrInst>(VU))
        MaybeGEP = GEP;
      for (const User *GU : MaybeGEP->users()) {
        if (const auto *FnLoad = dyn_cast<LoadInst>(GU)) {
          for (const User *FU : FnLoad->users()) {
            if (const auto *CB = dyn_cast<CallBase>(FU)) {
              if (CB->isIndirectCall() && CB->getCalledOperand() == FnLoad)
                return true;
            }
          }
        }
        if (const auto *CB = dyn_cast<CallBase>(GU)) {
          if (CB->isIndirectCall() && CB->getCalledOperand() == VU)
            return true;
        }
      }
    }
  }
  return false;
}

/// Find the vptr load instruction from a function argument.
/// The pattern is: %vptr = load ptr, ptr %arg
static LoadInst *findVPtrLoad(const Argument *Arg) {
  for (const User *U : Arg->users()) {
    if (const auto *LI = dyn_cast<LoadInst>(U)) {
      if (LI->getPointerOperand()->stripPointerCasts() == Arg)
        return const_cast<LoadInst *>(LI);
    }
  }
  return nullptr;
}

/// Find the compatible type string for a vptr load by scanning
/// llvm.type.test / llvm.public.type.test uses in the function.
static StringRef findCompatibleTypeStr(LoadInst *VPtrLoad, Function &F) {
  // The vptr load result feeds into llvm.type.test or llvm.public.type.test.
  for (const User *U : VPtrLoad->users()) {
    const auto *CI = dyn_cast<CallInst>(U);
    if (!CI)
      continue;
    Function *Callee = CI->getCalledFunction();
    if (!Callee)
      continue;
    if (Callee->getIntrinsicID() != Intrinsic::type_test &&
        Callee->getIntrinsicID() != Intrinsic::public_type_test)
      continue;
    auto *TypeMDVal = dyn_cast<MetadataAsValue>(CI->getArgOperand(1));
    if (!TypeMDVal)
      continue;
    auto *TypeId = dyn_cast<MDString>(TypeMDVal->getMetadata());
    if (TypeId)
      return TypeId->getString();
  }
  return StringRef();
}

/// Compute the IPVK_ArgumentValue site index (VPArgIdx) for a given argument.
/// Must be kept in sync with the ArgumentValueSpecializationPlugin.
static std::optional<uint32_t> computeVPArgIdx(const Argument *TargetArg) {
  Function *F = const_cast<Function *>(TargetArg->getParent());
  uint32_t VPArgIdx = 0;
  for (const Argument &Arg : F->args()) {
    if (&Arg == TargetArg)
      return VPArgIdx;
    Type *T = Arg.getType();
    if (T->isIntegerTy() || T->isFloatTy() || T->isDoubleTy())
      VPArgIdx++;
    else if (T->isPointerTy() && Arg.hasNonNullAttr() &&
             isArgUsedForVirtualDispatch(&Arg))
      VPArgIdx++;
  }
  return std::nullopt;
}

namespace {
/// Info about a specializable vtable argument in a function.
struct VTableArgInfo {
  Argument *PtrArg;
  LoadInst *VPtrLoad;
  StringRef CompatibleTypeStr;
  uint32_t VPArgIdx;
};

/// A call site with its monomorphic vtable info.
struct MonomorphicCallSite {
  CallBase *CS;
  uint64_t VTableMD5;
  uint64_t Count;
  uint64_t TotalCount;
};
} // anonymous namespace

/// Clone function F and replace ALL vptr loads from the polymorphic argument
/// with the constant vtable address point. Returns the cloned function.
static Function *cloneWithVTableConst(Function *F, Argument *PtrArg,
                                      GlobalVariable *VTableGV,
                                      Constant *AddrPointConst,
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

  // Replace ALL vptr loads (load ptr, ptr %arg) in the clone with the constant
  // address point. There may be multiple loads from the same argument if the
  // frontend emitted separate vptr loads for each virtual call.
  Argument *ClonedArg = cast<Argument>(VMap[PtrArg]);
  SmallVector<LoadInst *> VPtrLoads;
  for (User *U : ClonedArg->users()) {
    if (auto *LI = dyn_cast<LoadInst>(U)) {
      if (LI->getPointerOperand()->stripPointerCasts() == ClonedArg)
        VPtrLoads.push_back(LI);
    }
  }
  for (LoadInst *LI : VPtrLoads) {
    LI->replaceAllUsesWith(AddrPointConst);
    LI->eraseFromParent();
  }

  return Clone;
}

/// Rewrite a call site to dispatch to the specialized clone when the runtime
/// vptr matches the expected vtable.
static void rewriteCallSite(CallBase *CS, Function *Clone,
                             unsigned ArgNo, Constant *AddressPointConst,
                             uint64_t SpecCount, uint64_t TotalCount) {
  Function *Caller = CS->getFunction();
  LLVMContext &Ctx = Caller->getContext();
  Value *Arg = CS->getArgOperand(ArgNo);

  BasicBlock *OrigBB = CS->getParent();

  // Clear VP metadata before splitting (can trigger metadata uniquing issues).
  CS->setMetadata(LLVMContext::MD_prof, nullptr);

  BasicBlock *OrigCallBB = nullptr;
  BasicBlock *TailBB = nullptr;
  BasicBlock *UnwindBB = nullptr;

  if (auto *Invoke = dyn_cast<InvokeInst>(CS)) {
    OrigCallBB = SplitBlock(OrigBB, CS);

    BasicBlock *OrigNormalDest = Invoke->getNormalDest();
    UnwindBB = Invoke->getUnwindDest();

    TailBB = SplitBlock(OrigNormalDest, &OrigNormalDest->front());
    Invoke->setNormalDest(TailBB);

    // OrigNormalDest is now dead (just br TailBB). Make it unreachable
    // so it's not a spurious predecessor of TailBB.
    OrigNormalDest->getTerminator()->eraseFromParent();
    new UnreachableInst(Ctx, OrigNormalDest);
  } else {
    OrigCallBB = SplitBlock(OrigBB, CS);
    BasicBlock::iterator AfterCS(CS);
    ++AfterCS;
    assert(AfterCS != OrigCallBB->end());
    TailBB = SplitBlock(OrigCallBB, &(*AfterCS));
  }

  TailBB->setName("vtspec.merge");
  OrigCallBB->setName("vtspec.orig");

  OrigBB->getTerminator()->eraseFromParent();

  IRBuilder<> IRB(OrigBB);

  bool NeedsNullCheck = !(CS->paramHasAttr(ArgNo, Attribute::NonNull) ||
                          CS->paramHasAttr(ArgNo, Attribute::Dereferenceable));

  if (NeedsNullCheck) {
    BasicBlock *CheckVTableBB =
        BasicBlock::Create(Ctx, "vtspec.check", Caller, OrigCallBB);
    Value *IsNull = IRB.CreateICmpEQ(
        Arg, ConstantPointerNull::get(cast<PointerType>(Arg->getType())));
    IRB.CreateCondBr(IsNull, OrigCallBB, CheckVTableBB);
    IRB.SetInsertPoint(CheckVTableBB);
  }

  Value *VPtr = IRB.CreateLoad(PointerType::getUnqual(Ctx), Arg, "vtspec.vptr");
  Value *Cmp = IRB.CreateICmpEQ(VPtr, AddressPointConst, "vtspec.cmp");

  BasicBlock *SpecBB =
      BasicBlock::Create(Ctx, "vtspec.spec", Caller, OrigCallBB);

  uint64_t OrigCount = (TotalCount > SpecCount) ? (TotalCount - SpecCount) : 1;
  MDBuilder MDB(Ctx);
  BranchInst *VTableBr = IRB.CreateCondBr(Cmp, SpecBB, OrigCallBB);
  VTableBr->setMetadata(LLVMContext::MD_prof,
                        MDB.createBranchWeights(SpecCount, OrigCount));

  // Clone the original call, preserving InvokeInst if applicable.
  CallBase *SpecCall = cast<CallBase>(CS->clone());
  SpecCall->setCalledFunction(Clone);
  SpecCall->insertInto(SpecBB, SpecBB->end());

  if (auto *SpecInvoke = dyn_cast<InvokeInst>(SpecCall)) {
    SpecInvoke->setNormalDest(TailBB);
    // Update PHI nodes in UnwindBB: SpecBB is a new predecessor.
    for (PHINode &PN : UnwindBB->phis()) {
      Value *Val = PN.getIncomingValueForBlock(OrigCallBB);
      PN.addIncoming(Val, SpecBB);
    }
  } else {
    IRBuilder<> SpecIRB(SpecBB);
    SpecIRB.CreateBr(TailBB);
  }

  Type *RetTy = CS->getType();
  if (!RetTy->isVoidTy()) {
    IRBuilder<> TailIRB(TailBB, TailBB->getFirstInsertionPt());
    PHINode *PHI = TailIRB.CreatePHI(RetTy, 2, "vtspec.rv");
    CS->replaceAllUsesWith(PHI);
    PHI->addIncoming(CS, OrigCallBB);
    PHI->addIncoming(SpecCall, SpecBB);
  }

  ++NumCallSitesRewritten;
}

PreservedAnalyses
PGOVTableFunctionSpecializationPass::run(Module &M,
                                         ModuleAnalysisManager &AM) {
  if (!EnablePGOVTableFuncSpec)
    return PreservedAnalyses::all();

  // Build the symbol table for resolving vtable MD5 hashes to GlobalVariables.
  InstrProfSymtab Symtab;
  if (Error E = Symtab.create(M, /*InLTO=*/true)) {
    consumeError(std::move(E));
    return PreservedAnalyses::all();
  }

  bool Changed = false;

  // For each function, find pointer args suitable for vtable specialization.
  struct SpecTarget {
    Function *F;
    VTableArgInfo ArgInfo;
    // Grouped by vtable MD5: clone + call sites.
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

    // Find pointer args with nonnull + virtual dispatch.
    for (Argument &Arg : F.args()) {
      if (!Arg.getType()->isPointerTy())
        continue;
      if (!Arg.hasNonNullAttr())
        continue;
      if (!isArgUsedForVirtualDispatch(&Arg))
        continue;

      // Find the vptr load from this arg.
      LoadInst *VPtrLoad = findVPtrLoad(&Arg);
      if (!VPtrLoad)
        continue;

      // Find the compatible type string.
      StringRef CompatTypeStr = findCompatibleTypeStr(VPtrLoad, F);
      if (CompatTypeStr.empty())
        continue;

      LLVM_DEBUG(dbgs() << "PGOVTableSpec: " << F.getName() << " arg "
                        << Arg.getArgNo()
                        << " has nonnull+vdispatch, compat type: "
                        << CompatTypeStr << "\n");

      // Compute the VPArgIdx.
      auto MaybeIdx = computeVPArgIdx(&Arg);
      if (!MaybeIdx)
        continue;

      VTableArgInfo Info = {&Arg, VPtrLoad, CompatTypeStr, *MaybeIdx};

      // Scan all call sites to F and check per-call-site argument value data.
      DenseMap<uint64_t, SmallVector<MonomorphicCallSite>> VTableCallSites;

      for (User *U : F.users()) {
        auto *CS = dyn_cast<CallBase>(U);
        if (!CS || CS->getCalledFunction() != &F)
          continue;

        uint64_t TotalCount = 0;
        auto VPD = getValueProfDataFromInst(*CS, IPVK_ArgumentValue,
                                            /*MaxNumValueData=*/1, TotalCount,
                                            /*GetNoICPValue=*/false,
                                            /*SiteIndex=*/*MaybeIdx);
        if (VPD.empty() || TotalCount == 0)
          continue;

        // Require the dominant vtable to account for >90% of calls.
        if (VPD[0].Count * 10 < TotalCount * 9)
          continue;

        uint64_t VTableMD5 = VPD[0].Value;

        // Verify this looks like a vtable hash (not a raw scalar value).
        // Vtable MD5 hashes are large; small values are likely scalar args
        // that weren't remapped.
        if (VTableMD5 < (1ULL << 32))
          continue;

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

      // Get address point offset from type metadata.
      auto AddrPointOffset =
          getAddressPointOffset(*VTableGV, ArgInfo.CompatibleTypeStr);
      if (!AddrPointOffset) {
        LLVM_DEBUG(dbgs() << "PGOVTableSpec: no address point offset for "
                          << VTableGV->getName() << " with type "
                          << ArgInfo.CompatibleTypeStr << "\n");
        continue;
      }

      Constant *AddrPointConst =
          getVTableAddressPointOffset(VTableGV, *AddrPointOffset);

      
          dbgs() << "PGOVTableSpec: specializing " << F->getName()
                        << " arg " << ArgInfo.PtrArg->getArgNo()
                        << " on vtable " << VTableGV->getName()
                        << " (MD5=" << VTableMD5 << ", "
                        << CallSites.size() << " call sites)\n";

      Function *Clone = cloneWithVTableConst(
          F, ArgInfo.PtrArg, VTableGV, AddrPointConst, VTableMD5,
          *F->getParent());

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
