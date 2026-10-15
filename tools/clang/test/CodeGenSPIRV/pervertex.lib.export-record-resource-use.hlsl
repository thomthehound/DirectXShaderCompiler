// RUN: %dxc -T lib_6_6 -E main -fcgl -spirv -fspv-target-env=universal1.5 %s | FileCheck %s

// An exported resource-load result uses the source record without widening,
// even when the same record is used by a fragment input.
// CHECK: OpEntryPoint Fragment
// CHECK: PerVertexKHR

struct SharedData {
  nointerpolation float4 color : TEXCOORD0;
};

StructuredBuffer<SharedData> externalData : register(t0);

export float4 exportedRead(uint index) {
  return externalData[index].color;
}

[shader("pixel")]
float4 pixelMain(SharedData input) : SV_Target {
  return GetAttributeAtVertex(input.color, 1);
}
