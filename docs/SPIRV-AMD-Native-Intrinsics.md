# AMD-native-oriented HLSL to SPIR-V support

This fork treats preservation of specialized GPU intent as a correctness property for performance-critical shader code. It does **not** claim that DXC can make arbitrary AMD ISA instructions legal Vulkan SPIR-V. A Vulkan shader can only carry operations that the accepted SPIR-V environment defines. Where no such contract exists, the compiler either uses a legal optimized semantic lowering or, in strict mode, rejects the shader instead of pretending the native operation survived.

## Compiler controls

- `-fspv-enable-amd-intrinsics` enables AMD-oriented legal lowerings for specialized HLSL intrinsics. It may use core/KHR operations that preserve packed or cooperative semantics better than scalar expansion.
- `-fspv-require-native-intrinsics` turns missing native-preserving SPIR-V contracts into compile errors. This is an audit mode: it is intentionally stricter than ordinary Vulkan compilation.
- `-fspv-extension=AMD` (also `SPV_AMD`) enables every AMD/AMDX extension known to this DXC snapshot. Individual `SPV_AMD_*`/`SPV_AMDX_*` names remain supported.
- `<vk/amd/intrinsics.h>` exposes the four registered AMD extended-instruction sets directly to HLSL without scalar emulation.

## Specialized HLSL/DXIL coverage

| HLSL / feature | SPIR-V path in this fork | Native-intent status |
| --- | --- | --- |
| `msad4` / DXIL `IMsad` | AMD-oriented path computes four byte SAD vectors and uses packed `OpUDot` for each horizontal reduction | **No direct SPIR-V SAD/MSAD contract.** Strict native mode errors. Ordinary legacy lowering remains available when AMD mode is off. |
| Packed 4x8 integer dot (`dot4add_*packed`) | Existing `OpSDot` / `OpUDot` + `SPV_KHR_integer_dot_product` | Direct standardized packed-integer contract; retained. |
| SM 6.10 `dx::linalg` Wave matrices | `OpTypeCooperativeMatrixKHR` and `OpCooperativeMatrix*KHR` | Cooperative/WMMA intent preserved through `SPV_KHR_cooperative_matrix`. |
| SM 6.10 `dx::linalg` ThreadGroup matrices | Workgroup-scope `OpTypeCooperativeMatrixKHR` and KHR operations | Cooperative/WMMA intent preserved where the KHR operation has matching semantics. |
| SM 6.10 Thread-scope matrix/vector operations | No lowering | **No KHR/AMD cooperative-vector contract in this target.** Compiler errors instead of mapping AMD execution to another vendor's extension. |
| DXR ray tracing / ray query | Existing `SPV_KHR_ray_tracing` / `SPV_KHR_ray_query` paths | Already vendor-neutral and hardware-preserving; no AMD-private rewrite is appropriate. |

### `dx::linalg` operations currently bridged

The SPIR-V emitter recognizes all SM 6.10 linear-algebra builtin opcodes so none can silently fall through as an unknown intrinsic. The following have direct KHR implementations where their semantics match: fill/splat, copy/convert without transpose, length, element get/set, compatible groupshared load/store, matrix multiply, and matrix multiply-accumulate.

The compiler emits explicit diagnostics for operations for which the KHR cooperative-matrix contract is not equivalent: transpose, implementation-specific coordinate ownership queries, D3D accumulator-layout queries, ByteAddressBuffer forms requiring an untyped-pointer bridge that is not yet proven legal for the relevant operation, A/B-to-Accumulator `MatrixAccumulate`, interlocked matrix accumulation, and Thread-scope cooperative-vector-style operations.

These diagnostics are intentional unresolved engineering contracts, not temporary success fallbacks.

## AMD SPIR-V extension coverage

The feature manager recognizes the current registered AMD/AMDX SPIR-V extension family known to this fork. Some of these are semantic extensions that intentionally add no new core grammar token:

| Extension | Compiler treatment |
| --- | --- |
| `SPV_AMD_gcn_shader` | Recognized; extinst import automatically requests extension; HLSL wrappers for `CubeFaceIndexAMD`, `CubeFaceCoordAMD`, `TimeAMD`. |
| `SPV_AMD_gpu_shader_half_float` | Recognized as an allowed AMD extension. |
| `SPV_AMD_gpu_shader_half_float_fetch` | Recognized; `Float16ImageAMD` capability implies the extension. |
| `SPV_AMD_gpu_shader_int16` | Recognized as an allowed AMD semantic extension. |
| `SPV_AMD_shader_ballot` | Recognized; eight AMD non-uniform group opcodes infer extension/`Groups`; wrappers for the four AMD extinst operations. |
| `SPV_AMD_shader_early_and_late_fragment_tests` | Recognized; AMD execution modes infer the extension. |
| `SPV_AMD_shader_explicit_vertex_parameter` | Recognized; AMD barycentric builtins and `ExplicitInterpAMD` infer the extension; extinst import requests `InterpolationFunction`; wrapper for `InterpolateAtVertexAMD`. |
| `SPV_AMD_shader_fragment_mask` | Recognized; fragment fetch opcodes and `FragmentMaskAMD` capability infer extension/capability. |
| `SPV_AMD_shader_image_load_store_lod` | Recognized; `ImageReadWriteLodAMD` capability implies the extension. |
| `SPV_AMD_shader_trinary_minmax` | Recognized; extinst import automatically requests extension; wrappers for all nine min/max/mid operations. |
| `SPV_AMD_texture_gather_bias_lod` | Recognized; `ImageGatherBiasLodAMD` capability implies the extension. |
| `SPV_AMD_weak_linkage` | Recognized; `WeakLinkageAMD` capability implies the extension. |
| `SPV_AMDX_shader_enqueue` | Existing work-graph support retained. This remains a provisional AMDX extension rather than a production AMD contract. |

The AMD extinst wrappers intentionally emit the AMD instruction-set identity. Downstream tooling can choose to legalize old AMD operations to KHR/core equivalents, but DXC does not silently erase that identity at the source translation boundary.

## What a DXC-only fork cannot do

AMD ISA contains many operations with no Vulkan/SPIR-V opcode, extended instruction set, capability, or decoration representing them. DXC cannot legally emit `v_msad_u8`, arbitrary `V_*`/`S_*` ISA, undocumented WMMA encodings, or private ray-tracing instructions into a normal Vulkan SPIR-V module by inventing opcodes. Doing that would require a coordinated private extension across at least the SPIR-V producer, validator/toolchain, and the AMD Vulkan compiler/driver (for open AMD stacks, that means the relevant LLPC/PAL path as well).

`-fspv-require-native-intrinsics` exists specifically so these gaps cannot be mistaken for solved native support.

## Inventory and compiler boundary

The systematic AMDGPU family inventory and the DXC-to-driver compiler boundary are documented in `SPIRV-AMD-Native-Contract-Inventory.md`.

## Validation targets

DXC links SPIRV-Tools as a library and validates generated SPIR-V internally by default. The standalone `spirv-val` executable is intentionally not built by DXC's normal CMake configuration (`SPIRV_SKIP_EXECUTABLES=ON`); its absence from a DXC build directory is therefore not a regression.

Regression shaders cover:

1. legacy `msad4` behavior remains unchanged without AMD mode;
2. AMD `msad4` mode emits four packed `OpUDot` reductions and no GLSL `SAbs` chain;
3. strict `msad4` compilation fails with the native-contract diagnostic;
4. the AMD extension-family switch permits and preserves AMD extinst imports;
5. the bundled AMD intrinsic header emits registered AMD extinst operations;
6. SM 6.10 Wave cooperative-matrix multiply-accumulate emits KHR cooperative-matrix operations;
7. Thread-scope linalg rejects the vendor-incompatible fake path;
8. existing DXC KHR ray-tracing/ray-query tests remain the authoritative regression path for RT.
