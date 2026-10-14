# SPIR-V execution-context boundaries

DXC already has separate representations for the compilation profile,
the shader stage currently being translated, and the finished SPIR-V module.
These are not interchangeable in a library with multiple entry points.
The purpose of this note is to record their owners and existing behavior,
not to introduce a parallel compiler pipeline.

## Ownership and lifetime

| Question | Existing authority | Lifetime and use |
| --- | --- | --- |
| Which target profile was selected by `-T`? | `CompilerInstance::getCodeGenOpts().HLSLProfile`, resolved to `hlsl::ShaderModel` in `SpirvEmitter` | Immutable per compilation. Determines whether this is a library and whether input-attachment declarations are admissible at all. |
| Which stage is the emitter currently translating? | `SpirvContext::getCurrentShaderModelKind()` and `SpirvEmitter::FunctionInfo::shaderModelKind` | Changes while consuming the function work queue, and must remain available for stage-specific wrapper, signature and builtin construction. |
| Which stages can actually execute a lowered operation? | `SpirvModule`, `SpirvEntryPoint`, `OpFunctionCall` edges and `StageReachabilityVisitor` | Known once wrappers and implicit initialization calls have been generated; required for validation of fragment-only operations that do not affect earlier lowering. |
| Which pixel entry supplies a particular input signature? | Entry wrapper and `DeclResultIdMapper` mappings | Entry-specific lowering concern. Not inferred merely from the last shader stage translated, or from the fact that a module contains some pixel entry. |

### Source-grounded details

- `SpirvEmitter::HandleTranslationUnit()` seeds the work queue based on the
  **compilation** profile, then updates `SpirvContext` for each queued
  function. Callee translation is shared between entry points.
- `SpirvBuilder::takeModule()` ends the synthetic `module.init` function
  and attaches its calls to entry wrappers before visiting the lowered module.
  `SpirvFunction::invokeVisitor()` traverses reachable basic blocks rather
  than every AST expression or every unattached block.
- `SubpassLoad` is lowered to an image read. Its source-level stage
  requirement must be associated with an instruction that survives module
  traversal. Standalone nonfragment profiles cannot materialize an input
  attachment; libraries can contain a legal pixel entry and must defer the
  execution-stage check.
- `GetAttributeAtVertex` additionally changes data representation, input
  and parameter layouts. Today `PervertexInputVisitor` is invoked under
  `context.isPS()` in `takeModule()`. That **does not** establish library
  correctness: it depends on mutable context after translating the work
  queue. The mixed-stage library failure is a known, separate issue.
- AMD raw vertex-parameter intrinsics currently use
  `collectFunctionEntryReachability()`, a source-AST approximation of entry
  ownership, because their register lookup happens before the finished
  module's call graph exists. Replacing this needs a deliberate
  entry-specific lowering design, not a second stage-mask query.

## Rules for subsequent changes

1. First identify when the information is known: compilation target,
   current function, per-entry interface, or emitted operation.
2. Use the existing DXC `ShaderModel`, `FunctionInfo`, `SpirvContext`,
   `SpirvModule` and visitor abstractions. Do not add independent
   shadow stage/owner maps for convenience.
3. Do not infer legality from declaration order, whichever stage was most
   recently translated, or unreachable AST calls.
4. Distinguish stage legality from earlier representation changes. In
   particular, generic late IR validation is **not** a replacement for
   entry-specific interpolation/input-register lowering.
5. Preserve upstream standalone shader behavior, library exports, implicit
   initializers, hull patch-constant calls, and function-parameter semantics.
   Derivative operations and other extension-enabled stages have additional
   rules; not all stage constraints are fragment-only.
6. Keep upstream-oriented changes small enough to review in isolation,
   document genuine departures, and use existing LIT/FileCheck regression
   tests plus SPIR-V validation before considering an architectural change
   complete.

## Per-vertex input representation and linkage

`processGetAttributeAtVertex()` requests per-vertex interpolation during
expression lowering. `PervertexInputVisitor` later expands nointerpolation
inputs and propagates expanded values through function parameters and calls.
Its current `expandNointerpStructure()` rewrites `FieldDecl` types on the
shared Clang AST, so an entry-interface-only filter cannot isolate every
representation change.

An emitted `SpirvEntryPoint` interface identifies inputs for one shader
entry. It does not describe the callable ABI of a function exported through
`LinkageAttributes`. An export may be linked from a shader whose stage is
not represented among this module's local entries. The same distinction
applies to helpers reachable from both entries and exports.

The lowering contract is therefore:

- A pixel entry's per-vertex input representation must be determined from
  its interface and values, not the last mutable `SpirvContext` stage.
- An export's input parameter and return ABI must not vary with unrelated
  local pixel entries, their declaration order, or interpolation intrinsics.
- Shared callable signatures and record types require representation-safe
  lowering; filtering only the entry's module variables is insufficient.
- Correctness tests must exercise both the original late-library-work-item
  trigger and the unrelated-export ABI case. Neither can be replaced by a
  pixel-only root test that would pass under the original stage gate.

The entry-order and unrelated-export regression fixtures now assert correct,
validated SPIR-V output on both declaration orders. The former XFAILs and
failing-state baseline tests are removed once these cases pass.

The existing `StageReachabilityVisitor` supplies emitted, transitive stage
ownership to the per-vertex lowering pass: Fragment entry wrappers and their
private callees can be specialized; exports and functions they transitively
reach remain stage-unknown. The per-vertex visitor does not modify those
non-Fragment callable signatures or bodies. A pixel entry may pass its
ordinary provoking-vertex value to a cross-stage or exported callee via the
existing operand-replacement mapping. It cannot implicitly pass an expanded
array to a callable requiring the original scalar or vector ABI.

For records, this is not yet a complete representation-separation solution.
`PervertexInputVisitor` still expands record fields by mutating shared
`FieldDecl` objects. If a non-Fragment or exported function uses such a
record, the compiler diagnoses the conflict rather than silently changing
its callable ABI. This protection includes records used by emitted
temporaries and resource loads, not only function signatures and local
declarations. `pervertex.lib.export-shared-record.hlsl` and
`pervertex.lib.export-record-resource-use.hlsl` protect the fail-safe. A future lowering should replace global AST record mutation with
SPIR-V-private types and explicit conversions where necessary.

## Existing regression coverage

The focused `utils/amd_lit_smoke.py` gate includes upstream DXC
`GetAttributeAtVertex` tests, stage-restricted discard tests, and
`vk.subpass-input.shared-stages.hlsl` in both declaration orders.
The AMD contract gates protect existing vendor lowering.
These are **regression fences**, not proof that all mixed-stage library
interpolation and AMD ownership cases are already correct.

Relevant upstream sources:
`tools/clang/lib/SPIRV/SpirvEmitter.cpp`,
`tools/clang/lib/SPIRV/SpirvBuilder.cpp`,
`tools/clang/lib/SPIRV/DeclResultIdMapper.cpp`,
`tools/clang/lib/SPIRV/PervertexInputVisitor.cpp`, and
`tools/clang/include/clang/SPIRV/SpirvContext.h`.
