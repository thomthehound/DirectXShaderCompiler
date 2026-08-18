#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def write(path, text):
    (ROOT / path).write_text(text, encoding="utf-8")


def replace_once(path, old, new):
    text = read(path)
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one occurrence, got {count}: {old[:80]!r}")
    write(path, text.replace(old, new, 1))


def insert_before_once(path, marker, block):
    text = read(path)
    count = text.count(marker)
    if count != 1:
        raise RuntimeError(f"{path}: expected one marker, got {count}: {marker[:80]!r}")
    write(path, text.replace(marker, block + marker, 1))


# HLSL compiler intrinsics. They are deliberately direct vk:: intrinsics: AGS
# requires the indices to be immediate values, and a normal wrapper function
# would turn those literals into ordinary function parameters before SPIR-V
# emission.
replace_once(
    "utils/hct/gen_intrin_main.txt",
    "namespace VkIntrinsics {\n\nu64 [[]] ReadClock(in uint scope);",
    "namespace VkIntrinsics {\n\nu64 [[]] ReadClock(in uint scope);\n"
    "float [[rn]] AmdVertexParameterComponent(in uint vertexIdx, in uint parameterIdx, in uint componentIdx);\n"
    "float<4> [[rn]] AmdVertexParameter(in uint vertexIdx, in uint parameterIdx);",
)
replace_once("utils/hct/hlsl_intrinsic_opcodes.json", '"Num_Intrinsics": 424', '"Num_Intrinsics": 426')
replace_once(
    "utils/hct/hlsl_intrinsic_opcodes.json",
    '"IOP_VkReadClock": 223,',
    '"IOP_VkReadClock": 223,\n    "IOP_VkAmdVertexParameterComponent": 424,\n    "IOP_VkAmdVertexParameter": 425,',
)

# Generic builder support for registered extended instruction sets. This is the
# same operation createGLSLExtInst already performs, but without hard-coding the
# GLSL.std.450 import name.
replace_once(
    "tools/clang/include/clang/SPIRV/SpirvBuilder.h",
    "  /// \\brief Creates an OpExtInst instruction for the GLSL extended instruction\n",
    "  /// \\brief Creates an OpExtInst instruction for the named extended instruction\n"
    "  /// set and returns the resulting instruction pointer.\n"
    "  SpirvInstruction *createExtInst(\n"
    "      QualType resultType, llvm::StringRef setName, uint32_t instId,\n"
    "      llvm::ArrayRef<SpirvInstruction *> operands, SourceLocation,\n"
    "      SourceRange range = {});\n\n"
    "  /// \\brief Creates an OpExtInst instruction for the GLSL extended instruction\n",
)
insert_before_once(
    "tools/clang/lib/SPIRV/SpirvBuilder.cpp",
    "SpirvInstruction *\nSpirvBuilder::createGLSLExtInst(QualType resultType, GLSLstd450 inst,",
    "SpirvInstruction *SpirvBuilder::createExtInst(\n"
    "    QualType resultType, llvm::StringRef setName, uint32_t instId,\n"
    "    llvm::ArrayRef<SpirvInstruction *> operands, SourceLocation loc,\n"
    "    SourceRange range) {\n"
    "  assert(insertPoint && \"null insert point\");\n"
    "  auto *extInst = new (context) SpirvExtInst(\n"
    "      resultType, loc, getExtInstSet(setName), instId, operands, range);\n"
    "  insertPoint->addInstruction(extInst);\n"
    "  return extInst;\n"
    "}\n\n",
)

# Resolver API: translate an AGS D3D input-register row/component back to the
# actual SPIR-V fragment Input pointer. The implementation below intentionally
# does not use Vulkan Location as a proxy for the D3D parameter register.
replace_once(
    "tools/clang/lib/SPIRV/DeclResultIdMapper.h",
    "  bool createStageInputVar(const ParmVarDecl *paramDecl,\n"
    "                           SpirvInstruction **loadedValue, bool forPCF);\n",
    "  bool createStageInputVar(const ParmVarDecl *paramDecl,\n"
    "                           SpirvInstruction **loadedValue, bool forPCF);\n\n"
    "  /// Resolve an AGS raw D3D pixel-input register component to the actual\n"
    "  /// SPIR-V Input pointer. The returned componentType is the pointee type.\n"
    "  SpirvInstruction *getAmdVertexParameterComponentPtr(\n"
    "      uint32_t parameterIndex, uint32_t componentIndex,\n"
    "      QualType *componentType, SourceLocation loc);\n",
)

replace_once(
    "tools/clang/lib/SPIRV/DeclResultIdMapper.cpp",
    '#include "dxc/DXIL/DxilConstants.h"\n',
    '#include "dxc/DXIL/DxilConstants.h"\n#include "dxc/DXIL/DxilInterpolationMode.h"\n',
)
replace_once(
    "tools/clang/lib/SPIRV/DeclResultIdMapper.cpp",
    "#include <algorithm>\n#include <optional>\n#include <sstream>\n",
    "#include <algorithm>\n#include <array>\n#include <limits>\n#include <optional>\n#include <sstream>\n",
)

resolver = r'''
namespace {

// clangSPIRV deliberately does not link libHLSL, so calling
// DxilSignatureAllocator here would introduce a compiler-library dependency
// cycle. Keep this compact prefix-stable packer in lock-step with
// DxilSignatureAllocator::PackPrefixStable; the regression below specifically
// exercises component hole reuse and a Vulkan-Location/D3D-register mismatch.
struct AmdParameterElement {
  const StageVar *stageVar = nullptr;
  const ValueDecl *sourceDecl = nullptr;
  hlsl::DXIL::SemanticInterpretationKind interpretation =
      hlsl::DXIL::SemanticInterpretationKind::NA;
  hlsl::DXIL::InterpolationMode interpolation =
      hlsl::DXIL::InterpolationMode::Undefined;
  hlsl::DXIL::SignatureDataWidth dataWidth =
      hlsl::DXIL::SignatureDataWidth::Undefined;
  uint32_t rows = 0;
  uint32_t cols = 0;
  uint32_t row = std::numeric_limits<uint32_t>::max();
  uint32_t col = std::numeric_limits<uint32_t>::max();

  bool isAllocated() const {
    return row != std::numeric_limits<uint32_t>::max();
  }
  void clearLocation() {
    row = col = std::numeric_limits<uint32_t>::max();
  }
  void setLocation(uint32_t r, uint32_t c) {
    row = r;
    col = c;
  }
};

class AmdParameterPacker {
  enum ElementFlags : uint8_t {
    Occupied = 1u << 0,
    Arbitrary = 1u << 1,
    SGV = 1u << 2,
    SV = 1u << 3,
    TessFactor = 1u << 4,
    ClipCull = 1u << 5,
    ConflictsWithIndexed = SGV | SV,
  };

  struct PackedRegister {
    std::array<uint8_t, 4> flags{};
    hlsl::DXIL::InterpolationMode interpolation =
        hlsl::DXIL::InterpolationMode::Undefined;
    uint8_t indexFlags = 0;
    bool indexingFixed = false;
    hlsl::DXIL::SignatureDataWidth dataWidth =
        hlsl::DXIL::SignatureDataWidth::Undefined;
  };

public:
  explicit AmdParameterPacker(uint32_t registerCount)
      : registers(registerCount) {}

  uint32_t packPrefixStable(std::vector<AmdParameterElement *> elements,
                            uint32_t startRow, uint32_t numRows) {
    uint32_t rowsUsed = 0;

    constexpr uint32_t kClipCullRows = 2;
    uint32_t clipCullRowsUsed = 0;
    bool clipCullIndexed = false;
    AmdParameterPacker clipCullPacker(kClipCullRows);
    AmdParameterElement clipCullTemps[kClipCullRows];

    for (auto *element : elements) {
      element->clearLocation();
      switch (element->interpretation) {
      case hlsl::DXIL::SemanticInterpretationKind::Arb:
      case hlsl::DXIL::SemanticInterpretationKind::SGV:
      case hlsl::DXIL::SemanticInterpretationKind::SV:
        break;
      case hlsl::DXIL::SemanticInterpretationKind::ClipCull: {
        uint32_t row = 0, col = 0;
        const uint32_t used = clipCullPacker.findNext(
            row, col, element, 0, kClipCullRows, 0);
        if (!used)
          continue;

        if (element->rows > 1 && !clipCullIndexed) {
          if (clipCullRowsUsed == kClipCullRows &&
              clipCullTemps[0].row + 1 != clipCullTemps[1].row)
            continue;
          clipCullIndexed = true;
        }

        if (used > clipCullRowsUsed) {
          auto &temp = clipCullTemps[clipCullRowsUsed];
          temp.interpretation = element->interpretation;
          temp.interpolation = element->interpolation;
          temp.dataWidth = element->dataWidth;
          temp.rows = 1;
          temp.cols = 4;

          if (clipCullIndexed) {
            if (clipCullRowsUsed < 1) {
              temp.rows = kClipCullRows;
              rowsUsed = std::max(
                  rowsUsed, packNext(&temp, startRow, numRows, 0));
              if (!temp.isAllocated())
                continue;
              clipCullTemps[1] = temp;
              clipCullTemps[1].row = temp.row + 1;
              clipCullTemps[1].rows = 1;
            } else {
              rowsUsed = std::max(
                  rowsUsed,
                  packNext(&temp, clipCullTemps[0].row + 1,
                           clipCullTemps[0].row + 2, 0));
              if (!temp.isAllocated())
                continue;
            }
            clipCullRowsUsed = kClipCullRows;
          } else {
            rowsUsed = std::max(
                rowsUsed, packNext(&temp, startRow, numRows, 0));
            if (!temp.isAllocated())
              continue;
            clipCullRowsUsed = used;
          }
        }

        clipCullPacker.placeElement(element, row, col);
        element->setLocation(clipCullTemps[row].row, col);
        continue;
      }
      case hlsl::DXIL::SemanticInterpretationKind::TessFactor:
        if (element->rows > 1) {
          rowsUsed = std::max(
              rowsUsed, packNext(element, startRow, numRows, 3));
          continue;
        }
        break;
      default:
        continue;
      }
      rowsUsed =
          std::max(rowsUsed, packNext(element, startRow, numRows, 0));
    }
    return rowsUsed;
  }

private:
  enum Conflict {
    NoConflict,
    ConflictsWithIndexing,
    ConflictsWithIndexedTessFactor,
    ConflictsWithInterpolation,
    InsufficientComponents,
    Overlap,
    IllegalComponentOrder,
    DoesNotFit,
    DataWidthConflict,
  };

  static uint8_t getElementFlags(const AmdParameterElement *element) {
    switch (element->interpretation) {
    case hlsl::DXIL::SemanticInterpretationKind::Arb:
      return Arbitrary;
    case hlsl::DXIL::SemanticInterpretationKind::SV:
      return SV;
    case hlsl::DXIL::SemanticInterpretationKind::SGV:
      return SGV;
    case hlsl::DXIL::SemanticInterpretationKind::TessFactor:
      return TessFactor;
    case hlsl::DXIL::SemanticInterpretationKind::ClipCull:
      return ClipCull;
    default:
      return 0;
    }
  }

  static uint8_t getConflictFlagsLeft(uint8_t flags) {
    uint8_t conflicts = 0;
    if (flags & Arbitrary)
      conflicts |= SGV | SV | TessFactor | ClipCull;
    if (flags & SV)
      conflicts |= SGV;
    if (flags & TessFactor)
      conflicts |= SGV;
    if (flags & ClipCull)
      conflicts |= SGV;
    return conflicts;
  }

  static uint8_t getConflictFlagsRight(uint8_t flags) {
    uint8_t conflicts = 0;
    if (flags & SGV)
      conflicts |= Arbitrary | SV | TessFactor | ClipCull;
    if (flags & SV)
      conflicts |= Arbitrary;
    if (flags & TessFactor)
      conflicts |= Arbitrary;
    if (flags & ClipCull)
      conflicts |= Arbitrary;
    return conflicts;
  }

  static uint8_t getIndexFlags(uint32_t row, uint32_t rows) {
    return (row > 0 ? 1u : 0u) | (row + 1 < rows ? 2u : 0u);
  }

  Conflict detectRowConflict(const AmdParameterElement *element,
                             uint32_t row) const {
    if (row + element->rows > registers.size())
      return DoesNotFit;
    const uint8_t elementFlags = getElementFlags(element);
    for (uint32_t i = 0; i < element->rows; ++i) {
      const auto &reg = registers[row + i];
      const uint8_t indexFlags = getIndexFlags(i, element->rows);
      if (reg.indexFlags && (elementFlags & ConflictsWithIndexed))
        return ConflictsWithIndexing;
      if (reg.indexingFixed &&
          (uint8_t(indexFlags | reg.indexFlags) != reg.indexFlags))
        return ConflictsWithIndexing;
      if ((elementFlags & TessFactor) &&
          (uint8_t(indexFlags | reg.indexFlags) != indexFlags))
        return ConflictsWithIndexedTessFactor;
      if (reg.interpolation != hlsl::DXIL::InterpolationMode::Undefined &&
          reg.interpolation != element->interpolation)
        return ConflictsWithInterpolation;
      if (reg.dataWidth != hlsl::DXIL::SignatureDataWidth::Undefined &&
          reg.dataWidth != element->dataWidth)
        return DataWidthConflict;

      uint32_t freeWidth = 0;
      for (uint32_t c = 0; c < 4; ++c) {
        if ((reg.flags[c] & Occupied) || (reg.flags[c] & elementFlags))
          freeWidth = 0;
        else
          ++freeWidth;
        if (element->cols <= freeWidth)
          break;
      }
      if (element->cols > freeWidth)
        return InsufficientComponents;
    }
    return NoConflict;
  }

  Conflict detectColConflict(const AmdParameterElement *element,
                             uint32_t row, uint32_t col) const {
    if (col + element->cols > 4)
      return DoesNotFit;
    uint8_t elementFlags = getElementFlags(element) | Occupied;
    for (uint32_t r = 0; r < element->rows; ++r) {
      const auto &reg = registers[row + r];
      for (uint32_t c = col; c < col + element->cols; ++c) {
        if (elementFlags & reg.flags[c])
          return (reg.flags[c] & Occupied) ? Overlap : IllegalComponentOrder;
      }
    }
    return NoConflict;
  }

  void placeElement(const AmdParameterElement *element, uint32_t row,
                    uint32_t col) {
    const uint8_t elementFlags = getElementFlags(element);
    const uint8_t conflictLeft = getConflictFlagsLeft(elementFlags);
    const uint8_t conflictRight = getConflictFlagsRight(elementFlags);
    for (uint32_t r = 0; r < element->rows; ++r) {
      auto &reg = registers[row + r];
      const uint8_t indexFlags = getIndexFlags(r, element->rows);
      reg.interpolation = element->interpolation;
      reg.indexFlags |= indexFlags;
      reg.dataWidth = element->dataWidth;
      if ((elementFlags & ConflictsWithIndexed) ||
          (elementFlags & TessFactor))
        reg.indexingFixed = true;
      for (uint32_t c = 0; c < 4; ++c) {
        if (reg.flags[c] & Occupied)
          continue;
        if (c < col)
          reg.flags[c] |= conflictLeft;
        else if (c < col + element->cols)
          reg.flags[c] = Occupied | elementFlags;
        else
          reg.flags[c] |= conflictRight;
      }
    }
  }

  uint32_t findNext(uint32_t &foundRow, uint32_t &foundCol,
                    AmdParameterElement *element, uint32_t startRow,
                    uint32_t numRows, uint32_t startCol) const {
    if (element->rows > numRows || startCol + element->cols > 4)
      return 0;
    const uint32_t lastRow =
        std::min<uint32_t>(registers.size(), startRow + numRows);
    if (startRow >= lastRow || element->rows > lastRow - startRow)
      return 0;
    for (uint32_t row = startRow; row <= lastRow - element->rows; ++row) {
      if (detectRowConflict(element, row) != NoConflict)
        continue;
      for (uint32_t col = startCol; col <= 4 - element->cols; ++col) {
        if (detectColConflict(element, row, col) != NoConflict)
          continue;
        foundRow = row;
        foundCol = col;
        return row + element->rows;
      }
    }
    return 0;
  }

  uint32_t packNext(AmdParameterElement *element, uint32_t startRow,
                    uint32_t numRows, uint32_t startCol) {
    uint32_t row = 0, col = 0;
    const uint32_t rowsUsed =
        findNext(row, col, element, startRow, numRows, startCol);
    if (rowsUsed) {
      placeElement(element, row, col);
      element->setLocation(row, col);
    }
    return rowsUsed;
  }

  std::vector<PackedRegister> registers;
};

bool isAmdParameterPackableInterpretation(
    hlsl::DXIL::SemanticInterpretationKind interpretation) {
  switch (interpretation) {
  case hlsl::DXIL::SemanticInterpretationKind::Arb:
  case hlsl::DXIL::SemanticInterpretationKind::SV:
  case hlsl::DXIL::SemanticInterpretationKind::SGV:
  case hlsl::DXIL::SemanticInterpretationKind::TessFactor:
  case hlsl::DXIL::SemanticInterpretationKind::ClipCull:
    return true;
  default:
    return false;
  }
}

bool isAmdParameterAllocatedInterpretation(
    hlsl::DXIL::SemanticInterpretationKind interpretation) {
  switch (interpretation) {
  case hlsl::DXIL::SemanticInterpretationKind::NA:
  case hlsl::DXIL::SemanticInterpretationKind::NotInSig:
  case hlsl::DXIL::SemanticInterpretationKind::NotPacked:
  case hlsl::DXIL::SemanticInterpretationKind::Shadow:
    return false;
  default:
    return true;
  }
}

QualType getAmdParameterScalarType(ASTContext &context, QualType type) {
  while (const auto *arrayType = context.getAsConstantArrayType(type))
    type = arrayType->getElementType();

  QualType elementType;
  uint32_t rows = 0, cols = 0;
  if (isMxNMatrix(type, &elementType, &rows, &cols))
    return elementType;
  if (hlsl::IsHLSLVecType(type))
    return hlsl::GetHLSLVecElementType(type);
  return type;
}

hlsl::DXIL::SignatureDataWidth getAmdParameterDataWidth(
    ASTContext &context, QualType type, bool native16BitTypes) {
  type = getAmdParameterScalarType(context, type);
  if (type->isBooleanType())
    return hlsl::DXIL::SignatureDataWidth::Bits32;
  const uint64_t width = context.getTypeSize(type);
  if (width == 16)
    return native16BitTypes ? hlsl::DXIL::SignatureDataWidth::Bits16
                            : hlsl::DXIL::SignatureDataWidth::Bits32;
  if (width <= 32)
    return hlsl::DXIL::SignatureDataWidth::Bits32;
  return hlsl::DXIL::SignatureDataWidth::Undefined;
}

hlsl::DXIL::InterpolationMode getAmdParameterInterpolation(
    const ValueDecl *decl, ASTContext &context) {
  hlsl::InterpolationMode mode(
      decl->hasAttr<HLSLNoInterpolationAttr>(), decl->hasAttr<HLSLLinearAttr>(),
      decl->hasAttr<HLSLNoPerspectiveAttr>(), decl->hasAttr<HLSLCentroidAttr>(),
      decl->hasAttr<HLSLSampleAttr>());
  if (mode.IsUndefined()) {
    const QualType scalar = getAmdParameterScalarType(context, decl->getType());
    mode = scalar->isFloatingType()
               ? hlsl::InterpolationMode::Kind::Linear
               : hlsl::InterpolationMode::Kind::Constant;
  }
  return mode.GetKind();
}

SpirvInstruction *getAmdParameterComponentPtr(
    ASTContext &context, SpirvBuilder &builder, QualType type,
    SpirvInstruction *base, uint32_t row, uint32_t component,
    SourceLocation loc, QualType *componentType) {
  if (const auto *arrayType = context.getAsConstantArrayType(type)) {
    const QualType elementType = arrayType->getElementType();
    const auto elementShape = getLocationAndComponentCount(context, elementType);
    if (elementShape.location == 0)
      return nullptr;
    const uint32_t arrayIndex = row / elementShape.location;
    if (arrayIndex >= arrayType->getSize().getZExtValue())
      return nullptr;
    auto *elementPtr = builder.createAccessChain(
        elementType, base,
        {builder.getConstantInt(context.UnsignedIntTy,
                                llvm::APInt(32, arrayIndex))},
        loc);
    return getAmdParameterComponentPtr(
        context, builder, elementType, elementPtr,
        row % elementShape.location, component, loc, componentType);
  }

  QualType matrixElementType;
  uint32_t matrixRows = 0, matrixCols = 0;
  if (isMxNMatrix(type, &matrixElementType, &matrixRows, &matrixCols)) {
    if (row >= matrixRows || component >= matrixCols)
      return nullptr;
    const QualType vectorType =
        context.getExtVectorType(matrixElementType, matrixCols);
    auto *rowPtr = builder.createAccessChain(
        vectorType, base,
        {builder.getConstantInt(context.UnsignedIntTy, llvm::APInt(32, row))},
        loc);
    *componentType = matrixElementType;
    return builder.createAccessChain(
        matrixElementType, rowPtr,
        {builder.getConstantInt(context.UnsignedIntTy,
                                llvm::APInt(32, component))},
        loc);
  }

  if (hlsl::IsHLSLVecType(type)) {
    const uint32_t count = hlsl::GetHLSLVecSize(type);
    if (row != 0 || component >= count)
      return nullptr;
    const QualType elementType = hlsl::GetHLSLVecElementType(type);
    *componentType = elementType;
    return builder.createAccessChain(
        elementType, base,
        {builder.getConstantInt(context.UnsignedIntTy,
                                llvm::APInt(32, component))},
        loc);
  }

  if (row != 0 || component != 0)
    return nullptr;
  *componentType = type;
  return base;
}

} // namespace

SpirvInstruction *DeclResultIdMapper::getAmdVertexParameterComponentPtr(
    uint32_t parameterIndex, uint32_t componentIndex,
    QualType *componentType, SourceLocation loc) {
  assert(componentType);
  if (!spvContext.isPS()) {
    emitError("AMD vertex-parameter access is only valid in a pixel shader",
              loc);
    return nullptr;
  }

  std::vector<AmdParameterElement> elementStorage;
  elementStorage.reserve(stageVars.size());

  for (const auto &stageVar : stageVars) {
    if (!stageVar.getSigPoint() ||
        stageVar.getSigPoint()->GetKind() != hlsl::SigPoint::Kind::PSIn ||
        stageVar.getStorageClass() != spv::StorageClass::Input ||
        !stageVar.getSemanticInfo().isValid())
      continue;

    const auto interpretation = hlsl::SigPoint::GetInterpretation(
        stageVar.getSemanticInfo().getKind(),
        stageVar.getSigPoint()->GetKind(), spvContext.getMajorVersion(),
        spvContext.getMinorVersion());
    if (!isAmdParameterAllocatedInterpretation(interpretation))
      continue;
    if (!isAmdParameterPackableInterpretation(interpretation)) {
      emitError("AMD vertex-parameter resolver encountered an unsupported D3D "
                "signature interpretation",
                loc);
      return nullptr;
    }

    const ValueDecl *sourceDecl = nullptr;
    for (const auto &entry : stageVarInstructions) {
      if (entry.second == stageVar.getSpirvInstr()) {
        sourceDecl = entry.first;
        break;
      }
    }
    if (!sourceDecl) {
      emitError("AMD vertex-parameter resolver could not recover the source "
                "pixel-input declaration",
                loc);
      return nullptr;
    }

    const auto shape = stageVar.getLocationAndComponentCount();
    AmdParameterElement element;
    element.stageVar = &stageVar;
    element.sourceDecl = sourceDecl;
    element.interpretation = interpretation;
    element.interpolation =
        getAmdParameterInterpolation(sourceDecl, astContext);
    element.dataWidth = getAmdParameterDataWidth(
        astContext, sourceDecl->getType(), spirvOptions.enable16BitTypes);
    element.rows = shape.location;
    element.cols = shape.component;
    elementStorage.push_back(element);
  }

  std::vector<AmdParameterElement *> elements;
  elements.reserve(elementStorage.size());
  for (auto &element : elementStorage)
    elements.push_back(&element);

  constexpr uint32_t kD3DInputRegisterCount = 32;
  AmdParameterPacker packer(kD3DInputRegisterCount);
  packer.packPrefixStable(elements, 0, kD3DInputRegisterCount);

  for (const auto &element : elementStorage) {
    if (!element.isAllocated()) {
      emitError("AMD vertex-parameter resolver could not pack the D3D pixel "
                "input signature",
                loc);
      return nullptr;
    }
    if (parameterIndex < element.row ||
        parameterIndex >= element.row + element.rows ||
        componentIndex < element.col ||
        componentIndex >= element.col + element.cols)
      continue;

    if (element.stageVar->isSpirvBuitin()) {
      emitError("AMD vertex-parameter register component resolves to a Vulkan "
                "builtin rather than an interpolant",
                loc);
      return nullptr;
    }

    return getAmdParameterComponentPtr(
        astContext, spvBuilder, element.stageVar->getAstType(),
        element.stageVar->getSpirvInstr(), parameterIndex - element.row,
        componentIndex - element.col, loc, componentType);
  }

  emitError("AMD vertex-parameter register component is not backed by a pixel "
            "shader input",
            loc);
  return nullptr;
}

'''
insert_before_once(
    "tools/clang/lib/SPIRV/DeclResultIdMapper.cpp",
    "std::vector<SpirvVariableLike *>\nDeclResultIdMapper::collectStageVars(SpirvFunction *entryPoint) const {",
    resolver,
)

# Emitter declarations and opcode dispatch.
replace_once(
    "tools/clang/lib/SPIRV/SpirvEmitter.h",
    "  SpirvInstruction *processVkReadClock(const CallExpr *callExpr);\n",
    "  SpirvInstruction *processVkReadClock(const CallExpr *callExpr);\n"
    "  SpirvInstruction *processAmdVertexParameter(const CallExpr *callExpr);\n"
    "  SpirvInstruction *\n"
    "  processAmdVertexParameterComponent(const CallExpr *callExpr);\n"
    "  SpirvInstruction *emitAmdVertexParameterComponent(\n"
    "      uint32_t vertexIndex, uint32_t parameterIndex,\n"
    "      uint32_t componentIndex, SourceLocation loc, SourceRange range);\n",
)

emitter = read("tools/clang/lib/SPIRV/SpirvEmitter.cpp")
needle = "case hlsl::IntrinsicOp::IOP_VkReadClock:"
pos = emitter.find(needle)
if pos < 0:
    raise RuntimeError("SpirvEmitter.cpp: VkReadClock switch case not found")
# Insert two cases immediately before VkReadClock; formatting does not depend on
# the exact body of the existing case.
emitter = emitter[:pos] + (
    "case hlsl::IntrinsicOp::IOP_VkAmdVertexParameter:\n"
    "    retVal = processAmdVertexParameter(callExpr);\n"
    "    break;\n"
    "  case hlsl::IntrinsicOp::IOP_VkAmdVertexParameterComponent:\n"
    "    retVal = processAmdVertexParameterComponent(callExpr);\n"
    "    break;\n  "
) + emitter[pos:]
write("tools/clang/lib/SPIRV/SpirvEmitter.cpp", emitter)

emitter_impl = r'''
namespace {

bool evaluateAmdImmediate(const Expr *expr, ASTContext &astContext,
                          uint32_t *value) {
  Expr::EvalResult eval;
  if (!expr->EvaluateAsRValue(eval, astContext) || eval.HasSideEffects ||
      !eval.Val.isInt())
    return false;
  *value = static_cast<uint32_t>(eval.Val.getInt().getZExtValue());
  return true;
}

} // namespace

SpirvInstruction *SpirvEmitter::emitAmdVertexParameterComponent(
    uint32_t vertexIndex, uint32_t parameterIndex, uint32_t componentIndex,
    SourceLocation loc, SourceRange range) {
  QualType componentType;
  auto *componentPtr = declIdMapper.getAmdVertexParameterComponentPtr(
      parameterIndex, componentIndex, &componentType, loc);
  if (!componentPtr)
    return nullptr;

  if (astContext.getTypeSize(componentType) != 32) {
    emitError("AMD vertex-parameter access currently requires a 32-bit pixel "
              "input component; narrower/wider raw-register transport is not "
              "represented by SPV_AMD_shader_explicit_vertex_parameter",
              loc);
    return nullptr;
  }

  auto *vertex = spvBuilder.getConstantInt(
      astContext.UnsignedIntTy, llvm::APInt(32, vertexIndex));
  spvBuilder.requireCapability(spv::Capability::InterpolationFunction, loc);
  spvBuilder.requireExtension("SPV_AMD_shader_explicit_vertex_parameter", loc);
  auto *raw = spvBuilder.createExtInst(
      componentType, "SPV_AMD_shader_explicit_vertex_parameter",
      /* InterpolateAtVertexAMD */ 1, {componentPtr, vertex}, loc, range);

  if (componentType->isSpecificBuiltinType(BuiltinType::Float))
    return raw;
  return spvBuilder.createUnaryOp(spv::Op::OpBitcast, astContext.FloatTy, raw,
                                  loc, range);
}

SpirvInstruction *
SpirvEmitter::processAmdVertexParameterComponent(const CallExpr *callExpr) {
  if (!spvContext.isPS()) {
    emitError("vk::AmdVertexParameterComponent is only valid in a pixel shader",
              callExpr->getExprLoc());
    return nullptr;
  }
  if (callExpr->getNumArgs() != 3)
    llvm_unreachable("AmdVertexParameterComponent argument count mismatch");

  uint32_t vertex = 0, parameter = 0, component = 0;
  if (!evaluateAmdImmediate(callExpr->getArg(0), astContext, &vertex) ||
      !evaluateAmdImmediate(callExpr->getArg(1), astContext, &parameter) ||
      !evaluateAmdImmediate(callExpr->getArg(2), astContext, &component)) {
    emitError("AMD vertex-parameter indices must be compile-time integer "
              "constants",
              callExpr->getExprLoc());
    return nullptr;
  }
  if (vertex > 2 || parameter > 31 || component > 3) {
    emitError("AMD vertex-parameter index is outside the AGS range "
              "(vertex 0..2, parameter 0..31, component 0..3)",
              callExpr->getExprLoc());
    return nullptr;
  }
  return emitAmdVertexParameterComponent(
      vertex, parameter, component, callExpr->getExprLoc(),
      callExpr->getSourceRange());
}

SpirvInstruction *
SpirvEmitter::processAmdVertexParameter(const CallExpr *callExpr) {
  if (!spvContext.isPS()) {
    emitError("vk::AmdVertexParameter is only valid in a pixel shader",
              callExpr->getExprLoc());
    return nullptr;
  }
  if (callExpr->getNumArgs() != 2)
    llvm_unreachable("AmdVertexParameter argument count mismatch");

  uint32_t vertex = 0, parameter = 0;
  if (!evaluateAmdImmediate(callExpr->getArg(0), astContext, &vertex) ||
      !evaluateAmdImmediate(callExpr->getArg(1), astContext, &parameter)) {
    emitError("AMD vertex-parameter indices must be compile-time integer "
              "constants",
              callExpr->getExprLoc());
    return nullptr;
  }
  if (vertex > 2 || parameter > 31) {
    emitError("AMD vertex-parameter index is outside the AGS range "
              "(vertex 0..2, parameter 0..31)",
              callExpr->getExprLoc());
    return nullptr;
  }

  llvm::SmallVector<SpirvInstruction *, 4> values;
  for (uint32_t component = 0; component < 4; ++component) {
    auto *value = emitAmdVertexParameterComponent(
        vertex, parameter, component, callExpr->getExprLoc(),
        callExpr->getSourceRange());
    if (!value)
      return nullptr;
    values.push_back(value);
  }
  return spvBuilder.createCompositeConstruct(
      astContext.getExtVectorType(astContext.FloatTy, 4), values,
      callExpr->getExprLoc(), callExpr->getSourceRange());
}

'''
# Put the implementation immediately before the existing VkReadClock processor.
marker_match = re.search(
    r"SpirvInstruction\s*\*\s*SpirvEmitter::processVkReadClock\s*\(",
    read("tools/clang/lib/SPIRV/SpirvEmitter.cpp"),
)
if not marker_match:
    raise RuntimeError("SpirvEmitter.cpp: processVkReadClock implementation not found")
text = read("tools/clang/lib/SPIRV/SpirvEmitter.cpp")
write(
    "tools/clang/lib/SPIRV/SpirvEmitter.cpp",
    text[:marker_match.start()] + emitter_impl + text[marker_match.start():],
)

# Document the raw compiler spellings alongside the existing source-visible
# pointer form. Do not wrap them in a normal function: doing so would lose the
# immediate-index contract before the emitter sees the call.
replace_once(
    "tools/clang/lib/Headers/hlsl/vk/amd/intrinsics.h",
    "// SPV_AMD_shader_explicit_vertex_parameter. The source operand must remain a\n",
    "// Raw AGS signature-register forms are compiler intrinsics because their\n"
    "// indices must remain immediate values through AST emission:\n"
    "//   vk::AmdVertexParameter(vertexIdx, parameterIdx)\n"
    "//   vk::AmdVertexParameterComponent(vertexIdx, parameterIdx, componentIdx)\n"
    "// They resolve D3D parameter-register packing independently of Vulkan\n"
    "// Location and lower to the same InterpolateAtVertexAMD instruction below.\n\n"
    "// SPV_AMD_shader_explicit_vertex_parameter. The source operand must remain a\n",
)

valid_test = r'''// RUN: %dxc -T ps_6_0 -E main -fcgl -spirv -fspv-extension=AMD %s | FileCheck %s

#include <vk/amd/intrinsics.h>

// D3D prefix-stable packing places these two float2 arbitrary inputs in the
// same parameter register. Their explicit Vulkan locations deliberately do not
// match that D3D register number.
struct PSIn {
  [[vk::location(7)]] float2 lo : TEXCOORD0;
  [[vk::location(9)]] float2 hi : TEXCOORD1;
};

// CHECK: OpCapability InterpolationFunction
// CHECK: OpExtension "SPV_AMD_shader_explicit_vertex_parameter"
// CHECK: [[INTERP:%[0-9]+]] = OpExtInstImport "SPV_AMD_shader_explicit_vertex_parameter"
// CHECK-DAG: OpDecorate [[LO:%[a-zA-Z0-9_]+]] Location 7
// CHECK-DAG: OpDecorate [[HI:%[a-zA-Z0-9_]+]] Location 9
// CHECK: [[LO]] = OpVariable %_ptr_Input_v2float Input
// CHECK: [[HI]] = OpVariable %_ptr_Input_v2float Input
// CHECK-DAG: [[LO0:%[0-9]+]] = OpAccessChain %_ptr_Input_float [[LO]] %uint_0
// CHECK-DAG: [[LO1:%[0-9]+]] = OpAccessChain %_ptr_Input_float [[LO]] %uint_1
// CHECK-DAG: [[HI0:%[0-9]+]] = OpAccessChain %_ptr_Input_float [[HI]] %uint_0
// CHECK-DAG: [[HI1:%[0-9]+]] = OpAccessChain %_ptr_Input_float [[HI]] %uint_1
// CHECK-DAG: OpExtInst %float [[INTERP]] InterpolateAtVertexAMD [[LO0]] %uint_1
// CHECK-DAG: OpExtInst %float [[INTERP]] InterpolateAtVertexAMD [[LO1]] %uint_1
// CHECK-DAG: OpExtInst %float [[INTERP]] InterpolateAtVertexAMD [[HI0]] %uint_1
// CHECK-DAG: OpExtInst %float [[INTERP]] InterpolateAtVertexAMD [[HI1]] %uint_1

float4 main(PSIn input) : SV_Target {
  return vk::AmdVertexParameter(1u, 0u);
}
'''
write("tools/clang/test/CodeGenSPIRV/amd.intrinsics.vertex-parameter-index.hlsl", valid_test)

invalid_test = r'''// RUN: not %dxc -T ps_6_0 -E main -fcgl -spirv -fspv-extension=AMD %s 2>&1 | FileCheck %s

#include <vk/amd/intrinsics.h>

struct PSIn {
  float2 value : TEXCOORD0;
  nointerpolation uint parameter : TEXCOORD1;
};

// CHECK: error: AMD vertex-parameter indices must be compile-time integer constants
float4 main(PSIn input) : SV_Target {
  return vk::AmdVertexParameter(1u, input.parameter);
}
'''
write("tools/clang/test/CodeGenSPIRV/amd.intrinsics.vertex-parameter-index.invalid.hlsl", invalid_test)

# One-command regression coverage.
insert_before_once(
    "utils/amd_spirv_ci.py",
    "    require_success(\n        \"AMD exact core math contracts\",",
    "    require_success(\n"
    "        \"AMD raw vertex-parameter index parity\",\n"
    "        compile_shader(\n"
    "            \"amd.intrinsics.vertex-parameter-index.hlsl\",\n"
    "            \"-T\", \"ps_6_0\", \"-E\", \"main\", \"-fcgl\", \"-spirv\",\n"
    "            \"-fspv-extension=AMD\",\n"
    "        ),\n"
    "        required=(\n"
    "            r'OpExtension \"SPV_AMD_shader_explicit_vertex_parameter\"',\n"
    "            r\"\\bInterpolateAtVertexAMD\\b\",\n"
    "            r\"Location 7\\b\",\n"
    "            r\"Location 9\\b\",\n"
    "        ),\n"
    "        counts=((r\"\\bInterpolateAtVertexAMD\\b\", 4),),\n"
    "    )\n\n"
    "    require_failure(\n"
    "        \"AMD raw vertex-parameter immediate enforcement\",\n"
    "        compile_shader(\n"
    "            \"amd.intrinsics.vertex-parameter-index.invalid.hlsl\",\n"
    "            \"-T\", \"ps_6_0\", \"-E\", \"main\", \"-fcgl\", \"-spirv\",\n"
    "            \"-fspv-extension=AMD\",\n"
    "        ),\n"
    "        \"AMD vertex-parameter indices must be compile-time integer constants\",\n"
    "    )\n\n",
)

# Contract ledger and blocker cleanup. JSON is edited structurally so only the
# genuinely solved blocker is removed; other TODOs remain intact.
import json
contracts_path = ROOT / "utils/amd_spirv_contracts.json"
contracts = json.loads(contracts_path.read_text(encoding="utf-8"))
for row in contracts.get("ags_dx12_parity", []):
    if row.get("opcode") == "0x0e":
        row["spirv"] = (
            "SPV_AMD_shader_explicit_vertex_parameter InterpolateAtVertexAMD, "
            "including compiler-native AGS D3D parameter-register/component "
            "index resolution independent of Vulkan Location"
        )
        row["status"] = "native-contract"
        row["implementation"] = "implemented"
        row["test"] = (
            "amd.intrinsics.explicit-vertex.hlsl; "
            "amd.intrinsics.vertex-parameter-index.hlsl"
        )
        row["note"] = (
            "vk::AmdVertexParameter and vk::AmdVertexParameterComponent enforce "
            "the AGS immediate index ranges and reproduce DXC prefix-stable D3D "
            "input-register packing before selecting the actual SPIR-V Input pointer."
        )
    if row.get("surface") == "BF16 dot/conversion":
        row["spirv"] = (
            "first-class BFloat16KHR scalar/vector types, exact BF16 mixed-dot "
            "contracts, and scalar/vector OpFConvert ingress are implemented"
        )
        row["status"] = "native-contract"
        row["implementation"] = "implemented"
        row["note"] = (
            "Specific bit-exact F32->BF16 directed/stochastic rounding guarantees "
            "remain separately tracked; basic BF16 type/conversion ingress is no "
            "longer blocked."
        )
contracts_path.write_text(json.dumps(contracts, indent=2) + "\n", encoding="utf-8")

blockers_path = ROOT / "utils/apusr_amd_ingress_blockers.json"
blockers = json.loads(blockers_path.read_text(encoding="utf-8"))
blockers["blockers"] = [
    item for item in blockers.get("blockers", [])
    if item.get("id") != "ags-vtxparam-raw-signature-index"
]
blockers_path.write_text(json.dumps(blockers, indent=2) + "\n", encoding="utf-8")

print("raw AGS VertexParameter patch staged")
