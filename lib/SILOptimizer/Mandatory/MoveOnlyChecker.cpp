//===--- MoveOnlyChecker.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-move-only-checker"

#include "swift/AST/AccessScope.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FrozenMultiMap.h"
#include "swift/Basic/SmallBitVector.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/Consumption.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/PrunedLiveness.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILArgumentConvention.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/NonLocalAccessBlockAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CanonicalizeOSSALifetime.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

#include "MoveOnlyAddressCheckerUtils.h"
#include "MoveOnlyDiagnostics.h"
#include "MoveOnlyObjectCheckerUtils.h"
#include "MoveOnlyUtils.h"

using namespace swift;
using namespace swift::siloptimizer;

//===----------------------------------------------------------------------===//
//                     MARK: Top Level Object Entrypoint
//===----------------------------------------------------------------------===//

namespace {

struct MoveOnlyChecker {
  DiagnosticEmitter diagnosticEmitter;
  SILFunction *fn;
  DominanceInfo *domTree;
  PostOrderAnalysis *poa;
  bool madeChange = false;
  borrowtodestructure::IntervalMapAllocator allocator;

  MoveOnlyChecker(SILFunction *fn, DominanceInfo *domTree,
                  PostOrderAnalysis *poa)
      : diagnosticEmitter(fn), fn(fn), domTree(domTree), poa(poa) {
  }

  void checkObjects();
  void checkAddresses();
};

} // namespace

void MoveOnlyChecker::checkObjects() {
  SmallSetVector<MarkUnresolvedNonCopyableValueInst *, 32>
      moveIntroducersToProcess;
  unsigned diagCount = diagnosticEmitter.getDiagnosticCount();
  madeChange |= searchForCandidateObjectMarkUnresolvedNonCopyableValueInsts(
      fn, moveIntroducersToProcess, diagnosticEmitter);

  LLVM_DEBUG(
      llvm::dbgs()
      << "Emitting diagnostic when checking for mark must check inst: "
      << (diagCount != diagnosticEmitter.getDiagnosticCount() ? "yes" : "no")
      << '\n');

  if (moveIntroducersToProcess.empty()) {
    LLVM_DEBUG(llvm::dbgs()
               << "No move introducers found?! Returning early?!\n");
    return;
  }

  MoveOnlyObjectChecker checker{diagnosticEmitter, domTree, poa, allocator};
  madeChange |= checker.check(moveIntroducersToProcess);
}

void MoveOnlyChecker::checkAddresses() {
  unsigned diagCount = diagnosticEmitter.getDiagnosticCount();
  SmallSetVector<MarkUnresolvedNonCopyableValueInst *, 32>
      moveIntroducersToProcess;
  searchForCandidateAddressMarkUnresolvedNonCopyableValueInsts(
      fn, moveIntroducersToProcess, diagnosticEmitter);

  LLVM_DEBUG(
      llvm::dbgs()
      << "Emitting diagnostic when checking for mark must check inst: "
      << (diagCount != diagnosticEmitter.getDiagnosticCount() ? "yes" : "no")
      << '\n');

  if (moveIntroducersToProcess.empty()) {
    LLVM_DEBUG(llvm::dbgs()
               << "No move introducers found?! Returning early?!\n");
    return;
  }

  MoveOnlyAddressChecker checker{fn, diagnosticEmitter, allocator, domTree,
                                 poa};
  madeChange |= checker.check(moveIntroducersToProcess);
}

//===----------------------------------------------------------------------===//
//                         MARK: Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class MoveOnlyCheckerPass : public SILFunctionTransform {
  void run() override {
    auto *fn = getFunction();

    // Only run this pass if the move only language feature is enabled.
    if (!fn->getASTContext().supportsMoveOnlyTypes())
      return;

    // Don't rerun diagnostics on deserialized functions.
    if (getFunction()->wasDeserializedCanonical())
      return;

    assert(fn->getModule().getStage() == SILStage::Raw &&
           "Should only run on Raw SIL");

    // If an earlier pass asked us to eliminate the function body if it's
    // unused, and the function is in fact unused, do that now.
    if (fn->hasSemanticsAttr(semantics::MOVEONLY_DELETE_IF_UNUSED)) {
      if (fn->getRefCount() == 0
          && !isPossiblyUsedExternally(fn->getLinkage(),
                                       fn->getModule().isWholeModule())) {
        LLVM_DEBUG(llvm::dbgs() << "===> Deleting unused function " << fn->getName() << "'s body that was marked for deletion\n");
        // Remove all non-entry blocks.
        auto entryBB = fn->begin();
        auto nextBB = std::next(entryBB);
        
        while (nextBB != fn->end()) {
          auto thisBB = nextBB;
          ++nextBB;
          thisBB->eraseFromParent();
        }
        
        // Rewrite the entry block to only contain an unreachable.
        auto loc = entryBB->begin()->getLoc();
        entryBB->eraseAllInstructions(fn->getModule());
        {
          SILBuilder b(&*entryBB);
          b.createUnreachable(loc);
        }
        
        // If the function has shared linkage, reduce this version to private
        // linkage, because we don't want the deleted-body form to win in any
        // ODR shootouts.
        if (fn->getLinkage() == SILLinkage::Shared) {
          fn->setLinkage(SILLinkage::Private);
        }
        
        invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
        return;
      }
      // If the function wasn't unused, let it continue into diagnostics.
      // This would come up if a closure function somehow was used in different
      // functions with different escape analysis results. This shouldn't really
      // be possible, and we should try harder to make it impossible, but if
      // it does happen now, the least bad thing to do is to proceed with
      // move checking. This will either succeed and make sure the original
      // function contains valid SIL, or raise errors relating to the use
      // of the captures in an escaping way, which is the right thing to do
      // if they are in fact escapable.
      LLVM_DEBUG(llvm::dbgs() << "===> Function " << fn->getName()
                              << " was marked to be deleted, but has uses. Continuing with move checking\n");
      
    }

    // If an earlier pass told use to not emit diagnostics for this function,
    // clean up any copies, invalidate the analysis, and return early.
    if (fn->hasSemanticsAttr(semantics::NO_MOVEONLY_DIAGNOSTICS)) {
      if (cleanupNonCopyableCopiesAfterEmittingDiagnostic(getFunction()))
        invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
      return;
    }

    LLVM_DEBUG(llvm::dbgs()
               << "===> MoveOnly Checker. Visiting: " << fn->getName() << '\n');

    MoveOnlyChecker checker(
        fn, getAnalysis<DominanceAnalysis>()->get(fn),
        getAnalysis<PostOrderAnalysis>());

    checker.checkObjects();
    checker.checkAddresses();

    // If we did not emit any diagnostics, emit an error on any copies that
    // remain. If we emitted a diagnostic, we just want to rewrite all of the
    // non-copyable copies into explicit variants below and let the user
    // recompile.
    if (!checker.diagnosticEmitter.emittedDiagnostic()) {
      emitCheckerMissedCopyOfNonCopyableTypeErrors(fn,
                                                   checker.diagnosticEmitter);
    }

    checker.madeChange |=
        cleanupNonCopyableCopiesAfterEmittingDiagnostic(fn);

    if (checker.madeChange)
      invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
};

} // namespace

SILTransform *swift::createMoveOnlyChecker() {
  return new MoveOnlyCheckerPass();
}
