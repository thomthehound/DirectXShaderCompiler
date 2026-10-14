//===--- StageReachabilityVisitor.cpp - Validate emitted shader stages ----===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "StageReachabilityVisitor.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Type.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/SPIRV/SpirvFunction.h"
#include "clang/SPIRV/SpirvInstruction.h"
#include "clang/SPIRV/SpirvModule.h"

namespace clang {
namespace spirv {

namespace {

bool containsAstRecord(QualType type) {
  if (type.isNull())
    return false;
  if (type->isStructureType())
    return true;
  if (type->isPointerType())
    return containsAstRecord(type->getPointeeType());
  if (const auto *array = dyn_cast<clang::ArrayType>(type.getTypePtr()))
    return containsAstRecord(array->getElementType());
  return false;
}

} // namespace

void StageReachabilityVisitor::requireFragmentStage(
    SpirvInstruction *instruction, SourceLocation loc,
    llvm::StringRef diagnostic) {
  fragmentStageUses.push_back({instruction, loc, diagnostic.str()});
  liveStageUses[instruction] = nullptr;
}

bool StageReachabilityVisitor::visitInstruction(SpirvInstruction *instruction) {
  // SpirvFunction visits only CFG-reachable blocks. Resolve each marked
  // operation to the actual function traversed, not the builder's ambient
  // function (which can differ for synthetic module.init instructions).
  auto use = liveStageUses.find(instruction);
  if (use != liveStageUses.end())
    use->second = currentFunction;
  if (collectRecordTypes && currentFunction &&
      instruction->hasAstResultType() &&
      containsAstRecord(instruction->getAstResultType()))
    recordTypeUses[currentFunction].push_back(instruction);
  return true;
}

bool StageReachabilityVisitor::visit(SpirvEntryPoint *entry) {
  roots.emplace_back(entry->getEntryPoint(),
                     entry->getExecModel() == spv::ExecutionModel::Fragment
                         ? Fragment
                         : Other);
  return true;
}

bool StageReachabilityVisitor::visit(SpirvDecoration *decor) {
  // An HLSL export can be called by a shader of any stage after linking.
  // The linker-visible function must never inherit a pixel-only guarantee
  // merely because its only local caller happens to be a pixel entry point.
  if (decor->getDecoration() == spv::Decoration::LinkageAttributes &&
      decor->getTargetFunc() && !decor->getParams().empty() &&
      decor->getParams().back() ==
          static_cast<uint32_t>(spv::LinkageType::Export))
    roots.emplace_back(decor->getTargetFunc(), Other);
  return true;
}

bool StageReachabilityVisitor::visit(SpirvFunction *func, Phase phase) {
  currentFunction = phase == Phase::Init ? func : nullptr;
  return true;
}

bool StageReachabilityVisitor::visit(SpirvFunctionCall *call) {
  if (currentFunction)
    callGraph[currentFunction].push_back(call->getFunction());
  return true;
}

void StageReachabilityVisitor::recordDiscard(SourceLocation loc) {
  discards.emplace_back(currentFunction, loc);
}

bool StageReachabilityVisitor::visit(SpirvKill *kill) {
  recordDiscard(kill->getSourceLocation());
  return true;
}

bool StageReachabilityVisitor::visit(SpirvDemoteToHelperInvocation *demote) {
  recordDiscard(demote->getSourceLocation());
  return true;
}

bool StageReachabilityVisitor::visit(SpirvModule *, Phase phase) {
  if (phase != Phase::Done)
    return true;

  // Fixed-point propagation is necessary for shared helpers and recursion.
  // The call graph is recorded from actual OpFunctionCall instructions,
  // including wrapper-generated calls and the synthetic module initializer.
  reachableStages.clear();
  llvm::SmallVector<SpirvFunction *, 16> pending;
  auto mark = [&](SpirvFunction *fn, unsigned mask) {
    if (!fn)
      return;
    unsigned &existing = reachableStages[fn];
    if ((existing | mask) != existing) {
      existing |= mask;
      pending.push_back(fn);
    }
  };

  for (const auto &root : roots)
    mark(root.first, root.second);
  while (!pending.empty()) {
    SpirvFunction *caller = pending.pop_back_val();
    auto callees = callGraph.find(caller);
    if (callees == callGraph.end())
      continue;
    for (SpirvFunction *callee : callees->second)
      mark(callee, reachableStages.lookup(caller));
  }

  // Reject both OpKill and OpDemoteToHelperInvocation in any function whose
  // reachable execution stages are not exclusively Fragment. Report every
  // source location, preserving the existing diagnostics for regular HLSL.
  auto &diags = astContext.getDiagnostics();
  const unsigned errorId = diags.getCustomDiagID(
      DiagnosticsEngine::Error,
      "discard statement may only be used in pixel shaders");
  bool valid = true;
  for (const auto &discard : discards) {
    const unsigned mask = reachableStages.lookup(discard.first);
    // A helper may be emitted yet have no execution path from any entry
    // point. Do not classify an unreachable internal function as nonpixel.
    if (mask != 0 && mask != Fragment) {
      diags.Report(discard.second, errorId);
      valid = false;
    }
  }
  for (const auto &use : fragmentStageUses) {
    SpirvFunction *owner = liveStageUses.lookup(use.instruction);
    if (!owner)
      continue; // This operation was in a discarded basic block.
    const unsigned mask = reachableStages.lookup(owner);
    if (mask != 0 && mask != Fragment) {
      const unsigned id = diags.getCustomDiagID(DiagnosticsEngine::Error, "%0");
      diags.Report(use.loc, id) << use.diagnostic;
      valid = false;
    }
  }
  return valid;
}

} // namespace spirv
} // namespace clang
