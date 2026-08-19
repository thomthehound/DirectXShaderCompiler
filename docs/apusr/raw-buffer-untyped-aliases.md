# Untyped raw-buffer aliases

Status: **Accepted for the APUSR DXC fork; compiler/runtime integration still pending.**

## Context

The opt-in `SPV_KHR_untyped_pointers` raw-buffer path now validates for direct vector loads/stores, `GetDimensions`, 32-bit atomics, and native 64-bit atomics. The remaining failure is HLSL resource aliasing: local `ByteAddressBuffer` / `RWByteAddressBuffer` aliases and resource-valued helper parameters are represented as Function-scope variables containing an untyped `StorageBuffer` pointer.

DXC's existing HLSL legalization path was designed around typed resource pointers and assumes an `OpTypePointer` has a pointee operand. Feeding `OpTypeUntypedPointerKHR` through that path causes the current alias compiler failure. Other legalization passes also do not currently advertise support for `SPV_KHR_untyped_pointers`.

## Decision

For the APUSR untyped raw-buffer path, aliases will be emitted as legal SPIR-V directly instead of relying on the legacy HLSL resource-alias legalizer to erase them.

Mutable untyped raw-buffer aliases will use `VariablePointersStorageBuffer` together with `UntypedPointersKHR`. This preserves the existing HLSL alias semantics while keeping the raw resource pointer in the `StorageBuffer` storage class.

This decision applies only to the explicit untyped raw-buffer path. The legacy typed raw-buffer path and unrelated resource alias lowering remain unchanged.

## Runtime contract

A shader that uses untyped raw-buffer aliases therefore requires both:

- `shaderUntypedPointers` / `VK_KHR_shader_untyped_pointers`
- `variablePointersStorageBuffer`

APUSR must negotiate both features before selecting shader variants that contain untyped raw-buffer aliases. There is no emulation or silent slow fallback if `variablePointersStorageBuffer` is unavailable.

Direct untyped raw-buffer shaders that do not contain mutable aliases do not gain this additional requirement merely by using `SPV_KHR_untyped_pointers`.

## Compiler contract

The compiler implementation must:

- keep untyped raw descriptors and raw resource values in `StorageBuffer`;
- emit `OpCapability UntypedPointersKHR` when the untyped raw path is used;
- emit `OpCapability VariablePointersStorageBuffer` when a mutable untyped raw alias requires a Function-scope pointer holder;
- avoid invoking the legacy HLSL resource-alias legalization path solely for those untyped raw aliases;
- preserve the legacy typed alias path unchanged;
- continue to pass `spirv-val` for the complete raw-buffer contract.

The focused alias probe should explicitly verify the variable-pointer capability so the dependency cannot disappear accidentally.

## Rejected alternative for now

The other viable design is to extend SPIRV-Tools' HLSL legalization pipeline to understand untyped pointers and eliminate the alias holders before final validation. That would avoid the `variablePointersStorageBuffer` runtime requirement, but it requires maintaining suitable SPIRV-Tools changes and auditing multiple legalization passes rather than fixing only the first crash site.

That remains a valid future direction. If it is implemented completely, this decision can be revisited and the extra runtime feature requirement removed.

## Definition of done

The untyped alias path is complete when the focused contract produces valid SPIR-V containing `UntypedPointersKHR` and `VariablePointersStorageBuffer`, no longer enters the incompatible legacy alias legalization path, and all previously passing vector/surface/atomic cases remain unchanged. APUSR runtime negotiation must be updated before such shaders are enabled in production.