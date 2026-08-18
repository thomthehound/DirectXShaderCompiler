// RUN: %dxc -T cs_6_6 -E main -fcgl -spirv -fspv-target-env=vulkan1.2 -fspv-extension=SPV_EXT_shader_image_int64 %s | FileCheck %s

// AGS carries texture atomics through RWTexture*<uint2>, but the semantic
// operation is one unsigned 64-bit atomic value. Vulkan expresses that directly
// as a 64-bit storage-image texel when shaderImageInt64Atomics is enabled.
// CHECK: OpExtension "SPV_EXT_shader_image_int64"
// CHECK: OpCapability Int64
// CHECK: OpCapability Int64Atomics
// CHECK: OpCapability Int64ImageEXT
// CHECK: OpAtomicIAdd
// CHECK: OpAtomicAnd
// CHECK: OpAtomicOr
// CHECK: OpAtomicXor
// CHECK: OpAtomicUMin
// CHECK: OpAtomicUMax
// CHECK: OpAtomicExchange
// CHECK: OpAtomicCompareExchange

RWTexture2D<uint64_t> Image : register(u0);
RWStructuredBuffer<uint4> Out : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint2 p = tid.xy;
  uint64_t value = (uint64_t(tid.x + 1u) << 32u) | uint64_t(tid.y + 1u);
  uint64_t oldAdd, oldAnd, oldOr, oldXor, oldMin, oldMax, oldExchange, oldCmp;

  InterlockedAdd(Image[p], value, oldAdd);
  InterlockedAnd(Image[p], value | uint64_t(1u), oldAnd);
  InterlockedOr(Image[p], value, oldOr);
  InterlockedXor(Image[p], value, oldXor);
  InterlockedMin(Image[p], value, oldMin);
  InterlockedMax(Image[p], value, oldMax);
  InterlockedExchange(Image[p], value, oldExchange);
  InterlockedCompareExchange(Image[p], value, value + uint64_t(1u), oldCmp);

  uint64_t fold0 = oldAdd ^ oldAnd ^ oldOr ^ oldXor;
  uint64_t fold1 = oldMin ^ oldMax ^ oldExchange ^ oldCmp;
  uint index = tid.y * 8u + tid.x;
  Out[index] = uint4(uint(fold0), uint(fold0 >> 32u),
                     uint(fold1), uint(fold1 >> 32u));
}
