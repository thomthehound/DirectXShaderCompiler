# AMD Vulkan ISA probe

This utility answers one narrow question with the installed Vulkan driver:

> What AMD ISA did this exact SPIR-V module become?

It does **not** infer native lowering from SPIR-V shape. It creates a compute
pipeline and, when the driver exposes `VK_AMD_shader_info`, asks
`vkGetShaderInfoAMD(..., VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD, ...)` for the
compiled shader's own disassembly.

For the included HLSL `msad4` experiment, the exact one-instruction target is
`v_mqsad_u32_u8`. The report labels that result **native-mqsad**. Other
SAD-family mnemonics are preserved as specialized partial lowering, and
ordinary arithmetic is not relabeled as native support.

## APUSR optical-flow SAD/QSad probes

Before comparing native code, verify the source-level arithmetic contracts:

```powershell
python utils\amd_vulkan_isa_probe\sad_semantics.py
```

This checks the current APUSR packed SWAR byte-absolute-difference helper,
the rolling four-window QSad construction, and QSad equivalence to `msad4`
under FidelityFX's required nonzero-luma input contract. A zero-reference-byte
negative control is included so the test also proves why that contract matters.

The installed-driver comparison is one run:

```powershell
python utils\amd_vulkan_isa_probe\apusr_qsad_probe.py `
  --dxc <path-to-forked-dxc.exe> `
  --spirv-dis <path-to-spirv-dis.exe> `
  --driver-probe out\amd-vulkan-isa-probe\Release\amd_vulkan_isa_probe.exe
```

The probe defaults to `cs_6_6` and Vulkan 1.2, matching the relevant APUSR
Vulkan frame-generation compiler contract. It runs three cases over the same
hot 8-row x 2-QSad search shape:

1. `apusr.qsad.msad4-scalar`: ordinary DXC `msad4` expansion.
2. `apusr.qsad.msad4-udot`: the same `msad4` shader through the AMD-oriented
   packed-UDOT lowering.
3. `apusr.qsad.swar-udot`: APUSR's current Vulkan SWAR + packed-UDOT workaround.

The generated `REPORT.md` and `report.json` distinguish `not-probed`, probe
failure, successful ISA extraction with no SAD-family instruction, other
SAD-family lowering, and exact `v_mqsad_u32_u8`. Stale output files are removed
before each stage, so an earlier ISA dump cannot be mistaken for a new result.

The installed-driver result determines the next compiler move:

- MQSAD only from `msad4-scalar`: the ordinary graph preserves a Radeon pattern
  that the packed-UDOT lowering destroys; fix our AMD lowering rather than the
  shader.
- MQSAD only from `msad4-udot`: the AMD packed-UDOT graph is the useful Vulkan
  spelling; qualify and use it for the real optical-flow path.
- MQSAD from `swar-udot`: APUSR's production workaround is already recoverable
  below DXC; compare residual ISA and avoid replacing it merely for mnemonic
  aesthetics.
- MQSAD from more than one case: compare residual instruction counts and prefer
  the simplest source/compiler contract that preserves the native instruction.
- No SAD-family instruction from any case: current legal SPIR-V spellings are
  not enough on the installed Radeon compiler. The real fix then requires a
  stronger compiler/backend ingress, not another software SAD expansion.

RGA can be added to the same run as secondary evidence:

```powershell
python utils\amd_vulkan_isa_probe\apusr_qsad_probe.py `
  --dxc <path-to-forked-dxc.exe> `
  --spirv-dis <path-to-spirv-dis.exe> `
  --driver-probe out\amd-vulkan-isa-probe\Release\amd_vulkan_isa_probe.exe `
  --rga <path-to-rga.exe> `
  --rga-target gfx1151
```

Installed-driver ISA remains authoritative. RGA offline is useful target
evidence, not a substitute for the driver that actually runs APUSR.

## Build on Windows

Use a Vulkan SDK environment (or pass `-DVulkan_ROOT=...` to CMake):

```powershell
cmake -S utils/amd-vulkan-isa-probe -B out/amd-vulkan-isa-probe -A x64
cmake --build out/amd-vulkan-isa-probe --config Release
```

The probe uses C++23 and has no runtime dependency beyond the Vulkan loader.

## Direct driver query

```powershell
out\amd-vulkan-isa-probe\Release\amd_vulkan_isa_probe.exe `
  out\amd-native-probe\msad4.amd.spv `
  out\amd-native-probe\msad4.amd.driver.isa
```

Exit code 3 means the selected device does not expose `VK_AMD_shader_info`.
Exit code 4 means the extension exists but this driver does not expose the
requested disassembly. Neither result says anything about whether the shader
itself compiled natively; use RGA live-driver mode as the independent path.

The default pipeline layout matches the included probe shaders: descriptor set
0 / binding 0 is one storage buffer and the compute stage has a 32-byte push
constant range.

## RGA

RGA 2.14.2 has two useful paths:

```powershell
# Offline compiler, target-selectable. Useful evidence, not final proof of the
# installed Windows driver.
rga.exe -s vk-spv-offline -c gfx1151 --isa out\msad4.offline.isa out\msad4.amd.spv

# Live Vulkan path: compiles through the Vulkan driver. A matching .cpso may be
# supplied with --pso when the default compute pipeline state is insufficient.
rga.exe -s vulkan --comp out\msad4.amd.spv --isa out\msad4.live.isa
```

Do not treat an offline RGA hit as equivalent to a live-driver hit.
