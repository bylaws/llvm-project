//===- PGOVTableFunctionSpecialization.h - VTable Function Specialization --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_PGOVTABLEFUNCTIONSPECIALIZATION_H
#define LLVM_TRANSFORMS_IPO_PGOVTABLEFUNCTIONSPECIALIZATION_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class PGOVTableFunctionSpecializationPass
    : public PassInfoMixin<PGOVTableFunctionSpecializationPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_IPO_PGOVTABLEFUNCTIONSPECIALIZATION_H
