//===--- StageReachabilityVisitor.h - Validate emitted shader stages ------===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_SPIRV_STAGEREACHABILITYVISITOR_H
#define LLVM_CLANG_LIB_SPIRV_STAGEREACHABILITYVISITOR_H

#include "clang/Basic/SourceLocation.h"
#include "clang/SPIRV/SpirvVisitor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <string>
#include <utility>

namespace clang {

class ASTContext;

namespace spirv {

class SpirvFunction;

// Validates fragment-only instructions using the lowered module's real call
// graph. Entry wrappers, implicit module initialization, and exported
// functions are roots; this must not depend on HLSL AST call syntax or the
// order in which entry functions were translated.
//
// AMD input-register ownership is a different, entry-specific question and
// intentionally does not belong in this analysis.
class StageReachabilityVisitor : public Visitor {
public:
  StageReachabilityVisitor(ASTContext &astCtx, SpirvContext &spvCtx,
                           const SpirvCodeGenOptions &opts)
      : Visitor(opts, spvCtx), astContext(astCtx) {}

  // The same emitted call graph identifies whether a function has only
  // Fragment entry roots. Linkage exports are unknown-stage roots and therefore
  // cannot acquire an entry-specific callable representation.
  bool isFragmentOnly(SpirvFunction *function) const {
    return reachableStages.lookup(function) == Fragment;
  }

  // A missing stage mask means an emitted function has no reachable
  // shader root, not that its record types are safe to specialize.
  llvm::SmallVector<SpirvFunction *, 8> getNonFragmentFunctions() const {
    llvm::SmallVector<SpirvFunction *, 8> result;
    for (auto *function : emittedFunctions)
      if (!isFragmentOnly(function))
        result.push_back(function);
    return result;
  }

  // AST record types used by emitted instructions must also be protected:
  // neither export parameters nor its local variable declarations necessarily
  // capture the types of temporary composite and resource-load values.
  llvm::ArrayRef<SpirvInstruction *>
  getRecordTypeUses(SpirvFunction *function) const {
    const auto found = recordTypeUses.find(function);
    if (found == recordTypeUses.end())
      return {};
    return found->second;
  }

  // Lowerings that become generic SPIR-V instructions can retain their
  // source-level stage constraint without relying on AST caller inference.
  void requireFragmentStage(SpirvInstruction *instruction, SourceLocation loc,
                            llvm::StringRef diagnostic);

  using Visitor::visit;
  bool visitInstruction(SpirvInstruction *) override;
  bool visit(SpirvModule *, Phase phase) override;
  bool visit(SpirvFunction *, Phase phase) override;
  bool visit(SpirvEntryPoint *) override;
  bool visit(SpirvDecoration *) override;
  bool visit(SpirvFunctionCall *) override;
  bool visit(SpirvKill *) override;
  bool visit(SpirvDemoteToHelperInvocation *) override;

private:
  enum StageMask : unsigned { Fragment = 1, Other = 2 };

  void recordDiscard(SourceLocation loc);

  ASTContext &astContext;
  SpirvFunction *currentFunction = nullptr;
  bool collectRecordTypes = false;
  llvm::DenseMap<SpirvFunction *, llvm::SmallVector<SpirvFunction *, 4>>
      callGraph;
  llvm::SmallVector<std::pair<SpirvFunction *, unsigned>, 8> roots;
  llvm::DenseMap<SpirvFunction *, unsigned> reachableStages;
  llvm::SmallVector<SpirvFunction *, 16> emittedFunctions;
  llvm::DenseMap<SpirvFunction *, llvm::SmallVector<SpirvInstruction *, 8>>
      recordTypeUses;
  llvm::SmallVector<std::pair<SpirvFunction *, SourceLocation>, 8> discards;
  struct FragmentStageUse {
    SpirvInstruction *instruction;
    SourceLocation loc;
    std::string diagnostic;
  };
  llvm::SmallVector<FragmentStageUse, 8> fragmentStageUses;
  // The marker map is populated while the SPIR-V visitor is visiting the
  // instruction itself, so synthetic module.init uses get their real owner.
  // Null indicates the marked instruction did not survive CFG traversal.
  llvm::DenseMap<SpirvInstruction *, SpirvFunction *> liveStageUses;
};

} // namespace spirv
} // namespace clang

#endif // LLVM_CLANG_LIB_SPIRV_STAGEREACHABILITYVISITOR_H
