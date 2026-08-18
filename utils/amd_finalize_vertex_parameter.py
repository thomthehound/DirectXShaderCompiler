#!/usr/bin/env python3
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def write(path, text):
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(path, old, new):
    text = read(path)
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one occurrence, got {count}: {old[:100]!r}")
    write(path, text.replace(old, new, 1))


# Keep HCT allocation canonical: newly-added intrinsic names append in lexical
# key order. The staging helper predates this correction.
opcode_path = ROOT / "utils/hct/hlsl_intrinsic_opcodes.json"
data = json.loads(opcode_path.read_text(encoding="utf-8"))
ops = data["IntrinsicOpCodes"]
ops["IOP_VkAmdVertexParameter"] = 424
ops["IOP_VkAmdVertexParameterComponent"] = 425
ops["Num_Intrinsics"] = 426
opcode_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

# Preserve Constant interpolation for nointerpolation stage variables when
# reconstructing D3D input-register packing.
replace_once(
    "tools/clang/lib/SPIRV/DeclResultIdMapper.cpp",
    "hlsl::DXIL::InterpolationMode getAmdParameterInterpolation(\n"
    "    const ValueDecl *decl, ASTContext &context) {\n"
    "  hlsl::InterpolationMode mode(\n"
    "      decl->hasAttr<HLSLNoInterpolationAttr>(), decl->hasAttr<HLSLLinearAttr>(),\n"
    "      decl->hasAttr<HLSLNoPerspectiveAttr>(), decl->hasAttr<HLSLCentroidAttr>(),\n"
    "      decl->hasAttr<HLSLSampleAttr>());",
    "hlsl::DXIL::InterpolationMode getAmdParameterInterpolation(\n"
    "    const StageVar &stageVar, const ValueDecl *decl, ASTContext &context) {\n"
    "  if (stageVar.getSpirvInstr()->isNoninterpolated())\n"
    "    return hlsl::DXIL::InterpolationMode::Constant;\n"
    "  hlsl::InterpolationMode mode(\n"
    "      decl->hasAttr<HLSLNoInterpolationAttr>(), decl->hasAttr<HLSLLinearAttr>(),\n"
    "      decl->hasAttr<HLSLNoPerspectiveAttr>(), decl->hasAttr<HLSLCentroidAttr>(),\n"
    "      decl->hasAttr<HLSLSampleAttr>());",
)
replace_once(
    "tools/clang/lib/SPIRV/DeclResultIdMapper.cpp",
    "getAmdParameterInterpolation(sourceDecl, astContext);",
    "getAmdParameterInterpolation(stageVar, sourceDecl, astContext);",
)

# Frontend intrinsic count is global. Vulkan-only intrinsics still need explicit
# DXIL lowering-table slots, matching existing vk::* compiler intrinsics.
lower_path = ROOT / "lib/HLSL/HLOperationLower.cpp"
text = lower_path.read_text(encoding="utf-8")
marker = "};\nconstexpr size_t NumLowerTableEntries ="
if text.count(marker) != 1:
    raise RuntimeError(f"HLOperationLower.cpp: lowering table boundary count={text.count(marker)}")
entries = (
    "    {IntrinsicOp::IOP_VkAmdVertexParameter, UnsupportedVulkanIntrinsic,\n"
    "     DXIL::OpCode::NumOpCodes},\n"
    "    {IntrinsicOp::IOP_VkAmdVertexParameterComponent,\n"
    "     UnsupportedVulkanIntrinsic, DXIL::OpCode::NumOpCodes},\n"
)
lower_path.write_text(text.replace(marker, entries + marker, 1), encoding="utf-8")

# Eager vk namespace declaration historically handled only a narrow set of
# fixed scalar types. VertexParameter introduces fixed float and float4 return
# types. Materialize them with DXC's existing aggregate mapper; otherwise a
# Release build falls through with an empty paramTypes vector and SIGSEGVs while
# declaring vk intrinsics, even if VertexParameter is never called.
sema_path = ROOT / "tools/clang/lib/Sema/SemaHLSL.cpp"
text = sema_path.read_text(encoding="utf-8")
fn = "SmallVector<QualType, 2> getIntrinsicFunctionParamTypes("
start = text.find(fn)
if start < 0:
    raise RuntimeError("SemaHLSL.cpp: getIntrinsicFunctionParamTypes not found")
end = text.find("\n  QualType getIntrinsicFunctionType(", start)
if end < 0:
    raise RuntimeError("SemaHLSL.cpp: getIntrinsicFunctionParamTypes end not found")
segment = text[start:end]
case = "      case LICOMPTYPE_UINT64:"
if segment.count(case) != 1:
    raise RuntimeError(f"SemaHLSL.cpp: fixed-type UINT64 case count={segment.count(case)}")
float_case = (
    "      case LICOMPTYPE_FLOAT: {\n"
    "        QualType fixedType = GetSingleQualTypeForMapping(intrinsic, i);\n"
    "        DXASSERT(!fixedType.isNull(),\n"
    "                 \"fixed float intrinsic type must have one mapping\");\n"
    "        paramTypes.push_back(fixedType);\n"
    "        break;\n"
    "      }\n"
)
segment = segment.replace(case, float_case + case, 1)
sema_path.write_text(text[:start] + segment + text[end:], encoding="utf-8")

# Promote the real contracts and remove the old raw-signature ingress blocker.
contracts_path = ROOT / "utils/amd_spirv_contracts.json"
contracts = json.loads(contracts_path.read_text(encoding="utf-8"))
for row in contracts["ags_opcodes"]:
    if row.get("opcode") == "0x0e":
        row.update({
            "spirv": "SPV_AMD_shader_explicit_vertex_parameter InterpolateAtVertexAMD, including compiler-native AGS D3D parameter-register/component index resolution independent of Vulkan Location",
            "status": "native-contract",
            "test": "amd.intrinsics.explicit-vertex.hlsl + amd.intrinsics.vertex-parameter-index.hlsl",
            "note": "vk::AmdVertexParameter and vk::AmdVertexParameterComponent enforce AGS immediate index ranges and reproduce DXC prefix-stable D3D input-register packing before selecting the actual SPIR-V Input pointer.",
        })
        break
else:
    raise RuntimeError("VertexParameter contract row missing")
for row in contracts["low_level_math"]:
    if row.get("surface") == "BF16 dot/conversion":
        row.update({
            "spirv": "first-class BFloat16KHR scalar/vector types, exact BF16 mixed-dot contracts, and scalar/vector OpFConvert ingress",
            "status": "native-contract",
            "test": "amd.bfloat16.convert.hlsl + amd.mixed-dot.hlsl + BF16 compiler/ISA probes",
            "note": "Specific bit-exact F32->BF16 directed/stochastic rounding guarantees remain separately tracked; basic BF16 type/conversion ingress is implemented.",
        })
        break
contracts_path.write_text(json.dumps(contracts, indent=2) + "\n", encoding="utf-8")

blockers_path = ROOT / "utils/apusr_amd_ingress_blockers.json"
blockers = json.loads(blockers_path.read_text(encoding="utf-8"))
# The staging helper may already have removed this row; make the cleanup idempotent.
if isinstance(blockers, list):
    blockers = [r for r in blockers if r.get("id") != "ags-vtxparam-raw-signature-index"]
elif "blockers" in blockers:
    blockers["blockers"] = [
        r for r in blockers["blockers"] if r.get("id") != "ags-vtxparam-raw-signature-index"
    ]
blockers_path.write_text(json.dumps(blockers, indent=2) + "\n", encoding="utf-8")

print("raw AGS VertexParameter finalization staged")
