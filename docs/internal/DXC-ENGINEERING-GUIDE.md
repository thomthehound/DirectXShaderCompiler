# DXC AMD/SPIR-V Engineering Guide (internal)

> **Status:** Living internal project policy; **Group C / private engineering infrastructure**.  
> **Authority:** Applies to work in `thomthehound/DirectXShaderCompiler` until explicitly superseded. This is *not* a statement of Microsoft DXC maintainers' acceptance.  
> **Companion artifacts:** [Dated upstream-readiness audit](DXC-UPSTREAM-READINESS-AUDIT-2026-10-14.md) and [file-level delta ledger](DXC-DELTA-LEDGER-2026-10-14.csv). Update them when the baseline or decisions change.

## Mission and non-negotiables

Enable legal, correct HLSL-to-SPIR-V compilation for useful AMD (and where appropriate standardized KHR/core) functionality; preserve specialized hardware intent *only when a legal SPIR-V contract represents it*. Independently useful DXC defects should be fixed using upstream-style architecture. Nothing should be called AMD ISA-native merely because an AMDGPU LLVM intrinsic exists or a plausible SPIR-V pattern is emitted.

This is a **derivative of Microsoft DXC**, not a competing shader compiler. Prefer DXC's existing source/IR ownership, type lowering, feature management, legalizer, emitters, diagnostics, and testing idioms. An architectural departure needs a documented source-grounded reason and rejected alternatives. Never invent a second shader-execution pipeline, a second call graph, or a substitute ABI because one component is awkward.

**Correctness first; clean CI second; upstreamability third.** CI is necessary but not sufficient. A change can be correct yet unsuitable upstream, or pass focused tests while remaining incorrect.

## Founding objective: APUSR SAD/MSAD hardware lowering

**Do not confuse the first small upstream contribution with the project's primary performance objective.** This work originated because HLSL `msad4` was lowered by DXC into many generic SPIR-V byte extracts, subtract/absolute-difference operations and additions rather than conveying the packed, masked four-window SAD operation to the AMD Vulkan compiler. The intended target is the Radeon instruction `v_mqsad_u32_u8`, used to accelerate APUSR's 8-row × 2-QSad optical-flow hot path. LLVM AMDGPU's `llvm.amdgcn.mqsad.u32.u8` lowering establishes an AMD backend instruction-selection path; it **does not** prove the Vulkan SPIR-V ingress path can recognize that operation.

Current status:
- Ordinary DXC `msad4` uses a scalar/vector SPIR-V expansion. The private `-fspv-enable-amd-intrinsics` mode uses an **experimental four-`OpUDot` candidate**, not a dedicated SPIR-V MQSAD contract. `-fspv-require-native-intrinsics` deliberately rejects `msad4`; it audits only this case and is not a general ISA guarantee.
- The private probe compares `msad4-scalar`, `msad4-udot`, and APUSR's `swar-udot` implementation over the same optical-flow kernel. APUSR QSad/SWAR equivalence to masked `msad4` depends on the actual **nonzero-luma/reference-byte input contract**; the zero-reference case is a required counterexample in the semantic oracle.
- **No installed-driver ISA result or performance result is recorded in this engineering audit.** Do not claim an MQSAD win from correct SPIR-V, LLVM instruction selection, or an RGA offline ISA report. The live AMD Vulkan driver disassembly and workload timing decide.
- If a legal SPIR-V pattern is selected as MQSAD (or otherwise proves faster), prefer the smallest upstream-compatible producer change or APUSR shader expression that preserves it. If no legal pattern recovers MQSAD, investigate the downstream AMD Vulkan compiler's pattern recognition or a **properly specified, validator/driver-supported extension**. Never invent private SPIR-V opcodes in ordinary Vulkan output or present decomposition as hardware preservation.
- `SPV_AMD_shader_trinary_minmax` is an unrelated, deliberately low-risk **relationship-building B-group candidate**, not the SAD/MSAD solution. Pursue it independently; don't let it block decisive SAD/driver evidence.

Source files: [SAD native probe protocol](../SPIRV-AMD-SAD-Native-Probe.md), [AMD native contract inventory](../SPIRV-AMD-Native-Contract-Inventory.md), and [probe README](../../utils/amd_vulkan_isa_probe/README.md). The relevant three-way experiment is `utils/amd_vulkan_isa_probe/apusr_qsad_probe.py`; preserve its control conditions and report categories.

## The three destinations—and the extraction flag

| Code | Destination | What belongs | Review rule |
| --- | --- | --- | --- |
| **A** | Independently useful upstream DXC corrections | A defect reproducible on *unmodified current Microsoft DXC*, with a minimal regression and the smallest source-owned fix | Upstream candidate only after a stand-alone design and regression review |
| **B** | AMD / standard SPIR-V functionality | Precisely defined legal extension, capability, builtin, intrinsic, or KHR/core lowering with proven semantics | Upstream candidate only after producer validation and target-environment checks |
| **C** | **Private** research, qualification, and development | Driver ISA probes, microbenchmarks, feature inventories, exploratory shaders, experimental lowerings/options, broad harnesses, engineering policy, handoffs, internal CI | Keep useful indefinitely; *do not* submit merely because it is in the integration branch |
| **X** | **Split required**, not a destination | A modified file or conceptual patch containing A + B + C work or multiple unrelated features | Attribute each hunk and its tests to A, B, or C before extracting |

These labels classify **intent**, not readiness. An A or B label is *not* a claim the code is mergeable. Every newly encountered dependency, test, or generated file must be classified with its parent feature. A change that has no credible upstream reason defaults to C, not to optimistic B.

C is legitimate long-term scope. No pressure to push probes, homegrown CI, APUSR-specific behavior, hardware qualification ledgers, or this guidance document upstream. Private functionality may still have production users; it still must be correct, tested, and isolated from unrelated DXC behavior.

## Upstream's explicit AI contribution policy

Microsoft DXC's [AI Tool Use Policy](https://github.com/microsoft/DirectXShaderCompiler/blob/main/AIToolPolicy.md) **permits AI-assisted contributions**, but it requires human-in-the-loop ownership. **Before requesting upstream maintainer review**, the human contributor must personally read and review *all* AI-generated source or text, understand and be able to explain its invariants and alternatives, and confirm that requesting review is a good use of maintainer time. Do not merely feed reviewer comments to an agent and post unexamined generated patches. Disclose substantial tool-generated content in the PR description or commit message (e.g., `Assisted-by:` trailer). DXC expects smaller first contributions and recommends agents operate in forks.

Our internal Codex review, compiler tests, and AI-driven drafts supplement this gate but **cannot satisfy the human-review requirement on their own**. An A/B candidate stays private/draft until an accountable human has reviewed it, can explain every change, and approves asking upstream for review. This is an explicit upstream policy, not just our preference.

## Branch discipline and lifecycle

- `main`: keep a clean fork of Microsoft's DXC, synchronized deliberately. Never merge the entire AMD integration branch into it.
- `amd-sad-msad`: private integration/testbed branch. Contains experimental work and internal repairs; **not** an upstream candidate branch.
- `audit/*`: reviewable *C-group* plans, measurements, and documentation. Target the integration branch so internal guidance stays with the work.
- `upstream/*` or `extract/*`: one feature from a **fresh, current Microsoft `main`**, or a deliberately synchronized clean fork `main`; no inherited experimental ancestry. Rebuild the minimal patch, not a cherry-pick chain that drags unrelated fixes along.
- `experiment/*`: private proof-of-concept; never quietly promoted to a production fix.
- PRs remain draft when any P1/P2 correctness finding is substantiated, CI is incomplete, or architecture remains disputed. No merge for green checks alone. Do not churn commits during a live expensive CI run merely to fix formatting; use a single correction push when practical.

Record base/head revisions in audit snapshots and PRs as references, **not** blanket SHA checksum requirements. Functional/structural checks are preferred; exact byte identity is required only for a concrete integrity use case.

## Required workflow for **every** A/B proposal

1. **Characterize before implementation.** State problem, inputs, intended observable output, actual output, target environment, provenance of requirement, and smallest reproducer on unmodified Microsoft DXC. Also show successful controls and explain whether any behavior is AMD-only.
2. **Trace ownership.** Identify the responsible upstream classes/passes and their lifetime (source AST, current function, entry-wrapper, emitted module, SPIR-V type, downstream driver). Read callers and consumers; do not stop at the first tempting insertion point.
3. **Write invariants.** List the type/ABI/operand, control-flow, storage-class, capability/extension, layout, debug/reflection, and linkage properties that must remain true; list unsupported boundaries.
4. **Evaluate alternatives first.** In order: existing API/visitor/legalizer; small extension of an existing abstraction; explicit representation-conversion pass; only then a new abstraction. Explain why lesser changes are insufficient. Obtain design review *before* significant coding.
5. **Implement the minimum behavior.** No unrelated renames, broad formatting, new flags, generated large inventories, or tests for unrelated products. Preserve legacy behavior by default, but don't keep legacy hacks solely because they already exist.
6. **Prove it.** Positive and negative LIT/FileCheck tests; test that the regression fails against the old implementation; validate output with SPIRV-Tools; run all relevant existing DXC tests on Linux and Windows and affected DXIL paths, not solely the private smoke gate. Check expected diagnostics and no silent fallback. Check linked-library order, shared helper/exports, empty/unused CFG blocks, and downstream ABI consumers as appropriate.
7. **Review independently.** Have Codex/peer review the design and the implementation, with prompts targeting invariants and counterexamples. Resolve findings based on evidence rather than suppressing tests. A clean review is not a formal upstream approval.
8. **Extract cleanly.** Create a fresh-base, self-contained PR with rationale, API/semantic contract, tests, limitations, and source links. If extraction is awkward, the feature boundaries were not clean enough: return to the design.

**Go/no-go:** Stop implementation when key data provenance is unknown, a pass edits shared source state to represent backend-specific layout, function-call arguments/results disagree with signatures, or validators reveal inconsistent types. Explicit unsupported diagnostics are better than silently emitting broken SPIR-V, but an error-only workaround is not proof the original feature is complete.

## Cross-cutting compiler contracts

- **Stage and entry ownership:** compilation target, current translation-stage context, and emitted reachable entry stages are separate facts. Entry-specific interfaces and late module validation have different responsibilities. A linkage export is unknown-stage even if this module contains a pixel entry. Do not specialize a shared helper merely because its last caller was a pixel shader.
- **Source vs SPIR-V representation:** Clang `FieldDecl` and `QualType` are shared source semantics, not a scratch pad for per-entry SPIR-V layouts. Stage reachability alone does not establish **value/pointer representation provenance**. Propagate representation through actual producers and consumers; explicit conversions must precede ABI/storage boundaries.
- **SPIR-V legality:** obey pointer-as-memory-object rules, storage classes, structured-buffer vs untyped-pointer semantics, capability and extension ownership, interface decorations, layout and debug/reflection metadata.
- **Native intent:** four different evidence levels: legal SPIR-V contract; installed AMD driver accepts; shader ISA proves the instruction; performance measurement proves benefit. Do not promote one level into another. `msad4`/MQSAD remains a missing direct Vulkan ingress contract until proved otherwise.
- **Portability:** an AMD- or provisional extension must be explicitly gated. Don't change default DXIL or other vendors' Vulkan paths as collateral. Avoid licensing/third-party dependencies unrelated to the feature.
- **Tests and failure semantics:** distinguish failure to compile, failure to validate SPIR-V, failure at actual driver, and performance deficiency. A diagnostic can be a temporary safety barrier, not a completed representation solution.

## What we have learned from PRs #21–#23

- #21: valuable regression characterization (failure first).
- #22: green on Linux and Windows for the focused 38-test gate and merged internally; protects some exported/shared-record cases. **Not automatically upstream-ready**: its current implementation still conditionally mutates shared Clang AST fields and defers mixed-stage-library behavior.
- #23: speculative per-instruction record specialization triggered at least **five P1 and one P2** from independent review. The failures span helper returns, cross-stage returns, resource member access, local composite construction, entry-wrapper propagation, and field layout metadata. **Do not merge or salvage by accumulating local conditionals.** Preserve findings as design evidence; rebuild a representation-aware architecture only after provenance and conversion contracts are proved.

The private integration branch remains useful while upstream extraction is selective. We may delete/replace incorrect experiments without disturbing it.

## Definition of ready-to-present-to-upstream

All must be true:

- [ ] Reproduced defect or missing SPIR-V contract on current unmodified Microsoft DXC
- [ ] No dependency on private AMD integration commits, flags, test runners, or generated ledgers
- [ ] Maintainer-legible single responsibility and clear alternative analysis
- [ ] Source, IR, call ABI, type, layout, capability, and diagnostic invariants documented
- [ ] Minimal reliable tests including failure-before-fix and adversarial scenarios
- [ ] Upstream-relevant Linux/Windows suites and validator checks green at **exact PR head**
- [ ] No unresolved material review findings; known limitations explicit and acceptable
- [ ] Human contributor has personally reviewed all AI-generated code and text, can defend every change, and discloses substantial AI assistance per DXC's policy
- [ ] No unsupported claims of AMD native ISA or performance
- [ ] Scope, tests, and licenses are appropriate to upstream DXC conventions

Passing this checklist makes a proposal defensible, **not guaranteed accepted**.

## Next extraction sequence

1. Inventory the exact integration-to-baseline delta (dated audit and file ledger). Mark mixed sources **X**, and keep private tools, qualification infrastructure, and documentation in C.
2. Pick a *single registered AMD SPIR-V extension path* with a small feature-manager/capability and HLSL-header surface, and check what **current Microsoft DXC** already does. `SPV_AMD_shader_trinary_minmax` is a plausible initial candidate, not yet accepted or isolated. Do not start with the full `-fspv-extension=AMD` family switch.
3. Separately design-review the A-group stage/record changes. Make no additional record-lowering implementation in #23 until producer/consumer provenance and conversions have explicit contracts.
4. Prove the selected B candidate on a clean upstream base and submit only its essential source, tests, and narrow documentation. Keep native-ISA probing in C.
5. After one clean extraction, repeat with independently scoped features; merge successful changes selectively, never `amd-sad-msad` wholesale.

## Cross-session handoff / how to resume

**Start each new session by reading this file, the dated audit, and the CSV file ledger from the actual repo.** Then check the live upstream Microsoft `main`, fork `main`, integration branch, and open PR state. Do not assume an old commit or previous CI run is still current.

Suggested handoff text:

> We maintain `thomthehound/DirectXShaderCompiler`, a private AMD/SPIR-V integration branch derived from Microsoft DXC. Read `docs/internal/DXC-ENGINEERING-GUIDE.md`, `docs/internal/DXC-UPSTREAM-READINESS-AUDIT-2026-10-14.md`, and `docs/internal/DXC-DELTA-LEDGER-2026-10-14.csv` on the integration/audit branch before acting. Keep `main` clean. Classify every change as A (independent upstream DXC fix), B (legal AMD/KHR/core functionality), C (private research/tools/policy), or X (must split). Never infer upstream readiness from green smoke CI. #22 is an internally merged defensive correction, not automatically an upstream candidate; #23 is blocked architectural research with multiple P1 defects. Ground proposals in unmodified current upstream, prove source/IR/type/ABI correctness, and only extract smallest stand-alone patches. Check current PR/CI facts and ask for design review before building another representation layer.

When an audit classification or decision changes, update the dated evidence and this policy in a reviewable PR. Do not rely solely on chat memory or an unreviewed assistant summary.
