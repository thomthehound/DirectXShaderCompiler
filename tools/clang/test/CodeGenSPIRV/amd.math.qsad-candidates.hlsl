// RUN: %dxc -T cs_6_2 -E main -fcgl -spirv %s | FileCheck %s

#include <vk/amd/math.h>

// Keep all four rolling windows and all four packed accumulator lanes visible.
// The source is 64-bit and every window shifts by exactly one byte.
// CHECK: OpTypeInt 64 0
// CHECK: OpShiftRightLogical
// CHECK: OpBitFieldUExtract
// CHECK: OpSelect
// CHECK: OpBitwiseOr
// CHECK: OpShiftLeftLogical

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint64_t source = (uint64_t(tid.x * 0x45d9f3bu + 0x12345678u) << 32u) |
                    uint64_t(tid.x * 0x27d4eb2du + 0x89abcdefu);
  uint reference = tid.x * 0x0103070fu + 0x00112233u;
  uint64_t accum16 = (uint64_t(tid.x + 7u) << 48u) |
                     (uint64_t(tid.x + 5u) << 32u) |
                     (uint64_t(tid.x + 3u) << 16u) |
                     uint64_t(tid.x + 1u);
  uint4 accum32 = uint4(tid.x + 11u, tid.x + 13u, tid.x + 17u, tid.x + 19u);

  uint64_t q = vk::amd::QsadPkU16U8(source, reference, accum16);
  uint64_t mq = vk::amd::MqsadPkU16U8(source, reference, accum16);
  uint4 m32 = vk::amd::MqsadU32U8(source, reference, accum32);

  Out[tid.x] = uint4(uint(q), uint(q >> 32u), uint(mq) ^ m32.x,
                     uint(mq >> 32u) ^ m32.y ^ m32.z ^ m32.w);
}
