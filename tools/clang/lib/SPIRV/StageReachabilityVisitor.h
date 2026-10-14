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

  using Visitor::visit;
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
  llvm::DenseMap<SpirvFunction *, llvm::SmallVector<SpirvFunction *, 4>>
      callGraph;
  llvm::SmallVector<std::pair<SpirvFunction *, unsigned>, 8> roots;
  llvm::SmallVector<std::pair<SpirvFunction *, SourceLocation>, 8> discards;
};

} // namespace spirv
} // namespace clang

#endif // LLVM_CLANG_LIB_SPIRV_STAGEREACHABILITYVISITOR_H
