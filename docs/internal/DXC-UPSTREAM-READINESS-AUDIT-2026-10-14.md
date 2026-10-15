# DXC AMD/SPIR-V upstream-readiness audit — 2026-10-14

> **Internal, Group C — assessment / extraction plan, not a Microsoft DXC submission.**  
> **Read first:** [Engineering guide](DXC-ENGINEERING-GUIDE.md).  
> **Exhaustive provisional file inventory:** [208-file CSV ledger](DXC-DELTA-LEDGER-2026-10-14.csv).  
> **Method:** GitHub repository comparison and source/hunk inspection, current PR reviews, focused CI history, and current Microsoft upstream `main` inspection. **Not** a complete independently compiled correctness audit of every feature. New results supersede this dated snapshot only when documented.

## Decision in one paragraph

**Do not propose `amd-sad-msad` to upstream as a unit, and do not split its current commits mechanically into upstream PRs.** Keep it as a private integration/testbed branch. Extract only a minimal, independently proven A (DXC bugfix) or B (legal AMD/KHR/core SPIR-V feature) change on a fresh Microsoft DXC baseline. Retain all C-group hardware probes, inventories, private CI, experimental flags and guide documents privately; they are valuable without being upstream obligations. Any X-group compiler file must be split **by independent behavior and test**, not by path or commit count.

## Baseline evidence

| Reference | Value / meaning |
| --- | --- |
| Fork | `thomthehound/DirectXShaderCompiler` |
| Fork `main` at audit | `6e0fab36dc9ac8c423e1b08fed3aeb0e739d9e54` — clean upstream-derived fork baseline, last recorded commit 2026-10-09 |
| Integration `amd-sad-msad` | `e4d9cfb7ab955c90a53eaba66d92dbfa175cc95c` — post-#22 merge |
| Integration vs fork `main` | **369 commits ahead, 0 behind; 208 changed files; +17,855/-306 lines** via GitHub compare |
| Current Microsoft `main` at audit | `5e8c6aa90c6a1f84f657517773456eabf0e16554` (2026-10-14) |
| Microsoft `main` vs fork `main` | At audit, Microsoft is **2 commits ahead** of fork `main` (no fork-only divergence for this comparison) |
| PR #22 | Internally **squash-merged** into `amd-sad-msad`; Linux and Windows passed 38/38 focused LIT regressions at `3d8b9e8`. This is **not** evidence of full upstream readiness |
| PR #23 | Still **draft, unmerged and architecturally blocked** at `24176f6`; six unresolved Codex findings (five P1, one P2); formatter failed; **both Linux and Windows focused LIT each had 39 passes and 1 failure** (`pervertex.lib.unreachable-record-helper.hlsl`), as of this snapshot. Do not promote the experiment |

Source reference: [GitHub baseline comparison](https://github.com/thomthehound/DirectXShaderCompiler/compare/main...amd-sad-msad), [PR #22](https://github.com/thomthehound/DirectXShaderCompiler/pull/22), [PR #23](https://github.com/thomthehound/DirectXShaderCompiler/pull/23), [Microsoft DXC](https://github.com/microsoft/DirectXShaderCompiler).

The integration totals are **diff statistics**, not code review quality scores. `+17,855/-306` includes test infrastructure and generated or experimental content. Some entries in GitHub's comparison report zero or omitted patch statistics, including `utils/hct` metadata and some APUSR files; the ledger flags these for a content-level inspection instead of asserting that they are empty.

## Provisional A/B/C/X file dispositions

The following counts are an **automated first-pass classification of all 208 paths**, with source-review overrides for key files. They are not 208 independently verified behavioral assessments.

| Code | Files | Reported changed lines | Treatment |
| --- | ---: | ---: | --- |
| **A? — candidate independent DXC fix** | 19 | 903 | Requires pristine-upstream reproducer, design review and stand-alone extraction |
| **B? — candidate AMD/KHR/core feature** | 57 | 2,371 | Requires legal SPIR-V contract, gate, validators, ABI/semantic tests |
| **C — private research/process** | 104 | 11,933 | Retain internally; generally exclude from upstream patch |
| **X — mixed-purpose shared surfaces** | 28 | 2,954 | Split hunks and generated dependencies into A/B/C before assessing |
| **Total** | **208** | **18,161** | Includes 173 added, 35 modified paths |

The ledger is the canonical path-by-path classification. Counts will change as X files are decomposed. Notably **B? does not mean mergeable**, and X is a work queue rather than an ultimate fourth destination.

## High-risk compiler surfaces requiring manual decomposition

These source diffs were directly inspected at hunk level. Findings below are *scope/ownership observations*, not full proof of semantic correctness.

| Source / scale | Intermixed concerns | Required extraction proof / risk |
| --- | --- | --- |
| `tools/clang/lib/SPIRV/SpirvEmitter.cpp` (1,161 changed lines) | Raw-buffer aliases/atomic operations, AST entry ownership, shader-stage restrictions, `msad4`, cooperative-matrix handling, AMD vertex parameter mapping, and unrelated cleanup | **Highest-priority split.** Construct separate call-site and downstream-use maps per feature. No blanket cherry-pick or rewrite. |
| `DeclResultIdMapper.cpp` (350) and shared `lib/DXIL/DxilSignatureAllocator.cpp` (18) plus `lib/HLSL` | D3D register packing and per-entry AMD pixel-parameter resolution; source declaration provenance | Contract must be canonical to DXC/DXIL packing, deterministic under exported/shared helpers, and separate from Vulkan locations and private heuristics. |
| `CapabilityVisitor.cpp` (182), `FeatureManager.cpp` (69) | AMD extension registration, capabilities, KHR cooperative matrices, low-precision types, family aliases, AMDX behavior | Separate *one exact extension* and its SPIR-V prerequisites from bulk `-fspv-extension=AMD`; don't change unrelated target environment defaults. |
| `SpirvBuilder.cpp` (154), `SpirvInstruction.h/.cpp`, `EmitVisitor.cpp` | Untyped raw pointers, call ABI and argument semantics, lowered types, stage reachability, resources | Verify memory-object pointers, scope, storage classes, operand/result types, call copy-in/out and emission ordering; these are **cross-feature interfaces**. |
| `LowerTypeVisitor.cpp` (141) | SM 6.10 linear-algebra/KHR cooperative matrices, fp8/bf16 representation and type lowering | Exact SPIR-V type/capability/layout match and source-vs-IR provenance. Do not reuse for speculative record changes without design. |
| `RawBufferMethods.cpp` (165) and associated builder/emitter paths | Typed vs KHR untyped pointer alias/access, 16/32/64-bit reads, writes and atomics | Independent ordinary/alias/resource/offset/alignment tests under both extension on/off. No universal rewrite purely for AMD optimization. |
| `PervertexInputVisitor.cpp` (129), `StageReachabilityVisitor.{h,cpp}` (301 combined), associated builder | Shared Clang source record mutation, fragment-only operation legality, pixel-entry ownership, exports | The existing protective diagnostics are useful, but upstream readiness requires ownership/lifetime proof. A new type representation must track **actual operand/value provenance**, conversions and ABI. |
| `lib/DxcSupport/HLSLOptions.cpp`, `HLSLOptions.td`, `SPIRVOptions.h` | Experimental `msad4` packed `OpUDot` candidate and `require-native` mode | **C/private for now.** The strict mode currently audits only `msad4`; it does not guarantee native machine instructions. Don't present as general native-code enforcement. |
| `tools/clang/test/lit.cfg`, test CMake and `utils/amd_lit_smoke.py` | Private gate convenience, upstream LIT tests, runner configuration | Move truly general shader regression into the normal upstream LIT tree; do **not** drag our CI scripts or environment hacks into a tiny upstream PR. |

The classification of an implementation file as X is deliberately conservative: a developer must show the exact hunk, its owner, associated test, and its required dependencies before deciding the final destination.

## A-group opportunities: DXC defects, not AMD features by default

1. **Stage/context validation:** Compilation target vs currently translated shader stage vs emitted entry-call-graph reachability. Evidence: PRs #17–#19 and `StageReachabilityVisitor`; a standalone `discard`/`SubpassLoad` repro on *current unmodified Microsoft DXC* is required. Check shared helpers, exported functions, globals, recursion, and CFG-unreachable calls. Reuse existing DXC validation models wherever possible.
2. **Per-vertex/export ABI safety:** PRs #21–#22 characterize order-dependent lowering and exported-record safety. #22 passes its **focused** gate, but its `FieldDecl` mutation and mixed-entry skip remain architectural liabilities. This should be design-reviewed independently of our AMD intrinsics before anyone calls it an upstream-ready fix.
3. **SPIRV-Tools CMake integration:** The `external/CMakeLists.txt` change avoids `CMAKE_INCLUDE_CURRENT_DIR` affecting SPIRV-Tools command-line builds. Potentially a *small* standalone upstream fix **if** reproduced without our private build flags and SPIRV-Tools executable override. Not automatically part of AMD intrinsic proposals.

**Do not bundle** these with an AMD extension addition for convenience. Fixes should be independently useful and defensible.

## B-group opportunities: standards-grounded AMD/KHR work

| Candidate | What is known from this code audit | Next proof and priority |
| --- | --- | --- |
| **Registered `SPV_AMD_shader_trinary_minmax` extinst** | Fork has explicit extension registration and header `[[vk::ext_instruction]]` declarations for FMin3/UMin3/SMin3/FMax3/UMax3/SMax3/FMid3/UMid3/SMid3. Current Microsoft `FeatureManager.cpp` inspected and does not register this extension; fork's `hlsl/vk/amd/intrinsics.h` is absent upstream. | **Best first B extraction experiment**: only required name/capability handling, one tiny independent header or upstream-compatible declaration, and LIT/validator. Test all typed signatures and target-environment behavior; no broad family alias or ISA promise. Propose upstream scope only after clean-base proof. |
| Other registered AMD extinsts (ballot, gcn, explicit vertex parameter) | Fork has wrappers plus capability inference; explicit vertex parameter additionally relies on complex DXIL parameter-register packing and per-entry provenance. | Separate each extinst set; **defer raw vertex-parameter intrinsics** until DXIL-signature and emitted-stage ownership are proven independently. |
| KHR cooperative matrices / low-precision types | Fork bridges portions of SM 6.10 `dx::linalg` into `SPV_KHR_cooperative_matrix`, with explicit unsupported cases, fp8/bf16 type code and capabilities. | Medium/high risk. Extract separately by legal shape and operation, check exact upstream state and validator, no matrix ABI approximations; do not use as first trust-building patch. |
| SPIR-V raw buffer / `KHR_untyped_pointers` | Typed/untyped buffer aliases, pointers and access-chain materialization are changed across emitter, builder and methods. | High risk. Trace true pointer/value ownership and IR producer-consumer invariants. Independently test offset, alignment, aliasing, functions, atomics, resources, supported Vulkan environments. |
| AMD `msad4` via four packed `OpUDot` reductions | Explicitly experimental candidate; no proven direct SPIR-V ingress for `v_mqsad_u32_u8`; private hardware probe harness exists. | **C for candidate and native-intent flags until evidence**. Legal output and performance may be useful but are *not* a guaranteed native AMD path. Driver disassembly and performance are separate from compiler acceptance. |
| AMD helper headers and DXIL equivalents | A mix of registered extension wrappers, standard Wave operations and APUSR/FSR4-oriented conveniences. | Extract only the smallest public API worth maintaining, avoid bulk project-specific convenience functions and optional DXIL shims without independent utility. |

First candidate is a *hypothesis*, not yet a patch approved for upstream. Check what Microsoft DXC actually supports **at extraction time**, not merely as of this audit. Use existing `vk::ext_instruction` machinery before modifying the emitter.

## C-group is a first-class outcome, not waste

Keep privately unless a narrow upstream feature explicitly needs an adapted piece:

- `utils/amd_vulkan_isa_probe/**`: AMD driver and RGA disassembly, shader candidate zoo, runtime correctness/performance probes (hardware-specific, not DXC upstream CI).
- `utils/amd_*_ci.py`, `utils/amd_spirv_*.`, `utils/apusr_*`, local PowerShell/qualification wrappers: internal research and checks; **do not** impose the 10k+ lines of utilities on maintainers.
- `.github/workflows/amd-spirv-dev-ci.yml`, APUSR/raw-buffer workflows: fork-specific CI; preserve here, not on clean upstream candidate branches.
- `docs/SPIRV-AMD-*.md`, `docs/apusr/**`, this guide, the audit and ledger: design evidence, internal controls, experiment instructions, cross-session handoff.
- Exploratory `msad4`/SAD candidate shaders, unproven strict/native switches, ISA pattern experiments: stay private until a real contract and evidence exist.

If a C feature is production-useful in our own SDK, it can graduate **within the private product** with proper tests. That is independent of whether Microsoft wants it upstream.

## Why PR #23 is blocked: specific architectural evidence

Codex independently raised **five P1 findings and one P2** against the proposed instruction-local shared-record tag, including:

1. Helper return `OpTypeFunction` disagrees with specialized `OpReturnValue` and call result.
2. Ordinary exported record return stored into a specialized Fragment-local pointer without conversion.
3. Ordinary resource-record member access receives an inappropriate per-vertex index.
4. Ordinary composite constructor values are labeled expanded without converting their constituent members.
5. Wrapper composite loses declaration-level `nointerpolation` provenance.
6. Replacing `StructType::FieldInfo::type` without recomputing physical offsets/size/debug metadata.

These defects share **one missing abstraction**: stage ownership is not representation provenance. A correct design must follow actual emitted pointers/values from stage inputs, distinguish ordinary inputs and resource-loaded values, and make conversions explicit at each incompatible producer-consumer edge. Do not 'fix' this by adding six context-sensitive type heuristics.

Both Linux and Windows in [run 38661997026](https://github.com/thomthehound/DirectXShaderCompiler/actions/runs/38661997026) additionally report **39 passed / 1 failed** in focused LIT: the old `pervertex.lib.unreachable-record-helper.hlsl` safety-diagnostic check no longer observes its expected error. This confirms the experimental implementation **changes an established protective behavior** and cannot be accepted on review comments alone.

Recommended action: retain the draft as a **non-mergeable historical experiment** while transferring findings into a design brief; then close it rather than continuing patch-by-patch. No source changes from #23 have entered `amd-sad-msad` or fork `main`.

## Priority and sequencing

| Order | Deliverable | Hard acceptance gate |
| --- | --- | --- |
| **0 — now** | Freeze speculative representation changes; finish/record #23 failure analysis and quarantine experiment | No promotion of #23 code; existing #22 regression suite remains enforced on integration |
| **1 — now** | Living [guide](DXC-ENGINEERING-GUIDE.md) + dated audit + complete CSV ledger, all tracked as private Group C | 208/208 filenames classified provisionally; X clearly marked, uncertain diff stats disclosed |
| **2** | Identify first **clean-base B extraction**, likely one registered AMD extinst family | Current unmodified upstream state, explicit SPIR-V spec, smallest dependency diff, source-level tests |
| **3** | Independent A design dossier for stage validation and per-vertex | Minimal upstream reproducer and no shadow stage/context architecture |
| **4** | Correct shared-record representation model (research before code) | Written producer/consumer provenance rules, conversion operations, function ABI/return contract, full metadata/layout ownership, negative controls |
| **5** | Repeat per-feature extraction of remaining B/X work | No unrelated dependencies, complete relevant test suites, independent review |

A large breadth-first cleanup of the 208-file delta is **not** the right first coding task. Decompose X gradually as each extractable A/B feature proves itself. Let the private integration branch keep servicing experiments.

## Next-session audit command checklist

1. Read the [guide](DXC-ENGINEERING-GUIDE.md) and this audit, then CSV ledger; verify current GitHub PR #23 and integration state.
2. Compare latest `microsoft/DirectXShaderCompiler:main` with fork `main` and `amd-sad-msad` again. Do not assume these audit commit references still describe the current head.
3. For any selected feature, inspect hunk-level dependencies, current upstream API, and full emitter/lowering producer-consumer flow.
4. Add a minimal clean-upstream reproducer (or spec-defined positive shader), then obtain a design review before the implementation.
5. Test the exact candidate PR head using normal relevant DXC test suites. Private 38/40-test smoke runs are useful regression fences, **not** an upstream merge-quality certificate.
6. Update the ledger disposition and this snapshot when evidence changes; do not silently reclassify C work as B.

### Audit limitations and confidence

**High confidence:** recorded heads, GitHub compare counts and paths, presence of selected source hunks, verified current upstream extension-registry absence, PR #22/#23 state and review findings, recorded Linux and Windows failing LIT case.  
**Medium confidence:** provisional grouping and relative engineering risk; these follow source/patch observations but have not been independently reproduced for each feature.  
**Not established:** full DXC upstream test suite, AMD driver ISA/performance, clean-base builds for extraction candidates, approval from Microsoft maintainers, fully correct general per-vertex record representation.

This audit is a **triage and development-control result**. Its purpose is to prevent wasted engineering and future context loss, not to pretend the 208 files have received individual formal code review.
