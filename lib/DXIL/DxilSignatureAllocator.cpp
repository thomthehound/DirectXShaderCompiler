//===-- DxilSignatureAllocator.cpp - Shared HLSL signature packing --------===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// The allocator is part of the DXIL common library, not the HLSL lowering
// library: both DXIL generation and SPIR-V interop need the same D3D register
// packing rules without introducing a dependency on LLVMHLSL.
#include "dxc/HLSL/DxilSignatureAllocator.h"
#include "dxc/Support/Global.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "dxc/HLSL/DxilSignatureAllocator.inl"
