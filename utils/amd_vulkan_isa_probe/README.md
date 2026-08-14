# AMD Vulkan ISA probe

This utility answers one narrow question with the installed Vulkan driver:

> What AMD ISA did this exact SPIR-V module become?

It does **not** infer native lowering from SPIR-V shape. It creates a compute
pipeline and, when the driver exposes `VK_AMD_shader_info`, asks
`vkGetShaderInfoAMD(..., VK_SHADER_INFO_TYPE_DISASSEMBLY_AMD, ...)` for the
compiled shader's own disassembly.

For the included HLSL `msad4` experiment, the exact one-instruction target is
`v_mqsad_u32_u8`. The report labels that result **EXACT MQSAD**. Other
SAD-family mnemonics are preserved in the report as specialized partial
lowering, and ordinary arithmetic is not relabeled as native support.

## Build on Windows

Use a Vulkan SDK environment (or pass `-DVulkan_ROOT=...` to CMake):

```powershell
cmake -S utils/amd_vulkan_isa_probe -B out/amd-vulkan-isa-probe -A x64
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
