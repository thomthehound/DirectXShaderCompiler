# AMD SAD/MSAD native-lowering probe

## Why this exists

DXC can prove what SPIR-V it emitted. LLVM AMDGPU tests can prove that an
`llvm.amdgcn.*` intrinsic selects a native machine instruction. Neither fact by
itself proves that a Vulkan driver reconstructs the AMD intrinsic from the
SPIR-V graph that DXC emitted.

For SAD-family work we therefore keep four proof levels separate:

1. **SPIR-V contract/shape** — the module expresses the intended semantics and
   passes SPIR-V validation.
2. **AMD compiler acceptance** — the AMD compiler accepts that exact module.
3. **Native ISA** — disassembly of that compiled module contains the intended
   SAD-family machine instruction rather than just equivalent scalar/vector
   arithmetic.
4. **Performance** — runtime measurement confirms that the native-looking
   codegen is actually beneficial in the target workload.

The `-fspv-require-native-intrinsics` switch must continue to enforce level 3,
not level 1 or 2.

## Exact `msad4` target

HLSL `msad4` has a particularly strong AMD mapping. Its operands are a packed
four-byte reference, an eight-byte source, and four 32-bit accumulators. It
computes four masked four-byte SADs over source windows shifted by 0, 1, 2 and
3 bytes; a zero reference byte suppresses the corresponding difference.

AMD `v_mqsad_u32_u8` has the matching native shape: eight unsigned source
bytes, four unsigned reference bytes, four 32-bit accumulators, and four
32-bit results. For the defined HLSL result range this is the natural
one-instruction native target for `msad4`.

This is not obsolete ISA baggage on the target generation. Contemporary LLVM
models `MsadInsts`, `MqsadPkInsts`, and `MqsadInsts` in the GFX11 common feature
set, and `gfx1151` inherits that set. The GFX11 instruction tables contain real
encodings for `V_MSAD_U8`, `V_QSAD_PK_U16_U8`, `V_MQSAD_PK_U16_U8`, and
`V_MQSAD_U32_U8`.

LLVM's selector also has an explicit direct rule from
`llvm.amdgcn.mqsad.u32.u8` to `V_MQSAD_U32_U8`. That proves the lower AMDGPU
backend can select the exact instruction when semantic identity survives to
LLVM IR. It does **not** prove that a Vulkan SPIR-V front-end reconstructs that
intrinsic from ordinary arithmetic or integer-dot-product SPIR-V.

Therefore the gold-standard ISA result for this probe is:

```text
v_mqsad_u32_u8
```

Other `v_sad*`, `v_msad*`, `v_qsad*`, or `v_mqsad*` output is still useful
specialized lowering, but is reported as **partial** rather than being silently
promoted to exact `msad4` recovery.

## Probe corpus

`utils/amd_vulkan_isa_probe/shaders` contains two semantically related compute
shaders:

- `msad4_builtin.hlsl` uses the HLSL `msad4` intrinsic.
- `msad4_manual.hlsl` spells out the masked four-window byte SAD arithmetic.

`native_probe.py` compiles three primary cases:

- `msad4.builtin.baseline` — the normal DXC scalar/vector SPIR-V expansion.
- `msad4.builtin.amd` — an **experimental** AMD-oriented candidate that keeps
  each four-byte horizontal reduction packed and performs it with `OpUDot`.
  This is not assumed to be superior and is not called native; the entire
  point of the probe is to see whether it makes exact MQSAD reconstruction
  easier or harder for an AMD Vulkan compiler.
- `msad4.manual` — source-level arithmetic control, useful in case the driver's
  optimizer recognizes a different canonical graph than either DXC lowering.

An optional vanilla-DXC control can be supplied as well.

The harness also verifies that strict-native compilation of `msad4` still
fails. Strict mode must remain an error until an actual driver/compiler ISA
probe establishes a representation that reliably reaches the specialized
instruction.

## Installed Windows driver proof

`utils/amd_vulkan_isa_probe/amd_vulkan_isa_probe` creates a compute pipeline and
uses the `VK_AMD_shader_info` device extension to request
`VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD` from the driver itself.

This is the preferred proof when the installed Radeon driver exposes that
extension. If it does not, the probe returns an explicit unsupported result;
it does not infer anything about code generation.

Build:

```powershell
cmake -S utils/amd_vulkan_isa_probe -B out/amd-vulkan-isa-probe -A x64
cmake --build out/amd-vulkan-isa-probe --config Release
```

Or use the wrapper:

```powershell
utils\amd_vulkan_isa_probe\run_windows.ps1 -Dxc C:\path\to\dxc.exe
```

On a multi-GPU system the probe prefers an AMD physical device automatically;
`--device` can override the selected Vulkan device explicitly.

## RGA cross-check

RGA supports both a target-selectable Vulkan offline SPIR-V path and a live
Vulkan path. Keep them separate in reports:

```text
rga -s vk-spv-offline -c gfx1151 --isa out.isa shader.spv
rga -s vulkan --comp shader.spv --isa out.isa
```

The offline compiler is valuable for quickly testing multiple GFX targets. It
is not proof that the installed Windows driver makes the same combine. RGA's
own help text recommends live-driver mode when results need to reflect the real
driver pipeline.

## Decision rule

For each legal SPIR-V candidate:

- If installed-driver disassembly contains `v_mqsad_u32_u8`, preserve that
  SPIR-V shape in DXC, add a regression/proof fixture, and only then consider
  relaxing strict-native `msad4` for that proven AMD contract.
- If installed-driver disassembly contains another SAD-family instruction,
  record it as specialized partial lowering and compare its cost before
  deciding whether it is a worthwhile producer shape.
- If only RGA offline contains the desired instruction, record it as offline
  compiler evidence and do not relax strict-native mode.
- If no successful AMD driver/compiler disassembly contains a SAD-family
  instruction, classify the operation as a **SPIR-V ingress gap**. At that
  point a DXC-only change cannot promise native preservation; the remaining
  fixes are a recognized canonical pattern in the AMD Vulkan compiler or a
  real SPIR-V extension understood by both producer and consumer.
