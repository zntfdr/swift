//==-------- DiagnoseLifetimeIssues.cpp - Diagnose lifetime issues ---------==//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements a diagnostic pass that prints a warning if an object is
// stored to a weak property (or is weakly captured) and destroyed before the
// property (or captured reference) is ever used again.
// This can happen if the programmer relies on the lexical scope to keep an
// object alive, but copy-propagation can shrink the object's lifetime to its
// last use.
// For example:
//
// func test() {
//   let k = Klass()
//   // k is deallocated immediatly after the closure capture (a store_weak).
//   functionWithClosure({ [weak k] in
//                         // crash!
//                         k!.foo()
//                       })
// }
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "diagnose-lifetime-issues"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/PrunedLiveness.h"
#include "swift/Demangling/Demangler.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Debug.h"
#include "clang/AST/DeclObjC.h"

using namespace swift;

namespace {

/// Performs the analysis and prints the warnings.
class DiagnoseLifetimeIssues {
  enum State {
    /// There are no hidden uses which could keep the object alive.
    DoesNotEscape,
    
    /// For example, in case the object is stored somewhere.
    CanEscape,
    
    /// The object is stored to a weak reference field.
    /// Implies ``DoesNotEscape``.
    IsStoredWeakly
  };

  /// To avoid quadratic complexity in the rare corener case of very deep
  /// callgraphs, with pass down references.
  static constexpr int maxCallDepth = 8;

  /// The liveness of the object in question, computed in visitUses.
  PrunedLiveness liveness;

  /// All weak stores of the object, which are found in visitUses.
  llvm::SmallVector<SILInstruction *, 8> weakStores;

  /// A cache for function argument states of called functions.
  ///
  /// We could also cache this information in an Analysis, so that it persists
  /// over runs of this pass for different functions. But computing the state
  /// is very cheap and we avoid worst case scenarios with maxCallDepth. So it's
  /// propably not worth doing it.
  llvm::DenseMap<SILFunctionArgument *, State> argumentStates;

  State visitUses(SILValue def, bool updateLivenessAndWeakStores, int callDepth);

  State getArgumentState(ApplySite ai, Operand *applyOperand, int callDepth);

  void reportDeadStore(SILInstruction *allocationInst);

public:
  DiagnoseLifetimeIssues() {}

  void diagnose(SILFunction *function);
};

/// Returns true if def is an owned value resulting from an object allocation.
static bool isAllocation(SILInstruction *inst) {
  auto *svi = dyn_cast<SingleValueInstruction>(inst);
  if (!svi)
    return false;

  if (svi->getOwnershipKind() != OwnershipKind::Owned)
    return false;

  if (isa<AllocRefInst>(svi))
    return true;
  
  // Check if it's a call to an allocating initializer.
  if (auto *applyInst = dyn_cast<ApplyInst>(svi)) {
    SILFunction *callee = applyInst->getReferencedFunctionOrNull();
    if (!callee)
      return false;
    
    Demangle::StackAllocatedDemangler<1024> demangler;
    Demangle::Node *root = demangler.demangleSymbol(callee->getName());
    return root && root->getKind() == Demangle::Node::Kind::Global &&
           root->getFirstChild()->getKind() == Demangle::Node::Kind::Allocator;
  }
    
  return false;
}

/// Returns true if \p inst is a call of an ObjC setter to a weak property.
static bool isStoreObjcWeak(SILInstruction *inst, Operand *op) {
  auto *apply = dyn_cast<ApplyInst>(inst);
  if (!apply || apply->getNumArguments() < 1)
    return false;
  
  if (&apply->getArgumentOperands()[0] != op)
    return false;
  
  auto *method = dyn_cast<ObjCMethodInst>(apply->getCallee());
  if (!method)
    return false;
  
  Decl *decl = method->getMember().getDecl();
  auto *accessor = dyn_cast<AccessorDecl>(decl);
  if (!accessor)
    return false;

  auto *var = dyn_cast<VarDecl>(accessor->getStorage());
  if (!var)
    return false;

  ClangNode clangNode = var->getClangNode();
  if (!clangNode)
    return false;
  
  auto *objcDecl = dyn_cast_or_null<clang::ObjCPropertyDecl>(clangNode.getAsDecl());
  if (!objcDecl)
    return false;

  return objcDecl->getSetterKind() == clang::ObjCPropertyDecl::Weak;
}

/// Transitively iterates over all uses of \p def and and - if \p
/// updateLivenessAndWeakStores is true - adds them to self.liveness.
/// If any weak stores are seen, add them to self.weakStores (also only if
/// \p updateLivenessAndWeakStores is true).
///
/// Returns the state of \p def. See DiagnoseLifetimeIssues::State.
DiagnoseLifetimeIssues::State DiagnoseLifetimeIssues::
visitUses(SILValue def, bool updateLivenessAndWeakStores, int callDepth) {
  SmallSetVector<SILValue, 32> defUseWorklist;
  defUseWorklist.insert(def);
  bool foundWeakStore = false;
  while (!defUseWorklist.empty()) {
    SILValue value = defUseWorklist.pop_back_val();
    for (Operand *use : value->getUses()) {
      auto *user = use->getUser();

      // Recurse through copies and enums. Enums are important because the
      // operand of a store_weak is always an Optional.
      if (isa<CopyValueInst>(user) || isa<EnumInst>(user) ||
          isa<InitExistentialRefInst>(user)) {
        defUseWorklist.insert(cast<SingleValueInstruction>(user));
        continue;
      }
      if (isa<StoreWeakInst>(user) || isStoreObjcWeak(user, use)) {
        if (updateLivenessAndWeakStores)
          weakStores.push_back(user);
        foundWeakStore = true;
        continue;
      }
      if (ApplySite ai = ApplySite::isa(user)) {
        // Try to get information from the called function.
        switch (getArgumentState(ai, use, callDepth)) {
        case DoesNotEscape:
          break;
        case CanEscape:
          return CanEscape;
        case IsStoredWeakly:
          if (updateLivenessAndWeakStores)
            weakStores.push_back(user);
          foundWeakStore = true;
        }
        continue;
      }
      switch (use->getOperandOwnership()) {
      case OperandOwnership::NonUse:
        break;
      case OperandOwnership::TrivialUse:
        llvm_unreachable("this operand cannot handle ownership");

      // Conservatively treat a conversion to an unowned value as a pointer
      // escape. Is it legal to canonicalize ForwardingUnowned?
      case OperandOwnership::ForwardingUnowned:
      case OperandOwnership::PointerEscape:
        return CanEscape;
      case OperandOwnership::InstantaneousUse:
      case OperandOwnership::UnownedInstantaneousUse:
      case OperandOwnership::BitwiseEscape:
        if (updateLivenessAndWeakStores)
          liveness.updateForUse(user, /*lifetimeEnding*/ false);
        break;
      case OperandOwnership::ForwardingConsume:
        // TODO: handle forwarding instructions, e.g. casts.
        return CanEscape;
      case OperandOwnership::DestroyingConsume:
        // destroy_value does not force pruned liveness (but store etc. does).
        if (!isa<DestroyValueInst>(user))
          return CanEscape;
        break;
      case OperandOwnership::Borrow:
        if (updateLivenessAndWeakStores &&
            !liveness.updateForBorrowingOperand(use))
          return CanEscape;
        break;
      case OperandOwnership::InteriorPointer:
      case OperandOwnership::ForwardingBorrow:
      case OperandOwnership::EndBorrow:
      case OperandOwnership::Reborrow:
        return CanEscape;
      }
    }
  }
  return foundWeakStore ? IsStoredWeakly : DoesNotEscape;
}

/// Visits uses of an apply argument in the called function.
DiagnoseLifetimeIssues::State DiagnoseLifetimeIssues::
getArgumentState(ApplySite ai, Operand *applyOperand, int callDepth) {
  if (callDepth >= maxCallDepth)
    return CanEscape;

  if (!FullApplySite::isa(ai.getInstruction()))
    return CanEscape;
  
  SILFunction *callee = ai.getReferencedFunctionOrNull();
  if (!callee || callee->empty())
    return CanEscape;
  
  if (!ai.isArgumentOperand(*applyOperand))
    return CanEscape;

  SILBasicBlock *entryBlock = callee->getEntryBlock();
  unsigned calleeIdx = ai.getCalleeArgIndex(*applyOperand);
  auto *arg = cast<SILFunctionArgument>(entryBlock->getArgument(calleeIdx));

  // Check if we already cached the analysis result.
  auto iter = argumentStates.find(arg);
  if (iter != argumentStates.end())
    return iter->second;
  
  // Before we continue with the recursion, already set a (conservative) state.
  // This avoids infinite recursion in case of a cycle in the callgraph.
  argumentStates[arg] = CanEscape;

  State argState = visitUses(arg, /*updateLivenessAndWeakStores*/ false,
                             callDepth + 1);
  argumentStates[arg] = argState;
  return argState;
}

/// Returns true if \p inst is outside the the pruned \p liveness.
static bool isOutOfLifetime(SILInstruction *inst, PrunedLiveness &liveness) {
  // Check if the lifetime of the stored object ends at the store_weak.
  //
  // A more sophisticated analysis would be to check if there are no
  // (potential) loads from the store's destination address after the store,
  // but within the object's liferange. But without a good alias analysis (and
  // we don't want to use AliasAnalysis in a mandatory pass) it's practially
  // impossible that a use of the object is not a potential load. So we would
  // always see a potential load if the lifetime of the object goes beyond the
  // store_weak.

  SILBasicBlock *block = inst->getParent();
  if (liveness.getBlockLiveness(block) != PrunedLiveBlocks::LiveWithin)
    return false;

  // If there are any uses after the store_weak, it means that the liferange of
  // the object goes beyond the store_weak.
  for (SILInstruction &inst : make_range(std::next(inst->getIterator()),
                                         block->end())) {
    switch (liveness.isInterestingUser(&inst)) {
    case PrunedLiveness::NonUser:
      break;
    case PrunedLiveness::NonLifetimeEndingUse:
    case PrunedLiveness::LifetimeEndingUse:
      return false;
    }
  }
  return true;
}

/// Reports a warning if the stored object \p storedObj is never loaded within
/// the lifetime of the stored object.
void DiagnoseLifetimeIssues::reportDeadStore(SILInstruction *allocationInst) {
  liveness.clear();
  weakStores.clear();

  SILValue storedDef = cast<SingleValueInstruction>(allocationInst);
  liveness.initializeDefBlock(storedDef->getParentBlock());

  // Compute the canoncial lifetime of storedDef, like the copy-propagation pass
  // would do.
  State state = visitUses(storedDef, /*updateLivenessAndWeakStores*/ true,
                          /*callDepth*/ 0);

  // If the allocation escapes (e.g. it is stored somewhere), we should not
  // give a warning, becuase it can be a false alarm. The allocation could be
  // kept alive by references we don't see.
  if (state == CanEscape)
    return;

  assert((state == IsStoredWeakly) == !weakStores.empty());

  for (SILInstruction *storeInst : weakStores) {
    if (isOutOfLifetime(storeInst, liveness)) {
      // Issue the warning.
      storeInst->getModule().getASTContext().Diags.diagnose(
        storeInst->getLoc().getSourceLoc(), diag::warn_dead_weak_store);
    }
  }
}

/// Prints warnings for dead weak stores in \p function.
void DiagnoseLifetimeIssues::diagnose(SILFunction *function) {
  for (SILBasicBlock &block : *function) {
    for (SILInstruction &inst : block) {
      // Only for allocations we know that a destroy will actually deallocate
      // the object. Otherwise the object could be kept alive by other
      // references and we would issue a false alarm.
      if (isAllocation(&inst))
        reportDeadStore(&inst);
    }
  }
}

//===----------------------------------------------------------------------===//
//                            The function pass
//===----------------------------------------------------------------------===//

class DiagnoseLifetimeIssuesPass : public SILFunctionTransform {
public:
  DiagnoseLifetimeIssuesPass() {}

private:
  void run() override {
    SILFunction *function = getFunction();
    // Don't rerun diagnostics on deserialized functions.
    if (function->wasDeserializedCanonical())
      return;

    if (!function->hasOwnership())
      return;

    DiagnoseLifetimeIssues diagnoser;
    diagnoser.diagnose(function);
  }
};

} // end anonymous namespace

SILTransform *swift::createDiagnoseLifetimeIssues() {
  return new DiagnoseLifetimeIssuesPass();
}
