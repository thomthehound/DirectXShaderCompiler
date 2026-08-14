# AMD native-contract inventory

The AMD Vulkan/SPIR-V work in this fork is tracked as a compiler-contract problem, not as a list of HLSL conveniences. There are two separate questions for every hardware-facing operation:

1. Can DXC express the operation in legal Vulkan SPIR-V without destroying its specialized semantics?
2. Does the AMD Vulkan compiler lower that SPIR-V representation to the intended hardware operation?

A positive answer to only one of those questions is not sufficient for strict native support.

## Compiler boundary

For Vulkan, DXC stops at SPIR-V. The Vulkan implementation owns the next compilation stage. In AMD's open LLPC architecture, the front-end translates SPIR-V to LLVM IR, LGC lowers that IR further (including to `llvm.amdgcn.*` intrinsics), and the AMDGPU backend performs instruction selection and ISA emission. A native AMDGPU LLVM intrinsic therefore proves that the backend can represent an operation; it does not prove that the SPIR-V front-end can reconstruct that intrinsic after DXC has expanded the operation into ordinary arithmetic.

This distinction is especially important on Windows: AMD's open AMDVLK documentation states that its closed-source Vulkan driver uses a different pipeline compiler from LLPC. Changes to LLPC are therefore useful as a reference and for open AMD stacks, but are not a drop-in replacement for changing the Windows Radeon Vulkan compiler.

## Current high-value family map

The names below are taken from the contemporary LLVM AMDGPU subtarget-feature surface. They are deliberately family-level until each exact operation has been checked against current SPIR-V semantics and actual AMD Vulkan code generation.

| AMDGPU hardware family | SPIR-V ingress status | Work in this fork |
| --- | --- | --- |
| `v_sad*`, `v_qsad*`, `v_msad_u8`, `v_mqsad*` | **Missing direct SAD/MSAD contract.** | `msad4` strict mode errors. Its exact GFX11 native target is `v_mqsad_u32_u8`; non-strict AMD mode currently has an **experimental** 4x-`OpUDot` candidate, with the baseline scalar expansion and manual arithmetic retained as controls. No candidate is called native until AMD-driver ISA proves it. |
| integer `v_dot*` packed forms | Standard contract exists for important 4x8 cases through integer dot-product SPIR-V. | Preserve and audit exact signedness/width/accumulation semantics. |
| WMMA / SWMMAC | KHR cooperative matrix is a possible ingress contract for matching operations, but not an automatic one-to-one encoding of every AMD WMMA/SWMMAC form. | First SM 6.10 Wave/ThreadGroup cooperative-matrix bridge landed; expand by exact shape/type/accumulation semantics. |
| FP8/BF8 and newer FP4/FP6/BF6 conversion families | Mixed. SPIR-V has evolving low-precision format support, but AMD conversion instructions also encode selection, packing and rounding details. | Inventory first; do not substitute generic conversions where semantics/performance differ. |
| BVH/ray-tracing hardware instructions | High-level Vulkan ray tracing has a KHR contract; private BVH instruction details do not. | Keep DXR on KHR ray tracing/query. Audit only functionality that cannot be represented there. |
| DPP/permlane/cross-lane operations | Mixed core/KHR/AMD subgroup coverage. | Classify operation-by-operation; preserve AMD extinsts where registered. |

## Generated inventory

Use a current LLVM checkout to regenerate the raw review surface:

```powershell
python utils/amd_spirv_inventory.py --llvm-root C:\src\llvm-project `
  --interesting-only --output amd-native-inventory.md
```

The script extracts both AMDGPU subtarget features and explicit `llvm.amdgcn.*` intrinsics and assigns only coarse review buckets. It intentionally leaves unknown entries as `unclassified`; that is preferable to claiming a Vulkan native path which has not been proven.

## Proof levels

Use these proof levels when changing the compiler:

- **Contract:** a core/KHR/AMD SPIR-V operation has equivalent semantics.
- **Driver acceptance:** the target AMD Vulkan driver accepts the emitted module.
- **ISA proof:** disassembly confirms the desired AMD instruction family.
- **Performance proof:** profiling shows that the native path is actually beneficial in the target workload.

`-fspv-require-native-intrinsics` may only treat an operation as solved once the contract is strong enough that native intent is not silently discarded. Where the contract itself is missing, the correct result remains an explicit error, not an emulation relabeled as support.

## Native-code probe harness

The SAD/MSAD proof protocol and driver/RGA harness live in [`SPIRV-AMD-SAD-Native-Probe.md`](SPIRV-AMD-SAD-Native-Probe.md). Native status must be based on ISA evidence, not merely on the presence of an AMDGPU intrinsic in LLVM.
