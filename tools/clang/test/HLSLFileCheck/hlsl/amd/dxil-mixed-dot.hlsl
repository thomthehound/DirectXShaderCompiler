// RUN: %dxc -T cs_6_2 -E main -enable-16bit-types -Fc %t.ll %s
// RUN: FileCheck %s < %t.ll

// CHECK: dx.op.dot2.f16
// CHECK: dx.op.dot2.f32
// CHECK: fpext half

RWStructuredBuffer<uint4> Out : register(u0);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  float x = float(tid.x) * (1.0f / 64.0f) + 0.125f;
  float16_t2 a = float16_t2(x, x * 0.5f + 0.25f);
  float16_t2 b = float16_t2(x * 0.75f + 0.125f, x * 0.25f + 0.5f);
  float16_t acc16 = float16_t(x * 0.125f);

  // Native-half graph: useful for v_dot2_f16-style recovery.
  float16_t h = dot(a, b) + acc16;

  // Mixed-precision semantic graph: inputs are widened before multiplication,
  // matching the FP16-input/FP32-accumulation contract rather than computing a
  // half-precision dot and widening the already-rounded result.
  float f = dot(float2(a), float2(b)) + float(acc16);

  Out[tid.x] = uint4(asuint(f), uint(asuint16(h)), asuint(x), tid.x);
}
