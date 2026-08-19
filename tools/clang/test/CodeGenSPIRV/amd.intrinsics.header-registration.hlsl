// RUN: %dxc -T cs_6_2 -E main -spirv -fspv-extension=AMD %s -Fo %t.spv

#include <vk/amd/intrinsics.h>

[numthreads(1, 1, 1)]
void main() {}
