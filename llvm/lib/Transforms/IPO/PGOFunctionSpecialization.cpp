//===- PGOFunctionSpecialization.cpp - PGO Function Specialization --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/PGOFunctionSpecialization.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/CodeMetrics.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueLattice.h"
#include "llvm/Analysis/ValueLatticeUtils.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Transforms/Instrumentation/PGOInstrumentation.h"
#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Transforms/Scalar/LoopDeletion.h"
#include "llvm/Transforms/Scalar/LoopFlatten.h"
#include "llvm/Transforms/Scalar/LoopIdiomRecognize.h"
#include "llvm/Transforms/Scalar/LoopInstSimplify.h"
#include "llvm/Transforms/Scalar/LoopRotation.h"
#include "llvm/Transforms/Scalar/LoopSimplifyCFG.h"
#include "llvm/Transforms/Scalar/LoopSink.h"
#include "llvm/Transforms/Scalar/LoopUnrollAndJamPass.h"
#include "llvm/Transforms/Scalar/LoopUnrollPass.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimpleLoopUnswitch.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/SizeOpts.h"
#include <cmath>
#include <map>

using namespace llvm;

#define DEBUG_TYPE "pgo-function-specialization"

STATISTIC(NumSpecsCreated, "Number of specializations created");

static cl::opt<bool> ForceSpecialization(
    "force-pgo-specialization", cl::init(false), cl::Hidden,
    cl::desc(
        "Force function specialization for every call site with a constant "
        "argument"));

static cl::opt<unsigned> MinFunctionSize(
    "pgofuncspec-min-function-size", cl::init(50), cl::Hidden,
    cl::desc("Don't specialize functions that have less than this number of "
             "instructions"));

static cl::opt<unsigned>
    AnalysisCutoffThresh("pgofuncspec-analysis-cutoff-thresh", cl::init(50),
                         cl::Hidden, cl::desc("TODO"));

static cl::opt<unsigned>
    CandidateCutoffThresh("pgofuncspec-candidate-cutoff-thresh", cl::init(97),
                          cl::Hidden, cl::desc("TODO"));

static cl::opt<unsigned> DispatchCost("pgofuncspec-dispatch-cost", cl::init(7),
                                      cl::Hidden, cl::desc("TODO"));

static cl::opt<unsigned> HotFuncThresh("pgofuncspec-hot-func-thresh",
                                       cl::init(3500), cl::Hidden,
                                       cl::desc("TODO"));
static cl::opt<unsigned> BlowupFactor("pgofuncspec-blowup-factor", cl::init(300),
                                      cl::Hidden, cl::desc("TODO"));

static cl::opt<unsigned> MaxSpecSize("pgofuncspec-max-spec-size",
                                     cl::init(2000), cl::Hidden,
                                     cl::desc("TODO"));

static cl::opt<unsigned> MinLatencyThresh("pgofuncspec-min-latency-thresh",
                                          cl::init(5), cl::Hidden,
                                          cl::desc("TODO"));

static cl::opt<unsigned> MaxSpecInlineSize("pgofuncspec-max-spec-inline-size",
                                           cl::init(10), cl::Hidden,
                                           cl::desc("TODO"));
std::pair<Function *, Function *>
PGOFunctionSpecializer::cloneFunctionSpecialized(Function *F, Argument *Arg,
                                                 Constant *C, uint64_t V) {
  std::string NewName = F->getName().str() + ".spec." +
                        std::to_string(Arg->getArgNo()) + "." +
                        std::to_string(V); // Generate a unique name for dedeup

  // Check if a function with this specialized name already exists in the module
  Function *ExistingFunc = M.getFunction(NewName);
  if (ExistingFunc) {
    dbgs() << "PGOFnSpecialization: Found existing specialization " << NewName
           << "\n";
  }

  ValueToValueMapTy VMap;

  Function *ClonedF = CloneFunction(F, VMap);

  ClonedF->setName(NewName + ".analysis");
  ClonedF->setLinkage(GlobalValue::InternalLinkage);
  Argument *ClonedArg = ClonedF->getArg(Arg->getArgNo());
  ClonedArg->replaceAllUsesWith(C);
  FunctionPassManager FPM;

  FPM.addPass(InstCombinePass());
  FPM.addPass(
      SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));

  FPM.addPass(SROAPass(SROAOptions::ModifyCFG));

  FPM.addPass(EarlyCSEPass(false));
  FPM.addPass(InstCombinePass());
  FPM.addPass(
      SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));
  LoopPassManager LPM1, LPM2;
  LPM1.addPass(LoopInstSimplifyPass());
  LPM1.addPass(LoopSimplifyCFGPass());
  LPM1.addPass(LoopRotatePass(true, false));
  LPM1.addPass(SimpleLoopUnswitchPass());
  LPM1.addPass(LoopSimplifyCFGPass());
  LPM2.addPass(LoopIdiomRecognizePass());
  LPM2.addPass(IndVarSimplifyPass());
  LPM2.addPass(LoopDeletionPass());
  LPM2.addPass(LoopFullUnrollPass(OptLevel, false, false));
  FPM.addPass(createFunctionToLoopPassAdaptor(std::move(LPM1),
                                              /*UseMemorySSA=*/false,
                                              /*UseBlockFrequencyInfo=*/false));
  FPM.addPass(
      SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));
  FPM.addPass(InstCombinePass());
  FPM.addPass(createFunctionToLoopPassAdaptor(std::move(LPM2),
                                              /*UseMemorySSA=*/false,
                                              /*UseBlockFrequencyInfo=*/false));

  FPM.addPass(SCCPPass());
  FPM.addPass(InstCombinePass());
  FPM.addPass(ADCEPass());
  FPM.addPass(
      SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));

  FPM.run(*ClonedF, *FAM);


  // If no existing function was found, finalize the clone as the actual
  // specialization
  if (!ExistingFunc) {
    ClonedF->addFnAttr("pgo.specialization");
    ClonedF->setName(NewName);
    if (F->hasLocalLinkage()) {
      ClonedF->setLinkage(GlobalValue::InternalLinkage);
    } else {
      ClonedF->setLinkage(GlobalValue::LinkOnceODRLinkage);
      ClonedF->setVisibility(GlobalValue::HiddenVisibility);
      Comdat *CD = M.getOrInsertComdat(NewName);
      CD->setSelectionKind(Comdat::Any);
      ClonedF->setComdat(CD);
    }
  }

  return {ClonedF, ExistingFunc};
}

std::pair<unsigned, unsigned>
PGOFunctionSpecializer::calculateFunctionSizeLatency(
    std::map<unsigned, uint64_t> Tags, Function *F, FunctionCallee &BlockTag) {

  unsigned TotalSize = 0;
  uint64_t TotalLatency = 0;
  auto &TTI = GetTTI(*F);
  BlockFrequencyInfo *BFI = nullptr;
  uint64_t EntryFreq = 1;

  if (Tags.empty()) {
    BFI = &GetBFI(*F);
    EntryFreq = BFI->getEntryFreq().getFrequency();
    if (EntryFreq == 0)
      EntryFreq = 1;
  }

  unsigned ID = 0;

  for (BasicBlock &BB : *F) {
    uint64_t Weight = 0;

    if (!Tags.empty()) {
      if (auto It = Tags.find(ID); It != Tags.end()) {
        Weight = It->second;
      } else {
        Weight = 0;
      }
    } else {
      uint64_t BBFreq = BFI->getBlockFreq(&BB).getFrequency();
      Weight = (BBFreq * 100) / EntryFreq;
      if (Weight == 0 && BBFreq > 0)
        Weight = 1;
    }

    if (Weight) {
      for (Instruction &I : BB) {
        if (auto *C = dyn_cast<CallInst>(&I);
            C && C->getCalledFunction() == BlockTag.getCallee()) {
          continue;
        }

        InstructionCost SizeCost =
            TTI.getInstructionCost(&I, TargetTransformInfo::TCK_CodeSize);
        InstructionCost LatencyCost =
            TTI.getInstructionCost(&I, TargetTransformInfo::TCK_Latency);

        TotalSize += SizeCost.isValid() ? SizeCost.getValue() : 1;

        uint64_t InstLatency =
            LatencyCost.isValid() ? LatencyCost.getValue() : 1;
        TotalLatency += InstLatency * Weight;
      }
    }
    ID++;
  }

  TotalLatency /= 100;
  if (TotalLatency > UINT_MAX)
    TotalLatency = UINT_MAX;

  return {TotalSize, static_cast<unsigned>(TotalLatency)};
}

PGOFunctionSpecializer::~PGOFunctionSpecializer() {
  if (NumSpecsCreated > 0)
    dbgs() << "PGOFnSpecialization: Created " << NumSpecsCreated
           << " specializations in module " << M.getName() << "\n";
}

bool PGOFunctionSpecializer::run() {
  SmallVector<PGOSpec, 32> AllSpecs;
  unsigned NumCandidates = 0;

  AttributeList Attrs = AttributeList::get(
      M.getContext(), AttributeList::FunctionIndex, {Attribute::NoUnwind});

  FunctionCallee BlockTag = M.getOrInsertFunction(
      "__pgo_block_marker", Attrs, Type::getVoidTy(M.getContext()),
      Type::getInt32Ty(M.getContext()));
  dyn_cast<Function>(BlockTag.getCallee())->setOnlyAccessesInaccessibleMemory();

  for (Function &F : M) {
    if (!isCandidateFunction(&F))
      continue;

    auto [It, Inserted] = FunctionMetrics.try_emplace(&F);
    CodeMetrics &Metrics = It->second;
    if (Inserted) {
      SmallPtrSet<const Value *, 32> EphValues;
      CodeMetrics::collectEphemeralValues(&F, &GetAC(F), EphValues);
      for (BasicBlock &BB : F)
        Metrics.analyzeBasicBlock(&BB, GetTTI(F), EphValues);
    }

    const bool RequireMinSize =
        !ForceSpecialization && !F.hasFnAttribute(Attribute::NoInline);

    if (Metrics.notDuplicatable || !Metrics.NumInsts.isValid() ||
        (RequireMinSize && Metrics.NumInsts < MinFunctionSize))
      continue;

    if (Metrics.isRecursive)
      continue;

    int64_t Sz = Metrics.NumInsts.getValue();
    assert(Sz > 0 && "CodeSize should be positive");
    unsigned FuncSize = static_cast<unsigned>(Sz);

    dbgs() << "PGOFnSpecialization: Specialization cost for " << F.getName()
           << " is " << FuncSize << "\n";

    if (!findSpecializations(&F, FuncSize, AllSpecs, BlockTag)) {
      dbgs() << "PGOFnSpecialization: No possible specializations found for "
             << F.getName() << "\n";
      continue;
    }

    ++NumCandidates;
  }

  if (!NumCandidates) {
    dbgs()
        << "PGOFnSpecialization: No possible specializations found in module\n";
    return false;
  }

  const unsigned NSpecs = unsigned(AllSpecs.size());
  SmallVector<unsigned> BestSpecs(NSpecs + 1);
  std::iota(BestSpecs.begin(), BestSpecs.begin() + NSpecs, 0);

  dbgs() << "PGOFnSpecialization: List of specializations \n";
  for (unsigned I = 0; I < NSpecs; ++I) {
    const PGOSpec &S = AllSpecs[BestSpecs[I]];
    dbgs() << "PGOFnSpecialization: Function " << S.F->getName()
           << " , OriginalLatency " << S.OriginalLatency
           << " , SpecializedLatency " << S.SpecializedLatency << " , Count "
           << S.Count << " , SpecializedCodeSize " << S.SpecializedCodeSize
           << " , MinValueProp " << S.MinValueProp << "\n";
    dbgs() << "PGOFnSpecialization:   FormalArg = "
           << S.Arg.Formal->getNameOrAsOperand()
           << ", ActualArg = " << S.Arg.Actual->getNameOrAsOperand() << "\n";
  }

  struct CallSiteSpecsItem {
    uint64_t TotalCount = 0;
    SmallVector<std::pair<PGOSpec *, uint64_t>> Specs;
  };

  DenseMap<CallBase *, CallSiteSpecsItem> CallSiteSpecs;
  for (unsigned I = 0; I < NSpecs; ++I) {
    PGOSpec &S = AllSpecs[BestSpecs[I]];

    auto *Func = S.getSpecializedFunc();
    auto SpecFuncEntryCount = Func->getEntryCount();
    Func->setEntryCount(SpecFuncEntryCount->getCount() + S.Count, SpecFuncEntryCount->getType());

    for (auto &[C, Count] : S.CallSites) {
      auto &Item = CallSiteSpecs[C];
      Item.TotalCount += Count;
      Item.Specs.push_back({&S, Count});
    }
  }

  for (auto &[CS, SpecsInfo] : CallSiteSpecs) {
    // Derived from PGOMemOPSizeOpt, extended to support FP types and invoke
    Function *Func = CS->getFunction();
    auto &Ctx = Func->getContext();

    DominatorTree &DT = GetDT(*Func);
    BasicBlock *BB = CS->getParent();

    auto &BFI = GetBFI(*Func);
    auto OrigBBFreq = BFI.getBlockFreq(BB);
    uint64_t TotalCount = BFI.getBlockProfileCount(BB).value_or(0);

    CS->setMetadata(LLVMContext::MD_prof, nullptr);

    BasicBlock *DefaultBB = nullptr;
    BasicBlock *MergeBB = nullptr;
    BasicBlock *UnwindBB = nullptr;

    if (auto *Invoke = dyn_cast<InvokeInst>(CS)) {
      DefaultBB = SplitBlock(BB, CS, &DT);

      BasicBlock *OrigNormalDest = Invoke->getNormalDest();
      UnwindBB = Invoke->getUnwindDest();

      MergeBB = SplitBlock(OrigNormalDest, &OrigNormalDest->front(), &DT);

      Invoke->setNormalDest(MergeBB);
    } else {
      DefaultBB = SplitBlock(BB, CS, &DT);
      BasicBlock::iterator It(*CS);
      ++It;
      assert(It != DefaultBB->end());
      MergeBB = SplitBlock(DefaultBB, &(*It), &DT);
    }

    // While this pass invalidates BFI, there could be multiple call instructions that trigger specialization within the same block. As we depend on BFI to calculate switch weights for each specialization at a call instruction this information should be kept up to date.
    BFI.setBlockFreq(MergeBB, OrigBBFreq);

    MergeBB->setName("CS.Merge");
    DefaultBB->setName("CS.Default");

    DomTreeUpdater DTU(&DT, DomTreeUpdater::UpdateStrategy::Eager);

    auto ArgNo = SpecsInfo.Specs[0].first->Arg.Formal->getArgNo();
    Value *ArgVar = CS->getArgOperand(ArgNo);

    // Bitcast float/double to integer for switch
    Type *ArgType = ArgVar->getType();

    auto *Term = BB->getTerminator();
    IRBuilder<> IRB(Term);

    Type *IntType = nullptr;
    if (ArgType->isFloatTy() || ArgType->isDoubleTy()) {
      IntType =
          ArgType->isFloatTy() ? Type::getInt32Ty(Ctx) : Type::getInt64Ty(Ctx);
      ArgVar = IRB.CreateBitCast(ArgVar, IntType);
    }

    SwitchInst *SI = IRB.CreateSwitch(ArgVar, DefaultBB, SpecsInfo.Specs.size());

    SmallVector<uint64_t, 16> CaseCounts;
    uint64_t DefaultCount = (TotalCount > SpecsInfo.TotalCount) ? TotalCount - SpecsInfo.TotalCount : 0;
    uint64_t MaxCount = DefaultCount;
    CaseCounts.push_back(DefaultCount);

    auto OrigFuncEntryCount = Func->getEntryCount();
    Func->setEntryCount(OrigFuncEntryCount->getCount() > SpecsInfo.TotalCount ?
                        (OrigFuncEntryCount->getCount() - SpecsInfo.TotalCount) : 0, OrigFuncEntryCount->getType());

    Term->eraseFromParent();

    Type *CSTy = CS->getType();
    PHINode *PHI = nullptr;
    if (!CSTy->isVoidTy()) {
      // Insert a phi for the return values at the merge block.
      IRBuilder<> IRBM(MergeBB, MergeBB->getFirstNonPHIIt());
      PHI = IRBM.CreatePHI(CSTy, SpecsInfo.Specs.size() + 1, "CS.RVMerge");
      CS->replaceAllUsesWith(PHI);
      PHI->addIncoming(CS, DefaultBB);
    }

    dbgs() << "\n\n== Basic Block After==\n";

    std::vector<DominatorTree::UpdateType> Updates;
    Updates.reserve(2 * SpecsInfo.Specs.size());

    unsigned CaseIdx = 0;
    for (auto &[SpecPtr, Count] : SpecsInfo.Specs) {
      Constant *SpecArgVal = SpecPtr->Arg.Actual;
      BasicBlock *CaseBB = BasicBlock::Create(
          Ctx, Twine("CS.Case.") + Twine(CaseIdx++), Func, DefaultBB);
      CallBase *NewCB = cast<CallBase>(CS->clone());

      Function *FuncToCall = SpecPtr->getSpecializedFunc();
      NewCB->setCalledFunction(FuncToCall);
      NewCB->setArgOperand(ArgNo, SpecArgVal);
      NewCB->insertInto(CaseBB, CaseBB->end());

      if (auto *Invoke = dyn_cast<InvokeInst>(NewCB)) {
        Invoke->setNormalDest(MergeBB);
      } else {
        IRBuilder<> IRBCase(CaseBB);
        IRBCase.CreateBr(MergeBB);
      }

      // Convert constant to ConstantInt for switch case
      Constant *CaseVal = SpecArgVal;
      if (IntType) {
        CaseVal = ConstantInt::get(IntType, cast<ConstantFP>(CaseVal)
                                                ->getValue()
                                                .bitcastToAPInt()
                                                .getZExtValue());
      }
      SI->addCase(cast<ConstantInt>(CaseVal), CaseBB);
      if (!CSTy->isVoidTy())
        PHI->addIncoming(NewCB, CaseBB);

      Updates.push_back({DominatorTree::Insert, CaseBB, MergeBB});
      Updates.push_back({DominatorTree::Insert, BB, CaseBB});

      MaxCount = std::max(MaxCount, Count);
      CaseCounts.push_back(Count);

      dbgs() << *CaseBB << "\n";
    }

    DTU.applyUpdates(Updates);

    // Will be used to regenerate BFI appropriately, safe as this pass invalidates it
    setProfMetadata(Func->getParent(), SI, CaseCounts, MaxCount);
  }

  // Clean up analysis clones that were only used for cost analysis
  for (auto &[CS, SpecsInfo] : CallSiteSpecs) {
    for (auto &[SpecPtr, Count] : SpecsInfo.Specs) {
      // If we reused an existing function, delete the analysis clone
      if (SpecPtr->ExistingFunc && SpecPtr->Clone) {
        dbgs() << "PGOFnSpecialization: Deleting analysis clone "
               << SpecPtr->Clone->getName() << "\n";
        FAM->clear(*SpecPtr->Clone, SpecPtr->Clone->getName());
        SpecPtr->Clone->eraseFromParent();
        SpecPtr->Clone = nullptr;
      }
    }
  }

  return !CallSiteSpecs.empty();
}

static Constant *synthesizeConstant(Type *T, uint64_t V) {
  dbgs() << *T << '\n';
  if (T->isIntegerTy()) {
    return llvm::ConstantInt::get(T, V);
  } else if (T->isFloatTy()) {
    float F = llvm::bit_cast<float>(static_cast<uint32_t>(V));
    return llvm::ConstantFP::get(T->getContext(), llvm::APFloat(F));
  } else if (T->isDoubleTy()) {
    double D = llvm::bit_cast<double>(V);
    return llvm::ConstantFP::get(T->getContext(), llvm::APFloat(D));
  } else {
    return nullptr;
  }
}

static void tagBlocks(Function *F, FunctionCallee &BlockTag) {
  auto &Ctx = F->getContext();
  unsigned ID = 0;
  for (auto &BB : *F) {
    IRBuilder<> B(&*BB.getFirstInsertionPt());
    CallInst *CI =
        B.CreateCall(BlockTag, {ConstantInt::get(Type::getInt32Ty(Ctx), ID++)});
  }
}

static std::map<unsigned, uint64_t>
getTaggedBlocks(std::function<BlockFrequencyInfo &(Function &)> &GetBFI,
                Function *F, FunctionCallee &BlockTag) {
  auto &BFI = GetBFI(*F);

  uint64_t EntryFreq = BFI.getEntryFreq().getFrequency();
  if (EntryFreq == 0)
    EntryFreq = 1;
  std::map<unsigned, uint64_t> Res;
  for (auto &BB : *F) {
    uint64_t BBFreq = BFI.getBlockFreq(&BB).getFrequency();
    uint64_t Weight = (BBFreq * 100) / EntryFreq;
    if (Weight == 0 && BBFreq > 0)
      Weight = 1;
    for (auto &I : BB) {
      if (auto *C = dyn_cast<CallInst>(&I);
          C && C->getCalledFunction() == BlockTag.getCallee()) {
        unsigned ID = cast<ConstantInt>(C->getArgOperand(0))->getZExtValue();
        Res[ID] += Weight;
      }
    }
  }

  return Res;
}

static void untagBlocks(FunctionCallee &BlockTag) {
  // Avoid invalidation during iteration by taking a copy of users
  std::vector<User *> Users;
  for (User *U : BlockTag.getCallee()->users()) {
    Users.push_back(U);
  }

  for (User *U : Users) {
    if (Instruction *I = dyn_cast<Instruction>(U))
      I->eraseFromParent();
  }
}

bool PGOFunctionSpecializer::findSpecializations(
    Function *F, unsigned FuncSize, SmallVectorImpl<PGOSpec> &AllSpecs,
    FunctionCallee &BlockTag) {
  auto [OriginalCodeSize, _] = calculateFunctionSizeLatency({}, F, BlockTag);

  SmallVector<std::pair<Argument *, uint32_t>> Args;
  uint32_t VPArgIdx = 0;
  for (Argument &Arg : F->args()) {
    // Must be kept in sync with the argument value profiling plugin.
    Type *T = Arg.getType();

    if (T->isIntegerTy() || T->isFloatTy() || T->isDoubleTy())
      Args.push_back({&Arg, VPArgIdx++});
  }

  if (Args.empty()) {
    return false;
  }

  uint64_t BestArgSpecsScore = 0;
  DenseMap<Constant *, PGOSpec> BestArgSpecs;
  tagBlocks(F, BlockTag);
  auto PredictableThreshBranchProb = GetTTI(*F).getPredictableBranchThreshold();

  for (auto [A, VPIdx] : Args) {
    DenseMap<Constant *, PGOSpec> ArgSpecsMap;

    for (User *U : F->users()) {
      if (!isa<CallInst>(U) && !isa<InvokeInst>(U)) {
        continue;
      }

      auto &CS = *cast<CallBase>(U);

      if (CS.getCalledFunction() != F)
        continue;

      uint64_t TotalCount;
      auto ValueProfData = getValueProfDataFromInst(CS, IPVK_ArgumentValue, 5,
                                                 TotalCount, false, VPIdx);
      auto &BFI = GetBFI(*F);
      auto BBEdgeCount = BFI.getBlockProfileCount(CS.getParent());
      dbgs() << "TotalCount " << TotalCount << " , BBEdgeCount "
             << (BBEdgeCount ? *BBEdgeCount : 0) << '\n';
      if (BBEdgeCount) {
        // Use block profile count as the total if available, it is more
        // accurate as value profile counts will miss rare value counts.
        TotalCount = std::max(TotalCount, *BBEdgeCount);
      }
      if (TotalCount < HotFuncThresh)
        continue;

      for (const auto &ProfiledValue : ValueProfData) {
        // Drop values that have a percentage of calls below the configured
        // cutoff.
        unsigned ValProp = (ProfiledValue.Count * 100) / TotalCount;
        dbgs() << "PGOFnSpecialization: Value " << ProfiledValue.Value
               << " , ValProp " << ValProp << '\n';

        if (ValProp < AnalysisCutoffThresh)
          continue;

        Constant *C = synthesizeConstant(A->getType(), ProfiledValue.Value);
        if (!C) {
          continue;
        }

        BranchProbability ValBranchProb(ValProp, 100);

        auto checkWeightedSpecializedLatency =
            [&](unsigned OriginalLatency, unsigned SpecializedLatency) {
              if (OriginalLatency == 0)
                return false;

              // Account for dispatch cost at call site: all calls pay dispatch
              // overhead Calculate in absolute latency units to avoid rounding
              // to zero for large functions WeightedLatency =
              // (SpecializedLatency * ValProp + OriginalLatency * (100-ValProp)
              // + DispatchCost * 100) / OriginalLatency
              uint64_t WeightedLatencyAbs =
                  static_cast<uint64_t>(SpecializedLatency) * ValProp +
                  static_cast<uint64_t>(OriginalLatency) * (100 - ValProp);

              if (ValBranchProb < PredictableThreshBranchProb)
                WeightedLatencyAbs += static_cast<uint64_t>(DispatchCost) * 100;
              else
                WeightedLatencyAbs += 100;

              unsigned WeightedLatency = WeightedLatencyAbs / OriginalLatency;
              dbgs() << "PGOFnSpecialization: WeightedLatency="
                     << WeightedLatency << '\n';

              return WeightedLatency < CandidateCutoffThresh;
            };

        if (auto It = ArgSpecsMap.find(C); It != ArgSpecsMap.end()) {
          if (!checkWeightedSpecializedLatency(It->second.OriginalLatency,
                                               It->second.SpecializedLatency))
            continue;

          It->second.CallSites.push_back({&CS, ProfiledValue.Count});
          It->second.Count += ProfiledValue.Count;
          It->second.MinValueProp = std::min(It->second.MinValueProp, ValProp);
        } else {
          auto [ClonedF, ExistingFunc] =
              cloneFunctionSpecialized(F, A, C, ProfiledValue.Value);

          auto [SpecializedCodeSize, SpecializedLatency] =
              calculateFunctionSizeLatency({}, ClonedF, BlockTag);
          auto [_, OriginalLatency] = calculateFunctionSizeLatency(
              getTaggedBlocks(GetBFI, ClonedF, BlockTag), F, BlockTag);

          dbgs() << "PGOFnSpecialization: Original CodeSize="
                 << OriginalCodeSize << " Latency=" << OriginalLatency
                 << " Specialized CodeSize=" << SpecializedCodeSize
                 << " Latency=" << SpecializedLatency << "\n";


          if (SpecializedCodeSize < MaxSpecInlineSize) {
            ClonedF->addFnAttr(Attribute::AlwaysInline);
          }

          if (!checkWeightedSpecializedLatency(OriginalLatency,
                                               SpecializedLatency) ||
              ((OriginalLatency - SpecializedLatency) * BlowupFactor <
               SpecializedCodeSize) ||
              SpecializedCodeSize >= MaxSpecSize ||
              OriginalLatency < MinLatencyThresh) {
            FAM->clear(*ClonedF, ClonedF->getName());
            ClonedF->eraseFromParent();
            continue;
          }

          auto ASIt =
              ArgSpecsMap.try_emplace(C, F,
                                                      ClonedF,
              ExistingFunc,
                                                      ArgInfo{A, C},
                                                      OriginalCodeSize,
                                                      OriginalLatency,
                                                      SpecializedCodeSize,
                                                      SpecializedLatency,
                                                      ProfiledValue.Count,
                                          ValProp);
          ASIt.first->second.CallSites.push_back({&CS, ProfiledValue.Count});
        }
      }
    }

    uint64_t Score =
        std::accumulate(ArgSpecsMap.begin(), ArgSpecsMap.end(), 0,
                        [](uint64_t Acc, const auto &KV) {
                          return Acc + (KV.second.OriginalLatency -
                                        KV.second.SpecializedLatency) *
                                           KV.second.Count;
                        });

    dbgs() << "PGOFnSpecialization: Argument=" << A->getArgNo()
           << " , Score=" << Score << "\n";

    if (Score > BestArgSpecsScore) {
      for (auto &[Const, Spec] : BestArgSpecs) {
        if (Spec.Clone) {
          FAM->clear(*Spec.Clone, Spec.Clone->getName());
          Spec.Clone->eraseFromParent();
        }
        // Note: ExistingFunc is not owned by us, so we don't delete it
      }

      BestArgSpecs = std::move(ArgSpecsMap);
      BestArgSpecsScore = Score;
    } else {
      for (auto &[Const, Spec] : ArgSpecsMap) {
        if (Spec.Clone) {
          FAM->clear(*Spec.Clone, Spec.Clone->getName());
          Spec.Clone->eraseFromParent();
        }
        // Note: ExistingFunc is not owned by us, so we don't delete it
      }
    }
  }

  untagBlocks(BlockTag);

  AllSpecs.reserve(AllSpecs.size() + BestArgSpecs.size());
  std::transform(BestArgSpecs.begin(), BestArgSpecs.end(),
                 std::back_inserter(AllSpecs),
                 [](const auto &kv) { return kv.second; });

  return !BestArgSpecs.empty();
}

bool PGOFunctionSpecializer::isCandidateFunction(Function *F) {
  if (F->isDeclaration() || F->arg_empty())
    return false;

  if (F->hasFnAttribute(Attribute::NoDuplicate))
    return false;

  if (shouldOptimizeForSize(F, nullptr, nullptr, PGSOQueryType::IRPass))
    return false;

  if (F->hasFnAttribute(Attribute::AlwaysInline))
    return false;

  if (F->hasFnAttribute("pgo.specialization"))
    return false;

  dbgs() << "PGOFnSpecialization: Try function: " << F->getName() << "\n";
  return true;
}

PreservedAnalyses
PGOFunctionSpecializationPass::run(Module &M, ModuleAnalysisManager &AM) {
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  auto GetTLI = [&FAM](Function &F) -> const TargetLibraryInfo & {
    return FAM.getResult<TargetLibraryAnalysis>(F);
  };
  auto GetTTI = [&FAM](Function &F) -> TargetTransformInfo & {
    return FAM.getResult<TargetIRAnalysis>(F);
  };
  auto GetAC = [&FAM](Function &F) -> AssumptionCache & {
    return FAM.getResult<AssumptionAnalysis>(F);
  };
  auto GetDT = [&FAM](Function &F) -> DominatorTree & {
    return FAM.getResult<DominatorTreeAnalysis>(F);
  };
  auto GetBFI = [&FAM](Function &F) -> BlockFrequencyInfo & {
    return FAM.getResult<BlockFrequencyAnalysis>(F);
  };

  PGOFunctionSpecializer Specializer(M, &FAM, GetBFI, GetTLI, GetTTI, GetAC,
                                     GetDT, OptLevel);

  bool Changed = Specializer.run();

  if (!Changed)
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserve<FunctionAnalysisManagerModuleProxy>();
  return PA;
}
