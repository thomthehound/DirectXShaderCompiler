Warning: truncated output (original token count: 182017)
Total output lines: 17631

//===------- SpirvEmitter.cpp - SPIR-V Binary Code Emitter ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements a SPIR-V emitter class that takes in HLSL AST and emits
//  SPIR-V binary words.
//
//===----------------------------------------------------------------------===//

#include "SpirvEmitter.h"

#include "AlignmentSizeCalculator.h"
#include "InitListHandler.h"
#include "LowerTypeVisitor.h"
#include "RawBufferMethods.h"
#include "dxc/DXIL/DxilConstants.h"
#include "dxc/HlslIntrinsicOp.h"
#include "spirv-tools/optimizer.hpp"
#include "clang/AST/HlslTypes.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"
#include "clang/SPIRV/AstTypeProbe.h"
#include "clang/SPIRV/String.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Casting.h"

#ifdef SUPPORT_QUERY_GIT_COMMIT_INFO
#include "clang/Basic/Version.h"
#else
namespace clang {
uint32_t getGitCommitCount() { return 0; }
const char *getGitCommitHash() { return "<unknown-hash>"; }
} // namespace clang
#endif // SUPPORT_QUERY_GIT_COMMIT_INFO

namespace clang {
namespace spirv {

using spvtools::opt::DescriptorSetAndBinding;

namespace {

// Returns true if the given decl is an implicit variable declaration inside the
// "vk" namespace.
bool isImplicitVarDeclInVkNamespace(const Decl *decl) {
  if (!decl)
    return false;

  if (auto *varDecl = dyn_cast<VarDecl>(decl)) {
    // Check whether it is implicitly defined.
    if (!decl->isImplicit())
      return false;

    if (auto *nsDecl = dyn_cast<NamespaceDecl>(varDecl->getDeclContext()))
      if (nsDecl->getName().equals("vk"))
        return true;
  }
  return false;
}

// Returns true if the given decl has the given semantic.
bool hasSemantic(const DeclaratorDecl *decl,
                 hlsl::DXIL::SemanticKind semanticKind) {
  using namespace hlsl;
  for (auto *annotation : decl->getUnusualAnnotations()) {
    if (auto *semanticDecl = dyn_cast<SemanticDecl>(annotation)) {
      llvm::StringRef semanticName;
      uint32_t semanticIndex = 0;
      Semantic::DecomposeNameAndIndex(semanticDecl->SemanticName, &semanticName,
                                      &semanticIndex);
      const auto *semantic = Semantic::GetByName(semanticName);
      if (semantic->GetKind() == semanticKind)
        return true;
    }
  }
  return false;
}

const ParmVarDecl *patchConstFuncTakesHullOutputPatch(FunctionDecl *pcf) {
  for (const auto *param : pcf->parameters())
    if (hlsl::IsHLSLOutputPatchType(param->getType()))
      return param;
  return nullptr;
}

inline bool isSpirvMatrixOp(spv::Op opcode) {
  return opcode == spv::Op::OpMatrixTimesMatrix ||
         opcode == spv::Op::OpMatrixTimesVector ||
         opcode == spv::Op::OpMatrixTimesScalar;
}

/// If expr is a (RW)StructuredBuffer.Load(), returns the object and writes
/// index. Otherwiser, returns false.
// TODO: The following doesn't handle Load(int, int) yet. And it is basically a
// duplicate of doCXXMemberCallExpr.
const Expr *isStructuredBufferLoad(const Expr *expr, const Expr **index) {
  using namespace hlsl;

  if (const auto *indexing = dyn_cast<CXXMemberCallExpr>(expr)) {
    const auto *callee = indexing->getDirectCallee();
    uint32_t opcode = static_cast<uint32_t>(IntrinsicOp::Num_Intrinsics);
    llvm::StringRef group;

    if (GetIntrinsicOp(callee, opcode, group)) {
      if (static_cast<IntrinsicOp>(opcode) == IntrinsicOp::MOP_Load) {
        const auto *object = indexing->getImplicitObjectArgument();
        if (isStructuredBuffer(object->getType())) {
          *index = indexing->getArg(0);
          return indexing->getImplicitObjectArgument();
        }
      }
    }
  }

  return nullptr;
}

/// Returns true if
/// * the given expr is an DeclRefExpr referencing a kind of structured or byte
///   buffer and it is non-alias one, or
/// * the given expr is an CallExpr returning a kind of structured or byte
///   buffer.
/// * the given expr is an ArraySubscriptExpr referencing a kind of structured
///   or byte buffer.
///
/// Note: legalization specific code
bool isReferencingNonAliasStructuredOrByteBuffer(const Expr *expr) {
  expr = expr->IgnoreParenCasts();
  if (const auto *declRefExpr = dyn_cast<DeclRefExpr>(expr)) {
    if (const auto *varDecl = dyn_cast<VarDecl>(declRefExpr->getFoundDecl()))
      if (isAKindOfStructuredOrByteBuffer(varDecl->getType()))
        return SpirvEmitter::isExternalVar(varDecl);
  } else if (const auto *callExpr = dyn_cast<CallExpr>(expr)) {
    if (isAKindOfStructuredOrByteBuffer(callExpr->getType()))
      return true;
  } else if (isa<ArraySubscriptExpr>(expr)) {
    return isAKindOfStructuredOrByteBuffer(expr->getType());
  }
  return false;
}

/// Translates atomic HLSL opcodes into the equivalent SPIR-V opcode.
spv::Op translateAtomicHlslOpcodeToSpirvOpcode(hlsl::IntrinsicOp opcode) {
  using namespace hlsl;
  using namespace spv;

  switch (opcode) {
  case IntrinsicOp::IOP_InterlockedAdd:
  case IntrinsicOp::MOP_InterlockedAdd:
    return Op::OpAtomicIAdd;
  case IntrinsicOp::IOP_InterlockedAnd:
  case IntrinsicOp::MOP_InterlockedAnd:
    return Op::OpAtomicAnd;
  case IntrinsicOp::IOP_InterlockedOr:
  case IntrinsicOp::MOP_InterlockedOr:
    return Op::OpAtomicOr;
  case IntrinsicOp::IOP_InterlockedXor:
  case IntrinsicOp::MOP_InterlockedXor:
    return Op::OpAtomicXor;
  case IntrinsicOp::IOP_InterlockedUMax:
  case IntrinsicOp::MOP_InterlockedUMax:
    return Op::OpAtomicUMax;
  case IntrinsicOp::IOP_InterlockedUMin:
  case IntrinsicOp::MOP_InterlockedUMin:
    return Op::OpAtomicUMin;
  case IntrinsicOp::IOP_InterlockedMax:
  case IntrinsicOp::MOP_InterlockedMax:
    return Op::OpAtomicSMax;
  case IntrinsicOp::IOP_InterlockedMin:
  case IntrinsicOp::MOP_InterlockedMin:
    return Op::OpAtomicSMin;
  case IntrinsicOp::IOP_InterlockedExchange:
  case IntrinsicOp::MOP_InterlockedExchange:
    return Op::OpAtomicExchange;
  default:
    // Only atomic opcodes are relevant.
    break;
  }

  assert(false && "unimplemented hlsl intrinsic opcode");
  return Op::Max;
}

// Returns true if the given opcode is an accepted binary opcode in
// OpSpecConstantOp.
bool isAcceptedSpecConstantBinaryOp(spv::Op op) {
  switch (op) {
  case spv::Op::OpIAdd:
  case spv::Op::OpISub:
  case spv::Op::OpIMul:
  case spv::Op::OpUDiv:
  case spv::Op::OpSDiv:
  case spv::Op::OpUMod:
  case spv::Op::OpSRem:
  case spv::Op::OpSMod:
  case spv::Op::OpShiftRightLogical:
  case spv::Op::OpShiftRightArithmetic:
  case spv::Op::OpShiftLeftLogical:
  case spv::Op::OpBitwiseOr:
  case spv::Op::OpBitwiseXor:
  case spv::Op::OpBitwiseAnd:
  case spv::Op::OpVectorShuffle:
  case spv::Op::OpCompositeExtract:
  case spv::Op::OpCompositeInsert:
  case spv::Op::OpLogicalOr:
  case spv::Op::OpLogicalAnd:
  case spv::Op::OpLogicalNot:
  case spv::Op::OpLogicalEqual:
  case spv::Op::OpLogicalNotEqual:
  case spv::Op::OpIEqual:
  case spv::Op::OpINotEqual:
  case spv::Op::OpULessThan:
  case spv::Op::OpSLessThan:
  case spv::Op::OpUGreaterThan:
  case spv::Op::OpSGreaterThan:
  case spv::Op::OpULessThanEqual:
  case spv::Op::OpSLessThanEqual:
  case spv::Op::OpUGreaterThanEqual:
  case spv::Op::OpSGreaterThanEqual:
    return true;
  default:
    // Accepted binary opcodes return true. Anything else is false.
    return false;
  }
  return false;
}

/// Returns true if the given expression is an accepted initializer for a spec
/// constant.
bool isAcceptedSpecConstantInit(const Expr *init, ASTContext &astContext) {
  // Allow numeric casts
  init = init->IgnoreParenCasts();

  if (isa<CXXBoolLiteralExpr>(init) || isa<IntegerLiteral>(init) ||
      isa<FloatingLiteral>(init))
    return true;

  // Allow the minus operator which is used to specify negative values
  if (const auto *unaryOp = dyn_cast<UnaryOperator>(init))
    return unaryOp->getOpcode() == UO_Minus &&
           isAcceptedSpecConstantInit(unaryOp->getSubExpr(), astContext);

  // Allow values that can be evaluated to const.
  if (init->isEvaluatable(astContext)) {
    return true;
  }

  return false;
}

/// Returns true if the given function parameter can act as shader stage
/// input parameter.
inline bool canActAsInParmVar(const ParmVarDecl *param) {
  // If the parameter has no in/out/inout attribute, it is defaulted to
  // an in parameter.
  return !param->hasAttr<HLSLOutAttr>() &&
         // GS output streams are marked as inout, but it should not be
         // used as in parameter.
         !hlsl::IsHLSLStreamOutputType(param->getType()) &&
         !hlsl::IsHLSLNodeOutputType(param->getType());
}

/// Returns true if the given function parameter can act as shader stage
/// output parameter.
inline bool canActAsOutParmVar(const ParmVarDecl *param) {
  return param->hasAttr<HLSLOutAttr>() || param->hasAttr<HLSLInOutAttr>() ||
         hlsl::IsHLSLRayQueryType(param->getType());
}

/// Returns true if the given expression is of builtin type and can be evaluated
/// to a constant zero. Returns false otherwise.
inline bool evaluatesToConstZero(const Expr *expr, ASTContext &astContext) {
  const auto type = expr->getType();
  if (!type->isBuiltinType())
    return false;

  Expr::EvalResult evalResult;
  if (expr->EvaluateAsRValue(evalResult, astContext) &&
      !evalResult.HasSideEffects) {
    const auto &val = evalResult.Val;
    return ((type->isBooleanType() && !val.getInt().getBoolValue()) ||
            (type->isIntegerType() && !val.getInt().getBoolValue()) ||
            (type->isFloatingType() && val.getFloat().isZero()));
  }
  return false;
}

/// Returns the real definition of the callee of the given CallExpr.
///
/// If we are calling a forward-declared function, callee will be the
/// FunctionDecl for the foward-declared function, not the actual
/// definition. The foward-delcaration and defintion are two completely
/// different AST nodes.
inline const FunctionDecl *getCalleeDefinition(const CallExpr *expr) {
  const auto *callee = expr->getDirectCallee();

  if (callee->isThisDeclarationADefinition())
    return callee;

  // We need to update callee to the actual definition here
  if (!callee->isDefined(callee))
    return nullptr;

  return callee;
}

/// Returns the referenced definition. The given expr is expected to be a
/// DeclRefExpr or CallExpr after ignoring casts. Returns nullptr otherwise.
const DeclaratorDecl *getReferencedDef(const Expr *expr) {
  if (!expr)
    return nullptr;

  expr = expr->IgnoreParenCasts();
  while (const auto *arraySubscriptExpr = dyn_cast<ArraySubscriptExpr>(expr)) {
    expr = arraySubscriptExpr->getBase();
    expr = expr->IgnoreParenCasts();
  }

  if (const auto *declRefExpr = dyn_cast<DeclRefExpr>(expr)) {
    return dyn_cast_or_null<DeclaratorDecl>(declRefExpr->getDecl());
  }

  if (const auto *callExpr = dyn_cast<CallExpr>(expr)) {
    return getCalleeDefinition(callExpr);
  }

  return nullptr;
}

/// Returns the number of base classes if this type is a derived class/struct.
/// Returns zero otherwise.
inline uint32_t getNumBaseClasses(QualType type) {
  if (const auto *cxxDecl = type->getAsCXXRecordDecl())
    return cxxDecl->getNumBases();
  return 0;
}

/// Gets the index sequence of casting a derived object to a base object by
/// following the cast chain.
void getBaseClassIndices(const CastExpr *expr,
                         llvm::SmallVectorImpl<uint32_t> *indices) {
  assert(expr->getCastKind() == CK_UncheckedDerivedToBase ||
         expr->getCastKind() == CK_HLSLDerivedToBase);

  indices->clear();

  QualType derivedType = expr->getSubExpr()->getType();

  // There are two types of UncheckedDerivedToBase/HLSLDerivedToBase casts:
  //
  // The first is when a derived object tries to access a member in the base.
  // For example: derived.base_member.
  // ImplicitCastExpr 'Base' lvalue <UncheckedDerivedToBase (Base)>
  // `-DeclRefExpr 'Derived' lvalue Var 0x1f0d9bb2890 'derived' 'Derived'
  //
  // The second is when a pointer of the dervied is used to access members or
  // methods of the base. There are currently no pointers in HLSL, but the
  // method defintions can use the "this" pointer.
  // For example:
  // class Base { float value; };
  // class Derviced : Base {
  //   float4 getBaseValue() { return value; }
  // };
  //
  // In this example, the 'this' pointer (pointing to Derived) is used inside
  // 'getBaseValue', which is then cast to a Base pointer:
  //
  // ImplicitCastExpr 'Base *' <UncheckedDerivedToBase (Base)>
  // `-CXXThisExpr 'Derviced *' this
  //
  // Therefore in order to obtain the derivedDecl below, we must make sure that
  // we handle the second case too by using the pointee type.
  if (derivedType->isPointerType())
    derivedType = derivedType->getPointeeType();

  const auto *derivedDecl = derivedType->getAsCXXRecordDecl();

  // Go through the base cast chain: for each of the derived to base cast, find
  // the index of the base in question in the derived's bases.
  for (auto pathIt = expr->path_begin(), pathIe = expr->path_end();
       pathIt != pathIe; ++pathIt) {
    // The type of the base in question
    const auto baseType = (*pathIt)->getType();

    uint32_t index = 0;
    for (auto baseIt = derivedDecl->bases_begin(),
              baseIe = derivedDecl->bases_end();
         baseIt != baseIe; ++baseIt, ++index)
      if (baseIt->getType() == baseType) {
        indices->push_back(index);
        break;
      }

    assert(index < derivedDecl->getNumBases());

    // Continue to proceed the next base in the chain
    derivedType = baseType;
    if (derivedType->isPointerType())
      derivedType = derivedType->getPointeeType();
    derivedDecl = derivedType->getAsCXXRecordDecl();
  }
}

std::string getNamespacePrefix(const Decl *decl) {
  std::string nsPrefix = "";
  const DeclContext *dc = decl->getDeclContext();
  while (dc && !dc->isTranslationUnit()) {
    if (const NamespaceDecl *ns = dyn_cast<NamespaceDecl>(dc)) {
      if (!ns->isAnonymousNamespace()) {
        nsPrefix = ns->getName().str() + "::" + nsPrefix;
      }
    }
    dc = dc->getParent();
  }
  return nsPrefix;
}

std::string getFnName(const FunctionDecl *fn) {
  // Prefix the function name with the struct name if necessary
  std::string classOrStructName = "";
  if (const auto *memberFn = dyn_cast<CXXMethodDecl>(fn))
    if (const auto *st = dyn_cast<CXXRecordDecl>(memberFn->getDeclContext()))
      classOrStructName = st->getName().str() + ".";
  return getNamespacePrefix(fn) + classOrStructName +
         getFunctionOrOperatorName(fn, false);
}

bool isMemoryObjectDeclaration(SpirvInstruction *inst) {
  return isa<SpirvVariable>(inst) || isa<SpirvFunctionParameter>(inst);
}

// Returns a pair of the descriptor set and the binding that does not have
// bound Texture or Sampler.
DescriptorSetAndBinding getDSetBindingWithoutTextureOrSampler(
    const llvm::SmallVectorImpl<ResourceInfoToCombineSampledImage>
        &resourceInfoForSampledImages) {
  const DescriptorSetAndBinding kNotFound = {
      std::numeric_limits<uint32_t>::max(),
      std::numeric_limits<uint32_t>::max()};
  if (resourceInfoForSampledImages.empty()) {
    return kNotFound;
  }

  typedef uint8_t TextureAndSamplerExistExistance;
  const TextureAndSamplerExistExistance kTextureConfirmed = 1 << 0;
  const TextureAndSamplerExistExistance kSamplerConfirmed = 1 << 1;
  llvm::DenseMap<std::pair<uint32_t, uint32_t>, TextureAndSamplerExistExistance>
      dsetBindingsToTextureSamplerExistance;
  for (const auto &itr : resourceInfoForSampledImages) {
    auto dsetBinding = std::make_pair(itr.descriptorSet, itr.binding);

    TextureAndSamplerExistExistance status = 0;
    if (isTexture(itr.type))
      status = kTextureConfirmed;
    if (isSampler(itr.type))
      status = kSamplerConfirmed;

    auto existanceItr = dsetBindingsToTextureSamplerExistance.find(dsetBinding);
    if (existanceItr == dsetBindingsToTextureSamplerExistance.end()) {
      dsetBindingsToTextureSamplerExistance[dsetBinding] = status;
    } else {
      existanceItr->second = existanceItr->second | status;
    }
  }

  for (const auto &itr : dsetBindingsToTextureSamplerExistance) {
    if (itr.second != (kTextureConfirmed | kSamplerConfirmed))
      return {itr.first.first, itr.first.second};
  }
  return kNotFound;
}

// Collects pairs of the descriptor set and the binding to combine
// corresponding Texture and Sampler into the sampled image.
std::vector<DescriptorSetAndBinding> collectDSetBindingsToCombineSampledImage(
    const llvm::SmallVectorImpl<ResourceInfoToCombineSampledImage>
        &resourceInfoForSampledImages) {
  std::vector<DescriptorSetAndBinding> dsetBindings;
  for (const auto &itr : resourceInfoForSampledImages) {
    dsetBindings.push_back({itr.descriptorSet, itr.binding});
  }
  return dsetBindings;
}

// Returns a scalar unsigned integer type or a vector of them or a matrix of
// them depending on the scalar/vector/matrix type of boolType. The element
// type of boolType must be BuiltinType::Bool type.
QualType getUintTypeForBool(ASTContext &astContext,
                            CompilerInstance &theCompilerInstance,
                            QualType boolType) {
  assert(isBoolOrVecMatOfBoolType(boolType));

  uint32_t vecSize = 1, numRows = 0, numCols = 0;
  QualType uintType = astContext.UnsignedIntTy;
  if (isScalarType(boolType) || isVectorType(boolType, nullptr, &vecSize)) {
    if (vecSize == 1)
      return uintType;
    else
      return astContext.getExtVectorType(uintType, vecSize);
  } else {
    const bool isMat = isMxNMatrix(boolType, nullptr, &numRows, &numCols);
    assert(isMat);
    (void)isMat;

    const clang::Type *type = boolType.getCanonicalType().getTypePtr();
    const RecordType *RT = cast<RecordType>(type);
    const ClassTemplateSpecializationDecl *templateSpecDecl =
        cast<ClassTemplateSpecializationDecl>(RT->getDecl());
    ClassTemplateDecl *templateDecl =
        templateSpecDecl->getSpecializedTemplate();
    return getHLSLMatrixType(astContext, theCompilerInstance.getSema(),
                             templateDecl, uintType, numRows, numCols);
  }
  return QualType();
}

bool isVkRawBufferLoadIntrinsic(const clang::FunctionDecl *FD) {
  if (!FD->getName().equals("RawBufferLoad"))
    return false;

  if (auto *nsDecl = dyn_cast<NamespaceDecl>(FD->getDeclContext()))
    if (!nsDecl->getName().equals("vk"))
      return false;

  return true;
}

bool isCooperativeMatrixGetLengthIntrinsic(
    const FunctionDecl *functionDeclaration) {
  return functionDeclaration->getName().equals(
      "__builtin_spv_CooperativeMatrixLengthKHR");
}

// Takes an AST member type, and determines its index in the equivalent SPIR-V
// struct type. This is required as the struct layout might change between the
// AST representation and SPIR-V representation.
uint32_t getFieldIndexInStruct(const StructType *spirvStructType,
                               const QualType &astStructType,
                               const FieldDecl *fieldDecl) {
  assert(fieldDecl);
  const uint32_t indexAST =
      getNumBaseClasses(astStructType) + fieldDecl->getFieldIndex();

  const auto &fields = spirvStructType->getFields();
  assert(indexAST < fields.size());
  return fields[indexAST].fieldIndex;
}

// Takes an AST struct type, and lowers is to the equivalent SPIR-V type.
const StructType *lowerStructType(const SpirvCodeGenOptions &spirvOptions,
                                  LowerTypeVisitor &lowerTypeVisitor,
                                  const QualType &structType) {
  // If we are accessing a derived struct, we need to account for the number
  // of base structs, since they are placed as fields at the beginning of the
  // derived struct.
  auto baseType = structType;
  if (baseType->isPointerType()) {
    baseType = baseType->getPointeeType();
  }

  // The AST type index is not representative of the SPIR-V type index
  // because we might squash some fields (bitfields by ex.).
  // What we need is to match each AST node with the squashed field and then,
  // determine the real index.
  const SpirvType *spvType = lowerTypeVisitor.lowerType(
      baseType, spirvOptions.sBufferLayoutRule, llvm::None, SourceLocation());

  const StructType *output = dyn_cast<StructType>(spvType);
  assert(output != nullptr);
  return output;
}

} // namespace

SpirvEmitter::SpirvEmitter(CompilerInstance &ci)
    : theCompilerInstance(ci), astContext(ci.getASTContext()),
      diags(ci.getDiagnostics()),
      spirvOptions(ci.getCodeGenOpts().SpirvOptions),
      hlslEntryFunctionName(ci.getCodeGenOpts().HLSLEntryFunction),
      spvContext(), featureManager(diags, spirvOptions),
      spvBuilder(astContext, spvContext, spirvOptions, featureManager),
      declIdMapper(astContext, spvContext, spvBuilder, *this, featureManager,
                   spirvOptions),
      constEvaluator(astContext, spvBuilder), entryFunction(nullptr),
      curFunction(nullptr), curThis(nullptr), seenPushConstantAt(),
      isSpecConstantMode(false), needsLegalization(false),
      beforeHlslLegalization(false), mainSourceFile(nullptr) {

  // Get ShaderModel from command line hlsl profile option.
  const hlsl::ShaderModel *shaderModel =
      hlsl::ShaderModel::GetByName(ci.getCodeGenOpts().HLSLProfile.c_str());
  if (shaderModel->GetKind() == hlsl::ShaderModel::Kind::Invalid)
    emitError("unknown shader module: %0", {}) << shaderModel->GetName();

  if (spirvOptions.invertY && !shaderModel->IsVS() && !shaderModel->IsDS() &&
      !shaderModel->IsGS() && !shaderModel->IsMS() && !shaderModel->IsLib())
    emitError("-fvk-invert-y can only be used in VS/DS/GS/MS/Lib", {});

  if (spirvOptions.useGlLayout && spirvOptions.useDxLayout)
    emitError("cannot specify both -fvk-use-dx-layout and -fvk-use-gl-layout",
              {});

  // Set shader model kind and hlsl major/minor version.
  spvContext.setCurrentShaderModelKind(shaderModel->GetKind());
  spvContext.setMajorVersion(shaderModel->GetMajor());
  spvContext.setMinorVersion(shaderModel->GetMinor());
  spirvOptions.signaturePacking =
      ci.getCodeGenOpts().HLSLSignaturePackingStrategy ==
      (unsigned)hlsl::DXIL::PackingStrategy::Optimized;

  if (spirvOptions.useDxLayout) {
    spirvOptions.cBufferLayoutRule = SpirvLayoutRule::FxcCTBuffer;
    spirvOptions.tBufferLayoutRule = SpirvLayoutRule::FxcCTBuffer;
    spirvOptions.sBufferLayoutRule = SpirvLayoutRule::FxcSBuffer;
    spirvOptions.ampPayloadLayoutRule = SpirvLayoutRule::FxcSBuffer;
  } else if (spirvOptions.useGlLayout) {
    spirvOptions.cBufferLayoutRule = SpirvLayoutRule::GLSLStd140;
    spirvOptions.tBufferLayoutRule = SpirvLayoutRule::GLSLStd430;
    spirvOptions.sBufferLayoutRule = SpirvLayoutRule::GLSLStd430;
    spirvOptions.ampPayloadLayoutRule = SpirvLayoutRule::GLSLStd430;
  } else if (spirvOptions.useScalarLayout) {
    spirvOptions.cBufferLayoutRule = SpirvLayoutRule::Scalar;
    spirvOptions.tBufferLayoutRule = SpirvLayoutRule::Scalar;
    spirvOptions.sBufferLayoutRule = SpirvLayoutRule::Scalar;
    spirvOptions.ampPayloadLayoutRule = SpirvLayoutRule::Scalar;
  } else {
    spirvOptions.cBufferLayoutRule = SpirvLayoutRule::RelaxedGLSLStd140;
    spirvOptions.tBufferLayoutRule = SpirvLayoutRule::RelaxedGLSLStd430;
    spirvOptions.sBufferLayoutRule = SpirvLayoutRule::RelaxedGLSLStd430;
    spirvOptions.ampPayloadLayoutRule = SpirvLayoutRule::RelaxedGLSLStd430;
  }

  // Set shader module version, source file name, and source file content (if
  // needed).
  llvm::StringRef source = "";
  std::vector<llvm::StringRef> fileNames;
  const auto &inputFiles = ci.getFrontendOpts().Inputs;
  // File name
  if (spirvOptions.debugInfoFile && !inputFiles.empty()) {
    for (const auto &inputFile : inputFiles) {
      fileNames.push_back(inputFile.getFile());
    }
  }
  // Source code
  if (spirvOptions.debugInfoSource) {
    const auto &sm = ci.getSourceManager();
    const llvm::MemoryBuffer *mainFile =
        sm.getBuffer(sm.getMainFileID(), SourceLocation());
    source = StringRef(mainFile->getBufferStart(), mainFile->getBufferSize());
  }
  mainSourceFile = spvBuilder.setDebugSource(spvContext.getMajorVersion(),
                                             spvContext.getMinorVersion(),
                                             fileNames, source);

  // Rich DebugInfo DebugSource
  if (spirvOptions.debugInfoRich) {
    auto *dbgSrc = spvBuilder.createDebugSource(mainSourceFile->getString());
    // spvContext.getDebugInfo().insert() inserts {string key, RichDebugInfo}
    // pair and returns {{string key, RichDebugInfo}, true /*Success*/}.
    // spvContext.getDebugInfo().insert().first->second is a RichDebugInfo.
    auto *richDebugInfo =
        &spvContext.getDebugInfo()
             .insert(
                 {mainSourceFile->getString(),
                  RichDebugInfo(dbgSrc,
                                spvBuilder.createDebugCompilationUnit(dbgSrc))})
             .first->second;
    spvContext.pushDebugLexicalScope(richDebugInfo,
                                     richDebugInfo->scopeStack.back());
  }

  if (spirvOptions.debugInfoTool && !spirvOptions.debugInfoVulkan &&
      featureManager.isTargetEnvVulkan1p1OrAbove()) {
    // Emit OpModuleProcessed to indicate the commit information.
    std::string commitHash =
        std::string("dxc-commit-hash: ") + clang::getGitCommitHash();
    spvBuilder.addModuleProcessed(commitHash);

    // Emit OpModuleProcessed to indicate the command line options that were
    // used to generate this module.
    if (!spirvOptions.inputFile.empty() || !spirvOptions.clOptions.empty()) {
      // Using this format: "dxc-cl-option: XXXXXX"
      std::string clOptionStr =
          "dxc-cl-option: " + spirvOptions.inputFile + spirvOptions.clOptions;
      spvBuilder.addModuleProcessed(clOptionStr);
    }
  }
}

std::vector<SpirvVariableLike *>
SpirvEmitter::getInterfacesForEntryPoint(SpirvFunction *entryPoint) {
  auto stageVars = declIdMapper.collectStageVars(entryPoint);
  if (!featureManager.isTargetEnvVulkan1p1Spirv1p4OrAbove())
    return stageVars;

  // In SPIR-V 1.4 or above, we must include global variables in the 'Interface'
  // operands of OpEntryPoint. SpirvModule keeps all global variables, but some
  // of them can be duplicated with stage variables kept by declIdMapper. Since
  // declIdMapper keeps the mapping between variables with Input or Output
  // storage class and their storage class, we have to rely on
  // declIdMapper.collectStageVars() to collect them.
  llvm::SetVector<SpirvVariableLike *> interfaces(stageVars.begin(),
                                                  stageVars.end());

  for (auto *moduleVar : spvBuilder.getModule()->getVariables()) {
    if (moduleVar->getStorageClass() == spv::StorageClass::Input ||
        moduleVar->getStorageClass() == spv::StorageClass::Output)
      continue;

    auto *untypedVar = dyn_cast<SpirvUntypedVariableKHR>(moduleVar);
    if (untypedVar) {
      interfaces.insert(moduleVar);
      continue;
    }

    if (auto *varEntry = declIdMapper.getRayTracingStageVarEntryFunction(
            cast<SpirvVariable>(moduleVar))) {
      if (varEntry != entryPoint)
        continue;
    }
    interfaces.insert(moduleVar);
  }
  std::vector<SpirvVariableLike *> interfacesInVector;
  interfacesInVector.reserve(interfaces.size());
  for (auto *interface : interfaces) {
    interfacesInVector.push_back(interface);
  }
  return interfacesInVector;
}

void SpirvEmitter::beginInvocationInterlock(SourceLocation loc,
                                            SourceRange range) {
  spvBuilder.addExecutionMode(
      entryFunction, declIdMapper.getInterlockExecutionMode(), {}, loc);
  spvBuilder.createBeginInvocationInterlockEXT(loc, range);
  needsLegalization = true;
}

llvm::StringRef SpirvEmitter::getEntryPointName(const FunctionInfo *entryInfo) {
  llvm::StringRef entrypointName = entryInfo->funcDecl->getName();
  // If this is the -E HLSL entrypoint and -fspv-entrypoint-name was set,
  // rename the SPIR-V entrypoint.
  if (entrypointName == hlslEntryFunctionName &&
      !spirvOptions.entrypointName.empty()) {
    return spirvOptions.entrypointName;
  }
  return entrypointName;
}

void SpirvEmitter::HandleTranslationUnit(ASTContext &context) {
  // Stop translating if there are errors in previous compilation stages.
  if (context.getDiagnostics().hasErrorOccurred())
    return;

  if (spirvOptions.debugInfoRich && !spirvOptions.debugInfoVulkan) {
    emitWarning(
        "Member functions will not be linked to their class in the "
        "debug information. Prefer using -fspv-debug=vulkan-with-source. "
        "See https://github.com/KhronosGroup/SPIRV-Registry/issues/203",
        {});
  }

  TranslationUnitDecl *tu = context.getTranslationUnitDecl();
  uint32_t numEntryPoints = 0;

  // The entry function is the seed of the queue.
  for (auto *decl : tu->decls()) {
    if (auto *funcDecl = dyn_cast<FunctionDecl>(decl)) {
      if (spvContext.isLib()) {
        if (const auto *shaderAttr = funcDecl->getAttr<HLSLShaderAttr>()) {
          // If we are compiling as a library then add everything that has a
          // ShaderAttr.
          addFunctionToWorkQueue(getShaderModelKind(shaderAttr->getStage()),
                                 funcDecl, /*isEntryFunction*/ true);
          numEntryPoints++;
        } else if (funcDecl->getAttr<HLSLExportAttr>()) {
          addFunctionToWorkQueue(spvContext.getCurrentShaderModelKind(),
                                 funcDecl, /*isEntryFunction*/ false);
        }
      } else {
        const bool isPrototype = !funcDecl->isThisDeclarationADefinition();
        if (funcDecl->getIdentifier() &&
            funcDecl->getName() == hlslEntryFunctionName && !isPrototype) {
          addFunctionToWorkQueue(spvContext.getCurrentShaderModelKind(),
                                 funcDecl, /*isEntryFunction*/ true);
          numEntryPoints++;
        }
      }
    } else {
      doDecl(decl);
    }

    if (context.getDiagnostics().hasErrorOccurred())
      return;
  }

  // Translate all functions reachable from the entry function.
  // The queue can grow in the meanwhile; so need to keep evaluating
  // workQueue.size().
  for (uint32_t i = 0; i < workQueue.size(); ++i) {
    const FunctionInfo *curEntryOrCallee = workQueue[i];
    spvContext.setCurrentShaderModelKind(curEntryOrCallee->shaderModelKind);
    doDecl(curEntryOrCallee->funcDecl);
    if (context.getDiagnostics().hasErrorOccurred())
      return;
  }

  // Addressing and memory model are required in a valid SPIR-V module.
  // It may be promoted based on features used by this shader.
  spvBuilder.setMemoryModel(spv::AddressingModel::Logical,
                            spv::MemoryModel::GLSL450);

  for (uint32_t i = 0; i < workQueue.size(); ++i) {
    // TODO: assign specific StageVars w.r.t. to entry point
    const FunctionInfo *entryInfo = workQueue[i];
    if (entryInfo->isEntryFunction) {
      spvBuilder.addEntryPoint(
          getSpirvShaderStage(
              entryInfo->shaderModelKind,
              featureManager.isExtensionEnabled(Extension::EXT_mesh_shader)),
          entryInfo->entryFunction, getEntryPointName(entryInfo),
          getInterfacesForEntryPoint(entryInfo->entryFunction));
    }
  }

  // Add Location decorations to stage input/output variables.
  if (!declIdMapper.decorateStageIOLocations())
    return;

  // Add descriptor set and binding decorations to resource variables.
  if (!declIdMapper.decorateResourceBindings())
    return;

  // Add Coherent docrations to resource variables.
  if (!declIdMapper.decorateResourceCoherent())
    return;

  // Add source instruction(s)
  if (spirvOptions.debugInfoSource || spirvOptions.debugInfoFile) {
    std::vector<llvm::StringRef> fileNames;
    fileNames.clear();
    const auto &sm = context.getSourceManager();
    // Add each include file from preprocessor output
    for (unsigned int i = 0; i < sm.getNumLineTableFilenames(); i++) {
      llvm::StringRef file = sm.getLineTableFilename(i);
      if (spirvOptions.debugInfoVulkan) {
        getOrCreateRichDebugInfoImpl(file);
      } else {
        fileNames.push_back(file);
      }
    }
    if (!spirvOptions.debugInfoVulkan) {
      spvBuilder.setDebugSource(spvContext.getMajorVersion(),
                                spvContext.getMinorVersion(), fileNames);
    }
  }

  if (spirvOptions.enableMaximalReconvergence) {
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::MaximallyReconvergesKHR, {},
                                SourceLocation());
  }

  for (const FunctionInfo *entryInfo : workQueue) {
    if (!entryInfo->isEntryFunction)
      continue;

    if (entryInfo->shaderModelKind != hlsl::ShaderModel::Kind::Pixel) {
      continue;
    }

    const auto *funcDecl = entryInfo->funcDecl;
    if (!funcDecl->hasAttr<HLSLWaveOpsIncludeHelperLanesAttr>())
      continue;

    spvBuilder.requireExtension("SPV_KHR_maximal_reconvergence",
                                funcDecl->getLocation());
    spvBuilder.requireExtension("SPV_KHR_quad_control",
                                funcDecl->getLocation());
    spvBuilder.requireCapability(spv::Capability::QuadControlKHR,
                                 funcDecl->getLocation());
    spvBuilder.addExecutionMode(entryInfo->entryFunction,
                                spv::ExecutionMode::MaximallyReconvergesKHR, {},
                                funcDecl->getLocation());
    spvBuilder.addExecutionMode(entryInfo->entryFunction,
                                spv::ExecutionMode::RequireFullQuadsKHR, {},
                                funcDecl->getLocation());
  }

  // For Vulkan 1.2 and later, add SignedZeroInfNanPreserve when -Gis is
  // provided to preserve NaN/Inf and signed zeros.
  if (spirvOptions.IEEEStrict) {
    if (featureManager.getSpirvVersion(featureManager.getTargetEnv()) <
        VersionTuple(1, 2))
      spvBuilder.requireExtension("SPV_KHR_float_controls", SourceLocation());
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::SignedZeroInfNanPreserve,
                                {32}, SourceLocation());
    spvBuilder.requireCapability(spv::Capability::SignedZeroInfNanPreserve,
                                 SourceLocation());
  }

  llvm::StringRef denormMode = spirvOptions.floatDenormalMode;
  if (!denormMode.empty()) {
    if (denormMode.equals_lower("preserve")) {
      spvBuilder.addExecutionMode(entryFunction,
                                  spv::ExecutionMode::DenormPreserve, {32}, {});
    } else if (denormMode.equals_lower("ftz")) {
      spvBuilder.addExecutionMode(
          entryFunction, spv::ExecutionMode::DenormFlushToZero, {32}, {});
    } else if (denormMode.equals_lower("any")) {
      // Do nothing. Since any behavior is allowed, we could optionally choose
      // to translate to DenormPreserve or DenormFlushToZero if one was known to
      // be more performant on most platforms.
    } else {
      assert(false && "unsupported denorm value");
    }
  }

  // Output the constructed module.
  std::vector<uint32_t> m = spvBuilder.takeModule();
  if (context.getDiagnostics().hasErrorOccurred())
    return;

  if (!UpgradeToVulkanMemoryModelIfNeeded(&m)) {
    return;
  }

  // Check the existance of Texture and Sampler with
  // [[vk::combinedImageSampler]] for the same descriptor set and binding.
  auto resourceInfoForSampledImages =
      spvContext.getResourceInfoForSampledImages();
  auto dsetBindingWithoutTextureOrSampler =
      getDSetBindingWithoutTextureOrSampler(resourceInfoForSampledImages);
  if (dsetBindingWithoutTextureOrSampler.descriptor_set !=
      std::numeric_limits<uint32_t>::max()) {
    emitFatalError(
        "Texture or Sampler with [[vk::combinedImageSampler]] attribute is "
        "missing for descriptor set and binding: %0, %1",
        {})
        << dsetBindingWithoutTextureOrSampler.descriptor_set
        << dsetBindingWithoutTextureOrSampler.binding;
    return;
  }
  auto dsetbindingsToCombineImageSampler =
      collectDSetBindingsToCombineSampledImage(resourceInfoForSampledImages);

  // In order to flatten composite resources, we must also unroll loops.
  // Therefore we should run legalization before optimization.
  needsLegalization =
      needsLegalization || declIdMapper.requiresLegalization() ||
      spirvOptions.flattenResourceArrays || spirvOptions.reduceLoadSize ||
      declIdMapper.requiresFlatteningCompositeResources() ||
      !dsetbindingsToCombineImageSampler.empty() ||
      spirvOptions.signaturePacking;

  // Run legalization passes
  if (spirvOptions.codeGenHighLevel) {
    beforeHlslLegalization = needsLegalization;
  } else {
    if (needsLegalization) {
      std::string messages;
      if (!spirvToolsLegalize(&m, &messages,
                              &dsetbindingsToCombineImageSampler)) {
        emitFatalError("failed to legalize SPIR-V: %0", {}) << messages;
        emitNote("please file a bug report on "
                 "https://github.com/Microsoft/DirectXShaderCompiler/issues "
                 "with source code if possible",
                 {});
        return;
      } else if (!messages.empty()) {
        emitWarning("SPIR-V legalization: %0", {}) << messages;
      }
    }

    if (theCompilerInstance.getCodeGenOpts().OptimizationLevel > 0) {
      // Run optimization passes
      std::string messages;
      if (!spirvToolsOptimize(&m, &messages)) {
        emitFatalError("failed to optimize SPIR-V: %0", {}) << messages;
        emitNote("please file a bug report on "
                 "https://github.com/Microsoft/DirectXShaderCompiler/issues "
                 "with source code if possible",
                 {});
        return;
      }
    }

    // Fixup debug instruction opcodes: change the opcode to
    // OpExtInstWithForwardRefsKHR is the instruction at least one forward
    // reference.
    if (spirvOptions.debugInfoRich) {
      std::string messages;
      if (!spirvToolsFixupOpExtInst(&m, &messages)) {
        emitFatalError("failed to fix OpExtInst opcodes: %0", {}) << messages;
        emitNote("please file a bug report on "
                 "https://github.com/Microsoft/DirectXShaderCompiler/issues "
                 "with source code if possible",
                 {});
        return;
      } else if (!messages.empty()) {
        emitWarning("SPIR-V fix-opextinst-opcodes: %0", {}) << messages;
      }
    }

    // Trim unused capabilities.
    // When optimizations are enabled, some optimization passes like DCE could
    // make some capabilities useless. To avoid logic duplication between this
    // pass, and DXC, DXC generates some capabilities unconditionally. This
    // means we should run this pass, even when optimizations are disabled.
    {
      std::string messages;
      if (!spirvToolsTrimCapabilities(&m, &messages)) {
        emitFatalError("failed to trim capabilities: %0", {}) << messages;
        emitNote("please file a bug report on "
                 "https://github.com/Microsoft/DirectXShaderCompiler/issues "
                 "with source code if possible",
                 {});
        return;
      } else if (!messages.empty()) {
        emitWarning("SPIR-V capability trimming: %0", {}) << messages;
      }
    }
  }

  // Validate the generated SPIR-V code
  if (!spirvOptions.disableValidation) {
    std::string messages;
    if (!spirvToolsValidate(&m, &messages)) {
      emitFatalError("generated SPIR-V is invalid: %0", {}) << messages;
      emitNote("please file a bug report on "
               "https://github.com/Microsoft/DirectXShaderCompiler/issues "
               "with source code if possible",
               {});
      return;
    }
  }

  theCompilerInstance.getOutStream()->write(
      reinterpret_cast<const char *>(m.data()), m.size() * 4);
}

void SpirvEmitter::doDecl(const Decl *decl) {
  if (isa<EmptyDecl>(decl) || isa<TypeAliasTemplateDecl>(decl) ||
      isa<VarTemplateDecl>(decl))
    return;

  // Implicit decls are lazily created when needed.
  if (decl->isImplicit()) {
    return;
  }

  if (const auto *varDecl = dyn_cast<VarDecl>(decl)) {
    doVarDecl(varDecl);
  } else if (const auto *namespaceDecl = dyn_cast<NamespaceDecl>(decl)) {
    for (auto *subDecl : namespaceDecl->decls())
      // Note: We only emit functions as they are discovered through the call
      // graph starting from the entry-point. We should not emit unused
      // functions inside namespaces.
      if (!isa<FunctionDecl>(subDecl))
        doDecl(subDecl);
  } else if (const auto *classTemplateDecl =
                 dyn_cast<ClassTemplateDecl>(decl)) {
    doClassTemplateDecl(classTemplateDecl);
  } else if (const auto *classTemplateDecl =
                 dyn_cast<ClassTemplatePartialSpecializationDecl>(decl)) {
    // Do nothing. We cannot generate any code with a partial specialization,
    // and when there is a specialization of this decl it will be a
    // specialization of the orginal ClassTemplateDecl that this specializes.
    // The code for the full specialization will be handlded when processing the
    // ClassTemplateDecl. Note that this is also a RecordDecl, so we must check
    // for it before RecordDecl.
  } else if (const auto *funcDecl = dyn_cast<FunctionDecl>(decl)) {
    doFunctionDecl(funcDecl);
  } else if (const auto *bufferDecl = dyn_cast<HLSLBufferDecl>(decl)) {
    doHLSLBufferDecl(bufferDecl);
  } else if (const auto *recordDecl = dyn_cast<RecordDecl>(decl)) {
    doRecordDecl(recordDecl);
  } else if (const auto *enumDecl = dyn_cast<EnumDecl>(decl)) {
    doEnumDecl(enumDecl);
  } else if (isa<TypedefNameDecl>(decl)) {
    declIdMapper.recordsSpirvTypeAlias(decl);
  } else if (isa<FunctionTemplateDecl>(decl)) {
    // nothing to do.
  } else if (isa<UsingDecl>(decl)) {
    // nothing to do.
  } else if (isa<UsingDirectiveDecl>(decl)) {
    // nothing to do.
  } else {
    emitError("decl type %0 unimplemented", decl->getLocation())
        << decl->getDeclKindName();
  }
}

RichDebugInfo *
SpirvEmitter::getOrCreateRichDebugInfo(const SourceLocation &loc) {
  const StringRef file =
      astContext.getSourceManager().getPresumedLoc(loc).getFilename();
  return getOrCreateRichDebugInfoImpl(file);
}

RichDebugInfo *
SpirvEmitter::getOrCreateRichDebugInfoImpl(llvm::StringRef file) {
  auto &debugInfo = spvContext.getDebugInfo();
  auto it = debugInfo.find(file);
  if (it != debugInfo.end())
    return &it->second;

  auto *dbgSrc = spvBuilder.createDebugSource(file);
  // debugInfo.insert() inserts {string key, RichDebugInfo} pair and
  // returns {{string key, RichDebugInfo}, true /*Success*/}.
  // debugInfo.insert().first->second is a RichDebugInfo.
  return &debugInfo
              .insert({file,
                       RichDebugInfo(
                           dbgSrc,
                           spvBuilder.getModule()->getDebugCompilationUnit())})
              .first->second;
}

void SpirvEmitter::doStmt(const Stmt *stmt,
                          llvm::ArrayRef<const Attr *> attrs) {
  if (const auto *compoundStmt = dyn_cast<CompoundStmt>(stmt)) {
    if (spirvOptions.debugInfoRich && stmt->getLocStart() != SourceLocation()) {
      // Any opening of curly braces ('{') starts a CompoundStmt in the AST
      // tree. It also means we have a new lexical block!
      const auto loc = stmt->getLocStart();
      const auto &sm = astContext.getSourceManager();
      const uint32_t line = sm.getPresumedLineNumber(loc);
      const uint32_t column = sm.getPresumedColumnNumber(loc);
      RichDebugInfo *info = getOrCreateRichDebugInfo(loc);

      auto *debugLexicalBlock = spvBuilder.createDebugLexicalBlock(
          info->source, line, column, info->scopeStack.back());

      // Add this lexical block to the stack of lexical scopes.
      spvContext.pushDebugLexicalScope(info, debugLexicalBlock);

      // Update or add DebugScope.
      if (spvBuilder.getInsertPoint()->empty()) {
        spvBuilder.getInsertPoint()->updateDebugScope(
            new (spvContext) SpirvDebugScope(debugLexicalBlock));
      } else if (!spvBuilder.isCurrentBasicBlockTerminated()) {
        spvBuilder.createDebugScope(debugLexicalBlock);
      }

      // Iterate over sub-statements
      for (auto *st : compoundStmt->body())
        doStmt(st, {});

      // We are done with processing this compound statement. Remove its lexical
      // block from the stack of lexical scopes.
      spvContext.popDebugLexicalScope(info);
      if (!spvBuilder.isCurrentBasicBlockTerminated()) {
        spvBuilder.createDebugScope(spvContext.getCurrentLexicalScope());
      }
    } else {
      // Iterate over sub-statements
      for (auto *st : compoundStmt->body())
        doStmt(st);
    }
  } else if (const auto *retStmt = dyn_cast<ReturnStmt>(stmt)) {
    doReturnStmt(retStmt);
  } else if (const auto *declStmt = dyn_cast<DeclStmt>(stmt)) {
    doDeclStmt(declStmt);
  } else if (const auto *ifStmt = dyn_cast<IfStmt>(stmt)) {
    doIfStmt(ifStmt, attrs);
  } else if (const auto *switchStmt = dyn_cast<SwitchStmt>(stmt)) {
    doSwitchStmt(switchStmt, attrs);
  } else if (dyn_cast<CaseStmt>(stmt)) {
    processCaseStmtOrDefaultStmt(stmt);
  } else if (dyn_cast<DefaultStmt>(stmt)) {
    processCaseStmtOrDefaultStmt(stmt);
  } else if (const auto *breakStmt = dyn_cast<BreakStmt>(stmt)) {
    doBreakStmt(breakStmt);
  } else if (const auto *theDoStmt = dyn_cast<DoStmt>(stmt)) {
    doDoStmt(theDoStmt, attrs);
  } else if (const auto *discardStmt = dyn_cast<DiscardStmt>(stmt)) {
    doDiscardStmt(discardStmt);
  } else if (const auto *continueStmt = dyn_cast<ContinueStmt>(stmt)) {
    doContinueStmt(continueStmt);
  } else if (const auto *whileStmt = dyn_cast<WhileStmt>(stmt)) {
    doWhileStmt(whileStmt, attrs);
  } else if (const auto *forStmt = dyn_cast<ForStmt>(stmt)) {
    doForStmt(forStmt, attrs);
  } else if (dyn_cast<NullStmt>(stmt)) {
    // For the null statement ";". We don't need to do anything.
  } else if (const auto *attrStmt = dyn_cast<AttributedStmt>(stmt)) {
    doStmt(attrStmt->getSubStmt(), attrStmt->getAttrs());
  } else if (const auto *expr = dyn_cast<Expr>(stmt)) {
    // All cases for expressions used as statements
    SpirvInstruction *result = doExpr(expr);

    if (result && !attrs.empty() &&
        (result->getKind() == SpirvInstruction::IK_ExecutionMode ||
         result->getKind() == SpirvInstruction::IK_ExecutionModeId)) {
      // Handle [[vk::ext_capability(..)]] and [[vk::ext_extension(..)]]
      // attributes for vk::ext_execution_mode[_id](..).
      createSpirvIntrInstExt(
          attrs, QualType(),
          /*spvArgs*/ llvm::SmallVector<SpirvInstruction *, 1>{},
          /*isInstr*/ false, expr->getExprLoc());
    }
  } else {
    emitError("statement class '%0' unimplemented", stmt->getLocStart())
        << stmt->getStmtClassName() << stmt->getSourceRange();
  }
}

SpirvInstruction *SpirvEmitter::doExpr(const Expr *expr,
                                       SourceRange rangeOverride) {
  SpirvInstruction *result = nullptr;
  expr = expr->IgnoreParens();
  SourceRange range =
      (rangeOverride != SourceRange()) ? rangeOverride : expr->getSourceRange();

  if (const auto *declRefExpr = dyn_cast<DeclRefExpr>(expr)) {
    auto *decl = declRefExpr->getDecl();
    if (isImplicitVarDeclInVkNamespace(declRefExpr->getDecl())) {
      result = doExpr(cast<VarDecl>(decl)->getInit());
    } else {
      result = declIdMapper.getDeclEvalInfo(decl, expr->getLocStart(), range);
    }
  } else if (const auto *memberExpr = dyn_cast<MemberExpr>(expr)) {
    result = doMemberExpr(memberExpr, range);
  } else if (const auto *castExpr = dyn_cast<CastExpr>(expr)) {
    result = doCastExpr(castExpr, range);
  } else if (const auto *initListExpr = dyn_cast<InitListExpr>(expr)) {
    result = doInitListExpr(initListExpr, range);
  } else if (const auto *boolLiteral = dyn_cast<CXXBoolLiteralExpr>(expr)) {
    result =
        spvBuilder.getConstantBool(boolLiteral->getValue(), isSpecConstantMode);
    result->setRValue();
  } else if (const auto *intLiteral = dyn_cast<IntegerLiteral>(expr)) {
    result = constEvaluator.translateAPInt(intLiteral->getValue(),
                                           expr->getType(), isSpecConstantMode);
    result->setRValue();
  } else if (const auto *floatLiteral = dyn_cast<FloatingLiteral>(expr)) {
    result = constEvaluator.translateAPFloat(
        floatLiteral->getValue(), expr->getType(), isSpecConstantMode);
    result->setRValue();
  } else if (const auto *stringLiteral = dyn_cast<StringLiteral>(expr)) {
    result = spvBuilder.getString(stringLiteral->getString());
  } else if (const auto *compoundAssignOp =
                 dyn_cast<CompoundAssignOperator>(expr)) {
    // CompoundAssignOperator is a subclass of BinaryOperator. It should be
    // checked before BinaryOperator.
    result = doCompoundAssignOperator(compoundAssignOp);
  } else if (const auto *binOp = dyn_cast<BinaryOperator>(expr)) {
    result = doBinaryOperator(binOp);
  } else if (const auto *unaryOp = dyn_cast<UnaryOperator>(expr)) {
    result = doUnaryOperator(unaryOp);
  } else if (const auto *vecElemExpr = dyn_cast<HLSLVectorElementExpr>(expr)) {
    result = doHLSLVectorElementExpr(vecElemExpr, range);
  } else if (const auto *matElemExpr = dyn_cast<ExtMatrixElementExpr>(expr)) {
    result = doExtMatrixElementExpr(matElemExpr);
  } else if (const auto *funcCall = dyn_cast<CallExpr>(expr)) {
    result = doCallExpr(funcCall, range);
  } else if (const auto *subscriptExpr = dyn_cast<ArraySubscriptExpr>(expr)) {
    result = doArraySubscriptExpr(subscriptExpr, range);
  } else if (const auto *condExpr = dyn_cast<ConditionalOperator>(expr)) {
    // Beginning with HLSL 2021, the ternary operator is short-circuited.
    if (getCompilerInstance().getLangOpts().HLSLVersion >=
        hlsl::LangStd::v2021) {
      result = doShortCircuitedConditionalOperator(condExpr);
    } else {
      const Expr *cond = condExpr->getCond();
      const Expr *falseExpr = condExpr->getFalseExpr();
      const Expr *trueExpr = condExpr->getTrueExpr();
      result = doConditional(condExpr, cond, falseExpr, trueExpr);
    }
  } else if (const auto *defaultArgExpr = dyn_cast<CXXDefaultArgExpr>(expr)) {
    if (defaultArgExpr->getParam()->hasUninstantiatedDefaultArg()) {
      auto defaultArg =
          defaultArgExpr->getParam()->getUninstantiatedDefaultArg();
      result = castToType(doExpr(defaultArg), defaultArg->getType(),
                          defaultArgExpr->getType(), defaultArg->getLocStart(),
                          defaultArg->getSourceRange());
      result->setRValue();
    } else {
      result = doExpr(defaultArgExpr->getParam()->getDefaultArg());
    }
  } else if (isa<CXXThisExpr>(expr)) {
    assert(curThis);
    result = curThis;
  } else if (const auto *constructExpr = dyn_cast<CXXConstructExpr>(expr)) {
    // For RayQuery type, we should not explicitly initialize it using
    // CXXConstructExpr e.g., RayQuery<0> r = RayQuery<0>() is the same as we do
    // not have a variable initialization. Setting nullptr for the SPIR-V
    // instruction used for expr will let us skip the variable initialization.
    if (hlsl::IsVKBufferPointerType(expr->getType())) {
      const Expr *arg = constructExpr->getArg(0);
      SpirvInstruction *value = loadIfGLValue(arg, arg->getSourceRange());
      result = spvBuilder.createConvertUToPtr(value, expr->getType());
      result->setRValue();
    } else if (!hlsl::IsHLSLRayQueryType(expr->getType()))
      result = curThis;
  } else if (const auto *unaryExpr = dyn_cast<UnaryExprOrTypeTraitExpr>(expr)) {
    result = doUnaryExprOrTypeTraitExpr(unaryExpr);
  } else if (const auto *tmplParamExpr =
                 dyn_cast<SubstNonTypeTemplateParmExpr>(expr)) {
    result = doExpr(tmplParamExpr->getReplacement());
  } else {
    emitError("expression class '%0' unimplemented", expr->getExprLoc())
        << expr->getStmtClassName() << expr->getSourceRange();
  }

  return result;
}

SpirvInstruction *SpirvEmitter::doExprEnsuringRValue(const Expr *E,
                                                     SourceLocation location,
                                                     SourceRange range) {
  SpirvInstruction *I = doExpr(E);
  if (I->isRValue())
    return I;
  return spvBuilder.createLoad(E->getType(), I, location, range);
}

SpirvInstruction *SpirvEmitter::loadIfGLValue(const Expr *expr,
                                              SourceRange rangeOverride) {
  // We are trying to load the value here, which is what an LValueToRValue
  // implicit cast is intended to do. We can ignore the cast if exists.
  SourceRange range =
      (rangeOverride != SourceRange()) ? rangeOverride : expr->getSourceRange();
  expr = expr->IgnoreParenLValueCasts();

  return loadIfGLValue(expr, doExpr(expr, range));
}

SpirvInstruction *SpirvEmitter::loadIfGLValue(const Expr *expr,
                                              SpirvInstruction *info,
                                              SourceRange rangeOverride) {
  const auto exprType = expr->getType();

  // Do nothing if this is already rvalue
  if (!info || info->isRValue())
    return info;

  // Check whether we are trying to load an array of opaque objects as a whole.
  // If true, we are likely to copy it as a whole. To assist per-element
  // copying, avoid the load here and return the pointer directly.
  // TODO: consider moving this hack into SPIRV-Tools as a transformation.
  if (isOpaqueArrayType(exprType))
    return info;

  // Check whether we are trying to load an externally visible structured/byte
  // buffer as a whole. If true, it means we are creating alias for it. Avoid
  // the load and write the pointer directly to the alias variable then.
  //
  // Also for the case of alias function returns. If we are trying to load an
  // alias function return as a whole, it means we are assigning it to another
  // alias variable. Avoid the load and write the pointer directly.
  //
  // Note: legalization specific code
  if (isReferencingNonAliasStructuredOrByteBuffer(expr)) {
    return info;
  }

  if (loadIfAliasVarRef(expr, &info)) {
    // We are loading an alias variable as a whole here. This is likely for
    // wholesale assignments or function returns. Need to load the pointer.
    //
    // Note: legalization specific code
    return info;
  }

  SourceRange range =
      (rangeOverride != SourceRange()) ? rangeOverride : expr->getSourceRange();
  SpirvInstruction *loadedInstr = nullptr;
  loadedInstr =
      spvBuilder.createLoad(exprType, info, expr->getExprLoc(), range);
  assert(loadedInstr);

  // Special-case: According to the SPIR-V Spec: There is no physical size or
  // bit pattern defined for boolean type. Therefore an unsigned integer is used
  // to represent booleans when layout is required. In such cases, after loading
  // the uint, we should perform a comparison.
  {
    uint32_t vecSize = 1, numRows = 0, numCols = 0;
    if (info->getLayoutRule() != SpirvLayoutRule::Void &&
        isBoolOrVecMatOfBoolType(exprType)) {
      QualType uintType = astContext.UnsignedIntTy;
      if (isScalarType(exprType) || isVectorType(exprType, nullptr, &vecSize)) {
        const auto fromType =
            vecSize == 1 ? uintType
                         : astContext.getExtVectorType(uintType, vecSize);
        loadedInstr =
            castToBool(loadedInstr, fromType, exprType, expr->getLocStart());
      } else {
        const bool isMat = isMxNMatrix(exprType, nullptr, &numRows, &numCols);
        assert(isMat);
        (void)isMat;
        const clang::Type *type = exprType.getCanonicalType().getTypePtr();
        const RecordType *RT = cast<RecordType>(type);
        const ClassTemplateSpecializationDecl *templateSpecDecl =
            cast<ClassTemplateSpecializationDecl>(RT->getDecl());
        ClassTemplateDecl *templateDecl =
            templateSpecDecl->getSpecializedTemplate();
        const auto fromType = getHLSLMatrixType(
            astContext, theCompilerInstance.getSema(), templateDecl,
            astContext.UnsignedIntTy, numRows, numCols);
        loadedInstr =
            castToBool(loadedInstr, fromType, exprType, expr->getLocStart());
      }
      // Now that it is converted to Bool, it has no layout rule.
      // This result-id should be evaluated as bool from here on out.
      loadedInstr->setLayoutRule(SpirvLayoutRule::Void);
    }
  }

  loadedInstr->setRValue();
  return loadedInstr;
}

SpirvInstruction *SpirvEmitter::loadIfAliasVarRef(const Expr *expr,
                                                  SourceRange rangeOverride) {
  const auto range =
      (rangeOverride != SourceRange()) ? rangeOverride : expr->getSourceRange();
  auto *instr = doExpr(expr, range);
  loadIfAliasVarRef(expr, &instr, range);
  return instr;
}

bool SpirvEmitter::loadIfAliasVarRef(const Expr *varExpr,
                                     SpirvInstruction **instr,
                                     SourceRange rangeOverride) {
  assert(instr);
  const auto range = (rangeOverride != SourceRange())
                         ? rangeOverride
                         : varExpr->getSourceRange();
  if ((*instr) && (*instr)->containsAliasComponent() &&
      isAKindOfStructuredOrByteBuffer(varExpr->getType())) {
    // Load the pointer of the aliased-to-variable if the expression has a
    // pointer to pointer type.
    if (varExpr->isGLValue()) {
      *instr = spvBuilder.createLoad(varExpr->getType(), *instr,
                                     varExpr->getExprLoc(), range);
    }
    return true;
  }
  return false;
}

SpirvInstruction *SpirvEmitter::castToType(SpirvInstruction *value,
                                           QualType fromType, QualType toType,
                                           SourceLocation srcLoc,
                                           SourceRange range) {
  uint32_t fromSize = 0;
  uint32_t toSize = 0;
  assert(isVectorType(fromType, nullptr, &fromSize) ==
             isVectorType(toType, nullptr, &toSize) &&
         fromSize == toSize);
  // Avoid unused variable warning in release builds
  (void)(fromSize);
  (void)(toSize);

  if (isFloatOrVecMatOfFloatType(toType))
    return castToFloat(value, fromType, toType, srcLoc, range);

  // Order matters here. Bool (vector) values will also be considered as uint
  // (vector) values. So given a bool (vector) argument, isUintOrVecOfUintType()
  // will also return true. We need to check bool before uint. The opposite is
  // not true.
  if (isBoolOrVecMatOfBoolType(toType))
    return castToBool(value, fromType, toType, srcLoc, range);

  if (isSintOrVecMatOfSintType(toType) || isUintOrVecMatOfUintType(toType))
    return castToInt(value, fromType, toType, srcLoc, range);

  emitError("casting to type %0 unimplemented", {}) << toType;
  return nullptr;
}

static bool handleDispatchGrid(SpirvContext &spvContext,
                               const RecordDecl *recordDecl) {
  unsigned index = 0;
  for (auto fieldDecl : recordDecl->fields()) {
    QualType fieldType = fieldDecl->getType();
    for (const hlsl::UnusualAnnotation *it :
         fieldDecl->getUnusualAnnotations()) {
      if (it->getKind() == hlsl::UnusualAnnotation::UA_SemanticDecl) {
        const hlsl::SemanticDecl *sd = cast<hlsl::SemanticDecl>(it);
        if (sd->SemanticName.equals("SV_DispatchGrid")) {
          spvContext.registerDispatchGridIndex(recordDecl, index);
          return true;
        }
      }
    }
    if (const auto *innerType = fieldType->getAs<RecordType>()) {
      if (handleDispatchGrid(spvContext, innerType->getDecl()))
        return true;
    }
    ++index;
  }
  return false;
}

bool SpirvEmitter::handleNodePayloadArrayType(const ParmVarDecl *decl,
                                              SpirvInstruction *instr) {
  // Because SPIR-V node payload array types are node-specific, propagate
  // lowered types
  switch (instr->getKind()) {
  case SpirvInstruction::Kind::IK_Load: {
    SpirvInstruction *ptr = dyn_cast<SpirvLoad>(instr)->getPointer();
    if (handleNodePayloadArrayType(decl, ptr)) {
      const SpirvPointerType *ptrType =
          dyn_cast<SpirvPointerType>(ptr->getResultType());
      instr->setResultType(ptrType->getPointeeType());
      spvContext.addToInstructionsWithLoweredType(instr);
      return true;
    }
    return false;
  }
  case SpirvInstruction::Kind::IK_FunctionParameter:
  case SpirvInstruction::Kind::IK_Variable: {
    QualType varType = decl->getType();
    if (hlsl::IsHLSLNodeType(varType)) {
      if (auto *type = spvContext.getNodeDeclPayloadType(decl)) {
        instr->setResultType(
            spvContext.getPointerType(type, instr->getStorageClass()));
      } else {
        LowerTypeVisitor lowerTypeVisitor(astContext, spvContext, spirvOptions,
                                          spvBuilder);
        QualType resultType =
            hlsl::GetHLSLNodeIOResultType(astContext, varType);
        const auto *recordType = resultType->getAs<RecordType>();
        assert(recordType);
        if (hlsl::IsHLSLDispatchNodeInputRecordType(varType)) {
          handleDispatchGrid(spvContext, recordType->getDecl());
        }
        const SpirvType *elemType = lowerTypeVisitor.lowerType(
            resultType, clang::spirv::SpirvLayoutRule::Scalar, llvm::None,
            decl->getLocation());
        const NodePayloadArrayType *arrType =
            spvContext.getNodePayloadArrayType(elemType, decl);
        const SpirvType *ptrType =
            spvContext.getPointerType(arrType, instr->getStorageClass());
        instr->setResultType(ptrType);
        spvContext.registerNodeDeclPayloadType(arrType, decl);
      }
      spvContext.addToInstructionsWithLoweredType(instr);
      return true;
    }
    return false;
  }
  default:
    return false;
  }
}

void SpirvEmitter::doFunctionDecl(const FunctionDecl *decl) {
  // Forward declaration of a function inside another.
  if (!decl->isThisDeclarationADefinition()) {
    addFunctionToWorkQueue(spvContext.getCurrentShaderModelKind(), decl,
                           /*isEntryFunction*/ false);
    return;
  }

  // A RAII class for maintaining the current function under traversal.
  class FnEnvRAII {
  public:
    // Creates a new instance which sets fnEnv to the newFn on creation,
    // and resets fnEnv to its original value on destruction.
    FnEnvRAII(const FunctionDecl **fnEnv, const FunctionDecl *newFn)
        : oldFn(*fnEnv), fnSlot(fnEnv) {
      *fnEnv = newFn;
    }
    ~FnEnvRAII() { *fnSlot = oldFn; }

  private:
    const FunctionDecl *oldFn;
    const FunctionDecl **fnSlot;
  };

  FnEnvRAII fnEnvRAII(&curFunction, decl);

  // We are about to start translation for a new function. Clear the break stack
  // and the continue stack.
  breakStack = std::stack<SpirvBasicBlock *>();
  continueStack = std::stack<SpirvBasicBlock *>();

  // This will allow the entry-point name to be something like
  // myNamespace::myEntrypointFunc.
  std::string funcName = getFnName(decl);
  std::string srcFuncName = "src." + funcName;
  SpirvFunction *func = declIdMapper.getOrRegisterFn(decl);
  SpirvFunction *srcFunc = nullptr;

  auto loc = decl->getLocStart();
  auto range = decl->getSourceRange();
  RichDebugInfo *info = nullptr;
  SpirvDebugFunction *debugFunction = nullptr;
  SpirvDebugFunction *srcDebugFunction = nullptr;
  SpirvDebugInstruction *outer_scope = spvContext.getCurrentLexicalScope();

  bool isEntry = false;
  const auto iter = functionInfoMap.find(decl);
  if (iter != functionInfoMap.end()) {
    const auto &entryInfo = iter->second;
    if (entryInfo->isEntryFunction) {
      isEntry = true;
      // Create wrapper for the entry function
      srcFunc = func;

      func = emitEntryFunctionWrapper(decl, &info, &debugFunction, srcFunc);
      if (func == nullptr)
        return;
      if (spirvOptions.debugInfoRich && decl->hasBody()) {
        // use the HLSL name the debug user will expect to see
        srcDebugFunction = emitDebugFunction(decl, srcFunc, &info, funcName);
      }

      // Generate DebugEntryPoint if function definition
      if (spirvOptions.debugInfoVulkan && debugFunction) {
        auto *cu = dyn_cast<SpirvDebugCompilationUnit>(outer_scope);
        assert(cu && "expected DebugCompilationUnit");
        spvBuilder.createDebugEntryPoint(debugFunction, cu,
                                         clang::getGitCommitHash(),
                                         spirvOptions.clOptions);
      }
    } else {
      if (spirvOptions.debugInfoRich && decl->hasBody()) {
        debugFunction = emitDebugFunction(decl, func, &info, funcName);
      }
    }
  }

  if (spirvOptions.debugInfoRich) {
    if (srcDebugFunction) {
      spvContext.pushDebugLexicalScope(info, srcDebugFunction);
    } else if (debugFunction) {
      spvContext.pushDebugLexicalScope(info, debugFunction);
    } else {
      // A function which is not called directly in HLSL, and therefore does not
      // reside in the workQueue
      if (decl->hasBody()) {
        debugFunction = emitDebugFunction(decl, func, &info, funcName);
        spvContext.pushDebugLexicalScope(info, debugFunction);
      }
    }
  }

  const QualType retType =
      declIdMapper.getTypeAndCreateCounterForPotentialAliasVar(decl);

  if (srcFunc) {
    spvBuilder.beginFunction(retType, decl->getLocStart(), srcFuncName,
                             decl->hasAttr<HLSLPreciseAttr>(),
                             decl->hasAttr<NoInlineAttr>(), srcFunc);

  } else {
    spvBuilder.beginFunction(retType, decl->getLocStart(), funcName,
                             decl->hasAttr<HLSLPreciseAttr>(),
                             decl->hasAttr<NoInlineAttr>(), func);
  }

  bool isNonStaticMemberFn = false;
  if (const auto *memberFn = dyn_cast<CXXMethodDecl>(decl)) {
    if (!memberFn->isStatic()) {
      // For non-static member function, the first parameter should be the
      // object on which we are invoking this method.
      QualType valueType = memberFn->getThisType(astContext)->getPointeeType();

      // Remember the parameter for the 'this' object so later we can handle
      // CXXThisExpr correctly.
      curThis = spvBuilder.addFnParam(valueType, /*isPrecise*/ false,
                                      /*isNoInterp*/ false, decl->getLocStart(),
                                      "param.this");
      if (isOrContainsAKindOfStructuredOrByteBuffer(valueType)) {
        curThis->setContainsAliasComponent(true);
        needsLegalization = true;
      }

      if (spirvOptions.debugInfoRich) {
        // Add DebugLocalVariable information
        const auto &sm = astContext.getSourceManager();
        const uint32_t line = sm.getPresumedLineNumber(loc);
        const uint32_t column = sm.getPresumedColumnNumber(loc);
        if (!info)
          info = getOrCreateRichDebugInfo(loc);
        // TODO: replace this with FlagArtificial|FlagObjectPointer.
        uint32_t flags = (1 << 5) | (1 << 8);
        auto *debugLocalVar = spvBuilder.createDebugLocalVariable(
            valueType, "this", info->source, line, column,
            info->scopeStack.back(), flags, 1);
        spvBuilder.createDebugDeclare(debugLocalVar, curThis, loc, range);
      }

      isNonStaticMemberFn = true;
    }
  }

  // Create all parameters.
  for (uint32_t i = 0; i < decl->getNumParams(); ++i) {
    const ParmVarDecl *paramDecl = decl->getParamDecl(i);
    QualType paramType = paramDecl->getType();
    auto *param = declIdMapper.createFnParam(
        paramDecl, i + 1 + isNonStaticMemberFn, !isEntry);
    if (isEntry) {
      handleNodePayloadArrayType(paramDecl, param);
    }
#ifdef ENABLE_SPIRV_CODEGEN
    if (hlsl::IsVKBufferPointerType(paramType)) {
      Optional<bool> isRowMajor = llvm::None;
      QualType desugaredType = desugarType(paramType, &isRowMajor);
      if (hlsl::IsVKBufferPointerType(desugaredType)) {
        spvBuilder.decorateWithLiterals(
            param,
            static_cast<unsigned>(paramDecl->hasAttr<VKAliasedPointerAttr>()
                                      ? spv::Decoration::AliasedPointer
                                      : spv::Decoration::RestrictPointer),
            {}, loc);
      }
    }
#endif
  }

  if (decl->hasBody()) {
    // The entry basic block.
    auto *entryLabel = spvBuilder.createBasicBlock("bb.entry");
    spvBuilder.setInsertPoint(entryLabel);

    // Add DebugFunctionDefinition if we are emitting
    // NonSemantic.Shader.DebugInfo.100 debug info
    if (spirvOptions.debugInfoVulkan && srcDebugFunction)
      spvBuilder.createDebugFunctionDef(srcDebugFunction, srcFunc);
    else if (spirvOptions.debugInfoVulkan && debugFunction)
      spvBuilder.createDebugFunctionDef(debugFunction, func);

    // Process all statments in the body.
    parentMap = std::make_unique<ParentMap>(decl->getBody());
    doStmt(decl->getBody());
    parentMap.reset(nullptr);

    // We have processed all Stmts in this function and now in the last
    // basic block. Make sure we have a termination instruction.
    if (!spvBuilder.isCurrentBasicBlockTerminated()) {
      const auto retType = decl->getReturnType();
      const auto returnLoc = decl->getBody()->getLocEnd();

      if (retType->isVoidType()) {
        spvBuilder.createReturn(returnLoc);
      } else {
        // If the source code does not provide a proper return value for some
        // control flow path, it's undefined behavior. We just return an
        // undefined value here.
        spvBuilder.createReturnValue(spvBuilder.getUndef(retType), returnLoc);
      }
    }
  }

  spvBuilder.endFunction();

  if (spirvOptions.debugInfoRich) {
    spvContext.popDebugLexicalScope(info);
  }
}

bool SpirvEmitter::validateVKAttributes(const NamedDecl *decl) {
  bool success = true;

  if (decl->getAttr<VKInputAttachmentIndexAttr>()) {
    if (!decl->isExternallyVisible()) {
      emitError("SubpassInput(MS) must be externally visible",
                decl->getLocation());
      success = false;
    }

    // We only allow VKInputAttachmentIndexAttr to be attached to global
    // variables. So it should be fine to cast here.
    const auto elementType =
        hlsl::GetHLSLResourceResultType(cast<VarDecl>(decl)->getType());

    if (!isScalarType(elementType) && !isVectorType(elementType)) {
      emitError(
          "only scalar/vector types allowed as SubpassInput(MS) parameter type",
          decl->getLocation());
      // Return directly to avoid further type processing, which will hit
      // asserts when lowering the type.
      return false;
    }
  }

  // The frontend will make sure that
  // * vk::push_constant applies to global variables of struct type
  // * vk::binding applies to global variables or cbuffers/tbuffers
  // * vk::counter_binding applies to global variables of RW/Append/Consume
  //   StructuredBuffer
  // * vk::location applies to function parameters/returns and struct fields
  // So the only case we need to check co-existence is vk::push_constant and
  // vk::binding.

  if (const auto *pcAttr = decl->getAttr<VKPushConstantAttr>()) {
    const auto loc = pcAttr->getLocation();

    if (seenPushConstantAt.isInvalid()) {
      seenPushConstantAt = loc;
    } else {
      // TODO: Actually this is slightly incorrect. The Vulkan spec says:
      //   There must be no more than one push constant block statically used
      //   per shader entry point.
      // But we are checking whether there are more than one push constant
      // blocks defined. Tracking usage requires more work.
      emitError("cannot have more than one push constant block", loc);
      emitNote("push constant block previously defined here",
               seenPushConstantAt);
      success = false;
    }

    if (decl->hasAttr<VKBindingAttr>()) {
      emitError("vk::push_constant attribute cannot be used together with "
                "vk::binding attribute",
                loc);
      success = false;
    }

#ifdef ENABLE_SPIRV_CODEGEN
    if (hlsl::IsVKBufferPointerType(cast<VarDecl>(decl)->getType())) {
      emitError("vk::push_constant attribute cannot be used on declarations "
                "with vk::BufferPointer type",
                loc);
      success = false;
    }
#endif
  }

  // vk::shader_record_nv is supported only on cbuffer/ConstantBuffer
  if (const auto *srbAttr = decl->getAttr<VKShaderRecordNVAttr>()) {
    const auto loc = srbAttr->getLocation();
    const HLSLBufferDecl *bufDecl = nullptr;
    bool isValidType = false;
    if ((bufDecl = dyn_cast<HLSLBufferDecl>(decl)))
      isValidType = bufDecl->isCBuffer();
    else if ((bufDecl = dyn_cast<HLSLBufferDecl>(decl->getDeclContext())))
      isValidType = bufDecl->isCBuffer();
    else if (isa<VarDecl>(decl))
      isValidType = isConstantBuffer(dyn_cast<VarDecl>(decl)->getType());

    if (!isValidType) {
      emitError(
          "vk::shader_record_nv can be applied only to cbuffer/ConstantBuffer",
          loc);
      success = false;
    }
    if (decl->hasAttr<VKBindingAttr>()) {
      emitError("vk::shader_record_nv attribute cannot be used together with "
                "vk::binding attribute",
                loc);
      success = false;
    }
  }

  // vk::shader_record_ext is supported only on cbuffer/ConstantBuffer
  if (const auto *srbAttr = decl->getAttr<VKShaderRecordEXTAttr>()) {
    const auto loc = srbAttr->getLocation();
    const HLSLBufferDecl *bufDecl = nullptr;
    bool isValidType = false;
    if ((bufDecl = dyn_cast<HLSLBufferDecl>(decl)))
      isValidType = bufDecl->isCBuffer();
    else if ((bufDecl = dyn_cast<HLSLBufferDecl>(decl->getDeclContext())))
      isValidType = bufDecl->isCBuffer();
    else if (isa<VarDecl>(decl))
      isValidType = isConstantBuffer(dyn_cast<VarDecl>(decl)->getType());

    if (!isValidType) {
      emitError(
          "vk::shader_record_ext can be applied only to cbuffer/ConstantBuffer",
          loc);
      success = false;
    }
    if (decl->hasAttr<VKBindingAttr>()) {
      emitError("vk::shader_record_ext attribute cannot be used together with "
                "vk::binding attribute",
                loc);
      success = false;
    }
  }

  // a VarDecl should have only one of vk::ext_builtin_input or
  // vk::ext_builtin_output
  if (decl->hasAttr<VKExtBuiltinInputAttr>() &&
      decl->hasAttr<VKExtBuiltinOutputAttr>()) {
    emitError("vk::ext_builtin_input cannot be used together with "
              "vk::ext_builtin_output",
              decl->getAttr<VKExtBuiltinOutputAttr>()->getLocation());
    success = false;
  }

  // vk::ext_builtin_input and vk::ext_builtin_output must only be used for a
  // static variable. We only allow them to be attached to variables, so it
  // should be fine to cast here.
  if ((decl->hasAttr<VKExtBuiltinInputAttr>() ||
       decl->hasAttr<VKExtBuiltinOutputAttr>()) &&
      cast<VarDecl>(decl)->getStorageClass() != StorageClass::SC_Static) {
    emitError("vk::ext_builtin_input and vk::ext_builtin_output can only be "
              "applied to a static variable",
              decl->getLocation());
    success = false;
  }

  // vk::ext_builtin_input and vk::ext_builtin_output must only be used for a
  // static variable. We only allow them to be attached to variables, so it
  // should be fine to cast here.
  if (decl->hasAttr<VKExtBuiltinInputAttr>() &&
      !cast<VarDecl>(decl)->getType().isConstQualified()) {
    emitError("vk::ext_builtin_input can only be applied to a const-qualified "
              "variable",
              decl->getLocation());
    success = false;
  }

  return success;
}

void SpirvEmitter::registerCapabilitiesAndExtensionsForVarDecl(
    const VarDecl *varDecl) {
  // First record any extensions/capabilities declared on the variable itself.
  declIdMapper.registerCapabilitiesAndExtensionsForDecl(varDecl);

  // Now check for any capabilities or extensions that are part of the type.
  const TypedefType *type = dyn_cast<TypedefType>(varDecl->getType());
  if (!type)
    return;

  declIdMapper.registerCapabilitiesAndExtensionsForType(type);
}

void SpirvEmitter::doHLSLBufferDecl(const HLSLBufferDecl *bufferDecl) {
  // This is a cbuffer/tbuffer decl.
  // Check and emit warnings for member intializers which are not
  // supported in Vulkan
  for (const auto *member : bufferDecl->decls()) {
    if (const auto *varMember = dyn_cast<VarDecl>(member)) {
      if (!spirvOptions.noWarnIgnoredFeatures) {
        if (const auto *init = varMember->getInit())
          emitWarning("%select{tbuffer|cbuffer}0 member initializer "
                      "ignored since no Vulkan equivalent",
                      init->getExprLoc())
              << bufferDecl->isCBuffer() << init->getSourceRange();
      }

      // We cannot handle external initialization of column-major matrices now.
      if (isOrContainsNonFpColMajorMatrix(astContext, spirvOptions,
                                          varMember->getType(), varMember)) {
        emitError("externally initialized non-floating-point column-major "
                  "matrices not supported yet",
                  varMember->getLocation());
      }
    }
  }
  if (!validateVKAttributes(bufferDecl))
    return;
  if (bufferDecl->hasAttr<VKShaderRecordNVAttr>()) {
    (void)declIdMapper.createShaderRecordBuffer(
        bufferDecl, DeclResultIdMapper::ContextUsageKind::ShaderRecordBufferNV);
  } else if (bufferDecl->hasAttr<VKShaderRecordEXTAttr>()) {
    (void)declIdMapper.createShaderRecordBuffer(
        bufferDecl,
        DeclResultIdMapper::ContextUsageKind::ShaderRecordBufferKHR);
  } else {
    declIdMapper.createCTBuffer(bufferDecl);
  }
}

void SpirvEmitter::doClassTemplateDecl(
    const ClassTemplateDecl *classTemplateDecl) {
  for (auto classTemplateSpecializationDeclItr :
       classTemplateDecl->specializations()) {
    if (const CXXRecordDecl *recordDecl =
            dyn_cast<CXXRecordDecl>(&*classTemplateSpecializationDeclItr)) {
      doRecordDecl(recordDecl);
    }
  }
}

void SpirvEmitter::doRecordDecl(const RecordDecl *recordDecl) {
  // Ignore implict records
  // Somehow we'll have implicit records with:
  //   static const int Length = count;
  // that can mess up with the normal CodeGen.
  if (recordDecl->isImplicit())
    return;

  // Handle each static member with inline initializer.
  // Each static member has a corresponding VarDecl inside the
  // RecordDecl. For those defined in the translation unit,
  // their VarDecls do not have initializer.
  for (auto *subDecl : recordDecl->decls()) {
    if (auto *varDecl = dyn_cast<VarDecl>(subDecl)) {
      if (varDecl->isStaticDataMember() && varDecl->hasInit())
        doVarDecl(varDecl);
    } else if (auto *enumDecl = dyn_cast<EnumDecl>(subDecl)) {
      doEnumDecl(enumDecl);
    } else if (auto recordDecl = dyn_cast<RecordDecl>(subDecl)) {
      doRecordDecl(recordDecl);
    }
  }
}

void SpirvEmitter::doEnumDecl(const EnumDecl *decl) {
  for (auto it = decl->enumerator_begin(); it != decl->enumerator_end(); ++it)
    declIdMapper.createEnumConstant(*it);
}

void SpirvEmitter::doVarDecl(const VarDecl *decl) {
  if (!validateVKAttributes(decl))
    return;

  const auto loc = decl->getLocation();
  const auto range = decl->getSourceRange();

  if (isExtResultIdType(decl->getType())) {
    declIdMapper.createResultId(decl);
    return;
  }

  // HLSL has the 'string' type which can be used for rare purposes such as
  // printf (SPIR-V's DebugPrintf). SPIR-V does not have a 'char' or 'string'
  // type, and therefore any variable of such type should not be created.
  // DeclResultIdMapper maps such decl to an OpString instruction that
  // represents the variable's initializer literal.
  if (isStringType(decl->getType())) {
    declIdMapper.createOrUpdateStringVar(decl);
    return;
  }

  // Considering the following example:
  //
  // ```cpp
  //  template<typename T> const static T MyClass<T>::myVar[2] = { 1, 2 };
  //  [...]
  //  int use = MyClass<int>::myVar[0];
  // ```
  //
  // The AST will contain 2 variable declarations:
  //  - VarDecl for the template declaration
  //  - VarDecl for the template instantiation
  // One of them is not yet defined (InitExpr will have the type void), the
  // other is the actual instantiation, hence will have the proper type. They
  // can be differentiated by looking at the declaration context:
  //  - the undefined ones will be in the template declaration context.
  // We must not create a variable for the template declaration but wait
  // for the instantiation (if any).
  auto *RC = dyn_cast<clang::CXXRecordDecl>(decl->getDeclContext());
  auto *TC = RC ? RC->getDescribedClassTemplate() : nullptr;
  if (decl->getInit() && TC)
    return;

  // We cannot handle external initialization of column-major matrices now.
  if (isExternalVar(decl) &&
      isOrContainsNonFpColMajorMatrix(astContext, spirvOptions, decl->getType(),
                                      decl)) {
    emitError("externally initialized non-floating-point column-major "
              "matrices not supported yet",
              loc);
  }

  // Reject arrays of RW/append/consume structured buffers. They have assoicated
  // counters, which are quite nasty to handle.
  if (decl->getType()->isArrayType() &&
      isRWAppendConsumeSBuffer(decl->getType())) {
    if (decl->getType()
            ->getAsArrayTypeUnsafe()
            ->getElementType()
            ->isArrayType()) {
      // See
      // https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#interfaces-resources-setandbinding
      emitError("Multi-dimensional arrays of RW/append/consume structured "
                "buffers are unsupported in Vulkan",
                loc);
      return;
    }
  }

  if (featureManager.isTargetEnvVulkan() &&
      (isTexture(decl->getType()) || isRWTexture(decl->getType()) ||
       isBuffer(decl->getType()) || isRWBuffer(decl->getType()))) {
    const auto sampledType = hlsl::GetHLSLResourceResultType(decl->getType());
    if (isFloatOrVecMatOfFloatType(sampledType) &&
        isOrContains16BitType(sampledType, spirvOptions.enable16BitTypes)) {
      emitError("The sampled type for textures cannot be a floating point type "
                "smaller than 32-bits when targeting a Vulkan environment.",
                loc);
      return;
    }
  }

  if (decl->hasAttr<VKConstantIdAttr>()) {
    // This is a VarDecl for specialization constant.
    createSpecConstant(decl);
    return;
  }

  if (decl->hasAttr<VKPushConstantAttr>()) {
    // This is a VarDecl for PushConstant block.
    (void)declIdMapper.createPushConstant(decl);
    return;
  }

  if (decl->hasAttr<VKShaderRecordNVAttr>()) {
    (void)declIdMapper.createShaderRecordBuffer(
        decl, DeclResultIdMapper::ContextUsageKind::ShaderRecordBufferNV);
    return;
  }

  if (decl->hasAttr<VKShaderRecordEXTAttr>()) {
    (void)declIdMapper.createShaderRecordBuffer(
        decl, DeclResultIdMapper::ContextUsageKind::ShaderRecordBufferKHR);
    return;
  }

  registerCapabilitiesAndExtensionsForVarDecl(decl);

  // Handle vk::ext_builtin_input and vk::ext_builtin_input by using
  // getBuiltinVar to create the builtin and validate the storage class
  if (decl->hasAttr<VKExtBuiltinInputAttr>()) {
    auto *builtinAttr = decl->getAttr<VKExtBuiltinInputAttr>();
    int builtinId = builtinAttr->getBuiltInID();
    SpirvVariable *builtinVar =
        declIdMapper.getBuiltinVar(spv::BuiltIn(builtinId), decl->getType(),
                                   spv::StorageClass::Input, loc);
    if (builtinVar->getStorageClass() != spv::StorageClass::Input) {
      emitError("cannot redefine builtin %0 as an input",
                builtinAttr->getLocation())
          << builtinId;
      emitWarning("previous definition is here",
                  builtinVar->getSourceLocation());
    }
    return;
  } else if (decl->hasAttr<VKExtBuiltinOutputAttr>()) {
    auto *builtinAttr = decl->getAttr<VKExtBuiltinOutputAttr>();
    int builtinId = builtinAttr->getBuiltInID();
    SpirvVariable *builtinVar =
        declIdMapper.getBuiltinVar(spv::BuiltIn(builtinId), decl->getType(),
                                   spv::StorageClass::Output, loc);
    if (builtinVar->getStorageClass() != spv::StorageClass::Output) {
      emitError("cannot redefine builtin %0 as an output",
                builtinAttr->getLocation())
          << builtinId;
      emitWarning("previous definition is here",
                  builtinVar->getSourceLocation());
    }
    return;
  }

  if (hlsl::IsVKBufferPointerType(decl->getType()) && !decl->hasInit()) {
    emitError("vk::BufferPointer has no default constructor", loc);
    return;
  }

  // We can have VarDecls inside cbuffer/tbuffer. For those VarDecls, we need
  // to emit their cbuffer/tbuffer as a whole and access each individual one
  // using access chains.
  // cbuffers and tbuffers are HLSLBufferDecls
  // ConstantBuffers and TextureBuffers are not HLSLBufferDecls.
  if (const auto *bufferDecl =
          dyn_cast<HLSLBufferDecl>(decl->getDeclContext())) {
    // This is a VarDecl of cbuffer/tbuffer type.
    doHLSLBufferDecl(bufferDecl);
    return;
  }

  if (decl->getAttr<VKInputAttachmentIndexAttr>()) {
    if (!spvContext.isPS()) {
      // SubpassInput(MS) variables are only allowed in pixel shaders. In this
      // case, we avoid create the declaration because it should not be used.
      return;
    }
  }

  SpirvVariable *var = nullptr;

  // The contents in externally visible variables can be updated via the
  // pipeline. They should be handled differently from file and function scope
  // variables.
  // File scope variables (static "global" and "local" variables) belongs to
  // the Private storage class, while function scope variables (normal "local"
  // variables) belongs to the Function storage class.
  if (isExternalVar(decl)) {
    var = declIdMapper.createExternVar(decl);
    if (decl->hasInit()) {
      emitWarning("Initializer of external global will be ignored",
                  decl->getLocation());
    }
  } else {
    // We already know the variable is not externally visible here. If it does
    // not have local storage, it should be file scope variable.
    const bool isFileScopeVar = !decl->hasLocalStorage();
    if (isFileScopeVar) {
      if (decl->getType().isConstQualified() &&
          declIdMapper.tryToCreateConstantVar(decl))
        return;
      var = declIdMapper.createFileVar(decl, llvm::None);
    } else
      var = declIdMapper.createFnVar(decl, llvm::None);

    // Emit OpStore to initialize the variable
    // TODO: revert back to use OpVariable initializer

    // We should only evaluate the initializer once for a static variable.
    if (isFileScopeVar) {
      if (decl->isStaticLocal()) {
        initOnce(decl->getType(), decl->getName(), var, decl->getInit());
      } else {
        // Defer to initialize these global variables at the beginning of the
        // entry function.
        toInitGloalVars.push_back(decl);
      }

    }
    // Function local variables. Just emit OpStore at the current insert point.
    else if (const Expr *init = decl->getInit()) {
      if (auto *constInit =
              constEvaluator.tryToEvaluateAsConst(init, isSpecConstantMode)) {
        spvBuilder.createStore(var, constInit, loc, range);
      } else {
        storeValue(var, loadIfGLValue(init), decl->getType(), loc, range);
      }

      // Update counter variable associated with local variables
      tryToAssignCounterVar(decl, init);
    }

    if (!isFileScopeVar && spirvOptions.debugInfoRich) {
      // Add DebugLocalVariable information
      const auto &sm = astContext.getSourceManager();
      const uint32_t line = sm.getPresumedLineNumber(loc);
      const uint32_t column = sm.getPresumedColumnNumber(loc);
      const auto *info = getOrCreateRichDebugInfo(loc);
      // TODO: replace this with FlagIsLocal enum.
      uint32_t flags = 1 << 2;
      auto *debugLocalVar = spvBuilder.createDebugLocalVariable(
          decl->getType(), decl->getName(), info->source, line, column,
          info->scopeStack.back(), flags);
      spvBuilder.createDebugDeclare(debugLocalVar, var, loc, range);
    }

    // Variables that are not externally visible and of opaque types should
    // request legalization.
    if (!needsLegalization && isOpaqueType(decl->getType()))
      needsLegalization = true;
  }

  if (var != nullptr) {
    Optional<bool> isRowMajor = llvm::None;
    QualType desugaredType = desugarType(decl->getType(), &isRowMajor);
    if (hlsl::IsVKBufferPointerType(desugaredType)) {
      spvBuilder.decorateWithLiterals(
          var,
          static_cast<unsigned>(decl->hasAttr<VKAliasedPointerAttr>()
                                    ? spv::Decoration::AliasedPointer
                                    : spv::Decoration::RestrictPointer),
          {}, loc);
    }

    if (decl->hasAttrs()) {
      declIdMapper.decorateWithIntrinsicAttrs(decl, var);
      if (auto attr = decl->getAttr<VKStorageClassExtAttr>()) {
        var->setStorageClass(
            static_cast<spv::StorageClass>(attr->getStclass()));
      }
    }
  }

  // All variables that are of opaque struct types should request legalization.
  if (!needsLegalization && isOpaqueStructType(decl->getType()))
    needsLegalization = true;
}

spv::LoopControlMask SpirvEmitter::translateLoopAttribute(const Stmt *stmt,
                                                          const Attr &attr) {
  switch (attr.getKind()) {
  case attr::HLSLLoop:
  case attr::HLSLFastOpt:
    return spv::LoopControlMask::DontUnroll;
  case attr::HLSLUnroll:
    return spv::LoopControlMask::Unroll;
  case attr::HLSLAllowUAVCondition:
    if (!spirvOptions.noWarnIgnoredFeatures) {
      emitWarning("unsupported allow_uav_condition attribute ignored",
                  stmt->getLocStart());
    }
    break;
  default:
    llvm_unreachable("found unknown loop attribute");
  }
  return spv::LoopControlMask::MaskNone;
}

void SpirvEmitter::doDiscardStmt(const DiscardStmt *discardStmt) {
  assert(!spvBuilder.isCurrentBasicBlockTerminated());

  // The discard statement can only be called from a pixel shader
  if (!spvContext.isPS()) {
    emitError("discard statement may only be used in pixel shaders",
              discardStmt->getLoc());
    return;
  }

  if (featureManager.isExtensionEnabled(
          Extension::EXT_demote_to_helper_invocation) ||
      featureManager.isTargetEnvVulkan1p3OrAbove()) {
    // OpDemoteToHelperInvocation(EXT) provided by SPIR-V 1.6 or
    // SPV_EXT_demote_to_helper_invocation SPIR-V extension allow shaders to
    // "demote" a fragment shader invocation to behave like a helper invocation
    // for its duration. The demoted invocation will have no further side
    // effects and will not output to the framebuffer, but remains active and
    // can participate in computing derivatives and in subgroup operations. This
    // is a better match for the "discard" instruction in HLSL.
    spvBuilder.createDemoteToHelperInvocation(discardStmt->getLoc());
  } else {
    // Note: if/when the demote behavior becomes part of the core Vulkan spec,
    // we should no longer generate OpKill for 'discard', and always generate
    // the demote behavior.
    spvBuilder.createKill(discardStmt->getLoc());
    // Some statements that alter the control flow (break, continue, return, and
    // discard), require creation of a new basic block to hold any statement
    // that may follow them.
    auto *newBB = spvBuilder.createBasicBlock();
    spvBuilder.setInsertPoint(newBB);
  }
}

void SpirvEmitter::doDoStmt(const DoStmt *theDoStmt,
                            llvm::ArrayRef<const Attr *> attrs) {
  // do-while loops are composed of:
  //
  // do {
  //   <body>
  // } while(<check>);
  //
  // SPIR-V requires loops to have a merge basic block as well as a continue
  // basic block. Even though do-while loops do not have an explicit continue
  // block as in for-loops, we still do need to create a continue block.
  //
  // Since SPIR-V requires structured control flow, we need two more basic
  // blocks, <header> and <merge>. <header> is the block before control flow
  // diverges, and <merge> is the block where control flow subsequently
  // converges. The <check> can be performed in the <continue> basic block.
  // The final CFG should normally be like the following. Exceptions
  // will occur with non-local exits like loop breaks or early returns.
  //
  //            +----------+
  //            |  header  | <-----------------------------------+
  //            +----------+                                     |
  //                 |                                           |  (true)
  //                 v                                           |
  //             +------+       +--------------------+           |
  //             | body | ----> | continue (<check>) |-----------+
  //             +------+       +--------------------+
  //                                     |
  //                                     | (false)
  //             +-------+               |
  //             | merge | <-------------+
  //             +-------+
  //
  // For more details, see "2.11. Structured Control Flow" in the SPIR-V spec.

  const spv::LoopControlMask loopControl =
      attrs.empty() ? spv::LoopControlMask::MaskNone
                    : translateLoopAttribute(theDoStmt, *attrs.front());

  // Create basic blocks
  auto *headerBB = spvBuilder.createBasicBlock("do_while.header");
  auto *bodyBB = spvBuilder.createBasicBlock("do_while.body");
  auto *continueBB = spvBuilder.createBasicBlock("do_while.continue");
  auto *mergeBB = spvBuilder.createBasicBlock("do_while.merge");

  // Make sure any continue statements branch to the continue block, and any
  // break statements branch to the merge block.
  continueStack.push(continueBB);
  breakStack.push(mergeBB);

  // Branch from the current insert point to the header block.
  spvBuilder.createBranch(headerBB, theDoStmt->getLocStart());
  spvBuilder.addSuccessor(headerBB);

  // Process the <header> block
  // The header block must always branch to the body.
  spvBuilder.setInsertPoint(headerBB);
  const Stmt *body = theDoStmt->getBody();
  spvBuilder.createBranch(bodyBB,
                          body ? body->getLocStart() : theDoStmt->getLocStart(),
                          mergeBB, continueBB, loopControl);
  spvBuilder.addSuccessor(bodyBB);
  // The current basic block has OpLoopMerge instruction. We need to set its
  // continue and merge target.
  spvBuilder.setContinueTarget(continueBB);
  spvBuilder.setMergeTarget(mergeBB);

  // Process the <body> block
  spvBuilder.setInsertPoint(bodyBB);
  if (body) {
    doStmt(body);
  }
  if (!spvBuilder.isCurrentBasicBlockTerminated()) {
    spvBuilder.createBranch(continueBB, body ? body->getLocEnd()
                                             : theDoStmt->getLocStart());
  }
  spvBuilder.addSuccessor(continueBB);

  // Process the <continue> block. The check for whether the loop should
  // continue lies in the continue block.
  // *NOTE*: There's a SPIR-V rule that when a conditional branch is to occur in
  // a continue block of a loop, there should be no OpSelectionMerge. Only an
  // OpBranchConditional must be specified.
  spvBuilder.setInsertPoint(continueBB);
  SpirvInstruction *condition = nullptr;
  const auto check = theDoStmt->getCond();
  if (check) {
    condition = doExpr(check);
  } else {
    condition = spvBuilder.getConstantBool(true);
  }
  spvBuilder.createConditionalBranch(
      condition, headerBB, mergeBB, theDoStmt->getLocEnd(), nullptr, nullptr,
      spv::SelectionControlMask::MaskNone, spv::LoopControlMask::MaskNone,
      check ? check->getSourceRange()
            : SourceRange(theDoStmt->getWhileLoc(), theDoStmt->getLocEnd()));
  spvBuilder.addSuccessor(headerBB);
  spvBuilder.addSuccessor(mergeBB);

  // Set insertion point to the <merge> block for subsequent statements
  spvBuilder.setInsertPoint(mergeBB);

  // Done with the current scope's continue block and merge block.
  continueStack.pop();
  breakStack.pop();
}

void SpirvEmitter::doContinueStmt(const ContinueStmt *continueStmt) {
  assert(!spvBuilder.isCurrentBasicBlockTerminated());
  auto *continueTargetBB = continueStack.top();
  spvBuilder.createBranch(continueTargetBB, continueStmt->getLocStart());
  spvBuilder.addSuccessor(continueTargetBB);

  // Some statements that alter the control flow (break, continue, return, and
  // discard), require creation of a new basic block to hold any statement that
  // may follow them. For example: StmtB and StmtC below are put inside a new
  // basic block which is unreachable.
  //
  // while (true) {
  //   StmtA;
  //   continue;
  //   StmtB;
  //   StmtC;
  // }
  auto *newBB = spvBuilder.createBasicBlock();
  spvBuilder.setInsertPoint(newBB);
}

void SpirvEmitter::doWhileStmt(const WhileStmt *whileStmt,
                               llvm::ArrayRef<const Attr *> attrs) {
  // While loops are composed of:
  //   while (<check>)  { <body> }
  //
  // SPIR-V requires loops to have a merge basic block as well as a continue
  // basic block. Even though while loops do not have an explicit continue
  // block as in for-loops, we still do need to create a continue block.
  //
  // Since SPIR-V requires structured control flow, we need two more basic
  // blocks, <header> and <merge>. <header> is the block before control flow
  // diverges, and <merge> is the block where control flow subsequently
  // converges. The <check> block can take the responsibility of the <header>
  // block. The final CFG should normally be like the following. Exceptions
  // will occur with non-local exits like loop breaks or early returns.
  //
  //            +----------+
  //            |  header  | <------------------+
  //            | (check)  |                    |
  //            +----------+                    |
  //                 |                          |
  //         +-------+-------+                  |
  //         | false         | true             |
  //         |               v                  |
  //         |            +------+     +------------------+
  //         |            | body | --> | continue (no-op) |
  //         v            +------+     +------------------+
  //     +-------+
  //     | merge |
  //     +-------+
  //
  // The only exception is when the condition cannot be expressed in a single
  // block. Specifically, short-circuited operators end up producing multiple
  // blocks. In that case, we cannot treat the <check> block as the header
  // block, and must instead have a bespoke <header> block. The condition is
  // then moved into the loop. For example, given a loop in the form
  //   while (a && b) { <body> }
  // we will generate instructions for the equivalent loop
  //   while (true) { if (!(a && b)) { break }  <body> }
  //            +----------+
  //            |  header  | <------------------+
  //            +----------+                    |
  //                 |                          |
  //                 v                          |
  //            +----------+                    |
  //            |  check   |                    |
  //            +----------+                    |
  //                 |                          |
  //         +-------+-------+                  |
  //         | false         | true             |
  //         |               v                  |
  //         |            +------+     +------------------+
  //         |            | body | --> | continue (no-op) |
  //         v            +------+     +------------------+
  //     +-------+
  //     | merge |
  //     +-------+
  // The reason we don't unconditionally apply this transformation, which is
  // technically always legal, is because it prevents loop unrolling in SPIR-V
  // Tools, which does not support unrolling loops with early breaks.
  // For more details, see "2.11. Structured Control Flow" in the SPIR-V spec.

  const spv::LoopControlMask loopControl =
      attrs.empty() ? spv::LoopControlMask::MaskNone
                    : translateLoopAttribute(whileStmt, *attrs.front());

  const Expr *check = whileStmt->getCond();
  const Stmt *body = whileStmt->getBody();
  bool checkHasShortcircuitedOp = stmtTreeContainsShortCircuitedOp(check);

  // Create basic blocks
  auto *checkBB = spvBuilder.createBasicBlock("while.check");
  auto *headerBB = checkHasShortcircuitedOp
                       ? spvBuilder.createBasicBlock("while.header")
                       : checkBB;
  auto *bodyBB = spvBuilder.createBasicBlock("while.body");
  auto *continueBB = spvBuilder.createBasicBlock("while.continue");
  auto *mergeBB = spvBuilder.createBasicBlock("while.merge");

  // Make sure any continue statements branch to the continue block, and any
  // break statements branch to the merge block.
  continueStack.push(continueBB);
  breakStack.push(mergeBB);

  spvBuilder.createBranch(headerBB, whileStmt->getLocStart());
  spvBuilder.addSuccessor(headerBB);
  spvBuilder.setInsertPoint(headerBB);
  if (checkHasShortcircuitedOp) {
    // Process the <header> block.
    spvBuilder.setInsertPoint(headerBB);
    spvBuilder.createBranch(
        checkBB,
        check ? check->getLocStart()
              : (body ? body->getLocStart() : whileStmt->getLocStart()),
        mergeBB, continueBB, loopControl,
        check
            ? check->getSourceRange()
            : SourceRange(whileStmt->getLocStart(), whileStmt->getLocStart()));
    spvBuilder.addSuccessor(checkBB);
    // The current basic block has a OpLoopMerge instruction. We need to set
    // its continue and merge target.
    spvBuilder.setContinueTarget(continueBB);
    spvBuilder.setMergeTarget(mergeBB);

    // Process the <check> block.
    spvBuilder.setInsertPoint(checkBB);

    // If we have:
    //   while (int a = foo()) {...}
    // we should evaluate 'a' by calling 'foo()' every single time the check has
    // to occur.
    if (const auto *condVarDecl = whileStmt->getConditionVariableDeclStmt())
      doStmt(condVarDecl);

    SpirvInstruction *condition = doExpr(check);
    spvBuilder.createConditionalBranch(
        condition, bodyBB, mergeBB,
        check ? check->getLocEnd()
              : (body ? body->getLocStart() : whileStmt->getLocStart()),
        nullptr, nullptr, spv::SelectionControlMask::MaskNone,
        spv::LoopControlMask::MaskNone,
        check
            ? check->getSourceRange()
            : SourceRange(whileStmt->getLocStart(), whileStmt->getLocStart()));
    spvBuilder.addSuccessor(bodyBB);
    spvBuilder.addSuccessor(mergeBB);
  } else {
    // In the case of simple or empty conditions, we can use a
    // single block for <check> and <header>.

    // If we have:
    //   while (int a = foo()) {...}
    // we should evaluate 'a' by calling 'foo()' every single time the check has
    // to occur.
    if (const auto *condVarDecl = whileStmt->getConditionVariableDeclStmt())
      doStmt(condVarDecl);

    SpirvInstruction *condition = nullptr;
    if (check) {
      condition = doExpr(check);
    } else {
      condition = spvBuilder.getConstantBool(true);
    }
    spvBuilder.createConditionalBranch(
        condition, bodyBB, mergeBB, whileStmt->getLocStart(), mergeBB,
        continueBB, spv::SelectionControlMask::MaskNone, loopControl,
        check ? check->getSourceRange()
              : SourceRange(whileStmt->getWhileLoc(), whileStmt->getLocEnd()));
    spvBuilder.addSuccessor(bodyBB);
    spvBuilder.addSuccessor(mergeBB);
    // The current basic block has OpLoopMerge instruction. We need to set its
    // continue and merge target.
    spvBuilder.setContinueTarget(continueBB);
    spvBuilder.setMergeTarget(mergeBB);
  }

  // Process the <body> block.
  spvBuilder.setInsertPoint(bodyBB);
  if (body) {
    doStmt(body);
  }
  if (!spvBuilder.isCurrentBasicBlockTerminated())
    spvBuilder.createBranch(continueBB, whileStmt->getLocEnd());
  spvBuilder.addSuccessor(continueBB);

  // Process the <continue> block. While loops do not have an explicit
  // continue block. The continue block just branches to the <header> block.
  spvBuilder.setInsertPoint(continueBB);
  spvBuilder.createBranch(headerBB, whileStmt->getLocEnd());
  spvBuilder.addSuccessor(headerBB);

  // Set insertion point to the <merge> block for subsequent statements.
  spvBuilder.setInsertPoint(mergeBB);

  // Done with the current scope's continue and merge blocks.
  continueStack.pop();
  breakStack.pop();
}

void SpirvEmitter::doForStmt(const ForStmt *forStmt,
                             llvm::ArrayRef<const Attr *> attrs) {
  // for loops are composed of:
  //   for (<init>; <check>; <continue>) <body>
  //
  // To translate a for loop, we'll need to emit all <init> statements
  // in the current basic block, and then have separate basic blocks for
  // <check>, <continue>, and <body>. Besides, since SPIR-V requires
  // structured control flow, we need two more basic blocks, <header>
  // and <merge>. <header> is the block before control flow diverges,
  // while <merge> is the block where control flow subsequently converges.
  // The <check> block can take the responsibility of the <header> block.
  // The final CFG should normally be like the following. Exceptions will
  // occur with non-local exits like loop breaks or early returns.
  //             +--------+
  //             |  init  |
  //             +--------+
  //                 |
  //                 v
  //            +----------+
  //            |  header  | <---------------+
  //            | (check)  |                 |
  //            +----------+                 |
  //                 |                       |
  //         +-------+-------+               |
  //         | false         | true          |
  //         |               v               |
  //         |            +------+     +----------+
  //         |            | body | --> | continue |
  //         v            +------+     +----------+
  //     +-------+
  //     | merge |
  //     +-------+
  //
  // The only exception is when the condition cannot be expressed in a single
  // block. Specifically, short-circuited operators end up producing multiple
  // blocks. In that case, we cannot treat the <check> block as the header
  // block, and must instead have a bespoke <header> block. The condition is
  // then moved into the loop. For example, given a loop in the form
  //   for (<init>; a && b; <continue>) { <body> }
  // we will generate instructions for the equivalent loop
  //   for (<init>; ; <continue>) { if (!(a && b)) { break }  <body> }
  //             +--------+
  //             |  init  |
  //             +--------+
  //                 |
  //                 v
  //            +----------+
  //            |  header  | <---------------+
  //            +----------+                 |
  //                 |                       |
  //                 v                       |
  //            +----------+                 |
  //            |  check   |                 |
  //            +----------+                 |
  //                 |                       |
  //         +-------+-------+               |
  //         | false         | true          |
  //         |               v               |
  //         |            +------+     +----------+
  //         |            | body | --> | continue |
  //         v            +------+     +----------+
  //     +-------+
  //     | merge |
  //     +-------+
  // The reason we don't unconditionally apply this transformation, which is
  // technically always legal, is because it prevents loop unrolling in SPIR-V
  // Tools, which does not support unrolling loops with early breaks.
  // For more details, see "2.11. Structured Control Flow" in the SPIR-V spec.
  const spv::LoopControlMask loopControl =
      attrs.empty() ? spv::LoopControlMask::MaskNone
                    : translateLoopAttribute(forStmt, *attrs.front());

  const Stmt *initStmt = forStmt->getInit();
  const Stmt *body = forStmt->getBody();
  const Expr *check = forStmt->getCond();

  bool checkHasShortcircuitedOp = stmtTreeContainsShortCircuitedOp(check);

  // Create basic blocks.
  auto *checkBB = spvBuilder.createBasicBlock("for.check");
  auto *headerBB = checkHasShortcircuitedOp
                       ? spvBuilder.createBasicBlock("for.header")
                       : checkBB;
  auto *bodyBB = spvBuilder.createBasicBlock("for.body");
  auto *continueBB = spvBuilder.createBasicBlock("for.continue");
  auto *mergeBB = spvBuilder.createBasicBlock("for.merge");

  // Make sure any continue statements branch to the continue block, and any
  // break statements branch to the merge block.
  continueStack.push(continueBB);
  breakStack.push(mergeBB);

  // Process the <init> block.
  if (initStmt) {
    doStmt(initStmt);
  }
  spvBuilder.createBranch(
      headerBB, check ? check->getLocStart() : forStmt->getLocStart(), nullptr,
      nullptr, spv::LoopControlMask::MaskNone,
      initStmt ? initStmt->getSourceRange()
               : SourceRange(forStmt->getLocStart(), forStmt->getLocStart()));
  spvBuilder.addSuccessor(headerBB);

  if (checkHasShortcircuitedOp) {
    // Process the <header> block.
    spvBuilder.setInsertPoint(headerBB);
    spvBuilder.createBranch(
        checkBB,
        check ? check->getLocStart()
              : (body ? body->getLocStart() : forStmt->getLocStart()),
        mergeBB, continueBB, loopControl,
        check ? check->getSourceRange()
              : (initStmt ? initStmt->getSourceRange()
                          : SourceRange(forStmt->getLocStart(),
                                        forStmt->getLocStart())));
    spvBuilder.addSuccessor(checkBB);
    // The current basic block has a OpLoopMerge instruction. We need to set
    // its continue and merge target.
    spvBuilder.setContinueTarget(continueBB);
    spvBuilder.setMergeTarget(mergeBB);

    // Process the <check> block.
    spvBuilder.setInsertPoint(checkBB);
    SpirvInstruction *condition = doExpr(check);
    spvBuilder.createConditionalBranch(
        condition, bodyBB, mergeBB,
        check ? check->getLocEnd()
              : (body ? body->getLocStart() : forStmt->getLocStart()),
        nullptr, nullptr, spv::SelectionControlMask::MaskNone,
        spv::LoopControlMask::MaskNone,
        check ? check->getSourceRange()
              : (initStmt ? initStmt->getSourceRange()
                          : SourceRange(forStmt->getLocStart(),
                                        forStmt->getLocStart())));
    spvBuilder.addSuccessor(bodyBB);
    spvBuilder.addSuccessor(mergeBB);
  } else {
    // In the case of simple or empty conditions, we can use a
    // single block for <check> and <header>.
    spvBuilder.setInsertPoint(checkBB);
    SpirvInstruction *condition = nullptr;
    if (check) {
      condition = doExpr(check);
    } else {
      condition = spvBuilder.getConstantBool(true);
    }
    spvBuilder.createConditionalBranch(
        condition, bodyBB, mergeBB,
        check ? check->getLocEnd()
              : (body ? body->getLocStart() : forStmt->getLocStart()),
        mergeBB, continueBB, spv::SelectionControlMask::MaskNone, loopControl,
        check ? check->getSourceRange()
              : (initStmt ? initStmt->getSourceRange()
                          : SourceRange(forStmt->getLocStart(),
                                        forStmt->getLocStart())));
    spvBuilder.addSuccessor(bodyBB);
    spvBuilder.addSuccessor(mergeBB);
    // The current basic block has a OpLoopMerge instruction. We need to set
    // its continue and merge target.
    spvBuilder.setContinueTarget(continueBB);
    spvBuilder.setMergeTarget(mergeBB);
  }

  // Process the <body> block.
  spvBuilder.setInsertPoint(bodyBB);
  if (body) {
    doStmt(body);
  }
  const Expr *cont = forStmt->getInc();
  if (!spvBuilder.isCurrentBasicBlockTerminated())
    spvBuilder.createBranch(
        continueBB, forStmt->getLocEnd(), nullptr, nullptr,
        spv::LoopControlMask::MaskNone,
        cont ? cont->getSourceRange()
             : SourceRange(forStmt->getLocStart(), forStmt->getLocStart()));
  spvBuilder.addSuccessor(continueBB);

  // Process the <continue> block. It will jump back to the header.
  spvBuilder.setInsertPoint(continueBB);
  if (cont) {
    doExpr(cont);
  }
  spvBuilder.createBranch(
      headerBB, forStmt->getLocEnd(), nullptr, nullptr,
      spv::LoopControlMask::MaskNone,
      cont ? cont->getSourceRange()
           : SourceRange(forStmt->getLocStart(), forStmt->getLocStart()));
  spvBuilder.addSuccessor(headerBB);

  // Set insertion point to the <merge> block for subsequent statements.
  spvBuilder.setInsertPoint(mergeBB);

  // Done with the current scope's continue block and merge block.
  continueStack.pop();
  breakStack.pop();
}

void SpirvEmitter::doIfStmt(const IfStmt *ifStmt,
                            llvm::ArrayRef<const Attr *> attrs) {
  // if statements are composed of:
  //   if (<check>) { <then> } else { <else> }
  //
  // To translate if statements, we'll need to emit the <check> expressions
  // in the current basic block, and then create separate basic blocks for
  // <then> and <else>. Additionally, we'll need a <merge> block as per
  // SPIR-V's structured control flow requirements. Depending whether there
  // exists the else branch, the final CFG should normally be like the
  // following. Exceptions will occur with non-local exits like loop breaks
  // or early returns.
  //             +-------+                        +-------+
  //             | check |                        | check |
  //             +-------+                        +-------+
  //                 |                                |
  //         +-------+-------+                  +-----+-----+
  //         | true          | false            | true      | false
  //         v               v         or       v           |
  //     +------+         +------+           +------+       |
  //     | then |         | else |           | then |       |
  //     +------+         +------+           +------+       |
  //         |               |                  |           v
  //         |   +-------+   |                  |     +-------+
  //         +-> | merge | <-+                  +---> | merge |
  //             +-------+                            +-------+

  { // Try to see if we can const-eval the condition
    bool condition = false;
    if (ifStmt->getCond()->EvaluateAsBooleanCondition(condition, astContext)) {
      if (condition) {
        doStmt(ifStmt->getThen());
      } else if (ifStmt->getElse()) {
        doStmt(ifStmt->getElse());
      }
      return;
    }
  }

  auto selectionControl = spv::SelectionControlMask::MaskNone;
  if (!attrs.empty()) {
    const Attr *attribute = attrs.front();
    switch (attribute->getKind()) {
    case attr::HLSLBranch:
      selectionControl = spv::SelectionControlMask::DontFlatten;
      break;
    case attr::HLSLFlatten:
      selectionControl = spv::SelectionControlMask::Flatten;
      break;
    default:
      // warning emitted in hlsl::ProcessStmtAttributeForHLSL
      break;
    }
  }

  if (const auto *declStmt = ifStmt->getConditionVariableDeclStmt())
    doDeclStmt(declStmt);

  // First emit the instruction for evaluating the condition.
  auto *cond = ifStmt->getCond();
  auto *condition = doExpr(cond);

  // Then we need to emit the instruction for the conditional branch.
  // We'll need the <label-id> for the then/else/merge block to do so.
  const bool hasElse = ifStmt->getElse() != nullptr;
  auto *thenBB = spvBuilder.createBasicBlock("if.true");
  auto *mergeBB = spvBuilder.createBasicBlock("if.merge");
  auto *elseBB = hasElse ? spvBuilder.createBasicBlock("if.false") : mergeBB;

  // Create the branch instruction. This will end the current basic block.
  const auto *then = ifStmt->getThen();
  spvBuilder.createConditionalBranch(
      condition, thenBB, elseBB, then->getLocStart(), mergeBB,
      /*continue*/ 0, selectionControl, spv::LoopControlMask::MaskNone,
      cond->getSourceRange());
  spvBuilder.addSuccessor(thenBB);
  spvBuilder.addSuccessor(elseBB);
  // The current basic block has the OpSelectionMerge instruction. We need
  // to record its merge target.
  spvBuilder.setMergeTarget(mergeBB);

  // Handle the then branch
  spvBuilder.setInsertPoint(thenBB);
  doStmt(then);
  if (!spvBuilder.isCurrentBasicBlockTerminated())
    spvBuilder.createBranch(mergeBB, ifStmt->getLocEnd(), nullptr, nullptr,
                            spv::LoopControlMask::MaskNone,
                            SourceRange(then->getLocEnd(), then->getLocEnd()));
  spvBuilder.addSuccessor(mergeBB);

  // Handle the else branch (if exists)
  if (hasElse) {
    spvBuilder.setInsertPoint(elseBB);
    const auto *elseStmt = ifStmt->getElse();
    doStmt(elseStmt);
    if (!spvBuilder.isCurrentBasicBlockTerminated())
      spvBuilder.createBranch(
          mergeBB, elseStmt->getLocEnd(), nullptr, nullptr,
          spv::LoopControlMask::MaskNone,
          SourceRange(elseStmt->getLocEnd(), elseStmt->getLocEnd()));
    spvBuilder.addSuccessor(mergeBB);
  }

  // From now on, we'll emit instructions into the merge block.
  spvBuilder.setInsertPoint(mergeBB);
}

void SpirvEmitter::doReturnStmt(const ReturnStmt *stmt) {
  const auto *retVal = stmt->getRetValue();
  bool returnsVoid = curFunction->getReturnType().getTypePtr()->isVoidType();
  if (!returnsVoid) {
    assert(retVal);
    const Expr *srcExpr = retVal->IgnoreParenCasts();
    if (isDescriptorHeap(srcExpr)) {
      const Expr *base = nullptr;
      getDescriptorHeapOperands(srcExpr, &base, /* index= */ nullptr);
      const Expr *parentExpr = cast<CastExpr>(parentMap->getParent(srcExpr));
      QualType resourceType = parentExpr->getType();
      const auto *declRefExpr = dyn_cast<DeclRefExpr>(base->IgnoreCasts());
      auto *decl = cast<VarDecl>(declRefExpr->getDecl());
      declIdMapper.createResourceHeap(decl, resourceType);
    }

    auto *retInfo = loadIfGLValue(retVal);
    if (!retInfo)
      return;

    // Update counter variable associated with function returns
    tryToAssignCounterVar(curFunction, retVal);

    auto retType = retVal->getType();
    if (retInfo->getLayoutRule() != SpirvLayoutRule::Void &&
        retType->isStructureType()) {
      // We are returning some value from a non-Function storage class. Need to
      // create a temporary variable to "convert" the value to Function storage
      // class and then return.
      auto *tempVar =
          spvBuilder.addFnVar(retType, retVal->getLocEnd(), "temp.var.ret");
      storeValue(tempVar, retInfo, retType, retVal->getLocEnd());
      spvBuilder.createReturnValue(
          spvBuilder.createLoad(retType, tempVar, retVal->getLocEnd()),
          stmt->getReturnLoc());
    } else {
      spvBuilder.createReturnValue(retInfo, stmt->getReturnLoc(),
                                   {stmt->getReturnLoc(), retVal->getLocEnd()});
    }
  } else {
    if (retVal) {
      loadIfGLValue(retVal);
    }
    spvBuilder.createReturn(stmt->getReturnLoc());
  }

  // We are translating a ReturnStmt, we should be in some function's body.
  assert(curFunction->hasBody());
  // If this return statement is the last statement in the function, then
  // whe have no more work to do.
  if (cast<CompoundStmt>(curFunction->getBody())->body_back() == stmt)
    return;

  // Some statements that alter the control flow (break, continue, return, and
  // discard), require creation of a new basic block to hold any statement that
  // may follow them. In this case, the newly created basic block will contain
  // any statement that may come after an early return.
  auto *newBB = spvBuilder.createBasicBlock();
  spvBuilder.setInsertPoint(newBB);
}

void SpirvEmitter::doBreakStmt(const BreakStmt *breakStmt) {
  assert(!spvBuilder.isCurrentBasicBlockTerminated());
  auto *breakTargetBB = breakStack.top();
  spvBuilder.addSuccessor(breakTargetBB);
  spvBuilder.createBranch(breakTargetBB, breakStmt->getLocStart());

  // Some statements that alter the control flow (break, continue, return, and
  // discard), require creation of a new basic block to hold any statement that
  // may follow them. For example: StmtB and StmtC below are put inside a new
  // basic block which is unreachable.
  //
  // while (true) {
  //   StmtA;
  //   break;
  //   StmtB;
  //   StmtC;
  // }
  auto *newBB = spvBuilder.createBasicBlock();
  spvBuilder.setInsertPoint(newBB);
}

void SpirvEmitter::doSwitchStmt(const SwitchStmt *switchStmt,
                                llvm::ArrayRef<const Attr *> attrs) {
  // Switch statements are composed of:
  //   switch (<condition variable>) {
  //     <CaseStmt>
  //     <CaseStmt>
  //     <CaseStmt>
  //     <DefaultStmt> (optional)
  //   }
  //
  //                             +-------+
  //                             | check |
  //                             +-------+
  //                                 |
  //         +-------+-------+----------------+---------------+
  //         | 1             | 2              | 3             | (others)
  //         v               v                v               v
  //     +-------+      +-------------+     +-------+     +------------+
  //     | case1 |      | case2       |     | case3 | ... | default    |
  //     |       |      |(fallthrough)|---->|       |     | (optional) |
  //     +-------+      |+------------+     +-------+     +------------+
  //         |                                  |                |
  //         |                                  |                |
  //         |   +-------+                      |                |
  //         |   |       | <--------------------+                |
  //         +-> | merge |                                       |
  //             |       | <-------------------------------------+
  //             +-------+

  // If no attributes are given, or if "forcecase" attribute was provided,
  // we'll do our best to use OpSwitch if possible.
  // If any of the cases compares to a variable (rather than an integer
  // literal), we cannot use OpSwitch because OpSwitch expects literal
  // numbers as parameters.
  const bool isAttrForceCase =
      !attrs.empty() && attrs.front()->getKind() == attr::HLSLForceCase;
  const bool canUseSpirvOpSwitch =
      (attrs.empty() || isAttrForceCase) &&
      allSwitchCasesAreIntegerLiterals(switchStmt->getBody());

  if (isAttrForceCase && !canUseSpirvOpSwitch &&
      !spirvOptions.noWarnIgnoredFeatures) {
    emitWarning("ignored 'forcecase' attribute for the switch statement "
                "since one or more case values are not integer literals",
                switchStmt->getLocStart());
  }

  if (canUseSpirvOpSwitch)
    processSwitchStmtUsingSpirvOpSwitch(switchStmt);
  else
    processSwitchStmtUsingIfStmts(switchStmt);
}

SpirvInstruction *
SpirvEmitter::doArraySubscriptExpr(const ArraySubscriptExpr *expr,
                                   SourceRange rangeOverride) {
  Expr *base = const_cast<Expr *>(expr->getBase()->IgnoreParenLValueCasts());

  auto *info = loadIfAliasVarRef(base);
  SourceRange range =
      (rangeOverride != SourceRange()) ? rangeOverride : expr->getSourceRange();

  if (!info) {
    return info;
  }

  // The index into an array must be an integer number.
  const auto *idxExpr = expr->getIdx();
  const auto idxExprType = idxExpr->getType();
  SpirvInstruction *thisIndex = loadIfGLValue(idxExpr);
  if (!idxExprType->isIntegerType() || idxExprType->isBooleanType()) {
    thisIndex = castToInt(thisIndex, idxExprType, astContext.UnsignedIntTy,
                          idxExpr->getExprLoc());
  }

  llvm::SmallVector<SpirvInstruction *, 4> indices = {thisIndex};

  SpirvInstruction *loadVal =
      derefOrCreatePointerToValue(base->getType(), info, expr->getType(),
                                  indices, base->getExprLoc(), range);

  // TODO(#6259): This maintains the same incorrect behaviour as before.
  // When GetAttributeAtVertex is used, the array will be duplicated instead
  // of duplicating the elements of the array. This means that the access chain
  // feeding this one needs to be marked no interpolation, but this access chain
  // does not. However, this is still wrong in cases that were wrong before.
  loadVal->setNoninterpolated(false);
  return loadVal;
}

SpirvInstruction *SpirvEmitter::doBinaryOperator(const BinaryOperator *expr) {
  const auto opcode = expr->getOpcode();

  // Handle assignment first since we need to evaluate rhs before lhs.
  // For other binary operations, we need to evaluate lhs before rhs.
  if (opcode == BO_Assign) {
    // Update counter variable associated with lhs of assignments
    tryToAssignCounterVar(expr->getLHS(), expr->getRHS());

    return processAssignment(expr->getLHS(), loadIfGLValue(expr->getRHS()),
                             /*isCompoundAssignment=*/false, nullptr,
                             expr->getSourceRange());
  }

  // Try to optimize floatMxN * float and floatN * float case
  if (opcode == BO_Mul) {
    if (auto *result = tryToGenFloatMatrixScale(expr))
      return result;
    if (auto *result = tryToGenFloatVectorScale(expr))
      return result;
  }

  return processBinaryOp(expr->getLHS(), expr->getRHS(), opcode,
                         expr->getLHS()->getType(), expr->getType(),
                         expr->getSourceRange(), expr->getOperatorLoc());
}

SpirvInstruction *SpirvEmitter::doCallExpr(const CallExpr *callExpr,
                                           SourceRange rangeOverride) {
  if (const auto *operatorCall = dyn_cast<CXXOperatorCallExpr>(callExpr)) {
    if (const auto *cxxMethodDecl =
            dyn_cast<CXXMethodDecl>(operatorCall->getCalleeDecl())) {
      QualType parentType =
          QualType(cxxMethodDecl->getParent()->getTypeForDecl(), 0);
      if (hlsl::IsUserDefinedRecordType(parentType)) {
        // If the parent is a user-defined record type
        return processCall(callExpr);
      }
    }
    return doCXXOperatorCallExpr(operatorCall, rangeOverride);
  }

  if (const auto *memberCall = dyn_cast<CXXMemberCallExpr>(callExpr))
    return doCXXMemberCallExpr(memberCall);

  auto funcDecl = callExpr->getDirectCallee();
  if (funcDecl) {
    if (funcDecl->hasAttr<VKInstructionExtAttr>())
      return processSpvIntrinsicCallExpr(callExpr);
    else if (funcDecl->hasAttr<VKTypeDefExtAttr>())
      return processSpvIntrinsicTypeDef(callExpr);
  }
  // Intrinsic functions such as 'dot' or 'mul'
  if (hlsl::IsIntrinsicOp(funcDecl)) {
    return processIntrinsicCallExpr(callExpr);
  }

  // Handle 'vk::RawBufferLoad()'
  if (isVkRawBufferLoadIntrinsic(funcDecl)) {
    return processRawBufferLoad(callExpr);
  }

  // Handle CooperativeMatrix::GetLength()
  if (isCooperativeMatrixGetLengthIntrinsic(funcDecl)) {
    return processCooperativeMatrixGetLength(callExpr);
  }

  // Normal standalone functions
  return processCall(callExpr);
}

SpirvInstruction *SpirvEmitter::getBaseOfMemberFunction(
    QualType objectType, SpirvInstruction *objInstr,
    const CXXMethodDecl *memberFn, SourceLocation loc) {
  // If objectType is different from the parent of memberFn, memberFn should be
  // defined in a base struct/class of objectType. We create OpAccessChain with
  // index 0 while iterating bases of objectType until we find the base with
  // the definition of memberFn.
  if (const auto *ptrType = objectType->getAs<PointerType>()) {
    if (const auto *recordType =
            ptrType->getPointeeType()->getAs<RecordType>()) {
      const auto *parentDeclOfMemberFn = memberFn->getParent();
      if (recordType->getDecl() != parentDeclOfMemberFn) {
        const auto *cxxRecordDecl =
            dyn_cast<CXXRecordDecl>(recordType->getDecl());
        auto *zero = spvBuilder.getConstantInt(astContext.UnsignedIntTy,
                                               llvm::APInt(32, 0));
        for (auto baseItr = cxxRecordDecl->bases_begin(),
                  itrEnd = cxxRecordDecl->bases_end();
             baseItr != itrEnd; baseItr++) {
          const auto *baseType = baseItr->getType()->getAs<RecordType>();
          objectType = astContext.getPointerType(baseType->desugar());
          objInstr =
              spvBuilder.createAccessChain(objectType, objInstr, {zero}, loc);
          if (baseType->getDecl() == parentDeclOfMemberFn)
            return objInstr;
        }
      }
    }
  }
  return nullptr;
}

SpirvInstruction *SpirvEmitter::processCall(const CallExpr *callExpr) {
  const FunctionDecl *callee = getCalleeDefinition(callExpr);

  // Note that we always want the definition because Stmts/Exprs in the
  // function body reference the parameters in the definition.
  if (!callee) {
    emitError("found undefined function", callExpr->getExprLoc());
    return nullptr;
  }

  const auto paramTypeMatchesArgType = [](QualType paramType,
                                          QualType argType) {
    if (argType == paramType)
      return true;

    if (const auto *refType = paramType->getAs<ReferenceType>())
      paramType = refType->getPointeeType();
    auto argUnqualifiedType = argType->getUnqualifiedDesugaredType();
    auto paramUnqualifiedType = paramType->getUnqualifiedDesugaredType();
    if (argUnqualifiedType == paramUnqualifiedType)
      return true;

    return false;
  };

  const auto numParams = callee->getNumParams();

  bool isNonStaticMemberCall = false;
  bool isOperatorOverloading = false;
  QualType objectType = {};             // Type of the object (if exists)
  SpirvInstruction *objInstr = nullptr; // EvalInfo for the object (if exists)
  const Expr *object;

  llvm::SmallVector<SpirvInstruction *, 4> vars; // Variables for function call
  llvm::SmallVector<bool, 4> isTempVar;          // Temporary variable or not
  llvm::SmallVector<SpirvInstruction *, 4> args; // Evaluated arguments

  if (const auto *memberCall = dyn_cast<CXXMemberCallExpr>(callExpr)) {
    const auto *memberFn = cast<CXXMethodDecl>(memberCall->getCalleeDecl());
    isNonStaticMemberCall = !memberFn->isStatic();

    if (isNonStaticMemberCall) {
      // For non-static member calls, evaluate the object and pass it as the
      // first argument.
      object = memberCall->getImplicitObjectArgument();
      object = object->IgnoreParenNoopCasts(astContext);

      // Update counter variable associated with the implicit object
      tryToAssignCounterVar(getOrCreateDeclForMethodObject(memberFn), object);

      objectType = object->getType();
      objInstr = doExpr(object);
      if (auto *accessToBaseInstr = getBaseOfMemberFunction(
              objectType, objInstr, memberFn, memberCall->getExprLoc())) {
        objInstr = accessToBaseInstr;
        objectType = accessToBaseInstr->getAstResultType();
      }
    }
  } else if (const auto *operatorCallExpr =
                 dyn_cast<CXXOperatorCallExpr>(callExpr)) {
    isOperatorOverloading = true;
    isNonStaticMemberCall = true;
    // For overloaded operator calls, the first argument is considered as the
    // object.
    object = operatorCallExpr->getArg(0);
    object = object->IgnoreParenNoopCasts(astContext);
    objectType = object->getType();
    objInstr = doExpr(object);
  }

  if (objInstr != nullptr) {
    // If not already a variable, we need to create a temporary variable and
    // pass the object pointer to the function. Example:
    // getObject().objectMethod();
    // Also, any parameter passed to the member function must be of Function
    // storage class.
    if (objInstr->isRValue()) {
      args.push_back(createTemporaryVar(
          objectType, getAstTypeName(objectType),
          // May need to load to use as initializer
          loadIfGLValue(object, objInstr), object->getLocStart()));
    } else {
      // Based on SPIR-V spec, function parameter must always be in Function
      // scope. If we pass a non-function scope argument, we need
      // the legalization.
      if (objInstr->getStorageClass() != spv::StorageClass::Function ||
          !isMemoryObjectDeclaration(objInstr))
        needsLegalization = true;

      args.push_back(objInstr);
    }

    // We do not need to create a new temporary variable for the this
    // object. Use the evaluated argument.
    vars.push_back(args.back());
    isTempVar.push_back(false);
  }

  // Evaluate parameters
  for (uint32_t i = 0; i < numParams; ++i) {
    // Arguments for the overloaded operator includes the object itself. The
    // actual argument starts from the second one.
    const uint32_t argIndex = i + isOperatorOverloading;

    // We want the argument variable here so that we can write back to it
    // later. We will do the OpLoad of this argument manually. So ignore
    // the LValueToRValue implicit cast here.
    auto *arg = callExpr->getArg(argIndex)->IgnoreParenLValueCasts();
    const auto *param = callee->getParamDecl(i);
    const auto paramType = param->getType();

    if (isResourceDescriptorHeap(paramType) ||
        isSamplerDescriptorHeap(paramType)) {
      emitError(
          "Resource/sampler heaps are not allowed as function parameters.",
          param->getLocStart());
      return nullptr;
    }

    // Get the evaluation info if this argument is referencing some variable
    // *as a whole*, in which case we can avoid creating the temporary variable
    // for it if it can act as out parameter.
    SpirvInstruction *argInfo = nullptr;
    if (const auto *declRefExpr = dyn_cast<DeclRefExpr>(arg)) {
      argInfo = declIdMapper.getDeclEvalInfo(declRefExpr->getDecl(),
                                             arg->getLocStart());
    }

    auto *argInst = doExpr(arg);

    bool isArgGlobalVarWithResourceType =
        argInfo && argInfo->getStorageClass() != spv::StorageClass::Function &&
        isResourceType(paramType);

    // If argInfo is nullptr and argInst is a rvalue, we do not have a proper
    // pointer to pass to the function. we need a temporary variable in that
    // case.
    //
    // If we have an 'out/inout' resource as function argument, we need to
    // create a temporary variable for it because the function definition
    // expects are point-to-pointer argument for resources, which will be
    // resolved by legalization.
    if ((argInfo || (argInst && !argInst->isRValue())) &&
        canActAsOutParmVar(param) && !isArgGlobalVarWithResourceType &&
        paramTypeMatchesArgType(paramType, arg->getType())) {
      // Based on SPIR-V spec, function parameter must be always Function
      // scope. In addition, we must pass memory object declaration argument
      // to function. If we pass an argument that is not function scope
      // or not memory object declaration, we need the legalization.
      if (!argInfo || argInfo->getStorageClass() != spv::StorageClass::Function)
        needsLegalization = true;

      isTempVar.push_back(false);
      args.push_back(argInst);
      vars.push_back(argInfo ? argInfo : argInst);
    } else {
      // We need to create variables for holding the values to be used as
      // arguments. The variables themselves are of pointer types.
      const QualType varType =
          declIdMapper.getTypeAndCreateCounterForPotentialAliasVar(param);
      const std::string varName = "param.var." + param->getNameAsString();
      // Temporary "param.var.*" variables are used for OpFunctionCall purposes.
      // 'precise' attribute on function parameters only affect computations
      // inside the function, not the variables at the call sites. Therefore, we
      // do not need to mark the "param.var.*" variables as precise.
      const bool isPrecise = false;
      const bool isNoInterp = param->hasAttr<HLSLNoInterpolationAttr>() ||
                              (argInst && argInst->isNoninterpolated());

      auto *tempVar = spvBuilder.addFnVar(varType, arg->getLocStart(), varName,
                                          isPrecise, isNoInterp);

      vars.push_back(tempVar);
      isTempVar.push_back(true);
      args.push_back(argInst);

      // Update counter variable associated with function parameters
      tryToAssignCounterVar(param, arg);

      // Manually load the argument here
      auto *rhsVal = loadIfGLValue(arg, args.back());
      auto rhsRange = arg->getSourceRange();

      // The AST does not include cast nodes to and from the function parameter
      // type for 'out' and 'inout' cases. Example:
      //
      // void foo(out half3 param) {...}
      // void main() { float3 arg; foo(arg); }
      //
      // In such cases, we first do a manual cast before passing the argument to
      // the function. And we will cast back the results once the function call
      // has returned.
      if (canActAsOutParmVar(param) &&
          !paramTypeMatchesArgType(paramType, arg->getType())) {
        if (const auto *refType = paramType->getAs<ReferenceType>()) {
          QualType toType = refType->getPointeeType();
          if (isScalarType(rhsVal->getAstResultType())) {
            rhsVal =
                splatScalarToGenerate(toType, rhsVal, SpirvLayoutRule::Void);
          } else {
            rhsVal = castToType(rhsVal, rhsVal->getAstResultType(), toType,
                                arg->getLocStart(), rhsRange);
          }
        }
      }

      // Initialize the temporary variables using the contents of the arguments
      storeValue(tempVar, rhsVal, paramType, arg->getLocStart(), rhsRange);
    }
  }

  assert(vars.size() == isTempVar.size());
  assert(vars.size() == args.size());

  // Push the callee into the work queue if it is not there.
  addFunctionToWorkQueue(spvContext.getCurrentShaderModelKind(), callee,
                         /*isEntryFunction*/ false);

  const QualType retType =
      declIdMapper.getTypeAndCreateCounterForPotentialAliasVar(callee);
  // Get or forward declare the function <result-id>
  SpirvFunction *func = declIdMapper.getOrRegisterFn(callee);

  auto *retVal = spvBuilder.createFunctionCall(
      retType, func, vars, callExpr->getCallee()->getExprLoc(),
      callExpr->getSourceRange());

  // Go through all parameters and write those marked as out/inout
  for (uint32_t i = 0; i < numParams; ++i) {
    const auto *param = callee->getParamDecl(i);
    const auto paramType = param->getType();
    // If it calls a non-static member function, the object itself is argument
    // 0, and therefore all other argument positions are shifted by 1.
    const uint32_t index = i + isNonStaticMemberCall;
    // Using a resouce as a function parameter is never passed-by-copy. As a
    // result, even if the function parameter is marked as 'out' or 'inout',
    // there is no reason to copy back the results after the function call into
    // the resource.
    if (isTempVar[index] && canActAsOutParmVar(param) &&
        !isResourceType(paramType)) {
      // Arguments for the overloaded operator includes the object itself. The
      // actual argument starts from the second one.
      const uint32_t argIndex = i + isOperatorOverloading;

      const auto *arg = callExpr->getArg(argIndex);
      SpirvInstruction *value =
          spvBuilder.createLoad(paramType, vars[index], arg->getLocStart());

      // Now we want to assign 'value' to arg. But first, in rare cases when
      // using 'out' or 'inout' where the parameter and argument have a type
      // mismatch, we need to first cast 'value' to the type of 'arg' because
      // the AST will not include a cast node.
      if (!paramTypeMatchesArgType(paramType, arg->getType())) {
        if (const auto *refType = paramType->getAs<ReferenceType>()) {
          QualType elementType;
          QualType fromType = refType->getPointeeType();
          if (isVectorType(fromType, &elementType) &&
              isScalarType(arg->getType())) {
            value = spvBuilder.createCompositeExtract(
                elementType, value, {0}, value->getSourceLocation());
            fromType = elementType;
          }
          value =
              castToType(value, fromType, arg->getType(), arg->getLocStart());
        }
      }

      processAssignment(arg, value, false, args[index]);
    }
  }

  return retVal;
}

SpirvInstruction *SpirvEmitter::doCastExpr(const CastExpr *expr,
                                           SourceRange rangeOverride) {
  const Expr *subExpr = expr->getSubExpr();
  const QualType subExprType = subExpr->getType();
  const QualType toType = expr->getType();
  const auto srcLoc = expr->getExprLoc();
  SourceRange range =
      (rangeOverride != SourceRange()) ? rangeOverride : expr->getSourceRange();

  // The AST type for descriptor heap is not well defined. This means we needed
  // to look at the destination type already to generate the source type.
  // This makes implicit casts from heaps useless, and we can ignore them.
  // If you want to remove this check, the flat conversion heap->type needs to
  // be implemented, which would mostly duplicate the initial heap creation
  // code.
  if (isResourceDescriptorHeap(subExprType) ||
      isSamplerDescriptorHeap(subExprType)) {
    return doExpr(subExpr, range);
  }

  switch (expr->getCastKind()) {
  case CastKind::CK_LValueToRValue:
    return loadIfGLValue(subExpr, range);
  case CastKind::CK_NoOp:
    return doExpr(subExpr, range);
  case CastKind::CK_IntegralCast:
  case CastKind::CK_FloatingToIntegral:
  case CastKind::CK_HLSLCC_IntegralCast:
  case CastKind::CK_HLSLCC_FloatingToIntegral: {
    // Integer literals in the AST are represented using 64bit APInt
    // themselves and then implicitly casted into the expected bitwidth.
    // We need special treatment of integer literals here because generating
    // a 64bit constant and then explicit casting in SPIR-V requires Int64
    // capability. We should avoid introducing unnecessary capabilities to
    // our best.
    if (auto *value =
            constEvaluator.tryToEvaluateAsConst(expr, isSpecConstantMode)) {
      value->setRValue();
      return value;
    }

    auto *value = castToInt(loadIfGLValue(subExpr), subExprType, toType,
                            subExpr->getLocStart(), range);
    if (!value)
      return nullptr;

    value->setRValue();
    return value;
  }
  case CastKind::CK_FloatingCast:
  case CastKind::CK_IntegralToFloating:
  case CastKind::CK_HLSLCC_FloatingCast:
  case CastKind::CK_HLSLCC_IntegralToFloating: {
    // First try to see if we can do constant folding for floating point
    // numbers like what we are doing for integers in the above.
    if (auto *value =
            constEvaluator.tryToEvaluateAsConst(expr, isSpecConstantMode)) {
      value->setRValue();
      return value;
    }

    auto *value = castToFloat(loadIfGLValue(subExpr), subExprType, toType,
                              subExpr->getLocStart(), range);
    if (!value)
      return nullptr;

    value->setRValue();
    return value;
  }
  case CastKind::CK_IntegralToBoolean:
  case CastKind::CK_FloatingToBoolean:
  case CastKind::CK_HLSLCC_IntegralToBoolean:
  case CastKind::CK_HLSLCC_FloatingToBoolean: {
    // First try to see if we can do constant folding.
    if (auto *value =
            constEvaluator.tryToEvaluateAsConst(expr, isSpecConstantMode)) {
      value->setRValue();
      return value;
    }

    auto *value = castToBool(loadIfGLValue(subExpr), subExprType, toType,
                             subExpr->getLocStart(), range);
    if (!value)
      return nullptr;

    value->setRValue();
    return value;
  }
  case CastKind::CK_HLSLVectorSplat: {
    const size_t size = hlsl::GetHLSLVecSize(expr->getType());
    return createVectorSplat(subExpr, size, range);
  }
  case CastKind::CK_HLSLVectorTruncationCast: {
    const QualType toVecType = toType;
    const QualType elemType = hlsl::GetHLSLVecElementType(toType);
    const auto toSize = hlsl::GetHLSLVecSize(toType);
    auto *composite = doExpr(subExpr, range);
    llvm::SmallVector<SpirvInstruction *, 4> elements;

    for (uint32_t i = 0; i < toSize; ++i) {
      elements.push_back(spvBuilder.createCompositeExtract(
          elemType, composite, {i}, expr->getExprLoc(), range));
    }

    auto *value = elements.front();
    if (toSize > 1) {
      value = spvBuilder.createCompositeConstruct(toVecType, elements,
                                                  expr->getExprLoc(), range);
    }

    if (!value)
      return nullptr;

    value->setRValue();
    return value;
  }
  case CastKind::CK_HLSLVectorToScalarCast: {
    // The underlying should already be a vector of size 1.
    assert(hlsl::GetHLSLVecSize(subExprType) == 1);
    return doExpr(subExpr, range);
  }
  case CastKind::CK_HLSLVectorToMatrixCast: {
    // If target type is already an 1xN or Mx1 matrix type, we just return the
    // underlying vector.
    if (is1xNMatrix(toType) || isMx1Matrix(toType))
      return doExpr(subExpr, range);

    // A vector can have no more than 4 elements. The only remaining case
    // is casting from size-4 vector to size-2-by-2 matrix.

    auto *vec = loadIfGLValue(subExpr, range);
    QualType elemType = {};
    uint32_t rowCount = 0, colCount = 0;
    const bool isMat = isMxNMatrix(toType, &elemType, &rowCount, &colCount);
    assert(isMat && rowCount == 2 && colCount == 2);
    (void)isMat;
    QualType vec2Type = astContext.getExtVectorType(elemType, 2);
    auto *subVec1 = spvBuilder.createVectorShuffle(vec2Type, vec, vec, {0, 1},
                                                   expr->getLocStart(), range);
    auto *subVec2 = spvBuilder.createVectorShuffle(vec2Type, vec, vec, {2, 3},
                                                   expr->getLocStart(), range);
    auto *mat = spvBuilder.createCompositeConstruct(toType, {subVec1, subVec2},
                                                    expr->getLocStart(), range);
    if (!mat)
      return nullptr;

    mat->setRValue();
    return mat;
  }
  case CastKind::CK_HLSLMatrixSplat: {
    // From scalar to matrix
    uint32_t rowCount = 0, colCount = 0;
    hlsl::GetHLSLMatRowColCount(toType, rowCount, colCount);

    // Handle degenerated cases first
    if (rowCount == 1 && colCount == 1)
      return doExpr(subExpr, range);

    if (colCount == 1)
      return createVectorSplat(subExpr, rowCount, range);

    const auto vecSplat = createVectorSplat(subExpr, colCount, range);
    if (rowCount == 1)
      return vecSplat;

    if (isa<SpirvConstant>(vecSplat)) {
      llvm::SmallVector<SpirvConstant *, 4> vectors(
          size_t(rowCount), cast<SpirvConstant>(vecSplat));
      auto *value = spvBuilder.getConstantComposite(toType, vectors);
      if (!value)
        return nullptr;

      value->setRValue();
      return value;
    } else {
      llvm::SmallVector<SpirvInstruction *, 4> vectors(size_t(rowCount),
                                                       vecSplat);
      auto *value = spvBuilder.createCompositeConstruct(
          toType, vectors, expr->getLocEnd(), range);
      if (!value)
        return nullptr;

      value->setRValue();
      return value;
    }
  }
  case CastKind::CK_HLSLMatrixTruncationCast: {
    const QualType srcType = subExprType;
    auto *src = doExpr(subExpr, range);
    const QualType elemType = hlsl::GetHLSLMatElementType(srcType);
    llvm::SmallVector<uint32_t, 4> indexes;

    // It is possible that the source matrix is in fact a vector.
    // Example 1: Truncate float1x3 --> float1x2.
    // Example 2: Truncate float1x3 --> float1x1.
    // The front-end disallows float1x3 --> float2x1.
    {
      uint32_t srcVecSize = 0, dstVecSize = 0;
      if (isVectorType(srcType, nullptr, &srcVecSize) && isScalarType(toType)) {
        auto *val = spvBuilder.createCompositeExtract(
            toType, src, {0}, expr->getLocStart(), range);
        if (!val)
          return nullptr;

        val->setRValue();
        return val;
      }

      if (isVectorType(srcType, nullptr, &srcVecSize) &&
          isVectorType(toType, nullptr, &dstVecSize)) {
        for (uint32_t i = 0; i < dstVecSize; ++i)
          indexes.push_back(i);
        auto *val = spvBuilder.createVectorShuffle(toType, src, src, indexes,
                                                   expr->getLocStart(), range);
        if (!val)
          return nullptr;

        val->setRValue();
        return val;
      }
    }

    uint32_t srcRows = 0, srcCols = 0, dstRows = 0, dstCols = 0;
    hlsl::GetHLSLMatRowColCount(srcType, srcRows, srcCols);
    hlsl::GetHLSLMatRowColCount(toType, dstRows, dstCols);
    const QualType srcRowType = astContext.getExtVectorType(elemType, srcCols);
    const QualType dstRowType = astContext.getExtVectorType(elemType, dstCols);

    // Indexes to pass to OpVectorShuffle
    for (uint32_t i = 0; i < dstCols; ++i)
      indexes.push_back(i);

    llvm::SmallVector<SpirvInstruction *, 4> extractedVecs;
    for (uint32_t row = 0; row < dstRows; ++row) {
      // Extract a row
      SpirvInstruction *rowInstr = spvBuilder.createCompositeExtract(
          srcRowType, src, {row}, expr->getExprLoc(), range);
      // Extract the necessary columns from that row.
      // The front-end ensures dstCols <= srcCols.
      // If dstCols equals srcCols, we can use the whole row directly.
      if (dstCols == 1) {
        rowInstr = spvBuilder.createCompositeExtract(
            elemType, rowInstr, {0}, expr->getLocStart(), range);
      } else if (dstCols < srcCols) {
        rowInstr =
            spvBuilder.createVectorShuffle(dstRowType, rowInstr, rowInstr,
                                           indexes, expr->getLocStart(), range);
      }
      extractedVecs.push_back(rowInstr);
    }

    auto *val = extractedVecs.front();
    if (extractedVecs.size() > 1) {
      val = spvBuilder.createCompositeConstruct(toType, extractedVecs,
                                                expr->getExprLoc(), range);
    }
    if (!val)
      return nullptr;

    val->setRValue();
    return val;
  }
  case CastKind::CK_HLSLMatrixToScalarCast: {
    // The underlying should already be a matrix of 1x1.
    assert(is1x1Matrix(subExprType));
    return doExpr(subExpr, range);
  }
  case CastKind::CK_HLSLMatrixToVectorCast: {
    // If the underlying matrix is Mx1 or 1xM for M in {1, 2,3,4}, we can return
    // the underlying matrix because it'll be evaluated as a vector by default.
    if (is1x1Matrix(subExprType) || is1xNMatrix(subExprType) ||
        isMx1Matrix(subExprType))
      return doExpr(subExpr, range);

    // A vector can have no more than 4 elements. The only remaining case
    // is casting from a 2x2 matrix to a vector of size 4.

    auto *mat = loadIfGLValue(subExpr, range);
    QualType elemType = {};
    uint32_t rowCount = 0, colCount = 0, elemCount = 0;
    const bool isMat =
        isMxNMatrix(subExprType, &elemType, &rowCount, &colCount);
    const bool isVec = isVectorType(toType, nullptr, &elemCount);
    assert(isMat && rowCount == 2 && colCount == 2);
    assert(isVec && elemCount == 4);
    (void)isMat;
    (void)isVec;
    QualType vec2Type = astContext.getExtVectorType(elemType, 2);
    auto *row0 =
        spvBuilder.createCompositeExtract(vec2Type, mat, {0}, srcLoc, range);
    auto *row1 =
        spvBuilder.createCompositeExtract(vec2Type, mat, {1}, srcLoc, range);
    auto *vec = spvBuilder.createVectorShuffle(toType, row0, row1, {0, 1, 2, 3},
                                               srcLoc, range);
    if (!vec)
      return nullptr;

    vec->setRValue();
    return vec;
  }
  case CastKind::CK_FunctionToPointerDecay:
    // Just need to return the function id
    return doExpr(subExpr, range);
  case CastKind::CK_FlatConversion: {
    SpirvInstruction *subExprInstr = nullptr;
    QualType evalType = subExprType;

    // Optimization: we can use OpConstantNull for cases where we want to
    // initialize an entire data structure to zeros.
    if (evaluatesToConstZero(subExpr, astContext)) {
      subExprInstr = spvBuilder.getConstantNull(toType);
      subExprInstr->setRValue();
      return subExprInstr;
    }

    // Try to evaluate float literals as float rather than double.
    if (const auto *floatLiteral = dyn_cast<FloatingLiteral>(subExpr)) {
      subExprInstr = constEvaluator.tryToEvaluateAsFloat32(
          floatLiteral->getValue(), isSpecConstantMode);
      if (subExprInstr)
        evalType = astContext.FloatTy;
    }
    // Evaluate 'literal float' initializer type as float rather than double.
    // TODO: This could result in rounding error if the initializer is a
    // non-literal expression that requires larger than 32 bits and has the
    // 'literal float' type.
    else if (subExprType->isSpecificBuiltinType(BuiltinType::LitFloat)) {
      evalType = astContext.FloatTy;
    }
    // Try to evaluate integer literals as 32-bit int rather than 64-bit int.
    else if (const auto *intLiteral = dyn_cast<IntegerLiteral>(subExpr)) {
      const bool isSigned = subExprType->isSignedIntegerType();
      subExprInstr =
          constEvaluator.tryToEvaluateAsInt32(intLiteral->getValue(), isSigned);
      if (subExprInstr)
        evalType = isSigned ? astContext.IntTy : astContext.UnsignedIntTy;
    }
    // For assigning one array instance to another one with the same array type
    // (regardless of constness and literalness), the rhs will be wrapped in a
    // FlatConversion. Similarly for assigning a struct to another struct with
    // identical members.
    //  |- <lhs>
    //  `- ImplicitCastExpr <FlatConversion>
    //     `- ImplicitCastExpr <LValueToRValue>
    //        `- <rhs>
    else if (isSameType(astContext, toType, evalType) ||
             // We can have casts changing the shape but without affecting
             // memory order, e.g., `float4 a[2]; float b[8] = (float[8])a;`.
             // This is also represented as FlatConversion. For such cases, we
             // can rely on the InitListHandler, which can decompse
             // vectors/matrices.
             subExprType->isArrayType()) {
      auto *valInstr =
          InitListHandler(astContext, *this).processCast(toType, subExpr);
      if (valInstr)
        valInstr->setRValue();
      return valInstr;
    }
    // We can have casts changing the shape but without affecting memory order,
    // e.g., `float4 a[2]; float b[8] = (float[8])a;`. This is also represented
    // as FlatConversion. For such cases, we can rely on the InitListHandler,
    // which can decompse vectors/matrices.
    else if (subExprType->isArrayType()) {
      auto *valInstr = InitListHandler(astContext, *this)
                           .processCast(expr->getType(), subExpr);
      if (valInstr)
        valInstr->setRValue();
      return valInstr;
    }

    if (!subExprInstr)
      subExprInstr = loadIfGLValue(subExpr);

    if (!subExprInstr)
      return nullptr;

    auto *val =
        processFlatConversion(toType, subExprInstr, expr->getExprLoc(), range);
    val->setRValue();
    return val;
  }
  case CastKind::CK_UncheckedDerivedToBase:
  case CastKind::CK_HLSLDerivedToBase: {
    // Find the index sequence of the base to which we are casting
    llvm::SmallVector<uint32_t, 4> baseIndices;
    getBaseClassIndices(expr, &baseIndices);

    // Turn them in to SPIR-V constants
    llvm::SmallVector<SpirvInstruction *, 4> baseIndexInstructions(
        baseIndices.size(), nullptr);
    for (uint32_t i = 0; i < baseIndices.size(); ++i)
      baseIndexInstructions[i] = spvBuilder.getConstantInt(
          astContext.UnsignedIntTy, llvm::APInt(32, baseIndices[i]));

    auto *derivedInfo = doExpr(subExpr);
    return derefOrCreatePointerToValue(subExpr->getType(), derivedInfo,
                                       expr->getType(), baseIndexInstructions,
                                       subExpr->getExprLoc(), range);
  }
  case CastKind::CK_ArrayToPointerDecay: {
    // Literal string to const string conversion falls under this category.
    if (hlsl::IsStringLiteralType(subExprType) && hlsl::IsStringType(toType)) {
      return doExpr(subExpr, range);
    } else {
      emitError("implicit cast kind '%0' unimplemented", expr->getExprLoc())
          << expr->getCastKindName() << expr->getSourceRange();
      expr->dump();
      return nullptr;
    }
  }
  case CastKind::CK_ToVoid:
    return nullptr;
  case CastKind::CK_VK_BufferPointerToIntegral: {
    return spvBuilder.createConvertPtrToU(doExpr(subExpr, range), toType);
  }
  case CastKind::CK_VK_IntegralToBufferPointer: {
    return spvBuilder.createConvertUToPtr(doExpr(subExpr, range), toType);
  }
  default:
    emitError("implicit cast kind '%0' unimplemented", expr->getExprLoc())
        << expr->getCastKindName() << expr->getSourceRange();
    expr->dump();
    return nullptr;
  }
}

SpirvInstruction *
SpirvEmitter::processFlatConversion(const QualType type,
                                    SpirvInstruction *initInstr,
                                    SourceLocation srcLoc, SourceRange range) {

  // If the same literal is used in multiple instructions, then the literal
  // visitor may not be able to pick the correct type for the literal. That
  // happens when say one instruction uses the literal as a float and another
  // uses it as a double. We solve this by setting the type for the literal to
  // its 32-bit equivalent.
  //
  // TODO(6188): This is wrong when the literal is too large to be held in
  // the the 32-bit type. We do this because it is consistent with the long
  // standing behaviour. Changing now would result in more 64-bit arithmetic,
  // which the optimizer does not handle as well.
  QualType resultType = initInstr->getAstResultType();
  if (resultType->isSpecificBuiltinType(BuiltinType::LitFloat)) {
    initInstr->setAstResultType(astContext.FloatTy);
  } else if (resultType->isSpecificBuiltinType(BuiltinType::LitInt)) {
    if (resultType->isSignedIntegerType())
      initInstr->setAstResultType(astContext.LongLongTy);
    else
      initInstr->setAstResultType(astContext.UnsignedLongLongTy);
  }

  // Decompose `initInstr`.
  std::vector<SpirvInstruction *> flatValues = decomposeToScalars(initInstr);

  if (flatValues.size() == 1) {
    return splatScalarToGenerate(type, flatValues[0], SpirvLayoutRule::Void);
  }
  return generateFromScalars(type, flatValues, SpirvLayoutRule::Void);
}

SpirvInstruction *
SpirvEmitter::doCompoundAssignOperator(const CompoundAssignOperator *expr) {
  const auto opcode = expr->getOpcode();

  // Try to optimize floatMxN *= float and floatN *= float case
  if (opcode == BO_MulAssign) {
    if (auto *result = tryToGenFloatMatrixScale(expr))
      return result;
    if (auto *result = tryToGenFloatVectorScale(expr))
      return result;
  }

  const auto *rhs = expr->getRHS();
  const auto *lhs = expr->getLHS();

  SpirvInstruction *lhsPtr = nullptr;
  auto *result = processBinaryOp(
      lhs, rhs, opcode, expr->getComputationLHSType(), expr->getType(),
      expr->getSourceRange(), expr->getOperatorLoc(), &lhsPtr);
  return processAssignment(lhs, result, true, lhsPtr, expr->getSourceRange());
}

SpirvInstruction *SpirvEmitter::doShortCircuitedConditionalOperator(
    const ConditionalOperator *expr) {
  const auto type = expr->getType();
  const SourceLocation loc = expr->getExprLoc();
  const SourceRange range = expr->getSourceRange();
  const Expr *cond = expr->getCond();
  const Expr *falseExpr = expr->getFalseExpr();
  const Expr *trueExpr = expr->getTrueExpr();

  // Short-circuited operators can only be used with scalar conditions. This
  // is checked earlier.
  assert(cond->getType()->isScalarType());

  auto *tempVar = spvBuilder.addFnVar(type, loc, "temp.var.ternary");
  auto *thenBB = spvBuilder.createBasicBlock("ternary.lhs");
  auto *elseBB = spvBuilder.createBasicBlock("ternary.rhs");
  auto *mergeBB = spvBuilder.createBasicBlock("ternary.merge");

  // Create the branch instruction. This will end the current basic block.
  SpirvInstruction *condition = loadIfGLValue(cond);
  condition = castToBool(condition, cond->getType(), astContext.BoolTy,
                         cond->getLocEnd());
  spvBuilder.createConditionalBranch(condition, thenBB, elseBB, loc, mergeBB);
  spvBuilder.addSuccessor(thenBB);
  spvBuilder.addSuccessor(elseBB);
  spvBuilder.setMergeTarget(mergeBB);

  // Handle the true case.
  spvBuilder.setInsertPoint(thenBB);
  SpirvInstruction *trueVal = loadIfGLValue(trueExpr);
  trueVal = castToType(trueVal, trueExpr->getType(), type,
                       trueExpr->getExprLoc(), range);
  if (!trueVal)
    return nullptr;
  spvBuilder.createStore(tempVar, trueVal, trueExpr->getLocStart(), range);
  spvBuilder.createBranch(mergeBB, trueExpr->getLocEnd());
  spvBuilder.addSuccessor(mergeBB);

  // Handle the false case.
  spvBuilder.setInsertPoint(elseBB);
  SpirvInstruction *falseVal = loadIfGLValue(falseExpr);
  falseVal = castToType(falseVal, falseExpr->getType(), type,
                        falseExpr->getExprLoc(), range);
  if (!falseVal)
    return nullptr;
  spvBuilder.createStore(tempVar, falseVal, falseExpr->getLocStart(), range);
  spvBuilder.createBranch(mergeBB, falseExpr->getLocEnd());
  spvBuilder.addSuccessor(mergeBB);

  // From now on, emit instructions into the merge block.
  spvBuilder.setInsertPoint(mergeBB);
  SpirvInstruction *result = spvBuilder.createLoad(type, tempVar, loc, range);
  if (!result)
    return nullptr;
  result->setRValue();
  return result;
}

SpirvInstruction *SpirvEmitter::doConditional(const Expr *expr,
                                              const Expr *cond,
                                              const Expr *falseExpr,
                                              const Expr *trueExpr) {
  const auto type = expr->getType();
  const SourceLocation loc = expr->getExprLoc();
  const SourceRange range = expr->getSourceRange();

  // Corner-case: In HLSL, the condition of the ternary operator can be a
  // matrix of booleans which results in selecting between components of two
  // matrices. However, a matrix of booleans is not a valid type in SPIR-V.
  // If the AST has inserted a splat of a scalar/vector to a matrix, we can just
  // use that scalar/vector as an if-clause condition.
  if (auto *cast = dyn_cast<ImplicitCastExpr>(cond))
    if (cast->getCastKind() == CK_HLSLMatrixSplat)
      cond = cast->getSubExpr();

  // If we are selecting between two SampleState objects, none of the three
  // operands has a LValueToRValue implicit cast.
  auto *condition = loadIfGLValue(cond);
  auto *trueBranch = loadIfGLValue(trueExpr);
  auto *falseBranch = loadIfGLValue(falseExpr);

  // Corner-case: In HLSL, the condition of the ternary operator can be a
  // matrix of booleans which results in selecting between components of two
  // matrices. However, a matrix of booleans is not a valid type in SPIR-V.
  // Therefore, we need to perform OpSelect for each row of the matrix.
  {
    QualType condElemType = {}, elemType = {};
    uint32_t rowCount = 0, colCount = 0;
    if (isMxNMatrix(type, &elemType, &rowCount, &colCount) &&
        isMxNMatrix(cond->getType(), &condElemType) &&
        condElemType->isBooleanType()) {
      const auto rowType = astContext.getExtVectorType(elemType, colCount);
      const auto condRowType =
          astContext.getExtVectorType(condElemType, colCount);
      llvm::SmallVector<SpirvInstruction *, 4> rows;
      for (uint32_t i = 0; i < rowCount; ++i) {
        auto *condRow = spvBuilder.createCompositeExtract(
            condRowType, condition, {i}, loc, range);
        auto *trueRow = spvBuilder.createCompositeExtract(rowType, trueBranch,
                                                          {i}, loc, range);
        auto *falseRow = spvBuilder.createCompositeExtract(rowType, falseBranch,
                                                           {i}, loc, range);
        rows.push_back(spvBuilder.createSelect(rowType, condRow, trueRow,
                                               falseRow, loc, range));
      }
      auto *result =
          spvBuilder.createCompositeConstruct(type, rows, loc, range);
      if (!result)
        return nullptr;

      result->setRValue();
      return result;
    }
  }

  // For cases where the return type is a scalar or a vector, we can use
  // OpSelect to choose between the two. OpSelect's return type must be either
  // scalar or vector.
  if (isScalarType(type) || isVectorType(type)) {
    // The SPIR-V OpSelect instruction must have a selection argument that is
    // the same size as the return type. If the return type is a vector, the
    // selection must be a vector of booleans (one per output component).
    uint32_t count = 0;
    if (isVectorType(expr->getType(), nullptr, &count) &&
        !isVectorType(cond->getType())) {
      const llvm::SmallVector<SpirvInstruction *, 4> components(size_t(count),
                                                                condition);
      condition = spvBuilder.createCompositeConstruct(
          astContext.getExtVectorType(astContext.BoolTy, count), components,
          cond->getLocEnd());
    }

    auto *value = spvBuilder.createSelect(type, condition, trueBranch,
                                          falseBranch, loc, range);
    if (!value)
      return nullptr;

    value->setRValue();
    return value;
  }

  // Usually integer conditional types in HLSL will be wrapped in an
  // ImplicitCastExpr<IntegralToBoolean> in the Clang AST. However, some
  // combinations of result types can result in a bare integer (literal or
  // reference) as a condition, which still needs to be cast to bool.
  if (cond->getType()->isIntegerType()) {
    condition =
        castToBool(condition, cond->getType(), astContext.BoolTy, loc, range);
  }

  // If we can't use OpSelect, we need to create if-else control flow.
  auto *tempVar = spvBuilder.addFnVar(type, loc, "temp.var.ternary");
  auto *thenBB = spvBuilder.createBasicBlock("if.true");
  auto *mergeBB = spvBuilder.createBasicBlock("if.merge");
  auto *elseBB = spvBuilder.createBasicBlock("if.false");

  // Create the branch instruction. This will end the current basic block.
  spvBuilder.createConditionalBranch(condition, thenBB, elseBB,
                                     cond->getLocEnd(), mergeBB);
  spvBuilder.addSuccessor(thenBB);
  spvBuilder.addSuccessor(elseBB);
  spvBuilder.setMergeTarget(mergeBB);
  // Handle the then branch
  spvBuilder.setInsertPoint(thenBB);
  spvBuilder.createStore(tempVar, trueBranch, trueExpr->getLocStart(), range);
  spvBuilder.createBranch(mergeBB, trueExpr->getLocEnd());
  spvBuilder.addSuccessor(mergeBB);
  // Handle the else branch
  spvBuilder.setInsertPoint(elseBB);
  spvBuilder.createStore(tempVar, falseBranch, falseExpr->getLocStart(), range);
  spvBuilder.createBranch(mergeBB, falseExpr->getLocEnd());
  spvBuilder.addSuccessor(mergeBB);
  // From now on, emit instructions into the merge block.
  spvBuilder.setInsertPoint(mergeBB);
  auto *result = spvBuilder.createLoad(type, tempVar, expr->getLocEnd(), range);
  if (!result)
    return nullptr;

  result->setRValue();
  return result;
}

SpirvInstruction *
SpirvEmitter::processByteAddressBufferStructuredBufferGetDimensions(
    const CXXMemberCallExpr *expr) {
  const auto range = expr->getSourceRange();
  const auto *object = expr->getImplicitObjectArgument();
  auto *objectInstr = loadIfAliasVarRef(object);
  const auto type = object->getType();
  const bool isBABuf = isByteAddressBuffer(type) || isRWByteAddressBuffer(type);
  const bool isStructuredBuf = isStructuredBuffer(type) ||
                               isAppendStructuredBuffer(type) ||
                               isConsumeStructuredBuffer(type);
  assert(isBABuf || isStructuredBuf);

  // (RW)ByteAddressBuffers/(RW)StructuredBuffers are represented as a structure
  // with only one member that is a runtime array. We need to perform
  // OpArrayLength on member 0.
  SpirvInstruction *length = spvBuilder.createArrayLength(
      astContext.UnsignedIntTy, expr->getExprLoc(), objectInstr, 0, range);
  // For (RW)ByteAddressBuffers, GetDimensions() must return the array length
  // in bytes, but OpArrayLength returns the number of uints in the runtime
  // array. Therefore we must multiply the results by 4.
  if (isBABuf) {
    length = spvBuilder.createBinaryOp(
        spv::Op::OpIMul, astContext.UnsignedIntTy, length,
        // TODO(jaebaek): What line info we should emit for constants?
        spvBuilder.getConstantInt(astContext.UnsignedIntTy,
                                  llvm::APInt(32, 4u)),
        expr->getExprLoc(), range);
  }
  spvBuilder.createStore(doExpr(expr->getArg(0)), length,
                         expr->getArg(0)->getLocStart(), range);

  if (isStructuredBuf) {
    // For (RW)StructuredBuffer, the stride of the runtime array (which is the
    // size of the struct) must also be written to the second argument.
    AlignmentSizeCalculator alignmentCalc(astContext, spirvOptions);
    uint32_t size = 0, stride = 0;
    std::tie(std::ignore, size) =
        alignmentCalc.getAlignmentAndSize(type, spirvOptions.sBufferLayoutRule,
                                          /*isRowMajor*/ llvm::None, &stride);
    auto *sizeInstr = spvBuilder.getConstantInt(astContext.UnsignedIntTy,
                                                llvm::APInt(32, size));
    spvBuilder.createStore(doExpr(expr->getArg(1)), sizeInstr,
                           expr->getArg(1)->getLocStart(), range);
  }

  return nullptr;
}

SpirvInstruction *SpirvEmitter::processRWByteAddressBufferAtomicMethods(
    hlsl::IntrinsicOp opcode, const CXXMemberCallExpr *expr) {
  // The signature of RWByteAddressBuffer atomic methods are largely:
  // void Interlocked*(in UINT dest, in UINT value);
  // void Interlocked*(in UINT dest, in UINT value, out UINT original_value);
  const auto *object = expr->getImplicitObjectArgument();
  auto *objectInfo = loadIfAliasVarRef(object);

  auto *zero =
      spvBuilder.getConstantInt(astContext.UnsignedIntTy, llvm::APInt(32, 0));
  auto *offset = doExpr(expr->getArg(0));

  // Right shift by 2 to convert the byte offset to uint32_t offset
  const auto range = expr->getSourceRange();
  auto *address = spvBuilder.createBinaryOp(
      spv::Op::OpShiftRightLogical, astContext.UnsignedIntTy, offset,
      spvBuilder.getConstantInt(astContext.UnsignedIntTy, llvm::APInt(32, 2)),
      expr->getExprLoc(), range);
  auto *ptr = spvBuilder.createAccessChain(astContext.UnsignedIntTy, objectInfo,
                                           {zero, address},
                                           object->getLocStart(), range);

  const bool isCompareExchange =
      opcode == hlsl::IntrinsicOp::MOP_InterlockedCompareExchange;
  const bool isCompareStore =
      opcode == hlsl::IntrinsicOp::MOP_InterlockedCompareStore;

  if (isCompareExchange || isCompareStore) {
    auto *comparator = doExpr(expr->getArg(1));
    SpirvInstruction *originalVal = spvBuilder.createAtomicCompareExchange(
        astContext.UnsignedIntTy, ptr, spv::Scope::Device,
        spv::MemorySemanticsMask::MaskNone, spv::MemorySemanticsMask::MaskNone,
        doExpr(expr->getArg(2)), comparator, expr->getCallee()->getExprLoc(),
        range);
    if (isCompareExchange) {
      auto *resultAddress = expr->getArg(3);
      QualType resultType = resultAddress->getType();
      if (resultType != astContext.UnsignedIntTy)
        originalVal = castToInt(originalVal, astContext.UnsignedIntTy,
                                resultType, expr->getArg(3)->getLocStart());
      spvBuilder.createStore(doExpr(expr->getArg(3)), originalVal,
                             expr->getArg(3)->getLocStart(), range);
    }
  } else {
    const Expr *value = expr->getArg(1);
    SpirvInstruction *valueInstr = doExpr(expr->getArg(1));

    // Since a RWAB is represented by an array of 32-bit unsigned integers, the
    // destination pointee type will always be unsigned, and thus the SPIR-V
    // instruction's result type and value type must also be unsigned. The
    // signedness of the opcode is determined correctly by frontend and will
    // correctly determine the signedness of the actual operation, but the
    // necessary argument type cast will not be added by the frontend in the
    // case of a signed value.
    valueInstr =
        castToType(valueInstr, value->getType(), astContext.UnsignedIntTy,
                   value->getExprLoc(), range);

    SpirvInstruction *originalVal = spvBuilder.createAtomicOp(
        translateAtomicHlslOpcodeToSpirvOpcode(opcode),
        astContext.UnsignedIntTy, ptr, spv::Scope::Device,
        spv::MemorySemanticsMask::MaskNone, valueInstr,
        expr->getCallee()->getExprLoc(), range);
    if (expr->getNumArgs() > 2) {
      originalVal = castToType(originalVal, astContext.UnsignedIntTy,
                               expr->getArg(2)->getType(),
                               expr->getArg(2)->getLocStart(), range);
      spvBuilder.createStore(doExpr(expr->getArg(2)), originalVal,
                             expr->getArg(2)->getLocStart(), range);
    }
  }

  return nullptr;
}

SpirvInstruction *
SpirvEmitter::processGetSamplePosition(const CXXMemberCallExpr *expr) {
  const auto *object = expr->getImplicitObjectArgument()->IgnoreParens();
  auto *objectInstr = loadIfGLValue(object);
  if (isSampledTexture(object->getType())) {
    LowerTypeVisitor lowerTypeVisitor(astContext, spvContext, spirvOptions,
                                      spvBuilder);
    const SpirvType *spvType =
        lowerTypeVisitor.lowerType(object->getType(), SpirvLayoutRule::Void,
                                   llvm::None, expr->getExprLoc());
    const auto *sampledType = cast<SampledImageType>(spvType);
    const SpirvType *imgType = sampledType->getImageType();
    objectInstr = spvBuilder.createUnaryOp(spv::Op::OpImage, imgType,
                                           objectInstr, expr->getExprLoc());
  }
  auto *sampleCount = spvBuilder.createImageQuery(
      spv::Op::OpImageQuerySamples, astContext.UnsignedIntTy,
      expr->getExprLoc(), objectInstr);
  if (!spirvOptions.noWarnEmulatedFeatures)
    emitWarning("GetSamplePosition is emulated using many SPIR-V instructions "
                "due to lack of direct SPIR-V equivalent, so it only supports "
                "standard sample settings with 1, 2, 4, 8, or 16 samples and "
                "will return float2(0, 0) for other cases",
                expr->getCallee()->getExprLoc());
  return emitGetSamplePosition(sampleCount, doExpr(expr->getArg(0)),
                               expr->getCallee()->getExprLoc());
}

SpirvInstruction *
SpirvEmitter::processSubpassLoad(const CXXMemberCallExpr *expr) {
  if (!spvContext.isPS()) {
    emitError("SubpassInput(MS) only allowed in pixel shader",
              expr->getExprLoc());
    return nullptr;
  }
  const auto *object = expr->getImplicitObjectArgument()->IgnoreParens();
  SpirvInstruction *sample =
      expr->getNumArgs() == 1 ? doExpr(expr->getArg(0)) : nullptr;
  auto *zero = spvBuilder.getConstantInt(astContext.IntTy, llvm::APInt(32, 0));
  auto *location = spvBuilder.getConstantComposite(
      astContext.getExtVectorType(astContext.IntTy, 2), {zero, zero});

  return processBufferTextureLoad(object, location, /*constOffset*/ 0,
                                  /*lod*/ sample,
                                  /*residencyCode*/ 0, expr->getExprLoc());
}

SpirvInstruction *
SpirvEmitter::processBufferTextureGetDimensions(const CXXMemberCallExpr *expr) {
  const auto *object = expr->getImplicitObjectArgument();
  const auto range = expr->getSourceRange();
  auto *objectInstr = loadIfGLValue(object, range);
  const auto type = object->getType();
  const auto *recType = type->getAs<RecordType>();
  assert(recType);
  const auto typeName = recType->getDecl()->getName();
  const auto numArgs = expr->getNumArgs();
  const Expr *mipLevel = nullptr, *numLevels = nullptr, *numSamples = nullptr;

  assert(isTexture(type) || isRWTexture(type) || isBuffer(type) ||
         isRWBuffer(type) || isSampledTexture(type));
  if (isSampledTexture(type)) {
    LowerTypeVisitor lowerTypeVisitor(astContext, spvContext, spirvOptions,
                                      spvBuilder);
    const SpirvType *spvType = lowerTypeVisitor.lowerType(
        type, SpirvLayoutRule::Void, llvm::None, expr->getExprLoc());
    // Get image type based on type, assuming type is a sampledimage type
    const auto *sampledType = cast<SampledImageType>(spvType);
    const SpirvType *imgType = sampledType->getImageType();
    objectInstr = spvBuilder.createUnaryOp(spv::Op::OpImage, imgType,
                                           objectInstr, expr->getExprLoc());
  }

  // For Texture1D, arguments are either:
  // a) width
  // b) MipLevel, width, NumLevels

  // For Texture1DArray, arguments are either:
  // a) width, elements
  // b) MipLevel, width, elements, NumLevels

  // For Texture2D, arguments are either:
  // a) width, height
  // b) MipLevel, width, height, NumLevels

  // For Texture2DArray, arguments are either:
  // a) width, height, elements
  // b) MipLevel, width, height, elements, NumLevels

  // For Texture3D, arguments are either:
  // a) width, height, depth
  // b) MipLevel, width, height, depth, NumLevels

  // For Texture2DMS, arguments are: width, height, NumSamples

  // For Texture2DMSArray, arguments are: width, height, elements, NumSamples

  // For TextureCube, arguments are either:
  // a) width, height
  // b) MipLevel, width, height, NumLevels

  // For TextureCubeArray, arguments are either:
  // a) width, height, elements
  // b) MipLevel, width, height, elements, NumLevels

  // SampledTexture types follow the same rules above, as
  // this method doesn't require a Sampler argument.

  // Note: SPIR-V Spec requires return type of OpImageQuerySize(Lod) to be a
  // scalar/vector of integers. SPIR-V Spec also requires return type of
  // OpImageQueryLevels and OpImageQuerySamples to be scalar integers.
  // The HLSL methods, however, have overloaded functions which have float
  // output arguments. Since the AST naturally won't have casting AST nodes for
  // such cases, we'll have to perform the cast ourselves.
  const auto storeToOutputArg = [range, this](const Expr *outputArg,
                                              SpirvInstruction *id,
                                              QualType type) {
    id = castToType(id, type, outputArg->getType(), outputArg->getExprLoc(),
                    range);
    spvBuilder.createStore(doExpr(outputArg, range), id,
                           outputArg->getLocStart(), range);
  };

  if ((typeName == "Texture1D" && numArgs > 1) ||
      (typeName == "Texture2D" && numArgs > 2) ||
      (typeName == "SampledTexture1D" && numArgs > 1) ||
      (typeName == "SampledTexture1DArray" && numArgs > 2) ||
      (typeName == "SampledTexture2D" && numArgs > 2) ||
      (typeName == "SampledTexture2DArray" && numArgs > 3) ||
      (typeName == "SampledTextureCUBE" && numArgs > 2) ||
      (typeName == "SampledTextureCUBEArray" && numArgs > 3) ||
      (typeName == "SampledTexture3D" && numArgs > 3) ||
      (typeName == "TextureCube" && numArgs > 2) ||
      (typeName == "Texture3D" && numArgs > 3) ||
      (typeName == "Texture1DArray" && numArgs > 2) ||
      (typeName == "TextureCubeArray" && numArgs > 3) ||
      (typeName == "Texture2DArray" && numArgs > 3)) {
    mipLevel = expr->getArg(0);
    numLevels = expr->getArg(numArgs - 1);
  }

  if (isSampledTextureMS(type) || isTextureMS(type)) {
    numSamples = expr->getArg(numArgs - 1);
  }

  // Make sure that all output args are an l-value.
  for (uint32_t argIdx = (mipLevel ? 1 : 0); argIdx < numArgs; ++argIdx) {
    if (!expr->getArg(argIdx)->isLValue()) {
      emitError("Output argument must be an l-value",
                expr->getArg(argIdx)->getExprLoc());
      return nullptr;
    }
  }

  uint32_t querySize = numArgs;
  // If numLevels arg is present, mipLevel must also be present. These are not
  // queried via ImageQuerySizeLod.
  if (numLevels)
    querySize -= 2;
  // If numLevels arg is present, mipLevel must also be present.
  else if (numSamples)
    querySize -= 1;

  const QualType resultQualType =
      querySize == 1
          ? astContext.UnsignedIntTy
          : astContext.getExtVectorType(astContext.UnsignedIntTy, querySize);

  // Only Texture types use ImageQuerySizeLod.
  // TextureMS, RWTexture, Buffers, RWBuffers use ImageQuerySize.
  SpirvInstruction *lod = nullptr;
  if ((isTexture(type) || isSampledTexture(type)) && !numSamples) {
    if (mipLevel) {
      // For Texture types when mipLevel argument is present.
      lod = doExpr(mipLevel, range);
    } else {
      // For Texture types when mipLevel argument is omitted.
      lod = spvBuilder.getConstantInt(astContext.IntTy, llvm::APInt(32, 0));
    }
  }

  SpirvInstruction *query =
      lod ? cast<SpirvInstruction>(spvBuilder.createImageQuery(
                spv::Op::OpImageQuerySizeLod, resultQualType,
                expr->getCallee()->getExprLoc(), objectInstr, lod, range))
          : cast<SpirvInstruction>(spvBuilder.createImageQuery(
                spv::Op::OpImageQuerySize, resultQualType,
                expr->getCallee()->getExprLoc(), objectInstr, nullptr, range));

  if (querySize == 1) {
    const uint32_t argIndex = mipLevel ? 1 : 0;
    storeToOutputArg(expr->getArg(argIndex), query, resultQualType);
  } else {
    for (uint32_t i = 0; i < querySize; ++i) {
      const uint32_t argIndex = mipLevel ? i + 1 : i;
      auto *component = spvBuilder.createCompositeExtract(
          astContext.UnsignedIntTy, query, {i}, expr->getCallee()->getExprLoc(),
          range);
      // If the first arg is the mipmap level, we must write the results
      // starting from Arg(i+1), not Arg(i).
      storeToOutputArg(expr->getArg(argIndex), component,
                       astContext.UnsignedIntTy);
    }
  }

  if (numLevels || numSamples) {
    const Expr *numLevelsSamplesArg = numLevels ? numLevels : numSamples;
    const spv::Op opcode =
        numLevels ? spv::Op::OpImageQueryLevels : spv::Op::OpImageQuerySamples;
    auto *numLevelsSamplesQuery = spvBuilder.createImageQuery(
        opcode, astContext.UnsignedIntTy, expr->getCallee()->getExprLoc(),
        objectInstr);
    storeToOutputArg(numLevelsSamplesArg, numLevelsSamplesQuery,
                     astContext.UnsignedIntTy);
  }

  return nullptr;
}

SpirvInstruction *
SpirvEmitter::processTextureLevelOfDetail(const CXXMemberCallExpr *expr,
                                          bool unclamped) {
  // Possible signatures are as follows:
  // Texture1D(Array).CalculateLevelOfDetail(SamplerState S, float x);
  // Texture2D(Array).CalculateLevelOfDetail(SamplerState S, float2 xy);
  // TextureCube(Array).CalculateLevelOfDetail(SamplerState S, float3 xyz);
  // Texture3D.CalculateLevelOfDetail(SamplerState S, float3 xyz);
  // Return type is always a single float (LOD).
  //
  // Their SampledTexture variants have the same signature without the
  // sampler_state parameter.
  const auto *imageExpr = expr->getImplicitObjectArgument();
  const QualType imageType = imageExpr->getType();
  // numarg is 1 if isSampledTexture(imageType). otherwise 2.
  assert(expr->getNumArgs() == (isSampledTexture(imageType) ? 1u : 2u));

  auto *objectInfo = loadIfGLValue(imageExpr);

  SpirvInstruction *samplerState, *coordinate, *sampledImage;
  if (isSampledTexture(imageType)) {
    samplerState = nullptr;
    coordinate = doExpr(expr->getArg(0));
    sampledImage = objectInfo;
  } else {
    samplerState = doExpr(expr->getArg(0));
    coordinate = doExpr(expr->getArg(1));
    sampledImage = spvBuilder.createSampledImage(
        imageExpr->getType(), objectInfo, samplerState, expr->getExprLoc());
  }

  // The result type of OpImageQueryLod must be a float2.
  const QualType queryResultType =
      astContext.getExtVectorType(astContext.FloatTy, 2u);
  auto *query =
      spvBuilder.createImageQuery(spv::Op::OpImageQueryLod, queryResultType,
                                  expr->getExprLoc(), sampledImage, coordinate);

  addDerivativeGroupExecutionMode();
  // The first component of the float2 contains the mipmap array layer.
  // The second component of the float2 represents the unclamped lod.
  return spvBuilder.createCompositeExtract(astContext.FloatTy, query,
                                           unclamped ? 1 : 0,
                                           expr->getCallee()->getExprLoc());
}

SpirvInstruction *SpirvEmitter::processTextureGatherRGBACmpRGBA(
    const CXXMemberCallExpr *expr, const bool isCmp, const uint32_t component) {
  // Parameters for .Gather{Red|Green|Blue|Alpha}() are one of the following
  // two sets:
  // * SamplerState s, float2 location, int2 offset
  // * SamplerState s, float2 location, int2 offset0, int2 offset1,
  //   int offset2, int2 offset3
  //
  // An additional 'out uint status' parameter can appear in both of the above.
  //
  // Parameters for .GatherCmp{Red|Green|Blue|Alpha}() are one of the following
  // two sets:
  // * SamplerState s, float2 location, float compare_value, int2 offset
  // * SamplerState s, float2 location, float compare_value, int2 offset1,
  //   int2 offset2, int2 offset3, int2 offset4
  //
  // An additional 'out uint status' parameter can appear in both of the above.
  //
  // TextureCube's signature is somewhat different from the rest.
  // Parameters for .Gather{Red|Green|Blue|Alpha}() for TextureCube are:
  // * SamplerState s, float2 location, out uint status
  // Parameters for .GatherCmp{Red|Green|Blue|Alpha}() for TextureCube are:
  // * SamplerState s, float2 location, float compare_value, out uint status
  //
  // Return type is always a 4-component vector.
  //
  // SampledTexture variants have the same signatures without the SamplerState
  // parameter.
  const FunctionDecl *callee = expr->getDirectCallee();
  const auto numArgs = expr->getNumArgs();
  const auto *imageExpr = expr->getImplicitObjectArgument();
  const auto loc = expr->getCallee()->getExprLoc();
  const QualType imageType = imageExpr->getType();
  const QualType retType = callee->getReturnType();
  const bool isImageSampledTexture = isSampledTexture(imageType);

  // If the last arg is an unsigned integer, it must be the status.
  const bool hasStatusArg =
      expr->getArg(numArgs - 1)->getType()->isUnsignedIntegerType();

  // Subtract 1 for status arg (if it exists), subtract 1 for compare_value (if
  // it exists), and subtract 2 for SamplerState and location.
  int offsetStartIndex = (isImageSampledTexture ? 1 : 2) + isCmp;
  const auto numOffsetArgs = numArgs - hasStatusArg - offsetStartIndex;
  // No offset args for TextureCube, 1 or 4 offset args for the rest.
  assert(numOffsetArgs == 0 || numOffsetArgs == 1 || numOffsetArgs == 4);

  int samplerIndex, coordIndex, compareValIndex;
  if (isImageSampledTexture) {
    samplerIndex = -1; // non-existant
    coordIndex = 0;
    compareValIndex = 1;
  } else {
    samplerIndex = 0;
    coordIndex = 1;
    compareValIndex = 2;
  }
  auto *image = loadIfGLValue(imageExpr);
  auto *sampler =
      samplerIndex >= 0 ? doExpr(expr->getArg(samplerIndex)) : nullptr;
  auto *coordinate = doExpr(expr->getArg(coordIndex));
  auto *compareVal = isCmp ? doExpr(expr->getArg(compareValIndex)) : nullptr;

  // Handle offsets (if any).
  bool needsEmulation = false;
  SpirvInstruction *constOffset = nullptr, *varOffset = nullptr,
                   *constOffsets = nullptr;
  if (numOffsetArgs == 1) {
    // The offset arg is not optional.
    handleOffsetInMethodCall(expr, offsetStartIndex, &constOffset, &varOffset);
  } else if (numOffsetArgs == 4) {
    auto *offset0 = constEvaluator.tryToEvaluateAsConst(
        expr->getArg(offsetStartIndex), isSpecConstantMode);
    auto *offset1 = constEvaluator.tryToEvaluateAsConst(
        expr->getArg(offsetStartIndex + 1), isSpecConstantMode);
    auto *offset2 = constEvaluator.tryToEvaluateAsConst(
        expr->getArg(offsetStartIndex + 2), isSpecConstantMode);
    auto *offset3 = constEvaluator.tryToEvaluateAsConst(
        expr->getArg(offsetStartIndex + 3), isSpecConstantMode);

    // If any of the offsets is not constant, we then need to emulate the call
    // using 4 OpImageGather instructions. Otherwise, we can leverage the
    // ConstOffsets image operand.
    if (offset0 && offset1 && offset2 && offset3) {
      const QualType v2i32 = astContext.getExtVectorType(astContext.IntTy, 2);
      const auto offsetType = astContext.getConstantArrayType(
          v2i32, llvm::APInt(32, 4), clang::ArrayType::Normal, 0);
      constOffsets = spvBuilder.getConstantComposite(
          offsetType, {offset0, offset1, offset2, offset3});
    } else {
      needsEmulation = true;
    }
  }

  auto *status = hasStatusArg ? doExpr(expr->getArg(numArgs - 1)) : nullptr;

  if (needsEmulation) {
    const auto elemType = hlsl::GetHLSLVecElementType(callee->getReturnType());

    SpirvInstruction *texels[4];
    for (uint32_t i = 0; i < 4; ++i) {
      varOffset = doExpr(expr->getArg(offsetStartIndex + i));
      auto *gatherRet = spvBuilder.createImageGather(
          retType, imageType, image, sampler, coordinate,
          spvBuilder.getConstantInt(astContext.IntTy,
                                    llvm::APInt(32, component, true)),
          compareVal,
          /*constOffset*/ nullptr, varOffset, /*constOffsets*/ nullptr,
          /*sampleNumber*/ nullptr, status, loc);
      texels[i] =
          spvBuilder.createCompositeExtract(elemType, gatherRet, {i}, loc);
    }
    return spvBuilder.createCompositeConstruct(
        retType, {texels[0], texels[1], texels[2], texels[3]}, loc);
  }

  return spvBuilder.createImageGather(
      retType, imageType, image, sampler, coordinate,
      spvBuilder.getConstantInt(astContext.IntTy,
                                llvm::APInt(32, component, true)),
      compareVal, constOffset, varOffset, constOffsets,
      /*sampleNumber*/ nullptr, status, loc);
}

SpirvInstruction *
SpirvEmitter::processTextureGatherCmp(const CXXMemberCallExpr *expr) {
  // Signature for Texture2D/Texture2DArray:
  //
  // float4 GatherCmp(
  //   in SamplerComparisonState s,
  //   in float2 location,
  //   in float compare_value
  //   [,in int2 offset]
  //   [,out uint Status]
  // );
  //
  // Signature for TextureCube/TextureCubeArray:
  //
  // float4 GatherCmp(
  //   in SamplerComparisonState s,
  //   in float2 location,
  //   in float compare_value,
  //   out uint Status
  // );
  //
  // Other Texture types do not have the GatherCmp method.
  //
  // SampledTexture variants have the same signatures without the SamplerState
  // parameter.

  const FunctionDecl *callee = expr->getDirectCallee();
  const auto numArgs = expr->getNumArgs();
  const auto loc = expr->getExprLoc();
  const bool hasStatusArg =
      expr->getArg(numArgs - 1)->getType()->isUnsignedIntegerType();
  const auto *imageExpr = expr->getImplicitObjectArgument();

  const QualType imageType = imageExpr->getType();
  const bool isImageSampledTexture = isSampledTexture(imageType);

  int samplerIndex, coordIndex, compareValIndex;
  if (isImageSampledTexture) {
    samplerIndex = -1; // non-existant
    coordIndex = 0;
    compareValIndex = 1;
  } else {
    samplerIndex = 0;
    coordIndex = 1;
    compareValIndex = 2;
  }

  auto *image = loadIfGLValue(imageExpr);
  auto *sampler =
      samplerIndex >= 0 ? doExpr(expr->getArg(samplerIndex)) : nullptr;
  auto *coordinate = doExpr(expr->getArg(coordIndex));
  auto *comparator = doExpr(expr->getArg(compareValIndex));

  const bool hasOffsetArg = numArgs - hasStatusArg - compareValIndex > 1;
  SpirvInstruction *constOffset = nullptr, *varOffset = nullptr;
  if (hasOffsetArg)
    handleOffsetInMethodCall(expr, compareValIndex + 1, &constOffset,
                             &varOffset);

  const auto retType = callee->getReturnType();
  const auto status =
      hasStatusArg ? doExpr(expr->getArg(numArgs - 1)) : nullptr;

  return spvBuilder.createImageGather(
      retType, imageType, image, sampler, coordinate,
      /*component*/ nullptr, comparator, constOffset, varOffset,
      /*constOffsets*/ nullptr,
      /*sampleNumber*/ nullptr, status, loc);
}

SpirvInstruction *SpirvEmitter::processBufferTextureLoad(
    const Expr *object, SpirvInstruction *location,
    SpirvInstruction *constOffset, SpirvInstruction *lod,
    SpirvInstruction *residencyCode, SourceLocation loc, SourceRange range) {
  // Loading for Buffer and RWBuffer translates to an OpImageFetch.
  // The result type of an OpImageFetch must be a vec4 of float or int.
  const auto type = object->getType();
  assert(isBuffer(type) || isRWBuffer(type) || isTexture(type) ||
         isRWTexture(type) || isSubpassInput(type) || isSubpassInputMS(type) ||
         isSampledTexture(type));

  const bool doFetch =
      isBuffer(type) || isTexture(type) || isSampledTexture(type);
  const bool rasterizerOrdered = isRasterizerOrderedView(type);

  if (rasterizerOrdered) {
    beginInvocationInterlock(loc, range);
  }

  auto *objectInfo = loadIfGLValue(object, range);

  // For Texture2DMS and Texture2DMSArray, Sample must be used rather than Lod.
  SpirvInstruction *sampleNumber = nullptr;
  if (isSampledTextureMS(type) || isTextureMS(type) || isSubpassInputMS(type)) {
    sampleNumber = lod;
    lod = nullptr;
  }

  const auto sampledType = hlsl::GetHLSLResourceResultType(type);
  QualType elemType = sampledType;
  uint32_t elemCount = 1;
  bool isTemplateOverStruct = false;
  bool isTemplateTypeBool = false;

  // Check whether the template type is a vector type or struct type.
  if (!isVectorType(sampledType, &elemType, &elemCount)) {
    if (sampledType->getAsStructureType()) {
      isTemplateOverStruct = true;
      // For struct type, we need to make sure it can fit into a 4-component
      // vector. Detailed failing reasons will be emitted by the function so
      // we don't need to emit errors here.
      if (!canFitIntoOneRegister(astContext, sampledType, &elemType,
                                 &elemCount))
        return nullptr;
    }
  }

  // Check whether template type is bool.
  if (elemType->isBooleanType()) {
    isTemplateTypeBool = true;
    // Replace with unsigned int, and cast back to bool later.
    elemType = astContext.getUIntPtrType();
  }

  {
    // Treat a vector of size 1 the same as a scalar.
    if (hlsl::IsHLSLVecType(elemType) && hlsl::GetHLSLVecSize(elemType) == 1)
      elemType = hlsl::GetHLSLVecElementType(elemType);

    if (!elemType->isFloatingType() && !elemType->isIntegerType()) {
      emitError("loading %0 value unsupported", object->getExprLoc()) << type;
      return nullptr;
    }
  }

  // If residencyCode is nullptr, we are dealing with a Load method with 2
  // arguments which does not return the operation status.
  if (residencyCode && residencyCode->isRValue()) {
    emitError(
        "an lvalue argument should be used for returning the operation status",
        loc);
    return nullptr;
  }

  // OpImageFetch and OpImageRead can only fetch a vector of 4 elements.
  const QualType texelType = astContext.getExtVectorType(elemType, 4u);

  if (isSampledTexture(type)) {
    LowerTypeVisitor lowerTypeVisitor(astContext, spvContext, spirvOptions,
                                      spvBuilder);
    const SpirvType *spvType = lowerTypeVisitor.lowerType(
        type, SpirvLayoutRule::Void, llvm::None, loc);
    // Get image type based on type, assuming type is a sampledimage type
    const auto *sampledImageType = cast<SampledImageType>(spvType);
    const SpirvType *imgType = sampledImageType->getImageType();
    objectInfo =
        spvBuilder.createUnaryOp(spv::Op::OpImage, imgType, objectInfo, loc);
  }
  auto *texel = spvBuilder.createImageFetchOrRead(
      doFetch, texelType, type, objectInfo, location, lod, constOffset,
      /*constOffsets*/ nullptr, sampleNumber, residencyCode, loc, range);

  if (rasterizerOrdered) {
    spvBuilder.createEndInvocationInterlockEXT(loc, range);
  }

  // If the result type is a vec1, vec2, or vec3, some extra processing
  // (extraction) is required.
  auto *retVal = extractVecFromVec4(texel, elemCount, elemType, loc, range);
  if (isTemplateOverStruct) {
    // Convert to the struct so that we are consistent with types in the AST.
    retVal = convertVectorToStruct(sampledType, elemType, retVal, loc, range);
  }

  // If the result type is a bool, after loading the uint, convert it to
  // boolean.
  if (isTemplateTypeBool) {
    const QualType toType =
        elemCount > 1 ? astContext.getExtVectorType(elemType, elemCount)
                      : elemType;
    retVal = castToBool(retVal, toType, sampledType, loc);
  }
  if (!retVal)
    return nullptr;

  retVal->setRValue();
  return retVal;
}

SpirvInstruction *SpirvEmitter::processByteAddressBufferLoadStore(
    const CXXMemberCallExpr *expr, uint32_t numWords, bool doStore) {
  SpirvInstruction *result = nullptr;
  const auto object = expr->getImplicitObjectArgument();
  auto *objectInfo = loadIfAliasVarRef(object);
  assert(numWords >= 1 && numWords <= 4);
  if (doStore) {
    assert(isRWByteAddressBuffer(object->getType()));
    assert(expr->getNumArgs() == 2);
  } else {
    assert(isRWByteAddressBuffer(object->getType()) ||
           isByteAddressBuffer(object->getType()));
    if (expr->getNumArgs() == 2) {
      emitError(
          "(RW)ByteAddressBuffer::Load(in address, out status) not supported",
          expr->getExprLoc());
      return 0;
    }
  }
  const Expr *addressExpr = expr->getArg(0);
  auto *byteAddress = doExpr(addressExpr);
  const QualType addressType = addressExpr->getType();
  // The front-end prevents usage of templated Load2, Load3, Load4, Store2,
  // Store3, Store4 intrinsic functions.
  const bool isTemplatedLoadOrStore =
      (numWords == 1) &&
      (doStore ? !expr->getArg(1)->getType()->isSpecificBuiltinType(
                     BuiltinType::UInt)
               : !expr->getType()->isSpecificBuiltinType(BuiltinType::UInt));

  const auto range = expr->getSourceRange();

  const bool rasterizerOrder = isRasterizerOrderedView(object->getType());

  if (isTemplatedLoadOrStore) {
    // Templated load. Need to (potentially) perform more
    // loads/casts/composite-constructs.
    if (rasterizerOrder) {
      beginInvocationInterlock(expr->getLocStart(), range);
    }

    if (doStore) {
      auto *values = doExpr(expr->getArg(1));
      RawBufferHandler(*this).processTemplatedStoreToBuffer(
          values, objectInfo, byteAddress, expr->getArg(1)->getType(), range);
      result = nullptr;
    } else {
      RawBufferHandler rawBufferHandler(*this);
      result = rawBufferHandler.processTemplatedLoadFromBuffer(
          objectInfo, byteAddress, expr->getType(), range);
    }

    if (rasterizerOrder) {
      spvBuilder.createEndInvocationInterlockEXT(expr->getLocStart(), range);
    }
    return result;
  }

  //…82017 tokens truncated…guments are floats.
      if (arg1Type->isFloatingType())
        return spvBuilder.createBinaryOp(spv::Op::OpVectorTimesScalar,
                                         returnType, arg0Id, doExpr(arg1), loc,
                                         range);

      // Use OpIMul for integers
      return spvBuilder.createBinaryOp(spv::Op::OpIMul, returnType, arg0Id,
                                       createVectorSplat(arg1, elemCount), loc,
                                       range);
    }
  }

  // mul(vector, vector)
  if (isVectorType(arg0Type) && isVectorType(arg1Type)) {
    // mul( Mat(1xM), Mat(Mx1) ) results in a scalar (same as dot product)
    if (isScalarType(returnType)) {
      return processIntrinsicDot(callExpr);
    }

    // mul( Mat(Mx1), Mat(1xN) ) results in a MxN matrix.
    QualType elemType = {};
    uint32_t numRows = 0;
    if (isMxNMatrix(returnType, &elemType, &numRows)) {
      llvm::SmallVector<SpirvInstruction *, 4> rows;
      auto *arg0Id = doExprEnsuringRValue(arg0, loc, range);
      auto *arg1Id = doExprEnsuringRValue(arg1, loc, range);
      for (uint32_t i = 0; i < numRows; ++i) {
        auto *scalar = spvBuilder.createCompositeExtract(elemType, arg0Id, {i},
                                                         loc, range);
        rows.push_back(spvBuilder.createBinaryOp(spv::Op::OpVectorTimesScalar,
                                                 arg1Type, arg1Id, scalar, loc,
                                                 range));
      }
      return spvBuilder.createCompositeConstruct(returnType, rows, loc, range);
    }

    llvm_unreachable("bad arguments passed to mul");
  }

  // All the following cases require handling arg0 and arg1 expressions first.
  auto *arg0Id = doExprEnsuringRValue(arg0, loc, range);
  auto *arg1Id = doExprEnsuringRValue(arg1, loc, range);

  // mul(scalar, scalar)
  if (isScalarType(arg0Type) && isScalarType(arg1Type))
    return spvBuilder.createBinaryOp(translateOp(BO_Mul, arg0Type), returnType,
                                     arg0Id, arg1Id, loc, range);

  // mul(scalar, matrix)
  {
    QualType elemType = {};
    if (isScalarType(arg0Type) && isMxNMatrix(arg1Type, &elemType)) {
      // OpMatrixTimesScalar can only be used if *both* the matrix element type
      // and the scalar type are float.
      if (arg0Type->isFloatingType() && elemType->isFloatingType())
        return spvBuilder.createBinaryOp(spv::Op::OpMatrixTimesScalar,
                                         returnType, arg1Id, arg0Id, loc,
                                         range);
      else
        return processNonFpScalarTimesMatrix(arg0Type, arg0Id, arg1Type, arg1Id,
                                             callExpr->getExprLoc(), range);
    }
  }

  // mul(matrix, scalar)
  {
    QualType elemType = {};
    if (isScalarType(arg1Type) && isMxNMatrix(arg0Type, &elemType)) {
      // OpMatrixTimesScalar can only be used if *both* the matrix element type
      // and the scalar type are float.
      if (arg1Type->isFloatingType() && elemType->isFloatingType())
        return spvBuilder.createBinaryOp(spv::Op::OpMatrixTimesScalar,
                                         returnType, arg0Id, arg1Id, loc,
                                         range);
      else
        return processNonFpScalarTimesMatrix(arg1Type, arg1Id, arg0Type, arg0Id,
                                             callExpr->getExprLoc(), range);
    }
  }

  // mul(vector, matrix)
  {
    QualType vecElemType = {}, matElemType = {};
    uint32_t elemCount = 0, numRows = 0;
    if (isVectorType(arg0Type, &vecElemType, &elemCount) &&
        isMxNMatrix(arg1Type, &matElemType, &numRows)) {
      assert(elemCount == numRows);

      if (vecElemType->isFloatingType() && matElemType->isFloatingType())
        return spvBuilder.createBinaryOp(spv::Op::OpMatrixTimesVector,
                                         returnType, arg1Id, arg0Id, loc,
                                         range);
      else
        return processNonFpVectorTimesMatrix(arg0Type, arg0Id, arg1Type, arg1Id,
                                             callExpr->getExprLoc(), nullptr,
                                             range);
    }
  }

  // mul(matrix, vector)
  {
    QualType vecElemType = {}, matElemType = {};
    uint32_t elemCount = 0, numCols = 0;
    if (isMxNMatrix(arg0Type, &matElemType, nullptr, &numCols) &&
        isVectorType(arg1Type, &vecElemType, &elemCount)) {
      assert(elemCount == numCols);
      if (vecElemType->isFloatingType() && matElemType->isFloatingType())
        return spvBuilder.createBinaryOp(spv::Op::OpVectorTimesMatrix,
                                         returnType, arg1Id, arg0Id, loc,
                                         range);
      else
        return processNonFpMatrixTimesVector(arg0Type, arg0Id, arg1Type, arg1Id,
                                             callExpr->getExprLoc(), range);
    }
  }

  // mul(matrix, matrix)
  {
    // The front-end ensures that the two matrix element types match.
    QualType elemType = {};
    uint32_t lhsCols = 0, rhsRows = 0;
    if (isMxNMatrix(arg0Type, &elemType, nullptr, &lhsCols) &&
        isMxNMatrix(arg1Type, nullptr, &rhsRows, nullptr)) {
      assert(lhsCols == rhsRows);
      if (elemType->isFloatingType())
        return spvBuilder.createBinaryOp(spv::Op::OpMatrixTimesMatrix,
                                         returnType, arg1Id, arg0Id, loc,
                                         range);
      else
        return processNonFpMatrixTimesMatrix(arg0Type, arg0Id, arg1Type, arg1Id,
                                             callExpr->getExprLoc(), range);
    }
  }

  emitError("invalid argument type passed to mul intrinsic function",
            callExpr->getExprLoc());
  return nullptr;
}

SpirvInstruction *
SpirvEmitter::processIntrinsicPrintf(const CallExpr *callExpr) {
  // C99, s6.5.2.2/6: "If the expression that denotes the called function has a
  // type that does not include a prototype, the integer promotions are
  // performed on each argument, and arguments that have type float are promoted
  // to double. These are called the default argument promotions."
  // C++: All the variadic parameters undergo default promotions before they're
  // received by the function.
  //
  // Therefore by default floating point arguments will be evaluated as double
  // by this function.
  //
  // TODO: We may want to change this behavior for SPIR-V.

  const auto returnType = callExpr->getType();
  const auto numArgs = callExpr->getNumArgs();
  const auto loc = callExpr->getExprLoc();
  assert(numArgs >= 1u);
  llvm::SmallVector<SpirvInstruction *, 4> args;
  for (uint32_t argIndex = 0; argIndex < numArgs; ++argIndex)
    args.push_back(doExpr(callExpr->getArg(argIndex)));

  return spvBuilder.createNonSemanticDebugPrintfExtInst(
      returnType, NonSemanticDebugPrintfDebugPrintf, args, loc);
}

SpirvInstruction *SpirvEmitter::processIntrinsicDot(const CallExpr *callExpr) {
  // Get the function parameters. Expect 2 vectors as parameters.
  assert(callExpr->getNumArgs() == 2u);
  const Expr *arg0 = callExpr->getArg(0);
  const Expr *arg1 = callExpr->getArg(1);
  auto *arg0Id = doExpr(arg0);
  auto *arg1Id = doExpr(arg1);
  QualType arg0Type = arg0->getType();
  QualType arg1Type = arg1->getType();
  uint32_t vec0Size = 0, vec1Size = 0;
  QualType vec0ComponentType = {}, vec1ComponentType = {};
  QualType returnType = {};
  const bool arg0isScalarOrVec =
      isScalarOrVectorType(arg0Type, &vec0ComponentType, &vec0Size);
  const bool arg1isScalarOrVec =
      isScalarOrVectorType(arg1Type, &vec1ComponentType, &vec1Size);
  const bool returnIsScalar = isScalarType(callExpr->getType(), &returnType);
  // Each argument should either be a vector or a scalar
  assert(arg0isScalarOrVec && arg1isScalarOrVec);
  // The result type must be a scalar.
  assert(returnIsScalar);
  // The element type of each argument must be the same.
  assert(vec0ComponentType == vec1ComponentType);
  // The size of the two arguments must be equal.
  assert(vec0Size == vec1Size);
  // Acceptable vector sizes are 1,2,3,4.
  assert(vec0Size >= 1 && vec0Size <= 4);
  (void)arg0isScalarOrVec;
  (void)arg1isScalarOrVec;
  (void)returnIsScalar;
  (void)vec0ComponentType;
  (void)vec1ComponentType;
  (void)vec1Size;

  auto loc = callExpr->getLocStart();
  auto range = callExpr->getSourceRange();

  // According to HLSL reference, the dot function only works on integers
  // and floats.
  assert(returnType->isFloatingType() || returnType->isIntegerType());

  // Special case: dot product of two vectors, each of size 1. That is
  // basically the same as regular multiplication of 2 scalars.
  if (vec0Size == 1) {
    const spv::Op spvOp = translateOp(BO_Mul, arg0Type);
    return spvBuilder.createBinaryOp(spvOp, returnType, arg0Id, arg1Id, loc,
                                     range);
  }

  // If the vectors are of type Float, we can use OpDot.
  if (returnType->isFloatingType()) {
    return spvBuilder.createBinaryOp(spv::Op::OpDot, returnType, arg0Id, arg1Id,
                                     loc, range);
  }
  // Vector component type is Integer (signed or unsigned).
  // Create all instructions necessary to perform a dot product on
  // two integer vectors. SPIR-V OpDot does not support integer vectors.
  // Therefore, we use other SPIR-V instructions (addition and
  // multiplication).
  else {
    SpirvInstruction *result = nullptr;
    llvm::SmallVector<SpirvInstruction *, 4> multIds;
    const spv::Op multSpvOp = translateOp(BO_Mul, arg0Type);
    const spv::Op addSpvOp = translateOp(BO_Add, arg0Type);

    // Extract members from the two vectors and multiply them.
    for (unsigned int i = 0; i < vec0Size; ++i) {
      auto *vec0member = spvBuilder.createCompositeExtract(
          returnType, arg0Id, {i}, arg0->getLocStart(), range);
      auto *vec1member = spvBuilder.createCompositeExtract(
          returnType, arg1Id, {i}, arg1->getLocStart(), range);
      auto *multId = spvBuilder.createBinaryOp(
          multSpvOp, returnType, vec0member, vec1member, loc, range);
      multIds.push_back(multId);
    }
    // Add all the multiplications.
    result = multIds[0];
    for (unsigned int i = 1; i < vec0Size; ++i) {
      auto *additionId = spvBuilder.createBinaryOp(addSpvOp, returnType, result,
                                                   multIds[i], loc, range);
      result = additionId;
    }
    return result;
  }
}

SpirvInstruction *SpirvEmitter::processIntrinsicRcp(const CallExpr *callExpr) {
  // 'rcp' takes only 1 argument that is a scalar, vector, or matrix of type
  // float or double.
  assert(callExpr->getNumArgs() == 1u);
  const QualType returnType = callExpr->getType();
  const Expr *arg = callExpr->getArg(0);
  auto *argId = doExpr(arg);
  const QualType argType = arg->getType();
  auto loc = callExpr->getLocStart();
  auto range = callExpr->getSourceRange();

  // For cases with matrix argument.
  QualType elemType = {};
  uint32_t numRows = 0, numCols = 0;
  if (isMxNMatrix(argType, &elemType, &numRows, &numCols)) {
    auto *vecOne = getVecValueOne(elemType, numCols);
    const auto actOnEachVec =
        [this, vecOne, loc, range](uint32_t /*index*/, QualType inType,
                                   QualType outType, SpirvInstruction *curRow) {
          return spvBuilder.createBinaryOp(spv::Op::OpFDiv, outType, vecOne,
                                           curRow, loc, range);
        };
    return processEachVectorInMatrix(arg, argId, actOnEachVec, loc, range);
  }

  // For cases with scalar or vector arguments.
  return spvBuilder.createBinaryOp(spv::Op::OpFDiv, returnType,
                                   getValueOne(argType), argId, loc, range);
}

SpirvInstruction *
SpirvEmitter::processIntrinsicReadClock(const CallExpr *callExpr) {
  auto *scope = doExpr(callExpr->getArg(0));
  assert(scope->getAstResultType()->isIntegerType());
  return spvBuilder.createReadClock(scope, callExpr->getExprLoc());
}

SpirvInstruction *
SpirvEmitter::processIntrinsicAllOrAny(const CallExpr *callExpr,
                                       spv::Op spvOp) {
  // 'all' and 'any' take only 1 parameter.
  assert(callExpr->getNumArgs() == 1u);
  const QualType returnType = callExpr->getType();
  const Expr *arg = callExpr->getArg(0);
  const QualType argType = arg->getType();
  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();

  // Handle scalars, vectors of size 1, and 1x1 matrices as arguments.
  // Optimization: can directly cast them to boolean. No need for OpAny/OpAll.
  {
    QualType scalarType = {};
    if (isScalarType(argType, &scalarType) &&
        (scalarType->isBooleanType() || scalarType->isFloatingType() ||
         scalarType->isIntegerType()))
      return castToBool(doExpr(arg), argType, returnType, loc, range);
  }

  // Handle vectors larger than 1, Mx1 matrices, and 1xN matrices as arguments.
  // Cast the vector to a boolean vector, then run OpAny/OpAll on it.
  {
    QualType elemType = {};
    uint32_t size = 0;
    if (isVectorType(argType, &elemType, &size)) {
      const QualType castToBoolType =
          astContext.getExtVectorType(returnType, size);
      auto *castedToBool =
          castToBool(doExpr(arg), argType, castToBoolType, loc, range);
      return spvBuilder.createUnaryOp(spvOp, returnType, castedToBool, loc,
                                      range);
    }
  }

  // Handle MxN matrices as arguments.
  {
    QualType elemType = {};
    uint32_t matRowCount = 0, matColCount = 0;
    if (isMxNMatrix(argType, &elemType, &matRowCount, &matColCount)) {
      auto *matrix = doExpr(arg);
      const QualType vecType = getComponentVectorType(astContext, argType);
      llvm::SmallVector<SpirvInstruction *, 4> rowResults;
      for (uint32_t i = 0; i < matRowCount; ++i) {
        // Extract the row which is a float vector of size matColCount.
        auto *rowFloatVec = spvBuilder.createCompositeExtract(
            vecType, matrix, {i}, arg->getLocStart(), range);
        // Cast the float vector to boolean vector.
        const auto rowFloatQualType =
            astContext.getExtVectorType(elemType, matColCount);
        const auto rowBoolQualType =
            astContext.getExtVectorType(returnType, matColCount);
        auto *rowBoolVec =
            castToBool(rowFloatVec, rowFloatQualType, rowBoolQualType,
                       arg->getLocStart(), range);
        // Perform OpAny/OpAll on the boolean vector.
        rowResults.push_back(spvBuilder.createUnaryOp(spvOp, returnType,
                                                      rowBoolVec, loc, range));
      }
      // Create a new vector that is the concatenation of results of all rows.
      const QualType vecOfBools =
          astContext.getExtVectorType(astContext.BoolTy, matRowCount);
      auto *row = spvBuilder.createCompositeConstruct(vecOfBools, rowResults,
                                                      loc, range);

      // Run OpAny/OpAll on the newly-created vector.
      return spvBuilder.createUnaryOp(spvOp, returnType, row, loc, range);
    }
  }

  // All types should be handled already.
  llvm_unreachable("Unknown argument type passed to all()/any().");
  return nullptr;
}

void SpirvEmitter::splitDouble(SpirvInstruction *value, SourceLocation loc,
                               SourceRange range, SpirvInstruction *&lowbits,
                               SpirvInstruction *&highbits) {
  const QualType uintType = astContext.UnsignedIntTy;
  const QualType uintVec2Type = astContext.getExtVectorType(uintType, 2);

  SpirvInstruction *uints = spvBuilder.createUnaryOp(
      spv::Op::OpBitcast, uintVec2Type, value, loc, range);

  lowbits = spvBuilder.createCompositeExtract(uintType, uints, {0}, loc, range);
  highbits =
      spvBuilder.createCompositeExtract(uintType, uints, {1}, loc, range);
}

void SpirvEmitter::splitDoubleVector(QualType elemType, uint32_t count,
                                     QualType outputType,
                                     SpirvInstruction *value,
                                     SourceLocation loc, SourceRange range,
                                     SpirvInstruction *&lowbits,
                                     SpirvInstruction *&highbits) {
  llvm::SmallVector<SpirvInstruction *, 4> lowElems;
  llvm::SmallVector<SpirvInstruction *, 4> highElems;

  for (uint32_t i = 0; i < count; ++i) {
    SpirvInstruction *elem =
        spvBuilder.createCompositeExtract(elemType, value, {i}, loc, range);
    SpirvInstruction *lowbitsResult = nullptr;
    SpirvInstruction *highbitsResult = nullptr;
    splitDouble(elem, loc, range, lowbitsResult, highbitsResult);
    lowElems.push_back(lowbitsResult);
    highElems.push_back(highbitsResult);
  }

  lowbits =
      spvBuilder.createCompositeConstruct(outputType, lowElems, loc, range);
  highbits =
      spvBuilder.createCompositeConstruct(outputType, highElems, loc, range);
}

void SpirvEmitter::splitDoubleMatrix(QualType elemType, uint32_t rowCount,
                                     uint32_t colCount, QualType outputType,
                                     SpirvInstruction *value,
                                     SourceLocation loc, SourceRange range,
                                     SpirvInstruction *&lowbits,
                                     SpirvInstruction *&highbits) {

  llvm::SmallVector<SpirvInstruction *, 4> lowElems;
  llvm::SmallVector<SpirvInstruction *, 4> highElems;

  QualType colType = astContext.getExtVectorType(elemType, colCount);

  const QualType uintType = astContext.UnsignedIntTy;
  const QualType outputColType =
      astContext.getExtVectorType(uintType, colCount);

  for (uint32_t i = 0; i < rowCount; ++i) {
    SpirvInstruction *column =
        spvBuilder.createCompositeExtract(colType, value, {i}, loc, range);
    SpirvInstruction *lowbitsResult = nullptr;
    SpirvInstruction *highbitsResult = nullptr;
    splitDoubleVector(elemType, colCount, outputColType, column, loc, range,
                      lowbitsResult, highbitsResult);
    lowElems.push_back(lowbitsResult);
    highElems.push_back(highbitsResult);
  }

  lowbits =
      spvBuilder.createCompositeConstruct(outputType, lowElems, loc, range);
  highbits =
      spvBuilder.createCompositeConstruct(outputType, highElems, loc, range);
}

SpirvInstruction *
SpirvEmitter::processIntrinsicAsType(const CallExpr *callExpr) {
  // This function handles the following intrinsics:
  //    'asint'
  //    'asint16'
  //    'asuint'
  //    'asuint16'
  //    'asfloat'
  //    'asfloat16'
  //    'asdouble'

  // Note: The logic for the 32-bit and 16-bit variants of these functions is
  //       identical so we don't bother distinguishing between related types
  //       like float and float16 in the comments.

  // Method 1: ret asint(arg)
  //    arg component type = {float, uint}
  //    arg template  type = {scalar, vector, matrix}
  //    ret template  type = same as arg template type.
  //    ret component type = int

  // Method 2: ret asuint(arg)
  //    arg component type = {float, int}
  //    arg template  type = {scalar, vector, matrix}
  //    ret template  type = same as arg template type.
  //    ret component type = uint

  // Method 3: ret asfloat(arg)
  //    arg component type = {float, uint, int}
  //    arg template  type = {scalar, vector, matrix}
  //    ret template  type = same as arg template type.
  //    ret component type = float

  // Method 4: double  asdouble(uint lowbits, uint highbits)
  // Method 5: double2 asdouble(uint2 lowbits, uint2 highbits)
  // Method 6: double3 asdouble(uint3 lowbits, uint3 highbits)
  // Method 7: double4 asdouble(uint4 lowbits, uint4 highbits)
  // Method 8:
  //           void asuint(
  //           in  double value,
  //           out uint lowbits,
  //           out uint highbits
  //           );

  const QualType returnType = callExpr->getType();
  const uint32_t numArgs = callExpr->getNumArgs();
  const Expr *arg0 = callExpr->getArg(0);
  const QualType argType = arg0->getType();
  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();

  // Method 3 return type may be the same as arg type, so it would be a no-op.
  if (isSameType(astContext, returnType, argType))
    return doExpr(arg0);

  switch (numArgs) {
  case 1: {
    // Handling Method 1, 2, and 3.
    auto *argInstr = loadIfGLValue(arg0);
    QualType fromElemType = {};
    uint32_t numRows = 0, numCols = 0;
    // For non-matrix arguments (scalar or vector), just do an OpBitCast.
    if (!isMxNMatrix(argType, &fromElemType, &numRows, &numCols)) {
      return spvBuilder.createUnaryOp(spv::Op::OpBitcast, returnType, argInstr,
                                      loc, range);
    }

    // Input or output type is a matrix.
    const QualType toElemType = hlsl::GetHLSLMatElementType(returnType);
    llvm::SmallVector<SpirvInstruction *, 4> castedRows;
    const auto fromVecType = astContext.getExtVectorType(fromElemType, numCols);
    const auto toVecType = astContext.getExtVectorType(toElemType, numCols);
    for (uint32_t row = 0; row < numRows; ++row) {
      auto *rowInstr = spvBuilder.createCompositeExtract(
          fromVecType, argInstr, {row}, arg0->getLocStart(), range);
      castedRows.push_back(spvBuilder.createUnaryOp(
          spv::Op::OpBitcast, toVecType, rowInstr, loc, range));
    }
    return spvBuilder.createCompositeConstruct(returnType, castedRows, loc,
                                               range);
  }
  case 2: {
    auto *lowbits = doExpr(arg0);
    auto *highbits = doExpr(callExpr->getArg(1));
    const auto uintType = astContext.UnsignedIntTy;
    const auto doubleType = astContext.DoubleTy;
    uint32_t vecSize;
    // Handling Method 4
    if (!isVectorType(argType, nullptr, &vecSize)) {
      const auto uintVec2Type = astContext.getExtVectorType(uintType, 2);
      auto *operand = spvBuilder.createCompositeConstruct(
          uintVec2Type, {lowbits, highbits}, loc, range);
      return spvBuilder.createUnaryOp(spv::Op::OpBitcast, doubleType, operand,
                                      loc, range);
    }
    // Handling Method 5, 6, 7
    else {
      std::vector<SpirvInstruction *> doubles = {};
      const auto uintVec2Type = astContext.getExtVectorType(uintType, 2);
      // For each pair, convert them to double.
      for (uint32_t i = 0; i < vecSize; ++i) {
        auto *operand = spvBuilder.createVectorShuffle(
            uintVec2Type, lowbits, highbits, {i, vecSize + i}, loc, range);
        SpirvInstruction *doubleElem = spvBuilder.createUnaryOp(
            spv::Op::OpBitcast, doubleType, operand, loc, range);
        doubles.push_back(doubleElem);
      }
      return spvBuilder.createCompositeConstruct(returnType, doubles, loc,
                                                 range);
    }
  }
  case 3: {
    // Handling Method 8.
    const Expr *arg1 = callExpr->getArg(1);
    const Expr *arg2 = callExpr->getArg(2);

    SpirvInstruction *value = doExpr(arg0);

    QualType elemType = QualType();
    uint32_t rowCount = 0;
    uint32_t colCount = 0;

    SpirvInstruction *lowbitsResult = nullptr;
    SpirvInstruction *highbitsResult = nullptr;

    if (isScalarType(argType)) {
      splitDouble(value, loc, range, lowbitsResult, highbitsResult);
    } else if (isVectorType(argType, &elemType, &rowCount)) {
      splitDoubleVector(elemType, rowCount, arg1->getType(), value, loc, range,
                        lowbitsResult, highbitsResult);
    } else if (isMxNMatrix(argType, &elemType, &rowCount, &colCount)) {
      splitDoubleMatrix(elemType, rowCount, colCount, arg1->getType(), value,
                        loc, range, lowbitsResult, highbitsResult);
    } else {
      llvm_unreachable(
          "unexpected argument type is not scalar, vector, or matrix");
      return nullptr;
    }

    processAssignment(arg1, lowbitsResult, false, nullptr, range);
    processAssignment(arg2, highbitsResult, false, nullptr, range);
    return nullptr;
  }
  default:
    emitError("unrecognized signature for %0 intrinsic function", loc)
        << getFunctionOrOperatorName(callExpr->getDirectCallee(), true);
    return nullptr;
  }
}

SpirvInstruction *
SpirvEmitter::processD3DCOLORtoUBYTE4(const CallExpr *callExpr) {
  // Should take a float4 and return an int4 by doing:
  // int4 result = input.zyxw * 255.001953;
  // Maximum float precision makes the scaling factor 255.002.
  const auto arg = callExpr->getArg(0);
  auto *argId = doExpr(arg);
  const auto argType = arg->getType();
  auto loc = callExpr->getLocStart();
  auto range = callExpr->getSourceRange();
  auto *swizzle = spvBuilder.createVectorShuffle(argType, argId, argId,
                                                 {2, 1, 0, 3}, loc, range);
  auto *scaled = spvBuilder.createBinaryOp(
      spv::Op::OpVectorTimesScalar, argType, swizzle,
      spvBuilder.getConstantFloat(astContext.FloatTy, llvm::APFloat(255.002f)),
      loc, range);
  return castToInt(scaled, arg->getType(), callExpr->getType(), loc, range);
}

SpirvInstruction *
SpirvEmitter::processIntrinsicIsFinite(const CallExpr *callExpr) {
  // Since OpIsFinite needs the Kernel capability, translation is instead done
  // using OpIsNan and OpIsInf:
  // isFinite = !(isNan || isInf)
  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();
  const QualType returnType = callExpr->getType();
  const Expr *arg = callExpr->getArg(0);
  auto *argId = doExpr(arg);

  const auto actOnEachVec = [this, loc, range](
                                uint32_t /*index*/, QualType inType,
                                QualType outType, SpirvInstruction *curRow) {
    const auto isNan =
        spvBuilder.createUnaryOp(spv::Op::OpIsNan, outType, curRow, loc, range);
    isNan->setLayoutRule(SpirvLayoutRule::Void);
    const auto isInf =
        spvBuilder.createUnaryOp(spv::Op::OpIsInf, outType, curRow, loc, range);
    isInf->setLayoutRule(SpirvLayoutRule::Void);
    const auto isNanOrInf = spvBuilder.createBinaryOp(
        spv::Op::OpLogicalOr, outType, isNan, isInf, loc, range);
    isNanOrInf->setLayoutRule(SpirvLayoutRule::Void);
    const auto output = spvBuilder.createUnaryOp(spv::Op::OpLogicalNot, outType,
                                                 isNanOrInf, loc, range);
    output->setLayoutRule(SpirvLayoutRule::Void);
    return output;
  };

  // If the instruction does not operate on matrices, we can perform the
  // instruction on each vector of the matrix.
  if (isMxNMatrix(arg->getType())) {
    assert(isMxNMatrix(returnType));
    return processEachVectorInMatrix(arg, returnType, argId, actOnEachVec, loc,
                                     range);
  }
  return actOnEachVec(/* index= */ 0, arg->getType(), returnType, argId);
}

SpirvInstruction *
SpirvEmitter::processIntrinsicIsNormal(const CallExpr *callExpr) {
  // Since OpIsNormal needs the Kernel capability, translation is instead done
  // IsNormal = !(zero || NaN || Inf || Subnormal)
  // Check that the exponent is neither all 0s nor all 1s
  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();
  const QualType returnType = callExpr->getType();
  const Expr *arg = callExpr->getArg(0);
  auto *argId = doExpr(arg);

  const auto actOnEachVec = [this, loc, range](
                                uint32_t /*index*/, QualType inType,
                                QualType outType, SpirvInstruction *curRow) {
    QualType intTy;
    SpirvConstant *expMask;
    SpirvConstant *zero;
    if (isHalfOrVecOfHalfType(inType)) {
      intTy = astContext.UnsignedShortTy;
      expMask = spvBuilder.getConstantInt(intTy, llvm::APInt(16, 0x7c00));
      zero = spvBuilder.getConstantInt(intTy, llvm::APInt(16, 0));
    } else {
      assert(isFloatOrVecOfFloatType(inType));
      intTy = astContext.UnsignedIntTy;
      expMask = spvBuilder.getConstantInt(intTy, llvm::APInt(32, 0x7F800000));
      zero = spvBuilder.getConstantInt(intTy, llvm::APInt(32, 0));
    }

    QualType boolTy = astContext.BoolTy;
    uint32_t vecSize;
    if (isVectorType(inType, nullptr, &vecSize)) {
      intTy = astContext.getExtVectorType(intTy, vecSize);
      boolTy = astContext.getExtVectorType(boolTy, vecSize);
      expMask = spvBuilder.getConstantComposite(
          intTy, llvm::SmallVector<SpirvConstant *, 4>(vecSize, expMask));
      zero = spvBuilder.getConstantComposite(
          intTy, llvm::SmallVector<SpirvConstant *, 4>(vecSize, zero));
    }

    auto *bitCast =
        spvBuilder.createUnaryOp(spv::Op::OpBitcast, intTy, curRow, loc);
    bitCast->setLayoutRule(SpirvLayoutRule::Void);
    auto *masked = spvBuilder.createBinaryOp(spv::Op::OpBitwiseAnd, intTy,
                                             bitCast, expMask, loc, range);
    masked->setLayoutRule(SpirvLayoutRule::Void);
    auto *notZero = spvBuilder.createBinaryOp(spv::Op::OpINotEqual, boolTy,
                                              masked, zero, loc, range);
    notZero->setLayoutRule(SpirvLayoutRule::Void);
    auto *notOnes = spvBuilder.createBinaryOp(spv::Op::OpINotEqual, boolTy,
                                              masked, expMask, loc, range);
    notOnes->setLayoutRule(SpirvLayoutRule::Void);
    auto *isNormal = spvBuilder.createBinaryOp(spv::Op::OpLogicalAnd, boolTy,
                                               notZero, notOnes, loc, range);
    isNormal->setLayoutRule(SpirvLayoutRule::Void);
    return isNormal;
  };

  // If the instruction does not operate on matrices, we can perform the
  // instruction on each vector of the matrix.
  if (isMxNMatrix(arg->getType())) {
    assert(isMxNMatrix(returnType));
    return processEachVectorInMatrix(arg, returnType, argId, actOnEachVec, loc,
                                     range);
  }
  return actOnEachVec(/* index= */ 0, arg->getType(), returnType, argId);
}

SpirvInstruction *
SpirvEmitter::processIntrinsicSinCos(const CallExpr *callExpr) {
  // Since there is no sincos equivalent in SPIR-V, we need to perform Sin
  // once and Cos once. We can reuse existing Sine/Cosine handling functions.
  CallExpr *sincosExpr =
      new (astContext) CallExpr(astContext, Stmt::StmtClass::NoStmtClass, {});
  sincosExpr->setType(callExpr->getArg(0)->getType());
  sincosExpr->setNumArgs(astContext, 1);
  sincosExpr->setArg(0, const_cast<Expr *>(callExpr->getArg(0)));
  const auto srcLoc = callExpr->getExprLoc();
  const auto srcRange = callExpr->getSourceRange();

  // Perform Sin and store results in argument 1.
  auto *sin = processIntrinsicUsingGLSLInst(
      sincosExpr, GLSLstd450::GLSLstd450Sin,
      /*actPerRowForMatrices*/ true, srcLoc, srcRange);
  spvBuilder.createStore(doExpr(callExpr->getArg(1)), sin, srcLoc, srcRange);

  // Perform Cos and store results in argument 2.
  auto *cos = processIntrinsicUsingGLSLInst(
      sincosExpr, GLSLstd450::GLSLstd450Cos,
      /*actPerRowForMatrices*/ true, srcLoc, srcRange);
  spvBuilder.createStore(doExpr(callExpr->getArg(2)), cos, srcLoc, srcRange);
  return nullptr;
}

SpirvInstruction *
SpirvEmitter::processIntrinsicSaturate(const CallExpr *callExpr) {
  const auto *arg = callExpr->getArg(0);
  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();
  auto *argId = doExpr(arg);
  const auto argType = arg->getType();
  const QualType returnType = callExpr->getType();

  QualType elemType = {};
  uint32_t vecSize = 0;
  if (isScalarType(argType, &elemType)) {
    auto *floatZero = getValueZero(elemType);
    auto *floatOne = getValueOne(elemType);
    return spvBuilder.createGLSLExtInst(
        returnType, GLSLstd450::GLSLstd450FClamp, {argId, floatZero, floatOne},
        loc, range);
  }

  if (isVectorType(argType, &elemType, &vecSize)) {
    auto *vecZero = getVecValueZero(elemType, vecSize);
    auto *vecOne = getVecValueOne(elemType, vecSize);
    return spvBuilder.createGLSLExtInst(returnType,
                                        GLSLstd450::GLSLstd450FClamp,
                                        {argId, vecZero, vecOne}, loc, range);
  }

  uint32_t numRows = 0, numCols = 0;
  if (isMxNMatrix(argType, &elemType, &numRows, &numCols)) {
    auto *vecZero = getVecValueZero(elemType, numCols);
    auto *vecOne = getVecValueOne(elemType, numCols);
    const auto actOnEachVec = [this, loc, vecZero, vecOne, range](
                                  uint32_t /*index*/, QualType inType,
                                  QualType outType, SpirvInstruction *curRow) {
      return spvBuilder.createGLSLExtInst(outType, GLSLstd450::GLSLstd450FClamp,
                                          {curRow, vecZero, vecOne}, loc,
                                          range);
    };
    return processEachVectorInMatrix(arg, argId, actOnEachVec, loc, range);
  }

  emitError("invalid argument type passed to saturate intrinsic function",
            callExpr->getExprLoc());
  return nullptr;
}

SpirvInstruction *
SpirvEmitter::processIntrinsicSignUnsignedInt(const CallExpr *callExpr) {
  const auto srcLoc = callExpr->getExprLoc();
  const auto srcRange = callExpr->getSourceRange();

  const Expr *firstArg = callExpr->getArg(0);
  const QualType firstArgType = firstArg->getType();
  auto elemType = QualType{};
  uint32_t numRows;
  uint32_t numCols;
  uint32_t count;
  bool isScalar =
      isScalarType(firstArgType, &elemType) ||
      (isVectorType(firstArgType, &elemType, &count) && count == 1) ||
      (isMxNMatrix(firstArgType, &elemType, &numRows, &numCols) &&
       (numRows == 1 && numCols == 1));

  auto *zero = getValueZero(astContext.IntTy);
  auto *one = getValueOne(astContext.IntTy);
  if (isScalar) {
    auto *argVal = doExpr(callExpr->getArg(0));
    auto *zeroUint = getValueZero(callExpr->getArg(0)->getType());
    auto *cmp =
        spvBuilder.createBinaryOp(spv::Op::OpUGreaterThan, astContext.BoolTy,
                                  argVal, zeroUint, srcLoc, srcRange);
    return spvBuilder.createSelect(astContext.IntTy, cmp, one, zero, srcLoc,
                                   srcRange);
  }

  uint32_t size;
  if (isVectorType(firstArgType)) {
    size = count;
  } else if (is1xNMatrix(firstArgType)) {
    size = numCols;
  } else if (isMx1Matrix(firstArgType)) {
    size = numRows;
  } else {
    size = numRows;
  }

  const auto actOnEachVec = [this, srcLoc, srcRange, zero, one, elemType,
                             size](uint32_t index, QualType inType,
                                   QualType outType, SpirvInstruction *curRow) {
    auto zeroUint = getValueZero(elemType);
    // Create `size` vector of uint zeros.
    auto *zerosUint = spvBuilder.getConstantComposite(
        astContext.getExtVectorType(elemType, size),
        std::vector<clang::spirv::SpirvConstant *>(size, zeroUint));
    // Compare if they are greater than zero.
    auto *cmp = spvBuilder.createBinaryOp(
        spv::Op::OpUGreaterThan,
        astContext.getExtVectorType(astContext.BoolTy, size), curRow, zerosUint,
        srcLoc, srcRange);

    // Create a vector of int ones and zeros.
    auto *zeros = spvBuilder.getConstantComposite(
        astContext.getExtVectorType(astContext.IntTy, size),
        std::vector<clang::spirv::SpirvConstant *>(size, zero));
    auto *ones = spvBuilder.getConstantComposite(
        astContext.getExtVectorType(astContext.IntTy, size),
        std::vector<clang::spirv::SpirvConstant *>(size, one));
    // Select between ones and zeros based on the comparison.
    return spvBuilder.createSelect(
        astContext.getExtVectorType(astContext.IntTy, size), cmp, ones, zeros,
        srcLoc, srcRange);
  };

  if (isVectorType(firstArgType)) {
    return actOnEachVec(0, firstArgType, callExpr->getType(), doExpr(firstArg));
  }
  return processEachVectorInMatrix(firstArg, doExpr(firstArg), actOnEachVec,
                                   srcLoc, srcRange);
}

SpirvInstruction *
SpirvEmitter::processIntrinsicFloatSign(const CallExpr *callExpr) {
  // Import the GLSL.std.450 extended instruction set.
  const Expr *arg = callExpr->getArg(0);
  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();
  const QualType returnType = callExpr->getType();
  const QualType argType = arg->getType();
  assert(isFloatOrVecMatOfFloatType(argType));
  auto *argId = doExpr(arg);
  SpirvInstruction *floatSign = nullptr;

  // For matrices, we can perform the instruction on each vector of the matrix.
  if (isMxNMatrix(argType)) {
    const auto actOnEachVec = [this, loc, range](
                                  uint32_t /*index*/, QualType inType,
                                  QualType outType, SpirvInstruction *curRow) {
      return spvBuilder.createGLSLExtInst(outType, GLSLstd450::GLSLstd450FSign,
                                          {curRow}, loc, range);
    };
    floatSign = processEachVectorInMatrix(arg, argId, actOnEachVec, loc, range);
  } else {
    floatSign = spvBuilder.createGLSLExtInst(
        argType, GLSLstd450::GLSLstd450FSign, {argId}, loc, range);
  }

  return castToInt(floatSign, arg->getType(), returnType, arg->getLocStart());
}

SpirvInstruction *
SpirvEmitter::processIntrinsicF16ToF32(const CallExpr *callExpr) {
  // f16tof32() takes in (vector of) uint and returns (vector of) float.
  // The frontend should guarantee that by inserting implicit casts.
  const QualType f32Type = astContext.FloatTy;
  const QualType u32Type = astContext.UnsignedIntTy;
  const QualType v2f32Type = astContext.getExtVectorType(f32Type, 2);

  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();
  const auto *arg = callExpr->getArg(0);
  auto *argId = doExpr(arg);

  uint32_t elemCount = {};

  if (isVectorType(arg->getType(), nullptr, &elemCount)) {
    // The input is a vector. We need to handle each element separately.
    llvm::SmallVector<SpirvInstruction *, 4> elements;

    for (uint32_t i = 0; i < elemCount; ++i) {
      auto *srcElem = spvBuilder.createCompositeExtract(
          u32Type, argId, {i}, arg->getLocStart(), range);
      auto *convert = spvBuilder.createGLSLExtInst(
          v2f32Type, GLSLstd450::GLSLstd450UnpackHalf2x16, srcElem, loc, range);
      elements.push_back(
          spvBuilder.createCompositeExtract(f32Type, convert, {0}, loc, range));
    }
    return spvBuilder.createCompositeConstruct(
        astContext.getExtVectorType(f32Type, elemCount), elements, loc, range);
  }

  auto *convert = spvBuilder.createGLSLExtInst(
      v2f32Type, GLSLstd450::GLSLstd450UnpackHalf2x16, argId, loc, range);
  // f16tof32() converts the float16 stored in the low-half of the uint to
  // a float. So just need to return the first component.
  return spvBuilder.createCompositeExtract(f32Type, convert, {0}, loc, range);
}

SpirvInstruction *
SpirvEmitter::processIntrinsicF32ToF16(const CallExpr *callExpr) {
  // f32tof16() takes in (vector of) float and returns (vector of) uint.
  // The frontend should guarantee that by inserting implicit casts.
  const QualType f32Type = astContext.FloatTy;
  const QualType u32Type = astContext.UnsignedIntTy;
  const QualType v2f32Type = astContext.getExtVectorType(f32Type, 2);
  auto *zero = spvBuilder.getConstantFloat(f32Type, llvm::APFloat(0.0f));

  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();
  const auto *arg = callExpr->getArg(0);
  auto *argId = doExpr(arg);
  uint32_t elemCount = {};

  if (isVectorType(arg->getType(), nullptr, &elemCount)) {
    // The input is a vector. We need to handle each element separately.
    llvm::SmallVector<SpirvInstruction *, 4> elements;

    for (uint32_t i = 0; i < elemCount; ++i) {
      auto *srcElem = spvBuilder.createCompositeExtract(
          f32Type, argId, {i}, arg->getLocStart(), range);
      auto *srcVec = spvBuilder.createCompositeConstruct(
          v2f32Type, {srcElem, zero}, loc, range);
      elements.push_back(spvBuilder.createGLSLExtInst(
          u32Type, GLSLstd450::GLSLstd450PackHalf2x16, srcVec, loc, range));
    }
    return spvBuilder.createCompositeConstruct(
        astContext.getExtVectorType(u32Type, elemCount), elements, loc, range);
  }

  // f16tof32() stores the float into the low-half of the uint. So we need
  // to supply another zero to take the other half.
  auto *srcVec =
      spvBuilder.createCompositeConstruct(v2f32Type, {argId, zero}, loc, range);
  return spvBuilder.createGLSLExtInst(
      u32Type, GLSLstd450::GLSLstd450PackHalf2x16, srcVec, loc, range);
}

SpirvInstruction *SpirvEmitter::processIntrinsicUsingSpirvInst(
    const CallExpr *callExpr, spv::Op opcode, bool actPerRowForMatrices) {
  // The derivative opcodes are only allowed in pixel shader, or in compute
  // shaderers when the SPV_NV_compute_shader_derivatives is enabled.
  if (!spvContext.isPS()) {
    // For cases where the instructions are known to be invalid, we turn on
    // legalization expecting the invalid use to be optimized away. For compute
    // shaders, we add the execution mode to enable the derivatives. We legalize
    // in this case as well because that is what we did before the extension was
    // used, and we do not want to change previous behaviour too much.
    switch (opcode) {
    case spv::Op::OpDPdx:
    case spv::Op::OpDPdy:
    case spv::Op::OpDPdxFine:
    case spv::Op::OpDPdyFine:
    case spv::Op::OpDPdxCoarse:
    case spv::Op::OpDPdyCoarse:
    case spv::Op::OpFwidth:
    case spv::Op::OpFwidthFine:
    case spv::Op::OpFwidthCoarse:
      addDerivativeGroupExecutionMode();
      needsLegalization = true;
      break;
    default:
      // Only the given opcodes need legalization and the execution mode.
      break;
    }
  }

  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();
  const QualType returnType = callExpr->getType();
  if (callExpr->getNumArgs() == 1u) {
    const Expr *arg = callExpr->getArg(0);
    auto *argId = doExpr(arg);

    // If the instruction does not operate on matrices, we can perform the
    // instruction on each vector of the matrix.
    if (actPerRowForMatrices && isMxNMatrix(arg->getType())) {
      assert(isMxNMatrix(returnType));
      const auto actOnEachVec = [this, opcode, loc,
                                 range](uint32_t /*index*/, QualType inType,
                                        QualType outType,
                                        SpirvInstruction *curRow) {
        return spvBuilder.createUnaryOp(opcode, outType, curRow, loc, range);
      };
      return processEachVectorInMatrix(arg, returnType, argId, actOnEachVec,
                                       loc, range);
    }
    return spvBuilder.createUnaryOp(opcode, returnType, argId, loc, range);
  } else if (callExpr->getNumArgs() == 2u) {
    const Expr *arg0 = callExpr->getArg(0);
    auto *arg0Id = doExpr(arg0);
    auto *arg1Id = doExpr(callExpr->getArg(1));
    const auto arg1Loc = callExpr->getArg(1)->getLocStart();
    const auto arg1Range = callExpr->getArg(1)->getSourceRange();
    // If the instruction does not operate on matrices, we can perform the
    // instruction on each vector of the matrix.
    if (actPerRowForMatrices && isMxNMatrix(arg0->getType())) {
      const auto actOnEachVec = [this, opcode, arg1Id, loc, range, arg1Loc,
                                 arg1Range](uint32_t index, QualType inType,
                                            QualType outType,
                                            SpirvInstruction *arg0Row) {
        auto *arg1Row = spvBuilder.createCompositeExtract(
            inType, arg1Id, {index}, arg1Loc, arg1Range);
        return spvBuilder.createBinaryOp(opcode, outType, arg0Row, arg1Row, loc,
                                         range);
      };
      return processEachVectorInMatrix(arg0, arg0Id, actOnEachVec, loc, range);
    }
    return spvBuilder.createBinaryOp(opcode, returnType, arg0Id, arg1Id, loc,
                                     range);
  }

  emitError("unsupported %0 intrinsic function", loc)
      << cast<DeclRefExpr>(callExpr->getCallee())->getNameInfo().getAsString();
  return nullptr;
}

SpirvInstruction *SpirvEmitter::processIntrinsicUsingGLSLInst(
    const CallExpr *callExpr, GLSLstd450 opcode, bool actPerRowForMatrices,
    SourceLocation loc, SourceRange range) {
  // Import the GLSL.std.450 extended instruction set.
  const QualType returnType = callExpr->getType();

  if (callExpr->getNumArgs() == 1u) {
    const Expr *arg = callExpr->getArg(0);
    auto *argInstr = doExpr(arg);

    // If the instruction does not operate on matrices, we can perform the
    // instruction on each vector of the matrix.
    if (actPerRowForMatrices && isMxNMatrix(arg->getType())) {
      const auto actOnEachVec = [this, loc, range,
                                 opcode](uint32_t /*index*/, QualType inType,
                                         QualType outType,
                                         SpirvInstruction *curRowInstr) {
        return spvBuilder.createGLSLExtInst(outType, opcode, {curRowInstr}, loc,
                                            range);
      };
      return processEachVectorInMatrix(arg, argInstr, actOnEachVec, loc, range);
    }
    return spvBuilder.createGLSLExtInst(returnType, opcode, {argInstr}, loc,
                                        range);
  } else if (callExpr->getNumArgs() == 2u) {
    const Expr *arg0 = callExpr->getArg(0);
    auto *arg0Instr = doExpr(arg0);
    auto *arg1Instr = doExpr(callExpr->getArg(1));
    const auto arg1Loc = callExpr->getArg(1)->getLocStart();
    const auto arg1Range = callExpr->getArg(1)->getSourceRange();
    // If the instruction does not operate on matrices, we can perform the
    // instruction on each vector of the matrix.
    if (actPerRowForMatrices && isMxNMatrix(arg0->getType())) {
      const auto actOnEachVec = [this, loc, range, opcode, arg1Instr, arg1Range,
                                 arg1Loc](uint32_t index, QualType inType,
                                          QualType outType,
                                          SpirvInstruction *arg0RowInstr) {
        auto *arg1RowInstr = spvBuilder.createCompositeExtract(
            inType, arg1Instr, {index}, arg1Loc, arg1Range);
        return spvBuilder.createGLSLExtInst(
            outType, opcode, {arg0RowInstr, arg1RowInstr}, loc, range);
      };
      return processEachVectorInMatrix(arg0, arg0Instr, actOnEachVec, loc,
                                       range);
    }
    return spvBuilder.createGLSLExtInst(returnType, opcode,
                                        {arg0Instr, arg1Instr}, loc, range);
  } else if (callExpr->getNumArgs() == 3u) {
    const Expr *arg0 = callExpr->getArg(0);
    auto *arg0Instr = doExpr(arg0);
    auto *arg1Instr = doExpr(callExpr->getArg(1));
    auto *arg2Instr = doExpr(callExpr->getArg(2));
    auto arg1Loc = callExpr->getArg(1)->getLocStart();
    auto arg2Loc = callExpr->getArg(2)->getLocStart();
    const auto arg1Range = callExpr->getArg(1)->getSourceRange();
    const auto arg2Range = callExpr->getArg(2)->getSourceRange();
    // If the instruction does not operate on matrices, we can perform the
    // instruction on each vector of the matrix.
    if (actPerRowForMatrices && isMxNMatrix(arg0->getType())) {
      const auto actOnEachVec = [this, loc, range, opcode, arg1Instr, arg2Instr,
                                 arg1Loc, arg2Loc, arg1Range,
                                 arg2Range](uint32_t index, QualType inType,
                                            QualType outType,
                                            SpirvInstruction *arg0RowInstr) {
        auto *arg1RowInstr = spvBuilder.createCompositeExtract(
            inType, arg1Instr, {index}, arg1Loc, arg1Range);
        auto *arg2RowInstr = spvBuilder.createCompositeExtract(
            inType, arg2Instr, {index}, arg2Loc, arg2Range);
        return spvBuilder.createGLSLExtInst(
            outType, opcode, {arg0RowInstr, arg1RowInstr, arg2RowInstr}, loc,
            range);
      };
      return processEachVectorInMatrix(arg0, arg0Instr, actOnEachVec, loc,
                                       range);
    }
    return spvBuilder.createGLSLExtInst(
        returnType, opcode, {arg0Instr, arg1Instr, arg2Instr}, loc, range);
  }

  emitError("unsupported %0 intrinsic function", callExpr->getExprLoc())
      << cast<DeclRefExpr>(callExpr->getCallee())->getNameInfo().getAsString();
  return nullptr;
}

SpirvInstruction *SpirvEmitter::processEvaluateAttributeAt(
    const CallExpr *callExpr, hlsl::IntrinsicOp opcode, SourceLocation loc,
    SourceRange range) {
  QualType returnType = callExpr->getType();
  SpirvInstruction *arg0Instr = doExpr(callExpr->getArg(0));
  SpirvInstruction *interpolant =
      turnIntoLValue(callExpr->getType(), arg0Instr, callExpr->getExprLoc());

  switch (opcode) {
  case hlsl::IntrinsicOp::IOP_EvaluateAttributeCentroid:
    return spvBuilder.createGLSLExtInst(
        returnType, GLSLstd450InterpolateAtCentroid, {interpolant}, loc, range);
  case hlsl::IntrinsicOp::IOP_EvaluateAttributeAtSample: {
    SpirvInstruction *sample = doExpr(callExpr->getArg(1));
    return spvBuilder.createGLSLExtInst(returnType,
                                        GLSLstd450InterpolateAtSample,
                                        {interpolant, sample}, loc, range);
  }
  case hlsl::IntrinsicOp::IOP_EvaluateAttributeSnapped: {
    const Expr *arg1 = callExpr->getArg(1);
    SpirvInstruction *arg1Inst = doExpr(arg1);

    QualType float2Type = astContext.getExtVectorType(astContext.FloatTy, 2);
    SpirvInstruction *offset =
        castToFloat(arg1Inst, arg1->getType(), float2Type, arg1->getLocStart(),
                    arg1->getSourceRange());

    return spvBuilder.createGLSLExtInst(returnType,
                                        GLSLstd450InterpolateAtOffset,
                                        {interpolant, offset}, loc, range);
  }
  default:
    assert(false && "processEvaluateAttributeAt must be called with an "
                    "EvaluateAttribute* opcode");
    return nullptr;
  }
}

SpirvInstruction *
SpirvEmitter::processIntrinsicLog10(const CallExpr *callExpr) {
  // Since there is no log10 instruction in SPIR-V, we can use:
  // log10(x) = log2(x) * ( 1 / log2(10) )
  // 1 / log2(10) = 0.30103
  auto loc = callExpr->getExprLoc();
  auto range = callExpr->getSourceRange();

  const auto returnType = callExpr->getType();
  auto scalarType = getElementType(astContext, returnType);

  auto *scale =
      spvBuilder.getConstantFloat(scalarType, llvm::APFloat(0.30103f));
  auto *log2 = processIntrinsicUsingGLSLInst(
      callExpr, GLSLstd450::GLSLstd450Log2, true, loc, range);
  spv::Op scaleOp = isScalarType(returnType)   ? spv::Op::OpFMul
                    : isVectorType(returnType) ? spv::Op::OpVectorTimesScalar
                                               : spv::Op::OpMatrixTimesScalar;
  return spvBuilder.createBinaryOp(scaleOp, returnType, log2, scale, loc,
                                   range);
}

SpirvInstruction *SpirvEmitter::processIntrinsicDP4a(const CallExpr *callExpr,
                                                     hlsl::IntrinsicOp op) {
  // Processing the `dot4add_i8packed` and `dot4add_u8packed` intrinsics.
  // There is no direct substitution for them in SPIR-V, but the combination
  // of OpSDot / OpUDot and OpIAdd works. Note that the OpSDotAccSat and
  // OpUDotAccSat operations are not matching the HLSL intrinsics as there
  // should not be any saturation.
  //
  // int32 dot4add_i8packed(uint32 a, uint32 b, int32 acc);
  //    A 4-dimensional signed integer dot-product with add. Multiplies together
  //    each corresponding pair of signed 8-bit int bytes in the two input
  //    DWORDs, and sums the results into the 32-bit signed integer accumulator.
  //
  // uint32 dot4add_u8packed(uint32 a, uint32 b, uint32 acc);
  //    A 4-dimensional unsigned integer dot-product with add. Multiplies
  //    together each corresponding pair of unsigned 8-bit int bytes in the two
  //    input DWORDs, and sums the results into the 32-bit unsigned integer
  //    accumulator.

  auto loc = callExpr->getExprLoc();
  auto range = callExpr->getSourceRange();
  assert(op == hlsl::IntrinsicOp::IOP_dot4add_i8packed ||
         op == hlsl::IntrinsicOp::IOP_dot4add_u8packed);

  // Validate the argument count - if it's wrong, the compiler won't get
  // here anyway, so an assert should be fine.
  assert(callExpr->getNumArgs() == 3u);

  // Prepare the three arguments.
  const Expr *arg0 = callExpr->getArg(0);
  const Expr *arg1 = callExpr->getArg(1);
  const Expr *arg2 = callExpr->getArg(2);
  auto *arg0Instr = doExpr(arg0);
  auto *arg1Instr = doExpr(arg1);
  auto *arg2Instr = doExpr(arg2);

  // OpSDot/OpUDot need a Packed Vector Format operand when Vector 1 and
  // Vector 2 are scalar integer types.
  SpirvConstant *formatConstant = spvBuilder.getConstantInt(
      astContext.UnsignedIntTy,
      llvm::APInt(32,
                  uint32_t(spv::PackedVectorFormat::PackedVectorFormat4x8Bit)));
  // Make sure that the format is emitted as a literal constant and not
  // an instruction reference.
  formatConstant->setLiteral(true);

  // Prepare the array inputs for createSpirvIntrInstExt below.
  // Need to use this function because the OpSDot/OpUDot operations require
  // two capabilities and an extension to be declared in the module.
  SpirvInstruction *operands[]{arg0Instr, arg1Instr, formatConstant};
  uint32_t capabilities[]{
      uint32_t(spv::Capability::DotProduct),
      uint32_t(spv::Capability::DotProductInput4x8BitPacked)};
  llvm::StringRef extensions[]{"SPV_KHR_integer_dot_product"};
  llvm::StringRef instSet = "";

  // Pick the opcode based on the instruction.
  const bool isSigned = op == hlsl::IntrinsicOp::IOP_dot4add_i8packed;
  const spv::Op spirvOp = isSigned ? spv::Op::OpSDot : spv::Op::OpUDot;

  const auto returnType = callExpr->getType();

  // Create the dot product instruction.
  auto *dotResult =
      spvBuilder.createSpirvIntrInstExt(uint32_t(spirvOp), returnType, operands,
                                        extensions, instSet, capabilities, loc);

  // Create and return the integer addition instruction.
  return spvBuilder.createBinaryOp(spv::Op::OpIAdd, returnType, dotResult,
                                   arg2Instr, loc, range);
}

SpirvInstruction *SpirvEmitter::processIntrinsicDP2a(const CallExpr *callExpr) {
  // Processing the `dot2add` intrinsic.
  // There is no direct substitution for it in SPIR-V, so it is recreated with a
  // combination of OpDot and OpFAdd.
  //
  // float dot2add( half2 a, half2 b, float acc );
  //    A 2-dimensional floating point dot product of half2 vectors with add.
  //    Multiplies the elements of the two half-precision float input vectors
  //    together and sums the results into the 32-bit float accumulator.

  auto loc = callExpr->getExprLoc();
  auto range = callExpr->getSourceRange();

  assert(callExpr->getNumArgs() == 3u);

  const Expr *arg0 = callExpr->getArg(0);
  const Expr *arg1 = callExpr->getArg(1);
  const Expr *arg2 = callExpr->getArg(2);

  QualType vecType = arg0->getType();
  QualType componentType = {};
  uint32_t vecSize = {};
  bool isVec = isVectorType(vecType, &componentType, &vecSize);

  assert(isVec && vecSize == 2);
  (void)isVec;

  SpirvInstruction *arg0Instr = doExpr(arg0);
  SpirvInstruction *arg1Instr = doExpr(arg1);
  SpirvInstruction *arg2Instr = doExpr(arg2);

  // Multiply the two half2 vectors and convert the result to float2.
  SpirvInstruction *mulInstr = spvBuilder.createBinaryOp(
      spv::Op::OpFMul, vecType, arg0Instr, arg1Instr, loc, range);
  SpirvInstruction *convertInstr = spvBuilder.createUnaryOp(
      spv::Op::OpFConvert,
      astContext.getExtVectorType(astContext.FloatTy, vecSize), mulInstr, loc,
      range);

  // Extract each float element and and sum them up.
  SpirvInstruction *extractedElem0 = spvBuilder.createCompositeExtract(
      astContext.FloatTy, convertInstr, {0}, loc, range);
  SpirvInstruction *extractedElem1 = spvBuilder.createCompositeExtract(
      astContext.FloatTy, convertInstr, {1}, loc, range);
  SpirvInstruction *dotInstr =
      spvBuilder.createBinaryOp(spv::Op::OpFAdd, astContext.FloatTy,
                                extractedElem0, extractedElem1, loc, range);

  // Sum the dot product result and accumulator and return.
  return spvBuilder.createBinaryOp(spv::Op::OpFAdd, astContext.FloatTy,
                                   dotInstr, arg2Instr, loc, range);
}

SpirvInstruction *
SpirvEmitter::processIntrinsic8BitPack(const CallExpr *callExpr,
                                       hlsl::IntrinsicOp op) {
  const auto loc = callExpr->getExprLoc();
  assert(op == hlsl::IntrinsicOp::IOP_pack_s8 ||
         op == hlsl::IntrinsicOp::IOP_pack_u8 ||
         op == hlsl::IntrinsicOp::IOP_pack_clamp_s8 ||
         op == hlsl::IntrinsicOp::IOP_pack_clamp_u8);

  // Here's the signature for the pack intrinsic operations:
  //
  // uint8_t4_packed pack_u8(uint32_t4 unpackedVal);
  // uint8_t4_packed pack_u8(uint16_t4 unpackedVal);
  // int8_t4_packed pack_s8(int32_t4 unpackedVal);
  // int8_t4_packed pack_s8(int16_t4 unpackedVal);
  //
  // These functions take a vec4 of 16-bit or 32-bit integers as input. For each
  // element of the vec4, they pick the lower 8 bits, and drop the other bits.
  // The result is four 8-bit values (32 bits in total) which are packed in an
  // unsigned uint32_t.
  //
  //
  // Here's the signature for the pack_clamp intrinsic operations:
  //
  // uint8_t4_packed pack_clamp_u8(int32_t4 val); // Pack and Clamp [0, 255]
  // uint8_t4_packed pack_clamp_u8(int16_t4 val); // Pack and Clamp [0, 255]
  //
  // int8_t4_packed pack_clamp_s8(int32_t4 val);  // Pack and Clamp [-128, 127]
  // int8_t4_packed pack_clamp_s8(int16_t4 val);  // Pack and Clamp [-128, 127]
  //
  // These functions take a vec4 of 16-bit or 32-bit integers as input. For each
  // element of the vec4, they first clamp the value to a range (depending on
  // the signedness) then pick the lower 8 bits, and drop the other bits.
  // The result is four 8-bit values (32 bits in total) which are packed in an
  // unsigned uint32_t.
  //
  // Note: uint8_t4_packed and int8_t4_packed are NOT vector types! They are
  // both scalar 32-bit unsigned integer types where each byte represents one
  // value.
  //
  // Note: In pack_clamp_{s|u}8 intrinsics, an input of 0x100 will be turned
  // into 0xFF, not 0x00. Therefore, it is important to perform a clamp first,
  // and then a truncation.

  // Steps:
  // Use GLSL extended instruction set's clamp (only for clamp instructions).
  // Use OpUConvert/OpSConvert to truncate each element of the vec4 to 8 bits.
  // Use OpBitcast to make a 32-bit uint out of the new vec4.
  auto *arg = callExpr->getArg(0);
  const auto argType = arg->getType();
  SpirvInstruction *argInstr = doExpr(arg);
  QualType elemType = {};
  uint32_t elemCount = 0;
  (void)isVectorType(argType, &elemType, &elemCount);
  const bool isSigned = elemType->isSignedIntegerType();
  assert(elemCount == 4);

  const bool doesClamp = op == hlsl::IntrinsicOp::IOP_pack_clamp_s8 ||
                         op == hlsl::IntrinsicOp::IOP_pack_clamp_u8;
  if (doesClamp) {
    const auto bitwidth = getElementSpirvBitwidth(
        astContext, elemType, spirvOptions.enable16BitTypes);
    int32_t clampMin = op == hlsl::IntrinsicOp::IOP_pack_clamp_u8 ? 0 : -128;
    int32_t clampMax = op == hlsl::IntrinsicOp::IOP_pack_clamp_u8 ? 255 : 127;
    auto *minInstr = spvBuilder.getConstantInt(
        elemType, llvm::APInt(bitwidth, clampMin, isSigned));
    auto *maxInstr = spvBuilder.getConstantInt(
        elemType, llvm::APInt(bitwidth, clampMax, isSigned));
    auto *minVec = spvBuilder.getConstantComposite(
        argType, {minInstr, minInstr, minInstr, minInstr});
    auto *maxVec = spvBuilder.getConstantComposite(
        argType, {maxInstr, maxInstr, maxInstr, maxInstr});
    auto clampOp = isSigned ? GLSLstd450SClamp : GLSLstd450UClamp;
    argInstr = spvBuilder.createGLSLExtInst(argType, clampOp,
                                            {argInstr, minVec, maxVec}, loc);
  }

  if (isSigned) {
    QualType v4Int8Type =
        astContext.getExtVectorType(astContext.SignedCharTy, 4);
    auto *bytesVecInstr = spvBuilder.createUnaryOp(spv::Op::OpSConvert,
                                                   v4Int8Type, argInstr, loc);
    return spvBuilder.createUnaryOp(
        spv::Op::OpBitcast, astContext.Int8_4PackedTy, bytesVecInstr, loc);
  } else {
    QualType v4Uint8Type =
        astContext.getExtVectorType(astContext.UnsignedCharTy, 4);
    auto *bytesVecInstr = spvBuilder.createUnaryOp(spv::Op::OpUConvert,
                                                   v4Uint8Type, argInstr, loc);
    return spvBuilder.createUnaryOp(
        spv::Op::OpBitcast, astContext.UInt8_4PackedTy, bytesVecInstr, loc);
  }
}

SpirvInstruction *
SpirvEmitter::processIntrinsic8BitUnpack(const CallExpr *callExpr,
                                         hlsl::IntrinsicOp op) {
  const auto loc = callExpr->getExprLoc();
  assert(op == hlsl::IntrinsicOp::IOP_unpack_s8s16 ||
         op == hlsl::IntrinsicOp::IOP_unpack_s8s32 ||
         op == hlsl::IntrinsicOp::IOP_unpack_u8u16 ||
         op == hlsl::IntrinsicOp::IOP_unpack_u8u32);

  // Here's the signature for the pack intrinsic operations:
  //
  // int16_t4 unpack_s8s16(int8_t4_packed packedVal);   // Sign Extended
  // uint16_t4 unpack_u8u16(uint8_t4_packed packedVal); // Non-Sign Extended
  // int32_t4 unpack_s8s32(int8_t4_packed packedVal);   // Sign Extended
  // uint32_t4 unpack_u8u32(uint8_t4_packed packedVal); // Non-Sign Extended
  //
  // These functions take a 32-bit unsigned integer as input (where each byte of
  // the input represents one value, i.e. it's packed). They first unpack the
  // 32-bit integer to a vector of 4 bytes. Then for each element of the vec4,
  // they zero-extend or sign-extend the byte in order to achieve a 16-bit or
  // 32-bit vector of integers.
  //
  // Note: uint8_t4_packed and int8_t4_packed are NOT vector types! They are
  // both scalar 32-bit unsigned integer types where each byte represents one
  // value.

  // Steps:
  // Use OpBitcast to make a vec4 of bytes from a 32-bit value.
  // Use OpUConvert/OpSConvert to zero-extend/sign-extend each element of the
  // vec4 to 16 or 32 bits.
  auto *arg = callExpr->getArg(0);
  SpirvInstruction *argInstr = doExpr(arg);

  const bool isSigned = op == hlsl::IntrinsicOp::IOP_unpack_s8s16 ||
                        op == hlsl::IntrinsicOp::IOP_unpack_s8s32;

  QualType resultType = {};
  if (op == hlsl::IntrinsicOp::IOP_unpack_s8s16 ||
      op == hlsl::IntrinsicOp::IOP_unpack_u8u16) {
    resultType = astContext.getExtVectorType(
        isSigned ? astContext.ShortTy : astContext.UnsignedShortTy, 4);
  } else {
    resultType = astContext.getExtVectorType(
        isSigned ? astContext.IntTy : astContext.UnsignedIntTy, 4);
  }

  if (isSigned) {
    QualType v4Int8Type =
        astContext.getExtVectorType(astContext.SignedCharTy, 4);
    auto *bytesVecInstr =
        spvBuilder.createUnaryOp(spv::Op::OpBitcast, v4Int8Type, argInstr, loc);
    return spvBuilder.createUnaryOp(spv::Op::OpSConvert, resultType,
                                    bytesVecInstr, loc);
  } else {
    QualType v4Uint8Type =
        astContext.getExtVectorType(astContext.UnsignedCharTy, 4);
    auto *bytesVecInstr = spvBuilder.createUnaryOp(spv::Op::OpBitcast,
                                                   v4Uint8Type, argInstr, loc);
    return spvBuilder.createUnaryOp(spv::Op::OpUConvert, resultType,
                                    bytesVecInstr, loc);
  }
}

SpirvInstruction *SpirvEmitter::processRayBuiltins(const CallExpr *callExpr,
                                                   hlsl::IntrinsicOp op) {
  bool nvRayTracing =
      featureManager.isExtensionEnabled(Extension::NV_ray_tracing);
  spv::BuiltIn builtin = spv::BuiltIn::Max;
  bool transposeMatrix = false;
  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();
  switch (op) {
  case hlsl::IntrinsicOp::IOP_DispatchRaysDimensions:
    builtin = spv::BuiltIn::LaunchSizeNV;
    break;
  case hlsl::IntrinsicOp::IOP_DispatchRaysIndex:
    builtin = spv::BuiltIn::LaunchIdNV;
    break;
  case hlsl::IntrinsicOp::IOP_RayTCurrent:
    if (nvRayTracing)
      builtin = spv::BuiltIn::HitTNV;
    else
      builtin = spv::BuiltIn::RayTmaxKHR;
    break;
  case hlsl::IntrinsicOp::IOP_RayTMin:
    builtin = spv::BuiltIn::RayTminNV;
    break;
  case hlsl::IntrinsicOp::IOP_HitKind:
    builtin = spv::BuiltIn::HitKindNV;
    break;
  case hlsl::IntrinsicOp::IOP_WorldRayDirection:
    builtin = spv::BuiltIn::WorldRayDirectionNV;
    break;
  case hlsl::IntrinsicOp::IOP_WorldRayOrigin:
    builtin = spv::BuiltIn::WorldRayOriginNV;
    break;
  case hlsl::IntrinsicOp::IOP_ObjectRayDirection:
    builtin = spv::BuiltIn::ObjectRayDirectionNV;
    break;
  case hlsl::IntrinsicOp::IOP_ObjectRayOrigin:
    builtin = spv::BuiltIn::ObjectRayOriginNV;
    break;
  case hlsl::IntrinsicOp::IOP_GeometryIndex:
    featureManager.requestExtension(Extension::KHR_ray_tracing,
                                    "GeometryIndex()", loc);
    builtin = spv::BuiltIn::RayGeometryIndexKHR;
    break;
  case hlsl::IntrinsicOp::IOP_InstanceIndex:
    builtin = spv::BuiltIn::InstanceId;
    break;
  case hlsl::IntrinsicOp::IOP_PrimitiveIndex:
    builtin = spv::BuiltIn::PrimitiveId;
    break;
  case hlsl::IntrinsicOp::IOP_InstanceID:
    builtin = spv::BuiltIn::InstanceCustomIndexNV;
    break;
  case hlsl::IntrinsicOp::IOP_RayFlags:
    builtin = spv::BuiltIn::IncomingRayFlagsNV;
    break;
  case hlsl::IntrinsicOp::IOP_ObjectToWorld3x4:
    transposeMatrix = true;
    LLVM_FALLTHROUGH;
  case hlsl::IntrinsicOp::IOP_ObjectToWorld4x3:
    builtin = spv::BuiltIn::ObjectToWorldNV;
    break;
  case hlsl::IntrinsicOp::IOP_WorldToObject3x4:
    transposeMatrix = true;
    LLVM_FALLTHROUGH;
  case hlsl::IntrinsicOp::IOP_WorldToObject4x3:
    builtin = spv::BuiltIn::WorldToObjectNV;
    break;
  default:
    emitError("ray intrinsic function unimplemented", loc);
    return nullptr;
  }
  needsLegalization = true;

  QualType builtinType = callExpr->getType();
  if (transposeMatrix) {
    // DXR defines ObjectToWorld3x4, WorldToObject3x4 as transposed matrices.
    // SPIR-V has only non tranposed variant defined as a builtin
    // So perform read of original non transposed builtin and perform transpose.
    assert(hlsl::IsHLSLMatType(builtinType) && "Builtin should be matrix");
    const clang::Type *type = builtinType.getCanonicalType().getTypePtr();
    const RecordType *RT = cast<RecordType>(type);
    const ClassTemplateSpecializationDecl *templateSpecDecl =
        cast<ClassTemplateSpecializationDecl>(RT->getDecl());
    ClassTemplateDecl *templateDecl =
        templateSpecDecl->getSpecializedTemplate();
    builtinType = getHLSLMatrixType(astContext, theCompilerInstance.getSema(),
                                    templateDecl, astContext.FloatTy, 4, 3);
  }
  SpirvInstruction *retVal =
      declIdMapper.getBuiltinVar(builtin, builtinType, loc);
  retVal = spvBuilder.createLoad(builtinType, retVal, loc, range);
  if (transposeMatrix)
    retVal = spvBuilder.createUnaryOp(spv::Op::OpTranspose, callExpr->getType(),
                                      retVal, loc, range);
  return retVal;
}

SpirvInstruction *SpirvEmitter::processReportHit(const CallExpr *callExpr) {
  if (callExpr->getNumArgs() != 3) {
    emitError("invalid number of arguments to ReportHit",
              callExpr->getExprLoc());
  }

  // HLSL Function :
  // template<typename hitAttr>
  // ReportHit(in float, in uint, in hitAttr)
  const Expr *hitAttr = callExpr->getArg(2);
  SpirvInstruction *hitAttributeArgInstr =
      doExpr(hitAttr, hitAttr->getExprLoc());
  QualType hitAttributeType = hitAttr->getType();

  // TODO(#6364): Verify that this behavior is correct.
  SpirvInstruction *hitAttributeStageVar;
  const auto iter = hitAttributeMap.find(hitAttributeType);
  if (iter == hitAttributeMap.end()) {
    hitAttributeStageVar = declIdMapper.createRayTracingNVStageVar(
        spv::StorageClass::HitAttributeNV, hitAttributeType,
        hitAttributeArgInstr->getDebugName(), hitAttributeArgInstr->isPrecise(),
        hitAttributeArgInstr->isNoninterpolated());
    hitAttributeMap[hitAttributeType] = hitAttributeStageVar;
  } else {
    hitAttributeStageVar = iter->second;
  }

  // Copy argument to stage variable
  spvBuilder.createStore(hitAttributeStageVar, hitAttributeArgInstr,
                         callExpr->getExprLoc());

  // SPIR-V Instruction :
  // bool OpReportIntersection(<id> float Hit, <id> uint HitKind)
  llvm::SmallVector<SpirvInstruction *, 4> reportHitArgs;
  reportHitArgs.push_back(doExpr(callExpr->getArg(0))); // Hit
  reportHitArgs.push_back(doExpr(callExpr->getArg(1))); // HitKind
  return spvBuilder.createRayTracingOpsNV(spv::Op::OpReportIntersectionNV,
                                          astContext.BoolTy, reportHitArgs,
                                          callExpr->getExprLoc());
}

void SpirvEmitter::processCallShader(const CallExpr *callExpr) {
  bool nvRayTracing =
      featureManager.isExtensionEnabled(Extension::NV_ray_tracing);
  SpirvInstruction *callDataLocInst = nullptr;
  SpirvInstruction *callDataStageVar = nullptr;
  const VarDecl *callDataArg = nullptr;
  QualType callDataType;
  const auto args = callExpr->getArgs();

  if (callExpr->getNumArgs() != 2) {
    emitError("invalid number of arguments to CallShader",
              callExpr->getExprLoc());
  }

  // HLSL Func :
  // template<typename CallData>
  // void CallShader(in int sbtIndex, inout CallData arg)
  if (const auto *implCastExpr = dyn_cast<CastExpr>(args[1])) {
    if (const auto *arg = dyn_cast<DeclRefExpr>(implCastExpr->getSubExpr())) {
      if (const auto *varDecl = dyn_cast<VarDecl>(arg->getDecl())) {
        callDataType = varDecl->getType();
        callDataArg = varDecl;
        // Check if same type of callable data stage variable was already
        // created, if so re-use
        const auto callDataPair = callDataMap.find(callDataType);
        if (callDataPair == callDataMap.end()) {
          int numCallDataVars = callDataMap.size();
          callDataStageVar = declIdMapper.createRayTracingNVStageVar(
              spv::StorageClass::CallableDataNV, varDecl);
          // Decorate unique location id for each created stage var
          spvBuilder.decorateLocation(callDataStageVar, numCallDataVars);
          callDataLocInst = spvBuilder.getConstantInt(
              astContext.UnsignedIntTy, llvm::APInt(32, numCallDataVars));
          callDataMap[callDataType] =
              std::make_pair(callDataStageVar, callDataLocInst);
        } else {
          callDataStageVar = callDataPair->second.first;
          callDataLocInst = callDataPair->second.second;
        }
      }
    }
  }

  assert(callDataStageVar && callDataArg);

  // Copy argument to stage variable
  const auto callDataArgInst =
      declIdMapper.getDeclEvalInfo(callDataArg, callExpr->getExprLoc());
  auto tempLoad = spvBuilder.createLoad(callDataArg->getType(), callDataArgInst,
                                        callDataArg->getLocStart());
  spvBuilder.createStore(callDataStageVar, tempLoad, callExpr->getExprLoc());

  // SPIR-V Instruction
  // void OpExecuteCallable(<id> int SBT Index, <id> uint Callable Data Location
  // Id)
  llvm::SmallVector<SpirvInstruction *, 2> callShaderArgs;
  callShaderArgs.push_back(doExpr(args[0]));

  if (nvRayTracing) {
    callShaderArgs.push_back(callDataLocInst);
    spvBuilder.createRayTracingOpsNV(spv::Op::OpExecuteCallableNV, QualType(),
                                     callShaderArgs, callExpr->getExprLoc());
  } else {
    callShaderArgs.push_back(callDataStageVar);
    spvBuilder.createRayTracingOpsNV(spv::Op::OpExecuteCallableKHR, QualType(),
                                     callShaderArgs, callExpr->getExprLoc());
  }

  // Copy data back to argument
  tempLoad = spvBuilder.createLoad(callDataArg->getType(), callDataStageVar,
                                   callDataArg->getLocStart());
  spvBuilder.createStore(callDataArgInst, tempLoad, callExpr->getExprLoc());
  return;
}

void SpirvEmitter::processTraceRay(const CallExpr *callExpr) {
  bool nvRayTracing =
      featureManager.isExtensionEnabled(Extension::NV_ray_tracing);

  SpirvInstruction *rayPayloadLocInst = nullptr;
  SpirvInstruction *rayPayloadStageVar = nullptr;
  const VarDecl *rayPayloadArg = nullptr;
  QualType rayPayloadType;

  const auto args = callExpr->getArgs();

  if (callExpr->getNumArgs() != 8) {
    emitError("invalid number of arguments to TraceRay",
              callExpr->getExprLoc());
  }

  // HLSL Func
  // template<typename RayPayload>
  // void TraceRay(RaytracingAccelerationStructure rs,
  //              uint rayflags,
  //              uint InstanceInclusionMask
  //              uint RayContributionToHitGroupIndex,
  //              uint MultiplierForGeometryContributionToHitGroupIndex,
  //              uint MissShaderIndex,
  //              RayDesc ray,
  //              inout RayPayload p)
  // where RayDesc = {float3 origin, float tMin, float3 direction, float tMax}

  if (const auto *implCastExpr = dyn_cast<CastExpr>(args[7])) {
    if (const auto *arg = dyn_cast<DeclRefExpr>(implCastExpr->getSubExpr())) {
      if (const auto *varDecl = dyn_cast<VarDecl>(arg->getDecl())) {
        rayPayloadType = varDecl->getType();
        rayPayloadArg = varDecl;
        const auto rayPayloadPair = rayPayloadMap.find(rayPayloadType);
        // Check if same type of rayPayload stage variable was already
        // created, if so re-use
        if (rayPayloadPair == rayPayloadMap.end()) {
          int numPayloadVars = rayPayloadMap.size();
          rayPayloadStageVar = declIdMapper.createRayTracingNVStageVar(
              spv::StorageClass::RayPayloadNV, varDecl);
          // Decorate unique location id for each created stage var
          spvBuilder.decorateLocation(rayPayloadStageVar, numPayloadVars);
          rayPayloadLocInst = spvBuilder.getConstantInt(
              astContext.UnsignedIntTy, llvm::APInt(32, numPayloadVars));
          rayPayloadMap[rayPayloadType] =
              std::make_pair(rayPayloadStageVar, rayPayloadLocInst);
        } else {
          rayPayloadStageVar = rayPayloadPair->second.first;
          rayPayloadLocInst = rayPayloadPair->second.second;
        }
      }
    }
  }

  assert(rayPayloadStageVar && rayPayloadArg);

  const auto floatType = astContext.FloatTy;
  const auto vecType = astContext.getExtVectorType(astContext.FloatTy, 3);

  // Extract the ray description to match SPIR-V
  SpirvInstruction *rayDescArg = doExpr(args[6]);
  const auto loc = args[6]->getLocStart();
  const auto origin =
      spvBuilder.createCompositeExtract(vecType, rayDescArg, {0}, loc);
  const auto tMin =
      spvBuilder.createCompositeExtract(floatType, rayDescArg, {1}, loc);
  const auto direction =
      spvBuilder.createCompositeExtract(vecType, rayDescArg, {2}, loc);
  const auto tMax =
      spvBuilder.createCompositeExtract(floatType, rayDescArg, {3}, loc);

  // Copy argument to stage variable
  const auto rayPayloadArgInst =
      declIdMapper.getDeclEvalInfo(rayPayloadArg, rayPayloadArg->getLocStart());
  auto tempLoad =
      spvBuilder.createLoad(rayPayloadArg->getType(), rayPayloadArgInst,
                            rayPayloadArg->getLocStart());
  spvBuilder.createStore(rayPayloadStageVar, tempLoad, callExpr->getExprLoc());

  // SPIR-V Instruction
  // void OpTraceNV ( <id> AccelerationStructureNV acStruct,
  //                 <id> uint Ray Flags,
  //                 <id> uint Cull Mask,
  //                 <id> uint SBT Offset,
  //                 <id> uint SBT Stride,
  //                 <id> uint Miss Index,
  //                 <id> vec4 Ray Origin,
  //                 <id> float Ray Tmin,
  //                 <id> vec3 Ray Direction,
  //                 <id> float Ray Tmax,
  //                 <id> uint RayPayload number)

  llvm::SmallVector<SpirvInstruction *, 8> traceArgs;
  for (int ii = 0; ii < 6; ii++) {
    traceArgs.push_back(doExpr(args[ii]));
  }

  traceArgs.push_back(origin);
  traceArgs.push_back(tMin);
  traceArgs.push_back(direction);
  traceArgs.push_back(tMax);

  if (nvRayTracing) {
    traceArgs.push_back(rayPayloadLocInst);
    spvBuilder.createRayTracingOpsNV(spv::Op::OpTraceNV, QualType(), traceArgs,
                                     callExpr->getExprLoc());
  } else {
    traceArgs.push_back(rayPayloadStageVar);
    spvBuilder.createRayTracingOpsNV(spv::Op::OpTraceRayKHR, QualType(),
                                     traceArgs, callExpr->getExprLoc());
  }

  // Copy arguments back to stage variable
  tempLoad = spvBuilder.createLoad(rayPayloadArg->getType(), rayPayloadStageVar,
                                   rayPayloadArg->getLocStart());
  spvBuilder.createStore(rayPayloadArgInst, tempLoad, callExpr->getExprLoc());
  return;
}

void SpirvEmitter::processDispatchMesh(const CallExpr *callExpr) {
  // HLSL Func - void DispatchMesh(uint ThreadGroupCountX,
  //                               uint ThreadGroupCountY,
  //                               uint ThreadGroupCountZ,
  //                               groupshared <structType> MeshPayload);
  assert(callExpr->getNumArgs() == 4);
  const auto args = callExpr->getArgs();
  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();

  // 1) create a barrier GroupMemoryBarrierWithGroupSync().
  processIntrinsicMemoryBarrier(callExpr,
                                /*isDevice*/ false,
                                /*groupSync*/ true,
                                /*isAllBarrier*/ false);

  // 2) create PerTaskNV out attribute block and store MeshPayload info.
  const auto *sigPoint =
      hlsl::SigPoint::GetSigPoint(hlsl::DXIL::SigPointKind::MSOut);
  spv::StorageClass sc =
      featureManager.isExtensionEnabled(Extension::EXT_mesh_shader)
          ? spv::StorageClass::TaskPayloadWorkgroupEXT
          : spv::StorageClass::Output;
  auto *payloadArg = doExpr(args[3]);
  bool isValid = false;
  SpirvInstruction *param = nullptr;
  if (const auto *implCastExpr = dyn_cast<CastExpr>(args[3])) {
    if (const auto *arg = dyn_cast<DeclRefExpr>(implCastExpr->getSubExpr())) {
      if (const auto *paramDecl = dyn_cast<VarDecl>(arg->getDecl())) {
        if (paramDecl->hasAttr<HLSLGroupSharedAttr>()) {
          isValid = declIdMapper.createPayloadStageVars(
              sigPoint, sc, paramDecl, /*asInput=*/false, paramDecl->getType(),
              "out.var", &payloadArg);
          param =
              declIdMapper.getDeclEvalInfo(paramDecl, paramDecl->getLocation());
        }
      }
    }
  }
  if (!isValid) {
    emitError("expected groupshared object as argument to DispatchMesh()",
              args[3]->getExprLoc());
  }

  // 3) set up emit dimension.
  auto *threadX = doExpr(args[0]);
  auto *threadY = doExpr(args[1]);
  auto *threadZ = doExpr(args[2]);

  if (featureManager.isExtensionEnabled(Extension::EXT_mesh_shader)) {
    // for EXT_mesh_shader, create opEmitMeshTasksEXT.
    spvBuilder.createEmitMeshTasksEXT(threadX, threadY, threadZ, loc, param,
                                      range);
  } else {
    // for NV_mesh_shader, set TaskCountNV = threadX * threadY * threadZ.
    auto *var = declIdMapper.getBuiltinVar(spv::BuiltIn::TaskCountNV,
                                           astContext.UnsignedIntTy, loc);
    auto *taskCount = spvBuilder.createBinaryOp(
        spv::Op::OpIMul, astContext.UnsignedIntTy, threadX,
        spvBuilder.createBinaryOp(spv::Op::OpIMul, astContext.UnsignedIntTy,
                                  threadY, threadZ, loc, range),
        loc, range);
    spvBuilder.createStore(var, taskCount, loc, range);
  }
}

void SpirvEmitter::processMeshOutputCounts(const CallExpr *callExpr) {
  // HLSL Func - void SetMeshOutputCounts(uint numVertices, uint numPrimitives);
  assert(callExpr->getNumArgs() == 2);
  const auto args = callExpr->getArgs();
  const auto loc = callExpr->getExprLoc();
  const auto range = callExpr->getSourceRange();

  if (featureManager.isExtensionEnabled(Extension::EXT_mesh_shader)) {
    spvBuilder.createSetMeshOutputsEXT(doExpr(args[0]), doExpr(args[1]), loc,
                                       range);
  } else {
    auto *var = declIdMapper.getBuiltinVar(spv::BuiltIn::PrimitiveCountNV,
                                           astContext.UnsignedIntTy, loc);
    spvBuilder.createStore(var, doExpr(args[1]), loc, range);
  }
}

SpirvInstruction *
SpirvEmitter::processGetAttributeAtVertex(const CallExpr *expr) {
  if (!spvContext.isPS()) {
    emitError("GetAttributeAtVertex only allowed in pixel shader",
              expr->getExprLoc());
    return nullptr;
  }

  // Implicit type conversion should bound to two things:
  // 1. Function Parameter, and recursively redecl mapped function called's var
  // types.
  // 2. User defined structure types, which may be used in function local
  // variables (AccessChain add index 0 at end)
  const auto exprLoc = expr->getExprLoc();
  const auto exprRange = expr->getSourceRange();

  // arg1 : vertexId
  auto *arg1BaseExpr = doExpr(expr->getArg(1));

  // arg0 : <NoInterpolation> decorated input
  // Tip  : for input with boolean type, we need to ignore implicit cast first,
  //        to match surrounded workaround cast expr.
  auto *arg0NoCast = expr->getArg(0)->IgnoreCasts();
  SpirvInstruction *paramDeclInstr = doExpr(arg0NoCast);
  // Hans't be redeclared before
  QualType elementType = paramDeclInstr->getAstResultType();

  if (isBoolOrVecOfBoolType(elementType)) {
    emitError("attribute evaluation can only be done "
              "on values taken directly from inputs.",
              {});
  }
  // Change to access chain instr
  SpirvInstruction *accessChainPtr = paramDeclInstr;
  if (isa<SpirvAccessChain>(accessChainPtr)) {
    auto *accessInstr = dyn_cast<SpirvAccessChain>(accessChainPtr);
    accessInstr->insertIndex(arg1BaseExpr, accessInstr->getIndexes().size());
  } else
    accessChainPtr = spvBuilder.createAccessChain(
        elementType, accessChainPtr, arg1BaseExpr, exprLoc, exprRange);
  dyn_cast<SpirvAccessChain>(accessChainPtr)->setNoninterpolated(false);
  auto *loadPtr =
      spvBuilder.createLoad(elementType, accessChainPtr, exprLoc, exprRange);
  // PerVertexKHR Decorator and type redecl will be done in later pervertex
  // visitor.
  spvBuilder.setPerVertexInterpMode(true);
  return loadPtr;
}

SpirvConstant *SpirvEmitter::getValueZero(QualType type) {
  {
    QualType scalarType = {};
    if (isScalarType(type, &scalarType)) {
      if (scalarType->isBooleanType()) {
        return spvBuilder.getConstantBool(false);
      }
      if (scalarType->isIntegerType()) {
        return spvBuilder.getConstantInt(scalarType, llvm::APInt(32, 0));
      }
      if (scalarType->isFloatingType()) {
        return spvBuilder.getConstantFloat(scalarType, llvm::APFloat(0.0f));
      }
    }
  }

  {
    QualType elemType = {};
    uint32_t size = {};
    if (isVectorType(type, &elemType, &size)) {
      return getVecValueZero(elemType, size);
    }
  }

  {
    QualType elemType = {};
    uint32_t rowCount = 0, colCount = 0;
    if (isMxNMatrix(type, &elemType, &rowCount, &colCount)) {
      auto *row = getVecValueZero(elemType, colCount);
      llvm::SmallVector<SpirvConstant *, 4> rows((size_t)rowCount, row);
      return spvBuilder.getConstantComposite(type, rows);
    }
  }

  emitError("getting value 0 for type %0 unimplemented", {})
      << type.getAsString();
  return nullptr;
}

SpirvConstant *SpirvEmitter::getVecValueZero(QualType elemType, uint32_t size) {
  auto *elemZeroId = getValueZero(elemType);

  if (size == 1)
    return elemZeroId;

  llvm::SmallVector<SpirvConstant *, 4> elements(size_t(size), elemZeroId);
  const QualType vecType = astContext.getExtVectorType(elemType, size);
  return spvBuilder.getConstantComposite(vecType, elements);
}

SpirvConstant *SpirvEmitter::getValueOne(QualType type) {
  {
    QualType scalarType = {};
    if (isScalarType(type, &scalarType)) {
      if (scalarType->isBooleanType()) {
        return spvBuilder.getConstantBool(true);
      }
      if (scalarType->isIntegerType()) {
        return spvBuilder.getConstantInt(scalarType, llvm::APInt(32, 1));
      }
      if (scalarType->isFloatingType()) {
        return spvBuilder.getConstantFloat(scalarType, llvm::APFloat(1.0f));
      }
    }
  }

  {
    QualType elemType = {};
    uint32_t size = {};
    if (isVectorType(type, &elemType, &size)) {
      return getVecValueOne(elemType, size);
    }
  }

  emitError("getting value 1 for type %0 unimplemented", {}) << type;
  return 0;
}

SpirvConstant *SpirvEmitter::getVecValueOne(QualType elemType, uint32_t size) {
  auto *elemOne = getValueOne(elemType);

  if (size == 1)
    return elemOne;

  llvm::SmallVector<SpirvConstant *, 4> elements(size_t(size), elemOne);
  const QualType vecType = astContext.getExtVectorType(elemType, size);
  return spvBuilder.getConstantComposite(vecType, elements);
}

SpirvConstant *SpirvEmitter::getMatElemValueOne(QualType type) {
  assert(hlsl::IsHLSLMatType(type));
  const auto elemType = hlsl::GetHLSLMatElementType(type);

  uint32_t rowCount = 0, colCount = 0;
  hlsl::GetHLSLMatRowColCount(type, rowCount, colCount);

  if (rowCount == 1 && colCount == 1)
    return getValueOne(elemType);
  if (colCount == 1)
    return getVecValueOne(elemType, rowCount);
  return getVecValueOne(elemType, colCount);
}

SpirvConstant *SpirvEmitter::getMaskForBitwidthValue(QualType type) {
  QualType elemType = {};
  uint32_t count = 1;

  if (isScalarType(type, &elemType) || isVectorType(type, &elemType, &count)) {
    const auto bitwidth = getElementSpirvBitwidth(
        astContext, elemType, spirvOptions.enable16BitTypes);
    SpirvConstant *mask = spvBuilder.getConstantInt(
        elemType,
        llvm::APInt(bitwidth, bitwidth - 1, elemType->isSignedIntegerType()));

    if (count == 1)
      return mask;

    const QualType resultType = astContext.getExtVectorType(elemType, count);
    llvm::SmallVector<SpirvConstant *, 4> elements(size_t(count), mask);
    return spvBuilder.getConstantComposite(resultType, elements);
  }

  assert(false && "this method only supports scalars and vectors");
  return nullptr;
}

hlsl::ShaderModel::Kind SpirvEmitter::getShaderModelKind(StringRef stageName) {
  hlsl::ShaderModel::Kind SMK =
      llvm::StringSwitch<hlsl::ShaderModel::Kind>(stageName)
          .Case("pixel", hlsl::ShaderModel::Kind::Pixel)
          .Case("vertex", hlsl::ShaderModel::Kind::Vertex)
          .Case("geometry", hlsl::ShaderModel::Kind::Geometry)
          .Case("hull", hlsl::ShaderModel::Kind::Hull)
          .Case("domain", hlsl::ShaderModel::Kind::Domain)
          .Case("compute", hlsl::ShaderModel::Kind::Compute)
          .Case("raygeneration", hlsl::ShaderModel::Kind::RayGeneration)
          .Case("intersection", hlsl::ShaderModel::Kind::Intersection)
          .Case("anyhit", hlsl::ShaderModel::Kind::AnyHit)
          .Case("closesthit", hlsl::ShaderModel::Kind::ClosestHit)
          .Case("miss", hlsl::ShaderModel::Kind::Miss)
          .Case("callable", hlsl::ShaderModel::Kind::Callable)
          .Case("mesh", hlsl::ShaderModel::Kind::Mesh)
          .Case("amplification", hlsl::ShaderModel::Kind::Amplification)
          .Case("node", hlsl::ShaderModel::Kind::Node)
          .Default(hlsl::ShaderModel::Kind::Invalid);
  assert(SMK != hlsl::ShaderModel::Kind::Invalid);
  return SMK;
}

spv::ExecutionModel
SpirvEmitter::getSpirvShaderStage(hlsl::ShaderModel::Kind smk,
                                  bool extMeshShading) {
  switch (smk) {
  case hlsl::ShaderModel::Kind::Vertex:
    return spv::ExecutionModel::Vertex;
  case hlsl::ShaderModel::Kind::Hull:
    return spv::ExecutionModel::TessellationControl;
  case hlsl::ShaderModel::Kind::Domain:
    return spv::ExecutionModel::TessellationEvaluation;
  case hlsl::ShaderModel::Kind::Geometry:
    return spv::ExecutionModel::Geometry;
  case hlsl::ShaderModel::Kind::Pixel:
    return spv::ExecutionModel::Fragment;
  case hlsl::ShaderModel::Kind::Compute:
  case hlsl::ShaderModel::Kind::Node:
    return spv::ExecutionModel::GLCompute;
  case hlsl::ShaderModel::Kind::RayGeneration:
    return spv::ExecutionModel::RayGenerationNV;
  case hlsl::ShaderModel::Kind::Intersection:
    return spv::ExecutionModel::IntersectionNV;
  case hlsl::ShaderModel::Kind::AnyHit:
    return spv::ExecutionModel::AnyHitNV;
  case hlsl::ShaderModel::Kind::ClosestHit:
    return spv::ExecutionModel::ClosestHitNV;
  case hlsl::ShaderModel::Kind::Miss:
    return spv::ExecutionModel::MissNV;
  case hlsl::ShaderModel::Kind::Callable:
    return spv::ExecutionModel::CallableNV;
  case hlsl::ShaderModel::Kind::Mesh:
    return extMeshShading ? spv::ExecutionModel::MeshEXT
                          : spv::ExecutionModel::MeshNV;
  case hlsl::ShaderModel::Kind::Amplification:
    return extMeshShading ? spv::ExecutionModel::TaskEXT
                          : spv::ExecutionModel::TaskNV;
  default:
    llvm_unreachable("invalid shader model kind");
    break;
  }
}

void SpirvEmitter::processInlineSpirvAttributes(const FunctionDecl *decl) {
  if (!decl->hasAttrs())
    return;

  for (auto &attr : decl->getAttrs()) {
    if (auto *modeAttr = dyn_cast<VKSpvExecutionModeAttr>(attr)) {
      spvBuilder.addExecutionMode(
          entryFunction, spv::ExecutionMode(modeAttr->getExecutionMode()), {},
          modeAttr->getLocation());
    }
  }

  // Handle extension and capability attrs
  if (decl->hasAttr<VKExtensionExtAttr>() ||
      decl->hasAttr<VKCapabilityExtAttr>()) {
    createSpirvIntrInstExt(decl->getAttrs(), QualType(), /* spvArgs */ {},
                           /* isInst */ false, decl->getLocStart());
  }
}

bool SpirvEmitter::processGeometryShaderAttributes(const FunctionDecl *decl,
                                                   uint32_t *arraySize) {
  bool success = true;
  assert(spvContext.isGS());
  if (auto *vcAttr = decl->getAttr<HLSLMaxVertexCountAttr>()) {
    spvBuilder.addExecutionMode(
        entryFunction, spv::ExecutionMode::OutputVertices,
        {static_cast<uint32_t>(vcAttr->getCount())}, decl->getLocation());
  }

  uint32_t invocations = 1;
  if (auto *instanceAttr = decl->getAttr<HLSLInstanceAttr>()) {
    invocations = static_cast<uint32_t>(instanceAttr->getCount());
  }
  spvBuilder.addExecutionMode(entryFunction, spv::ExecutionMode::Invocations,
                              {invocations}, decl->getLocation());

  // Only one primitive type is permitted for the geometry shader.
  bool outPoint = false, outLine = false, outTriangle = false, inPoint = false,
       inLine = false, inTriangle = false, inLineAdj = false,
       inTriangleAdj = false;
  for (const auto *param : decl->params()) {
    // Add an execution mode based on the output stream type. Do not an
    // execution mode more than once.
    if (param->hasAttr<HLSLInOutAttr>()) {
      const auto paramType = param->getType();
      if (hlsl::IsHLSLTriangleStreamType(paramType) && !outTriangle) {
        spvBuilder.addExecutionMode(entryFunction,
                                    spv::ExecutionMode::OutputTriangleStrip, {},
                                    param->getLocation());
        outTriangle = true;
      } else if (hlsl::IsHLSLLineStreamType(paramType) && !outLine) {
        spvBuilder.addExecutionMode(entryFunction,
                                    spv::ExecutionMode::OutputLineStrip, {},
                                    param->getLocation());
        outLine = true;
      } else if (hlsl::IsHLSLPointStreamType(paramType) && !outPoint) {
        spvBuilder.addExecutionMode(entryFunction,
                                    spv::ExecutionMode::OutputPoints, {},
                                    param->getLocation());
        outPoint = true;
      }
      // An output stream parameter will not have the input primitive type
      // attributes, so we can continue to the next parameter.
      continue;
    }

    // Add an execution mode based on the input primitive type. Do not add an
    // execution mode more than once.
    if (param->hasAttr<HLSLPointAttr>() && !inPoint) {
      spvBuilder.addExecutionMode(entryFunction,
                                  spv::ExecutionMode::InputPoints, {},
                                  param->getLocation());
      *arraySize = 1;
      inPoint = true;
    } else if (param->hasAttr<HLSLLineAttr>() && !inLine) {
      spvBuilder.addExecutionMode(entryFunction, spv::ExecutionMode::InputLines,
                                  {}, param->getLocation());
      *arraySize = 2;
      inLine = true;
    } else if (param->hasAttr<HLSLTriangleAttr>() && !inTriangle) {
      spvBuilder.addExecutionMode(entryFunction, spv::ExecutionMode::Triangles,
                                  {}, param->getLocation());
      *arraySize = 3;
      inTriangle = true;
    } else if (param->hasAttr<HLSLLineAdjAttr>() && !inLineAdj) {
      spvBuilder.addExecutionMode(entryFunction,
                                  spv::ExecutionMode::InputLinesAdjacency, {},
                                  param->getLocation());
      *arraySize = 4;
      inLineAdj = true;
    } else if (param->hasAttr<HLSLTriangleAdjAttr>() && !inTriangleAdj) {
      spvBuilder.addExecutionMode(entryFunction,
                                  spv::ExecutionMode::InputTrianglesAdjacency,
                                  {}, param->getLocation());
      *arraySize = 6;
      inTriangleAdj = true;
    }
  }
  if (inPoint + inLine + inLineAdj + inTriangle + inTriangleAdj > 1) {
    emitError("only one input primitive type can be specified in the geometry "
              "shader",
              {});
    success = false;
  }
  if (outPoint + outTriangle + outLine > 1) {
    emitError("only one output primitive type can be specified in the geometry "
              "shader",
              {});
    success = false;
  }

  return success;
}

void SpirvEmitter::processPixelShaderAttributes(const FunctionDecl *decl) {
  spvBuilder.addExecutionMode(entryFunction,
                              spv::ExecutionMode::OriginUpperLeft, {},
                              decl->getLocation());
  if (decl->getAttr<HLSLEarlyDepthStencilAttr>()) {
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::EarlyFragmentTests, {},
                                decl->getLocation());
  }
  if (decl->getAttr<VKPostDepthCoverageAttr>()) {
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::PostDepthCoverage, {},
                                decl->getLocation());
  }
  if (decl->getAttr<VKEarlyAndLateTestsAttr>()) {
    spvBuilder.addExecutionMode(
        entryFunction, spv::ExecutionMode::EarlyAndLateFragmentTestsAMD, {},
        decl->getLocation());
  }
  if (decl->getAttr<VKDepthUnchangedAttr>()) {
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::DepthUnchanged, {},
                                decl->getLocation());
  }

  // Shaders must not specify more than one of stencil_ref_unchanged_front,
  // stencil_ref_greater_equal_front, and stencil_ref_less_equal_front.
  // Shaders must not specify more than one of stencil_ref_unchanged_back,
  // stencil_ref_greater_equal_back,and stencil_ref_less_equal_back.
  uint32_t stencilFrontAttrCount = 0, stencilBackAttrCount = 0;
  if (decl->getAttr<VKStencilRefUnchangedFrontAttr>()) {
    ++stencilFrontAttrCount;
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::StencilRefUnchangedFrontAMD,
                                {}, decl->getLocation());
  }
  if (decl->getAttr<VKStencilRefGreaterEqualFrontAttr>()) {
    ++stencilFrontAttrCount;
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::StencilRefGreaterFrontAMD,
                                {}, decl->getLocation());
  }
  if (decl->getAttr<VKStencilRefLessEqualFrontAttr>()) {
    ++stencilFrontAttrCount;
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::StencilRefLessFrontAMD, {},
                                decl->getLocation());
  }
  if (decl->getAttr<VKStencilRefUnchangedBackAttr>()) {
    ++stencilBackAttrCount;
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::StencilRefUnchangedBackAMD,
                                {}, decl->getLocation());
  }
  if (decl->getAttr<VKStencilRefGreaterEqualBackAttr>()) {
    ++stencilBackAttrCount;
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::StencilRefGreaterBackAMD,
                                {}, decl->getLocation());
  }
  if (decl->getAttr<VKStencilRefLessEqualBackAttr>()) {
    ++stencilBackAttrCount;
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::StencilRefLessBackAMD, {},
                                decl->getLocation());
  }
  if (stencilFrontAttrCount > 1) {
    emitError("Shaders must not specify more than one of "
              "stencil_ref_unchanged_front, stencil_ref_greater_equal_front, "
              "and stencil_ref_less_equal_front.",
              {});
  }
  if (stencilBackAttrCount > 1) {
    emitError(
        "Shaders must not specify more than one of stencil_ref_unchanged_back, "
        "stencil_ref_greater_equal_back, and stencil_ref_less_equal_back.",
        {});
  }
}

void SpirvEmitter::checkForWaveSizeAttr(const FunctionDecl *decl) {
  if (auto *waveSizeAttr = decl->getAttr<HLSLWaveSizeAttr>()) {
    // Not supported in Vulkan SPIR-V, warn and ignore.

    // SPIR-V SubgroupSize execution mode would work but it is Kernel only
    // (requires the SubgroupDispatch capability, which implies the
    // DeviceEnqueue capability, which is Kernel only). Subgroup sizes can be
    // specified in Vulkan on the application side via
    // VK_EXT_subgroup_size_control.
    emitWarning("Wave size is not supported by Vulkan SPIR-V. Consider using "
                "VK_EXT_subgroup_size_control.",
                waveSizeAttr->getLocation());
  }
}

void SpirvEmitter::processComputeShaderAttributes(const FunctionDecl *decl) {
  auto *numThreadsAttr = decl->getAttr<HLSLNumThreadsAttr>();
  assert(numThreadsAttr && "thread group size missing from entry-point");

  uint32_t x = static_cast<uint32_t>(numThreadsAttr->getX());
  uint32_t y = static_cast<uint32_t>(numThreadsAttr->getY());
  uint32_t z = static_cast<uint32_t>(numThreadsAttr->getZ());

  spvBuilder.addExecutionMode(entryFunction, spv::ExecutionMode::LocalSize,
                              {x, y, z}, decl->getLocation());

  checkForWaveSizeAttr(decl);
}

void SpirvEmitter::processNodeShaderAttributes(const FunctionDecl *decl) {
  uint32_t x = 1, y = 1, z = 1;
  if (auto *numThreadsAttr = decl->getAttr<HLSLNumThreadsAttr>()) {
    x = static_cast<uint32_t>(numThreadsAttr->getX());
    y = static_cast<uint32_t>(numThreadsAttr->getY());
    z = static_cast<uint32_t>(numThreadsAttr->getZ());
  }
  spvBuilder.addExecutionMode(entryFunction, spv::ExecutionMode::LocalSize,
                              {x, y, z}, decl->getLocation());

  auto *nodeLaunchAttr = decl->getAttr<HLSLNodeLaunchAttr>();
  StringRef launchType = nodeLaunchAttr ? nodeLaunchAttr->getLaunchType() : "";
  if (launchType.equals("coalescing") || launchType.equals("thread")) {
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::CoalescingAMDX, {},
                                decl->getLocation());
  }

  uint64_t nodeId = 0;
  if (const auto nodeIdAttr = decl->getAttr<HLSLNodeIdAttr>())
    nodeId = static_cast<uint64_t>(nodeIdAttr->getArrayIndex());
  spvBuilder.addExecutionModeId(
      entryFunction, spv::ExecutionMode::ShaderIndexAMDX,
      {spvBuilder.getConstantInt(astContext.UnsignedIntTy,
                                 llvm::APInt(32, nodeId))},
      decl->getLocation());

  if (const auto *nodeMaxRecursionDepthAttr =
          decl->getAttr<HLSLNodeMaxRecursionDepthAttr>()) {
    SpirvInstruction *count = spvBuilder.getConstantInt(
        astContext.UnsignedIntTy,
        llvm::APInt(32, nodeMaxRecursionDepthAttr->getCount()));
    spvBuilder.addExecutionModeId(entryFunction,
                                  spv::ExecutionMode::MaxNodeRecursionAMDX,
                                  {count}, decl->getLocation());
  }

  if (const auto *nodeShareInputOfAttr =
          decl->getAttr<HLSLNodeShareInputOfAttr>()) {
    SpirvInstruction *name =
        spvBuilder.getConstantString(nodeShareInputOfAttr->getName());
    SpirvInstruction *index = spvBuilder.getConstantInt(
        astContext.UnsignedIntTy,
        llvm::APInt(32, nodeShareInputOfAttr->getArrayIndex()));
    spvBuilder.addExecutionModeId(entryFunction,
                                  spv::ExecutionMode::SharesInputWithAMDX,
                                  {name, index}, decl->getLocation());
  }

  if (const auto *dispatchGrid = decl->getAttr<HLSLNodeDispatchGridAttr>()) {
    SpirvInstruction *gridX = spvBuilder.getConstantInt(
        astContext.UnsignedIntTy, llvm::APInt(32, dispatchGrid->getX()));
    SpirvInstruction *gridY = spvBuilder.getConstantInt(
        astContext.UnsignedIntTy, llvm::APInt(32, dispatchGrid->getY()));
    SpirvInstruction *gridZ = spvBuilder.getConstantInt(
        astContext.UnsignedIntTy, llvm::APInt(32, dispatchGrid->getZ()));
    spvBuilder.addExecutionModeId(entryFunction,
                                  spv::ExecutionMode::StaticNumWorkgroupsAMDX,
                                  {gridX, gridY, gridZ}, decl->getLocation());
  } else if (const auto *maxDispatchGrid =
                 decl->getAttr<HLSLNodeMaxDispatchGridAttr>()) {
    SpirvInstruction *gridX = spvBuilder.getConstantInt(
        astContext.UnsignedIntTy, llvm::APInt(32, maxDispatchGrid->getX()));
    SpirvInstruction *gridY = spvBuilder.getConstantInt(
        astContext.UnsignedIntTy, llvm::APInt(32, maxDispatchGrid->getY()));
    SpirvInstruction *gridZ = spvBuilder.getConstantInt(
        astContext.UnsignedIntTy, llvm::APInt(32, maxDispatchGrid->getZ()));
    spvBuilder.addExecutionModeId(entryFunction,
                                  spv::ExecutionMode::MaxNumWorkgroupsAMDX,
                                  {gridX, gridY, gridZ}, decl->getLocation());
  }

  checkForWaveSizeAttr(decl);
}

bool SpirvEmitter::processTessellationShaderAttributes(
    const FunctionDecl *decl, uint32_t *numOutputControlPoints) {
  assert(spvContext.isHS() || spvContext.isDS());
  using namespace spv;

  if (auto *domain = decl->getAttr<HLSLDomainAttr>()) {
    const auto domainType = domain->getDomainType().lower();
    const ExecutionMode hsExecMode =
        llvm::StringSwitch<ExecutionMode>(domainType)
            .Case("tri", ExecutionMode::Triangles)
            .Case("quad", ExecutionMode::Quads)
            .Case("isoline", ExecutionMode::Isolines)
            .Default(ExecutionMode::Max);
    if (hsExecMode == ExecutionMode::Max) {
      emitError("unknown domain type specified for entry function",
                domain->getLocation());
      return false;
    }
    spvBuilder.addExecutionMode(entryFunction, hsExecMode, {},
                                decl->getLocation());
  }

  // Early return for domain shaders as domain shaders only takes the 'domain'
  // attribute.
  if (spvContext.isDS())
    return true;

  if (auto *partitioning = decl->getAttr<HLSLPartitioningAttr>()) {
    const auto scheme = partitioning->getScheme().lower();
    if (scheme == "pow2") {
      emitError("pow2 partitioning scheme is not supported since there is no "
                "equivalent in Vulkan",
                partitioning->getLocation());
      return false;
    }
    const ExecutionMode hsExecMode =
        llvm::StringSwitch<ExecutionMode>(scheme)
            .Case("fractional_even", ExecutionMode::SpacingFractionalEven)
            .Case("fractional_odd", ExecutionMode::SpacingFractionalOdd)
            .Case("integer", ExecutionMode::SpacingEqual)
            .Default(ExecutionMode::Max);
    if (hsExecMode == ExecutionMode::Max) {
      emitError("unknown partitioning scheme in hull shader",
                partitioning->getLocation());
      return false;
    }
    spvBuilder.addExecutionMode(entryFunction, hsExecMode, {},
                                decl->getLocation());
  }
  if (auto *outputTopology = decl->getAttr<HLSLOutputTopologyAttr>()) {
    const auto topology = outputTopology->getTopology().lower();
    const ExecutionMode hsExecMode =
        llvm::StringSwitch<ExecutionMode>(topology)
            .Case("point", ExecutionMode::PointMode)
            .Case("triangle_cw", ExecutionMode::VertexOrderCw)
            .Case("triangle_ccw", ExecutionMode::VertexOrderCcw)
            .Default(ExecutionMode::Max);
    // TODO: There is no SPIR-V equivalent for "line" topology. Is it the
    // default?
    if (topology != "line") {
      if (hsExecMode != spv::ExecutionMode::Max) {
        spvBuilder.addExecutionMode(entryFunction, hsExecMode, {},
                                    decl->getLocation());
      } else {
        emitError("unknown output topology in hull shader",
                  outputTopology->getLocation());
        return false;
      }
    }
  }
  if (auto *controlPoints = decl->getAttr<HLSLOutputControlPointsAttr>()) {
    *numOutputControlPoints = controlPoints->getCount();
    spvBuilder.addExecutionMode(entryFunction,
                                spv::ExecutionMode::OutputVertices,
                                {*numOutputControlPoints}, decl->getLocation());
  }
  if (auto *pcf = decl->getAttr<HLSLPatchConstantFuncAttr>()) {
    llvm::StringRef pcf_name = pcf->getFunctionName();
    for (auto *decl : astContext.getTranslationUnitDecl()->decls())
      if (auto *funcDecl = dyn_cast<FunctionDecl>(decl))
        if (astContext.IsPatchConstantFunctionDecl(funcDecl) &&
            funcDecl->getName() == pcf_name)
          patchConstFunc = funcDecl;
  }

  return true;
}

bool SpirvEmitter::emitEntryFunctionWrapperForRayTracing(
    const FunctionDecl *decl, RichDebugInfo **info,
    SpirvDebugFunction *debugFunction, SpirvFunction *entryFuncInstr) {
  // The entry basic block.
  auto *entryLabel = spvBuilder.createBasicBlock();
  spvBuilder.setInsertPoint(entryLabel);

  // Add DebugFunctionDefinition if we are emitting
  // NonSemantic.Shader.DebugInfo.100 debug info.
  if (spirvOptions.debugInfoVulkan && debugFunction)
    spvBuilder.createDebugFunctionDef(debugFunction, entryFunction);

  // Initialize all global variables at the beginning of the wrapper
  for (const VarDecl *varDecl : toInitGloalVars) {
    const auto varInfo =
        declIdMapper.getDeclEvalInfo(varDecl, varDecl->getLocation());
    if (const auto *init = varDecl->getInit()) {
      parentMap = std::make_unique<ParentMap>(const_cast<Expr *>(init));
      storeValue(varInfo, loadIfGLValue(init), varDecl->getType(),
                 init->getLocStart());

      // Update counter variable associated with global variables
      tryToAssignCounterVar(varDecl, init);
      parentMap.reset(nullptr);
    }
    // If not explicitly initialized, initialize with their zero values if not
    // resource objects
    else if (!hlsl::IsHLSLResourceType(varDecl->getType())) {
      auto *nullValue = spvBuilder.getConstantNull(varDecl->getType());
      spvBuilder.createStore(varInfo, nullValue, varDecl->getLocation());
    }
  }

  // Create temporary variables for holding function call arguments
  llvm::SmallVector<SpirvInstruction *, 4> params;
  llvm::SmallVector<QualType, 4> paramTypes;
  llvm::SmallVector<SpirvInstruction *, 4> stageVars;
  hlsl::ShaderModel::Kind sKind = spvContext.getCurrentShaderModelKind();
  for (uint32_t i = 0; i < decl->getNumParams(); i++) {
    const auto param = decl->getParamDecl(i);
    const auto paramType = param->getType();
    std::string tempVarName = "param.var." + param->getNameAsString();
    auto *tempVar =
        spvBuilder.addFnVar(paramType, param->getLocation(), tempVarName,
                            param->hasAttr<HLSLPreciseAttr>(),
                            param->hasAttr<HLSLNoInterpolationAttr>());

    SpirvVariable *curStageVar = nullptr;

    params.push_back(tempVar);
    paramTypes.push_back(paramType);

    // Order of arguments is fixed
    // Any-Hit/Closest-Hit : Arg 0 = rayPayload(inout), Arg1 = attribute(in)
    // Miss : Arg 0 = rayPayload(inout)
    // Callable : Arg 0 = callable data(inout)
    // Raygeneration/Intersection : No Args allowed
    if (sKind == hlsl::ShaderModel::Kind::RayGeneration) {
      assert("Raygeneration shaders have no arguments of entry function");
    } else if (sKind == hlsl::ShaderModel::Kind::Intersection) {
      assert("Intersection shaders have no arguments of entry function");
    } else if (sKind == hlsl::ShaderModel::Kind::ClosestHit ||
               sKind == hlsl::ShaderModel::Kind::AnyHit) {
      // Generate rayPayloadInNV and hitAttributeNV stage variables
      if (i == 0) {
        // First argument is always rayPayload
        curStageVar = declIdMapper.createRayTracingNVStageVar(
            spv::StorageClass::IncomingRayPayloadNV, param);
        currentRayPayload = curStageVar;
      } else {
        // Second argument is always attribute
        curStageVar = declIdMapper.createRayTracingNVStageVar(
            spv::StorageClass::HitAttributeNV, param);
      }
    } else if (sKind == hlsl::ShaderModel::Kind::Miss) {
      // Generate rayPayloadInNV stage variable
      // First and only argument is rayPayload
      curStageVar = declIdMapper.createRayTracingNVStageVar(
          spv::StorageClass::IncomingRayPayloadNV, param);
    } else if (sKind == hlsl::ShaderModel::Kind::Callable) {
      curStageVar = declIdMapper.createRayTracingNVStageVar(
          spv::StorageClass::IncomingCallableDataNV, param);
    }

    if (curStageVar != nullptr) {
      stageVars.push_back(curStageVar);
      // Copy data to temporary
      auto *tempLoadInst =
          spvBuilder.createLoad(paramType, curStageVar, param->getLocation());
      spvBuilder.createStore(tempVar, tempLoadInst, param->getLocation());
    }
  }

  // Call the original entry function
  const QualType retType = decl->getReturnType();
  spvBuilder.createFunctionCall(retType, entryFuncInstr, params,
                                decl->getLocStart());

  // Write certain output variables back
  if (sKind == hlsl::ShaderModel::Kind::ClosestHit ||
      sKind == hlsl::ShaderModel::Kind::AnyHit ||
      sKind == hlsl::ShaderModel::Kind::Miss ||
      sKind == hlsl::ShaderModel::Kind::Callable) {
    // Write back results to IncomingRayPayloadNV/IncomingCallableDataNV
    auto *tempLoad = spvBuilder.createLoad(paramTypes[0], params[0],
                                           decl->getBody()->getLocEnd());
    spvBuilder.createStore(stageVars[0], tempLoad,
                           decl->getBody()->getLocEnd());
  }

  spvBuilder.createReturn(decl->getBody()->getLocEnd());
  spvBuilder.endFunction();

  if (spirvOptions.debugInfoRich && decl->hasBody()) {
    spvContext.popDebugLexicalScope(*info);
  }

  return true;
}

bool SpirvEmitter::processMeshOrAmplificationShaderAttributes(
    const FunctionDecl *decl, uint32_t *outVerticesArraySize) {
  if (auto *numThreadsAttr = decl->getAttr<HLSLNumThreadsAttr>()) {
    uint32_t x, y, z;
    x = static_cast<uint32_t>(numThreadsAttr->getX());
    y = static_cast<uint32_t>(numThreadsAttr->getY());
    z = static_cast<uint32_t>(numThreadsAttr->getZ());
    spvBuilder.addExecutionMode(entryFunction, spv::ExecutionMode::LocalSize,
                                {x, y, z}, decl->getLocation());
  }

  // Early return for amplification shaders as they only take the 'numthreads'
  // attribute.
  if (spvContext.isAS())
    return true;

  spv::ExecutionMode outputPrimitive = spv::ExecutionMode::Max;
  if (auto *outputTopology = decl->getAttr<HLSLOutputTopologyAttr>()) {
    const auto topology = outputTopology->getTopology().lower();
    outputPrimitive =
        llvm::StringSwitch<spv::ExecutionMode>(topology)
            .Case("point", spv::ExecutionMode::OutputPoints)
            .Case("line", spv::ExecutionMode::OutputLinesNV)
            .Case("triangle", spv::ExecutionMode::OutputTrianglesNV);
    if (outputPrimitive != spv::ExecutionMode::Max) {
      spvBuilder.addExecutionMode(entryFunction, outputPrimitive, {},
                                  decl->getLocation());
    } else {
      emitError("unknown output topology in mesh shader",
                outputTopology->getLocation());
      return false;
    }
  }

  uint32_t numVertices = 0;
  uint32_t numIndices = 0;
  uint32_t numPrimitives = 0;
  bool payloadDeclSeen = false;

  for (uint32_t i = 0; i < decl->getNumParams(); i++) {
    const auto param = decl->getParamDecl(i);
    const auto paramType = param->getType();
    const auto paramLoc = param->getLocation();
    if (param->hasAttr<HLSLVerticesAttr>() ||
        param->hasAttr<HLSLIndicesAttr>() ||
        param->hasAttr<HLSLPrimitivesAttr>()) {
      uint32_t arraySize = 0;
      if (const auto *arrayType =
              astContext.getAsConstantArrayType(paramType)) {
        const auto eleType =
            arrayType->getElementType()->getCanonicalTypeUnqualified();
        if (param->hasAttr<HLSLIndicesAttr>()) {
          switch (outputPrimitive) {
          case spv::ExecutionMode::OutputPoints:
            if (eleType != astContext.UnsignedIntTy) {
              emitError("expected 1D array of uint type", paramLoc);
              return false;
            }
            break;
          case spv::ExecutionMode::OutputLinesNV: {
            QualType baseType;
            uint32_t length;
            if (!isVectorType(eleType, &baseType, &length) ||
                baseType != astContext.UnsignedIntTy || length != 2) {
              emitError("expected 1D array of uint2 type", paramLoc);
              return false;
            }
            break;
          }
          case spv::ExecutionMode::OutputTrianglesNV: {
            QualType baseType;
            uint32_t length;
            if (!isVectorType(eleType, &baseType, &length) ||
                baseType != astContext.UnsignedIntTy || length != 3) {
              emitError("expected 1D array of uint3 type", paramLoc);
              return false;
            }
            break;
          }
          default:
            assert(false && "unexpected spirv execution mode");
          }
        } else if (!eleType->isStructureType()) {
          // vertices/primitives objects
          emitError("expected 1D array of struct type", paramLoc);
          return false;
        }
        arraySize = static_cast<uint32_t>(arrayType->getSize().getZExtValue());
      } else {
        emitError("expected 1D array of indices/vertices/primitives object",
                  paramLoc);
        return false;
      }
      if (param->hasAttr<HLSLVerticesAttr>()) {
        if (numVertices != 0) {
          emitError("only one object with 'vertices' modifier is allowed",
                    paramLoc);
          return false;
        }
        numVertices = arraySize;
      } else if (param->hasAttr<HLSLIndicesAttr>()) {
        if (numIndices != 0) {
          emitError("only one object with 'indices' modifier is allowed",
                    paramLoc);
          return false;
        }
        numIndices = arraySize;
      } else if (param->hasAttr<HLSLPrimitivesAttr>()) {
        if (numPrimitives != 0) {
          emitError("only one object with 'primitives' modifier is allowed",
                    paramLoc);
          return false;
        }
        numPrimitives = arraySize;
      }
    } else if (param->hasAttr<HLSLPayloadAttr>()) {
      if (payloadDeclSeen) {
        emitError("only one object with 'payload' modifier is allowed",
                  paramLoc);
        return false;
      }
      payloadDeclSeen = true;
      if (!paramType->isStructureType()) {
        emitError("expected payload of struct type", paramLoc);
        return false;
      }
    }
  }

  // Vertex attribute array is a mandatory param to mesh entry function.
  if (numVertices != 0) {
    *outVerticesArraySize = numVertices;
    spvBuilder.addExecutionMode(
        entryFunction, spv::ExecutionMode::OutputVertices,
        {static_cast<uint32_t>(numVertices)}, decl->getLocation());
  } else {
    emitError("expected vertices object declaration", decl->getLocation());
    return false;
  }

  // Vertex indices array is a mandatory param to mesh entry function.
  if (numIndices != 0) {
    spvBuilder.addExecutionMode(
        entryFunction, spv::ExecutionMode::OutputPrimitivesNV,
        {static_cast<uint32_t>(numIndices)}, decl->getLocation());
    // Primitive attribute array is an optional param to mesh entry function,
    // but the array size should match the indices array.
    if (numPrimitives != 0 && numPrimitives != numIndices) {
      emitError("array size of primitives object should match 'indices' object",
                decl->getLocation());
      return false;
    }
  } else {
    emitError("expected indices object declaration", decl->getLocation());
    return false;
  }

  return true;
}

SpirvDebugFunction *SpirvEmitter::emitDebugFunction(const FunctionDecl *decl,
                                                    SpirvFunction *func,
                                                    RichDebugInfo **info,
                                                    std::string name) {
  auto loc = decl->getLocStart();
  const auto &sm = astContext.getSourceManager();
  const uint32_t line = sm.getPresumedLineNumber(loc);
  const uint32_t column = sm.getPresumedColumnNumber(loc);
  *info = getOrCreateRichDebugInfo(loc);

  SpirvDebugSource *source = (*info)->source;
  // Note that info->scopeStack.back() is a lexical scope of the function
  // caller.
  SpirvDebugInstruction *parentScope = (*info)->compilationUnit;
  // TODO: figure out the proper flag based on the function decl.
  // using FlagIsPublic for now.
  uint32_t flags = 3u;
  // The line number in the source program at which the function scope begins.
  auto scopeLine = sm.getPresumedLineNumber(decl->getBody()->getLocStart());
  SpirvDebugFunction *debugFunction =
      spvBuilder.createDebugFunction(decl, name, source, line, column,
                                     parentScope, "", flags, scopeLine, func);
  func->setDebugScope(new (spvContext) SpirvDebugScope(debugFunction));

  return debugFunction;
}

SpirvFunction *SpirvEmitter::emitEntryFunctionWrapper(
    const FunctionDecl *decl, RichDebugInfo **info,
    SpirvDebugFunction **debugFunction, SpirvFunction *entryFuncInstr) {
  // HS specific attributes
  uint32_t numOutputControlPoints = 0;
  SpirvInstruction *outputControlPointIdVal =
      nullptr;                                // SV_OutputControlPointID value
  SpirvInstruction *primitiveIdVar = nullptr; // SV_PrimitiveID variable
  SpirvInstruction *viewIdVar = nullptr;      // SV_ViewID variable
  SpirvInstruction *hullMainInputPatchParam =
      nullptr; // Temporary parameter for InputPatch<>

  // The array size of per-vertex input/output variables
  // Used by HS/DS/GS for the additional arrayness, zero means not an array.
  uint32_t inputArraySize = 0;
  uint32_t outputArraySize = 0;

  // The wrapper entry function surely does not have pre-assigned <result-id>
  // for it like other functions that got added to the work queue following
  // function calls. And the wrapper is the entry function.
  entryFunction = spvBuilder.beginFunction(
      astContext.VoidTy, decl->getLocStart(), decl->getName());

  if (spirvOptions.debugInfoRich && decl->hasBody()) {
    *debugFunction =
        emitDebugFunction(decl, entryFunction, info, "__dxc_setup");
    spvContext.pushDebugLexicalScope(*info, *debugFunction);
  }

  // Specify that entryFunction is an entry function wrapper.
  entryFunction->setEntryFunctionWrapper();

  // Note this should happen before using declIdMapper for other tasks.
  declIdMapper.setEntryFunction(entryFunction);

  // Set entryFunction for current entry point.
  auto iter = functionInfoMap.find(decl);
  assert(iter != functionInfoMap.end());
  auto &entryInfo = iter->second;
  assert(entryInfo->isEntryFunction);
  entryInfo->entryFunction = entryFunction;

  if (spvContext.isRay()) {
    return emitEntryFunctionWrapperForRayTracing(decl, info, *debugFunction,
                                                 entryFuncInstr)
               ? entryFunction
               : nullptr;
  }
  // Handle attributes specific to each shader stage
  if (spvContext.isPS()) {
    processPixelShaderAttributes(decl);
  } else if (spvContext.isCS()) {
    processComputeShaderAttributes(decl);
  } else if (spvContext.isNode()) {
    processNodeShaderAttributes(decl);
  } else if (spvContext.isHS()) {
    if (!processTessellationShaderAttributes(decl, &numOutputControlPoints))
      return nullptr;

    // The input array size for HS is specified in the InputPatch parameter.
    for (const auto *param : decl->params())
      if (hlsl::IsHLSLInputPatchType(param->getType())) {
        inputArraySize = hlsl::GetHLSLInputPatchCount(param->getType());
        break;
      }

    outputArraySize = numOutputControlPoints;
  } else if (spvContext.isDS()) {
    if (!processTessellationShaderAttributes(decl, &numOutputControlPoints))
      return nullptr;

    // The input array size for HS is specified in the OutputPatch parameter.
    for (const auto *param : decl->params())
      if (hlsl::IsHLSLOutputPatchType(param->getType())) {
        inputArraySize = hlsl::GetHLSLOutputPatchCount(param->getType());
        break;
      }
    // The per-vertex output of DS is not an array.
  } else if (spvContext.isGS()) {
    if (!processGeometryShaderAttributes(decl, &inputArraySize))
      return nullptr;
    // The per-vertex output of GS is not an array.
  } else if (spvContext.isMS() || spvContext.isAS()) {
    if (!processMeshOrAmplificationShaderAttributes(decl, &outputArraySize))
      return nullptr;
  }

  // Go through all parameters and record the declaration of SV_ClipDistance
  // and SV_CullDistance. We need to do this extra step because in HLSL we
  // can declare multiple SV_ClipDistance/SV_CullDistance variables of float
  // or vector of float types, but we can only have one single float array
  // for the ClipDistance/CullDistance builtin. So we need to group all
  // SV_ClipDistance/SV_CullDistance variables into one float array, thus we
  // need to calculate the total size of the array and the offset of each
  // variable within that array.
  // Also go through all parameters to record the semantic strings provided for
  // the builtins in gl_PerVertex.
  for (const auto *param : decl->params()) {
    if (canActAsInParmVar(param))
      if (!declIdMapper.glPerVertex.recordGlPerVertexDeclFacts(param, true))
        return nullptr;
    if (canActAsOutParmVar(param))
      if (!declIdMapper.glPerVertex.recordGlPerVertexDeclFacts(param, false))
        return nullptr;
  }
  // Also consider the SV_ClipDistance/SV_CullDistance in the return type
  if (!declIdMapper.glPerVertex.recordGlPerVertexDeclFacts(decl, false))
    return nullptr;

  // Calculate the total size of the ClipDistance/CullDistance array and the
  // offset of SV_ClipDistance/SV_CullDistance variables within the array.
  declIdMapper.glPerVertex.calculateClipCullDistanceArraySize();

  if (!spvContext.isCS() && !spvContext.isAS()) {
    // Generate stand-alone builtins of Position, ClipDistance, and
    // CullDistance, which belongs to gl_PerVertex.
    declIdMapper.glPerVertex.generateVars(inputArraySize, outputArraySize);
  }

  // The entry basic block.
  auto *entryLabel = spvBuilder.createBasicBlock();
  spvBuilder.setInsertPoint(entryLabel);

  // Handle vk::execution_mode, vk::ext_extension and vk::ext_capability
  // attributes. Uses pseudo-instructions for extensions and capabilities, which
  // are added to the beginning of the entry basic block, so must be called
  // after the basic block is created and insert point is set.
  processInlineSpirvAttributes(decl);

  // Add DebugFunctionDefinition if we are emitting
  // NonSemantic.Shader.DebugInfo.100 debug info.
  if (spirvOptions.debugInfoVulkan && *debugFunction)
    spvBuilder.createDebugFunctionDef(*debugFunction, entryFunction);

  // Initialize all global variables at the beginning of the wrapper
  for (const VarDecl *varDecl : toInitGloalVars) {
    // SPIR-V does not have string variables
    if (isStringType(varDecl->getType()))
      continue;

    const auto varInfo =
        declIdMapper.getDeclEvalInfo(varDecl, varDecl->getLocation());
    if (const auto *init = varDecl->getInit()) {
      parentMap = std::make_unique<ParentMap>(const_cast<Expr *>(init));
      storeValue(varInfo, loadIfGLValue(init), varDecl->getType(),
                 init->getLocStart());
      parentMap.reset(nullptr);

      // Update counter variable associated with global variables
      tryToAssignCounterVar(varDecl, init);
    }
    // If not explicitly initialized, initialize with their zero values if not
    // resource objects
    else if (!hlsl::IsHLSLResourceType(varDecl->getType())) {
      auto *nullValue = spvBuilder.getConstantNull(varDecl->getType());
      spvBuilder.createStore(varInfo, nullValue, varDecl->getLocation());
    }
  }

  // Create temporary variables for holding function call arguments
  llvm::SmallVector<SpirvInstruction *, 4> params;
  for (const auto *param : decl->params()) {
    const auto paramType = param->getType();
    if (hlsl::IsHLSLNodeInputType(paramType)) {
      SpirvInstruction *value = nullptr;
      if (!declIdMapper.createStageInputVar(param, &value, false))
        return nullptr;
      if (value && value->getKind() == SpirvInstruction::Kind::IK_Variable) {
        handleNodePayloadArrayType(param, value);
        params.push_back(value);
      }
      continue;
    }

    std::string tempVarName = "param.var." + param->getNameAsString();
    auto *tempVar =
        spvBuilder.addFnVar(paramType, param->getLocation(), tempVarName,
                            param->hasAttr<HLSLPreciseAttr>(),
                            param->hasAttr<HLSLNoInterpolationAttr>());
    handleNodePayloadArrayType(param, tempVar);
    params.push_back(tempVar);

    // Create the stage input variable for parameter not marked as pure out and
    // initialize the corresponding temporary variable
    // Also do not create input variables for output stream objects of geometry
    // shaders (e.g. TriangleStream) which are required to be marked as 'inout'.
    if (canActAsInParmVar(param)) {
      if (spvContext.isHS() && hlsl::IsHLSLInputPatchType(paramType)) {
        // Record the temporary variable holding InputPatch. It may be used
        // later in the patch constant function.
        hullMainInputPatchParam = tempVar;
      }

      SpirvInstruction *loadedValue = nullptr;

      if (!declIdMapper.createStageInputVar(param, &loadedValue, false))
        return nullptr;
      if (loadedValue) {
        handleNodePayloadArrayType(param, loadedValue);
      }

      // Only initialize the temporary variable if the parameter is indeed used,
      // or if it is an inout parameter.
      if (param->isUsed() || param->hasAttr<HLSLInOutAttr>()) {
        spvBuilder.createStore(tempVar, loadedValue, param->getLocation());
        // param is mapped to store object. Coule be a componentConstruct or
        // Load
        if (spvContext.isPS())
          spvBuilder.addPerVertexStgInputFuncVarEntry(loadedValue, tempVar);
      }

      // Record the temporary variable holding SV_OutputControlPointID,
      // SV_PrimitiveID, and SV_ViewID. It may be used later in the patch
      // constant function.
      if (hasSemantic(param, hlsl::DXIL::SemanticKind::OutputControlPointID))
        outputControlPointIdVal = loadedValue;
      else if (hasSemantic(param, hlsl::DXIL::SemanticKind::PrimitiveID))
        primitiveIdVar = tempVar;
      else if (hasSemantic(param, hlsl::DXIL::SemanticKind::ViewID))
        viewIdVar = tempVar;
    }
  }

  // Call the original entry function
  const QualType retType = decl->getReturnType();
  auto *retVal = spvBuilder.createFunctionCall(retType, entryFuncInstr, params,
                                               decl->getLocStart());

  // Create and write stage output variables for return value. Special case for
  // Hull shaders since they operate differently in 2 ways:
  // 1- Their return value is in fact an array and each invocation should write
  //    to the proper offset in the array.
  // 2- The patch constant function must be called *once* after all invocations
  //    of the main entry point function is done.
  if (spvContext.isHS()) {
    // Create stage output variables out of the return type.
    if (!declIdMapper.createStageOutputVar(decl, numOutputControlPoints,
                                           outputControlPointIdVal, retVal))
      return nullptr;
    if (!processHSEntryPointOutputAndPCF(
            decl, retType, retVal, numOutputControlPoints,
            outputControlPointIdVal, primitiveIdVar, viewIdVar,
            hullMainInputPatchParam))
      return nullptr;
  } else {
    if (!declIdMapper.createStageOutputVar(decl, retVal, /*forPCF*/ false))
      return nullptr;
  }

  // Create and write stage output variables for parameters marked as
  // out/inout
  for (uint32_t i = 0; i < decl->getNumParams(); ++i) {
    const auto *param = decl->getParamDecl(i);
    if (canActAsOutParmVar(param)) {
      // Load the value from the parameter after function call
      SpirvInstruction *loadedParam = nullptr;

      // No need to write back the value if the parameter is not used at all in
      // the original entry function, unless it is an inout paramter.
      //
      // Write back of stage output variables in GS is manually controlled by
      // .Append() intrinsic method. No need to load the parameter since we
      // won't need to write back here.
      if ((param->isUsed() || param->hasAttr<HLSLInOutAttr>()) &&
          !spvContext.isGS())
        loadedParam = spvBuilder.createLoad(param->getType(), params[i],
                                            param->getLocStart());

      if (!declIdMapper.createStageOutputVar(param, loadedParam, false))
        return nullptr;
    }
  }

  // To prevent spirv-opt from removing all debug info, we emit at least
  // a single OpLine to specify the end of the shader. This SourceLocation
  // will provide the information.
  spvBuilder.createReturn(decl->getLocEnd());
  spvBuilder.endFunction();

  // For Hull shaders, there is no explicit call to the PCF in the HLSL source.
  // We should invoke a translation of the PCF manually.
  if (spvContext.isHS())
    doDecl(patchConstFunc);

  if (spirvOptions.debugInfoRich && decl->hasBody()) {
    spvContext.popDebugLexicalScope(*info);
  }

  return entryFunction;
}

bool SpirvEmitter::processHSEntryPointOutputAndPCF(
    const FunctionDecl *hullMainFuncDecl, QualType retType,
    SpirvInstruction *retVal, uint32_t numOutputControlPoints,
    SpirvInstruction *outputControlPointId, SpirvInstruction *primitiveId,
    SpirvInstruction *viewId, SpirvInstruction *hullMainInputPatch) {
  // This method may only be called for Hull shaders.
  assert(spvContext.isHS());

  auto loc = hullMainFuncDecl->getLocation();
  auto locEnd = hullMainFuncDecl->getLocEnd();

  // For Hull shaders, the real output is an array of size
  // numOutputControlPoints. The results of the main should be written to the
  // correct offset in the array (based on InvocationID).
  if (!numOutputControlPoints) {
    emitError("number of output control points cannot be zero", loc);
    return false;
  }
  // TODO: We should be able to handle cases where the SV_OutputControlPointID
  // is not provided.
  if (!outputControlPointId) {
    emitError(
        "SV_OutputControlPointID semantic must be provided in hull shader",
        loc);
    return false;
  }
  if (!patchConstFunc) {
    emitError("patch constant function not defined in hull shader", loc);
    return false;
  }

  // Now create a barrier before calling the Patch Constant Function (PCF).
  // Flags are:
  // Execution Barrier scope = Workgroup (2)
  // Memory Barrier scope = Invocation (4)
  // Memory Semantics Barrier scope = None (0)
  spvBuilder.createBarrier(spv::Scope::Invocation,
                           spv::MemorySemanticsMask::MaskNone,
                           spv::Scope::Workgroup, {});

  SpirvInstruction *hullMainOutputPatch = nullptr;
  // If the patch constant function (PCF) takes the result of the Hull main
  // entry point, create a temporary function-scope variable and write the
  // results to it, so it can be passed to the PCF.
  if (const ParmVarDecl *outputPatchDecl =
          patchConstFuncTakesHullOutputPatch(patchConstFunc)) {
    const QualType hullMainRetType = astContext.getConstantArrayType(
        retType, llvm::APInt(32, numOutputControlPoints),
        clang::ArrayType::Normal, 0);
    hullMainOutputPatch =
        spvBuilder.addFnVar(hullMainRetType, locEnd, "temp.var.hullMainRetVal");
    declIdMapper.copyHullOutStageVarsToOutputPatch(
        hullMainOutputPatch, outputPatchDecl, retType, numOutputControlPoints);
  }

  // The PCF should be called only once. Therefore, we check the invocationID,
  // and we only allow ID 0 to call the PCF.
  auto *condition = spvBuilder.createBinaryOp(
      spv::Op::OpIEqual, astContext.BoolTy, outputControlPointId,
      spvBuilder.getConstantInt(astContext.UnsignedIntTy, llvm::APInt(32, 0)),
      loc);
  auto *thenBB = spvBuilder.createBasicBlock("if.true");
  auto *mergeBB = spvBuilder.createBasicBlock("if.merge");
  spvBuilder.createConditionalBranch(condition, thenBB, mergeBB, loc, mergeBB);
  spvBuilder.addSuccessor(thenBB);
  spvBuilder.addSuccessor(mergeBB);
  spvBuilder.setMergeTarget(mergeBB);

  spvBuilder.setInsertPoint(thenBB);

  // Call the PCF. Since the function is not explicitly called, we must first
  // register an ID for it.
  SpirvFunction *pcfId = declIdMapper.getOrRegisterFn(patchConstFunc);
  const QualType pcfRetType = patchConstFunc->getReturnType();

  std::vector<SpirvInstruction *> pcfParams;
  for (const auto *param : patchConstFunc->parameters()) {
    // Note: According to the HLSL reference, the PCF takes an InputPatch of
    // ControlPoints as well as the PatchID (PrimitiveID). This does not
    // necessarily mean that they are present. There is also no requirement
    // for the order of parameters passed to PCF.
    if (hlsl::IsHLSLInputPatchType(param->getType())) {
      pcfParams.push_back(hullMainInputPatch);
    } else if (hlsl::IsHLSLOutputPatchType(param->getType())) {
      pcfParams.push_back(hullMainOutputPatch);
    } else if (hasSemantic(param, hlsl::DXIL::SemanticKind::PrimitiveID)) {
      if (!primitiveId) {
        primitiveId = createPCFParmVarAndInitFromStageInputVar(param);
      }
      pcfParams.push_back(primitiveId);
    } else if (hasSemantic(param, hlsl::DXIL::SemanticKind::ViewID)) {
      if (!viewId) {
        viewId = createPCFParmVarAndInitFromStageInputVar(param);
      }
      pcfParams.push_back(viewId);
    } else if (param->hasAttr<HLSLOutAttr>()) {
      // Create a temporary function scope variable to pass to the PCF function
      // for the output. The value of this variable should be copied to an
      // output variable for the param after the function call.
      pcfParams.push_back(createFunctionScopeTempFromParameter(param));
    } else {
      emitError("patch constant function parameter '%0' unknown",
                param->getLocation())
          << param->getName();
    }
  }
  auto *pcfResultId = spvBuilder.createFunctionCall(
      pcfRetType, pcfId, {pcfParams}, hullMainFuncDecl->getLocStart());
  if (!declIdMapper.createStageOutputVar(patchConstFunc, pcfResultId,
                                         /*forPCF*/ true))
    return false;

  // Traverse all of the parameters for the patch constant function and copy out
  // all of the output variables.
  for (uint32_t idx = 0; idx < patchConstFunc->parameters().size(); idx++) {
    const auto *param = patchConstFunc->parameters()[idx];
    if (param->hasAttr<HLSLOutAttr>()) {
      SpirvInstruction *pcfParam = pcfParams[idx];
      SpirvInstruction *loadedValue = spvBuilder.createLoad(
          pcfParam->getAstResultType(), pcfParam, param->getLocation());
      declIdMapper.createStageOutputVar(param, loadedValue, /*forPCF*/ true);
    }
  }

  spvBuilder.createBranch(mergeBB, locEnd);
  spvBuilder.addSuccessor(mergeBB);
  spvBuilder.setInsertPoint(mergeBB);
  return true;
}

bool SpirvEmitter::allSwitchCasesAreIntegerLiterals(const Stmt *root) {
  if (!root)
    return false;

  const auto *caseStmt = dyn_cast<CaseStmt>(root);
  const auto *compoundStmt = dyn_cast<CompoundStmt>(root);
  if (!caseStmt && !compoundStmt)
    return true;

  if (caseStmt) {
    const Expr *caseExpr = caseStmt->getLHS();
    return caseExpr && caseExpr->isEvaluatable(astContext);
  }

  // Recurse down if facing a compound statement.
  for (auto *st : compoundStmt->body())
    if (!allSwitchCasesAreIntegerLiterals(st))
      return false;

  return true;
}

void SpirvEmitter::discoverAllCaseStmtInSwitchStmt(
    const Stmt *root, SpirvBasicBlock **defaultBB,
    std::vector<std::pair<llvm::APInt, SpirvBasicBlock *>> *targets) {
  if (!root)
    return;

  // A switch case can only appear in DefaultStmt, CaseStmt, or
  // CompoundStmt. For the rest, we can just return.
  const auto *defaultStmt = dyn_cast<DefaultStmt>(root);
  const auto *caseStmt = dyn_cast<CaseStmt>(root);
  const auto *compoundStmt = dyn_cast<CompoundStmt>(root);
  if (!defaultStmt && !caseStmt && !compoundStmt)
    return;

  // Recurse down if facing a compound statement.
  if (compoundStmt) {
    for (auto *st : compoundStmt->body())
      discoverAllCaseStmtInSwitchStmt(st, defaultBB, targets);
    return;
  }

  std::string caseLabel;
  llvm::APInt caseValue;
  if (defaultStmt) {
    // This is the default branch.
    caseLabel = "switch.default";
  } else if (caseStmt) {
    // This is a non-default case.
    // When using OpSwitch, we only allow integer literal cases. e.g:
    // case <literal_integer>: {...; break;}
    const Expr *caseExpr = caseStmt->getLHS();
    assert(caseExpr && caseExpr->isEvaluatable(astContext));
    Expr::EvalResult evalResult;
    caseExpr->EvaluateAsRValue(evalResult, astContext);
    caseValue = evalResult.Val.getInt();
    const int64_t value = caseValue.getSExtValue();
    caseLabel = "switch." + std::string(value < 0 ? "n" : "") +
                llvm::itostr(std::abs(value));
  }
  auto *caseBB = spvBuilder.createBasicBlock(caseLabel);
  spvBuilder.addSuccessor(caseBB);
  stmtBasicBlock[root] = caseBB;

  // Add all cases to the 'targets' vector.
  if (caseStmt)
    targets->emplace_back(caseValue, caseBB);

  // The default label is not part of the 'targets' vector that is passed
  // to the OpSwitch instruction.
  // If default statement was discovered, return its label via defaultBB.
  if (defaultStmt)
    *defaultBB = caseBB;

  // Process cases nested in other cases. It happens when we have fall through
  // cases. For example:
  // case 1: case 2: ...; break;
  // will result in the CaseSmt for case 2 nested in the one for case 1.
  discoverAllCaseStmtInSwitchStmt(caseStmt ? caseStmt->getSubStmt()
                                           : defaultStmt->getSubStmt(),
                                  defaultBB, targets);
}

void SpirvEmitter::flattenSwitchStmtAST(const Stmt *root,
                                        std::vector<const Stmt *> *flatSwitch) {
  const auto *caseStmt = dyn_cast<CaseStmt>(root);
  const auto *compoundStmt = dyn_cast<CompoundStmt>(root);
  const auto *defaultStmt = dyn_cast<DefaultStmt>(root);

  if (!compoundStmt) {
    flatSwitch->push_back(root);
  }

  if (compoundStmt) {
    for (const auto *st : compoundStmt->body())
      flattenSwitchStmtAST(st, flatSwitch);
  } else if (caseStmt) {
    flattenSwitchStmtAST(caseStmt->getSubStmt(), flatSwitch);
  } else if (defaultStmt) {
    flattenSwitchStmtAST(defaultStmt->getSubStmt(), flatSwitch);
  }
}

void SpirvEmitter::processCaseStmtOrDefaultStmt(const Stmt *stmt) {
  auto *caseStmt = dyn_cast<CaseStmt>(stmt);
  auto *defaultStmt = dyn_cast<DefaultStmt>(stmt);
  assert(caseStmt || defaultStmt);

  auto *caseBB = stmtBasicBlock[stmt];
  if (!spvBuilder.isCurrentBasicBlockTerminated()) {
    // We are about to handle the case passed in as parameter. If the current
    // basic block is not terminated, it means the previous case is a fall
    // through case. We need to link it to the case to be processed.
    spvBuilder.createBranch(caseBB, stmt->getLocStart());
    spvBuilder.addSuccessor(caseBB);
  }
  spvBuilder.setInsertPoint(caseBB);
  doStmt(caseStmt ? caseStmt->getSubStmt() : defaultStmt->getSubStmt());
}

void SpirvEmitter::processSwitchStmtUsingSpirvOpSwitch(
    const SwitchStmt *switchStmt) {
  const SourceLocation srcLoc = switchStmt->getSwitchLoc();

  // First handle the condition variable DeclStmt if one exists.
  // For example: handle 'int a = b' in the following:
  // switch (int a = b) {...}
  if (const auto *condVarDeclStmt = switchStmt->getConditionVariableDeclStmt())
    doDeclStmt(condVarDeclStmt);

  auto *cond = switchStmt->getCond();
  auto *selector = doExpr(cond);

  // We need a merge block regardless of the number of switch cases.
  // Since OpSwitch always requires a default label, if the switch statement
  // does not have a default branch, we use the merge block as the default
  // target.
  auto *mergeBB = spvBuilder.createBasicBlock("switch.merge");
  spvBuilder.setMergeTarget(mergeBB);
  breakStack.push(mergeBB);
  auto *defaultBB = mergeBB;

  // (literal, labelId) pairs to pass to the OpSwitch instruction.
  std::vector<std::pair<llvm::APInt, SpirvBasicBlock *>> targets;
  discoverAllCaseStmtInSwitchStmt(switchStmt->getBody(), &defaultBB, &targets);

  // Create the OpSelectionMerge and OpSwitch.
  spvBuilder.createSwitch(mergeBB, selector, defaultBB, targets, srcLoc,
                          cond->getSourceRange());

  // Handle the switch body.
  doStmt(switchStmt->getBody());

  if (!spvBuilder.isCurrentBasicBlockTerminated())
    spvBuilder.createBranch(mergeBB, switchStmt->getLocEnd());
  spvBuilder.setInsertPoint(mergeBB);
  breakStack.pop();
}

void SpirvEmitter::processSwitchStmtUsingIfStmts(const SwitchStmt *switchStmt) {
  std::vector<const Stmt *> flatSwitch;
  flattenSwitchStmtAST(switchStmt->getBody(), &flatSwitch);

  // First handle the condition variable DeclStmt if one exists.
  // For example: handle 'int a = b' in the following:
  // switch (int a = b) {...}
  if (const auto *condVarDeclStmt = switchStmt->getConditionVariableDeclStmt())
    doDeclStmt(condVarDeclStmt);

  // Figure out the indexes of CaseStmts (and DefaultStmt if it exists) in
  // the flattened switch AST.
  // For instance, for the following flat vector:
  // +-----+-----+-----+-----+-----+-----+-----+-----+-----+-------+-----+
  // |Case1|Stmt1|Case2|Stmt2|Break|Case3|Case4|Stmt4|Break|Default|Stmt5|
  // +-----+-----+-----+-----+-----+-----+-----+-----+-----+-------+-----+
  // The indexes are: {0, 2, 5, 6, 9}
  std::vector<uint32_t> caseStmtLocs;
  for (uint32_t i = 0; i < flatSwitch.size(); ++i)
    if (isa<CaseStmt>(flatSwitch[i]) || isa<DefaultStmt>(flatSwitch[i]))
      caseStmtLocs.push_back(i);

  IfStmt *prevIfStmt = nullptr;
  IfStmt *rootIfStmt = nullptr;
  CompoundStmt *defaultBody = nullptr;

  // For each case, start at its index in the vector, and go forward
  // accumulating statements until BreakStmt or end of vector is reached.
  for (auto curCaseIndex : caseStmtLocs) {
    const Stmt *curCase = flatSwitch[curCaseIndex];

    // CompoundStmt to hold all statements for this case.
    CompoundStmt *cs = new (astContext) CompoundStmt(Stmt::EmptyShell());

    // Accumulate all non-case/default/break statements as the body for the
    // current case.
    std::vector<Stmt *> statements;
    unsigned i = curCaseIndex + 1;
    for (; i < flatSwitch.size() && !isa<BreakStmt>(flatSwitch[i]); ++i) {
      if (!isa<CaseStmt>(flatSwitch[i]) && !isa<DefaultStmt>(flatSwitch[i]))
        statements.push_back(const_cast<Stmt *>(flatSwitch[i]));
      if (isa<ReturnStmt>(flatSwitch[i]))
        break;
    }
    if (!statements.empty())
      cs->setStmts(astContext, statements.data(), statements.size());

    SourceLocation mergeLoc =
        (i < flatSwitch.size() && isa<BreakStmt>(flatSwitch[i]))
            ? flatSwitch[i]->getLocStart()
            : SourceLocation();

    // For non-default cases, generate the IfStmt that compares the switch
    // value to the case value.
    if (auto *caseStmt = dyn_cast<CaseStmt>(curCase)) {
      IfStmt *curIf = new (astContext) IfStmt(Stmt::EmptyShell());
      BinaryOperator *bo = new (astContext) BinaryOperator(Stmt::EmptyShell());
      // Expr *tmp_cond = new (astContext) Expr(*switchStmt->getCond());
      bo->setLHS(const_cast<Expr *>(switchStmt->getCond()));
      bo->setRHS(const_cast<Expr *>(caseStmt->getLHS()));
      bo->setOpcode(BO_EQ);
      bo->setType(astContext.getLogicalOperationType());
      curIf->setCond(bo);
      curIf->setThen(cs);
      curIf->setMergeLoc(mergeLoc);
      curIf->setIfLoc(prevIfStmt ? SourceLocation() : caseStmt->getCaseLoc());
      // No conditional variable associated with this faux if statement.
      curIf->setConditionVariable(astContext, nullptr);
      // Each If statement is the "else" of the previous if statement.
      if (prevIfStmt) {
        prevIfStmt->setElse(curIf);
        prevIfStmt->setElseLoc(caseStmt->getCaseLoc());
      } else
        rootIfStmt = curIf;
      prevIfStmt = curIf;
    } else {
      // Record the DefaultStmt body as it will be used as the body of the
      // "else" block in the if-elseif-...-else pattern.
      defaultBody = cs;
    }
  }

  // If a default case exists, it is the "else" of the last if statement.
  if (prevIfStmt)
    prevIfStmt->setElse(defaultBody);

  // Since all else-if and else statements are the child nodes of the first
  // IfStmt, we only need to call doStmt for the first IfStmt.
  if (rootIfStmt)
    doStmt(rootIfStmt);
  // If there are no CaseStmt and there is only 1 DefaultStmt, there will be
  // no if statements. The switch in that case only executes the body of the
  // default case.
  else if (defaultBody)
    doStmt(defaultBody);
}

SpirvInstruction *SpirvEmitter::extractVecFromVec4(SpirvInstruction *from,
                                                   uint32_t targetVecSize,
                                                   QualType targetElemType,
                                                   SourceLocation loc,
                                                   SourceRange range) {
  assert(targetVecSize > 0 && targetVecSize < 5);
  const QualType retType =
      targetVecSize == 1
          ? targetElemType
          : astContext.getExtVectorType(targetElemType, targetVecSize);
  switch (targetVecSize) {
  case 1:
    return spvBuilder.createCompositeExtract(retType, from, {0}, loc, range);
    break;
  case 2:
    return spvBuilder.createVectorShuffle(retType, from, from, {0, 1}, loc,
                                          range);
    break;
  case 3:
    return spvBuilder.createVectorShuffle(retType, from, from, {0, 1, 2}, loc,
                                          range);
    break;
  case 4:
    return from;
  default:
    llvm_unreachable("vector element count must be 1, 2, 3, or 4");
  }
}

void SpirvEmitter::addFunctionToWorkQueue(hlsl::DXIL::ShaderKind shaderKind,
                                          const clang::FunctionDecl *fnDecl,
                                          bool isEntryFunction) {
  // Only update the workQueue and the function info map if the given
  // FunctionDecl hasn't been added already.
  if (functionInfoMap.find(fnDecl) == functionInfoMap.end()) {
    // Note: The function is just discovered and is being added to the
    // workQueue, therefore it does not have the entryFunction SPIR-V
    // instruction yet (use nullptr).
    auto *fnInfo = new (spvContext) FunctionInfo(
        shaderKind, fnDecl, /*entryFunction*/ nullptr, isEntryFunction);
    functionInfoMap[fnDecl] = fnInfo;
    workQueue.push_back(fnInfo);
  }
}

SpirvInstruction *
SpirvEmitter::processTraceRayInline(const CXXMemberCallExpr *expr) {
  const auto object = expr->getImplicitObjectArgument();
  uint32_t templateFlags = hlsl::GetHLSLResourceTemplateUInt(object->getType());
  const auto constFlags = spvBuilder.getConstantInt(
      astContext.UnsignedIntTy, llvm::APInt(32, templateFlags));

  SpirvInstruction *rayqueryObj = loadIfAliasVarRef(object);

  const auto args = expr->getArgs();

  if (expr->getNumArgs() != 4) {
    emitError("invalid number of arguments to RayQueryInitialize",
              expr->getExprLoc());
  }

  // HLSL Func
  // void RayQuery::TraceRayInline(
  //      RaytracingAccelerationStructure AccelerationStructure,
  //      uint RayFlags,
  //      uint InstanceInclusionMask,
  //      RayDesc Ray);

  // void OpRayQueryInitializeKHR ( <id> RayQuery,
  //                               <id> Acceleration Structure
  //                               <id> RayFlags
  //                               <id> CullMask
  //                               <id> RayOrigin
  //                               <id> RayTmin
  //                               <id> RayDirection
  //                               <id> Ray Tmax)

  const auto accelStructure = doExpr(args[0]);
  SpirvInstruction *rayFlags = nullptr;

  if ((rayFlags =
           constEvaluator.tryToEvaluateAsConst(args[1], isSpecConstantMode))) {
    rayFlags->setRValue();
  } else {
    rayFlags = doExpr(args[1]);
  }

  if (auto constFlags = dyn_cast<SpirvConstantInteger>(rayFlags)) {
    auto interRayFlags = constFlags->getValue().getZExtValue();
    templateFlags |= interRayFlags;
  }

  bool hasCullFlags =
      templateFlags & (uint32_t(hlsl::DXIL::RayFlag::SkipTriangles) |
                       uint32_t(hlsl::DXIL::RayFlag::SkipProceduralPrimitives));

  auto loc = args[1]->getLocStart();
  rayFlags =
      spvBuilder.createBinaryOp(spv::Op::OpBitwiseOr, astContext.UnsignedIntTy,
                                constFlags, rayFlags, loc);
  const auto cullMask = doExpr(args[2]);

  // Extract the ray description to match SPIR-V
  const auto floatType = astContext.FloatTy;
  const auto vecType = astContext.getExtVectorType(astContext.FloatTy, 3);
  SpirvInstruction *rayDescArg = doExpr(args[3]);
  loc = args[3]->getLocStart();
  const auto origin =
      spvBuilder.createCompositeExtract(vecType, rayDescArg, {0}, loc);
  const auto tMin =
      spvBuilder.createCompositeExtract(floatType, rayDescArg, {1}, loc);
  const auto direction =
      spvBuilder.createCompositeExtract(vecType, rayDescArg, {2}, loc);
  const auto tMax =
      spvBuilder.createCompositeExtract(floatType, rayDescArg, {3}, loc);

  llvm::SmallVector<SpirvInstruction *, 8> traceArgs = {
      rayqueryObj, accelStructure, rayFlags,  cullMask,
      origin,      tMin,           direction, tMax};

  return spvBuilder.createRayQueryOpsKHR(spv::Op::OpRayQueryInitializeKHR,
                                         QualType(), traceArgs, hasCullFlags,
                                         expr->getExprLoc());
}

SpirvInstruction *
SpirvEmitter::processRayQueryIntrinsics(const CXXMemberCallExpr *expr,
                                        hlsl::IntrinsicOp opcode) {
  const auto object = expr->getImplicitObjectArgument();
  SpirvInstruction *rayqueryObj = loadIfAliasVarRef(object);

  const auto args = expr->getArgs();

  llvm::SmallVector<SpirvInstruction *, 8> traceArgs;
  traceArgs.push_back(rayqueryObj);

  for (uint32_t i = 0; i < expr->getNumArgs(); ++i) {
    traceArgs.push_back(doExpr(args[i]));
  }

  spv::Op spvCode = spv::Op::Max;
  QualType exprType = expr->getType();

  exprType = exprType->isVoidType() ? QualType() : exprType;

  const auto candidateIntersection =
      spvBuilder.getConstantInt(astContext.UnsignedIntTy, llvm::APInt(32, 0));
  const auto committedIntersection =
      spvBuilder.getConstantInt(astContext.UnsignedIntTy, llvm::APInt(32, 1));

  bool transposeMatrix = false;
  bool logicalNot = false;

  using namespace hlsl;
  switch (opcode) {
  case IntrinsicOp::MOP_Proceed:
    spvCode = spv::Op::OpRayQueryProceedKHR;
    break;
  case IntrinsicOp::MOP_Abort:
    spvCode = spv::Op::OpRayQueryTerminateKHR;
    exprType = QualType();
    break;
  case IntrinsicOp::MOP_CandidateGeometryIndex:
    traceArgs.push_back(candidateIntersection);
    spvCode = spv::Op::OpRayQueryGetIntersectionGeometryIndexKHR;
    break;
  case IntrinsicOp::MOP_CandidateInstanceContributionToHitGroupIndex:
    traceArgs.push_back(candidateIntersection);
    spvCode = spv::Op::
        OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR;
    break;
  case IntrinsicOp::MOP_CandidateInstanceID:
    traceArgs.push_back(candidateIntersection);
    spvCode = spv::Op::OpRayQueryGetIntersectionInstanceCustomIndexKHR;
    break;
  case IntrinsicOp::MOP_CandidateInstanceIndex:
    traceArgs.push_back(candidateIntersection);
    spvCode = spv::Op::OpRayQueryGetIntersectionInstanceIdKHR;
    break;
  case IntrinsicOp::MOP_CandidateObjectRayDirection:
    traceArgs.push_back(candidateIntersection);
    spvCode = spv::Op::OpRayQueryGetIntersectionObjectRayDirectionKHR;
    break;
  case IntrinsicOp::MOP_CandidateObjectRayOrigin:
    traceArgs.push_back(candidateIntersection);
    spvCode = spv::Op::OpRayQueryGetIntersectionObjectRayOriginKHR;
    break;
  case IntrinsicOp::MOP_CandidateObjectToWorld3x4:
    spvCode = spv::Op::OpRayQueryGetIntersectionObjectToWorldKHR;
    traceArgs.push_back(candidateIntersection);
    transposeMatrix = true;
    break;
  case IntrinsicOp::MOP_CandidateObjectToWorld4x3:
    spvCode = spv::Op::OpRayQueryGetIntersectionObjectToWorldKHR;
    traceArgs.push_back(candidateIntersection);
    break;
  case IntrinsicOp::MOP_CandidatePrimitiveIndex:
    traceArgs.push_back(candidateIntersection);
    spvCode = spv::Op::OpRayQueryGetIntersectionPrimitiveIndexKHR;
    break;
  case IntrinsicOp::MOP_CandidateProceduralPrimitiveNonOpaque:
    spvCode = spv::Op::OpRayQueryGetIntersectionCandidateAABBOpaqueKHR;
    logicalNot = true;
    break;
  case IntrinsicOp::MOP_CandidateTriangleBarycentrics:
    traceArgs.push_back(candidateIntersection);
    spvCode = spv::Op::OpRayQueryGetIntersectionBarycentricsKHR;
    break;
  case IntrinsicOp::MOP_CandidateTriangleFrontFace:
    traceArgs.push_back(candidateIntersection);
    spvCode = spv::Op::OpRayQueryGetIntersectionFrontFaceKHR;
    break;
  case IntrinsicOp::MOP_CandidateTriangleRayT:
    traceArgs.push_back(candidateIntersection);
    spvCode = spv::Op::OpRayQueryGetIntersectionTKHR;
    break;
  case IntrinsicOp::MOP_CandidateType:
    spvCode = spv::Op::OpRayQueryGetIntersectionTypeKHR;
    traceArgs.push_back(candidateIntersection);
    break;
  case IntrinsicOp::MOP_CandidateWorldToObject4x3:
    spvCode = spv::Op::OpRayQueryGetIntersectionWorldToObjectKHR;
    traceArgs.push_back(candidateIntersection);
    break;
  case IntrinsicOp::MOP_CandidateWorldToObject3x4:
    spvCode = spv::Op::OpRayQueryGetIntersectionWorldToObjectKHR;
    traceArgs.push_back(candidateIntersection);
    transposeMatrix = true;
    break;
  case IntrinsicOp::MOP_CommitNonOpaqueTriangleHit:
    spvCode = spv::Op::OpRayQueryConfirmIntersectionKHR;
    exprType = QualType();
    break;
  case IntrinsicOp::MOP_CommitProceduralPrimitiveHit:
    spvCode = spv::Op::OpRayQueryGenerateIntersectionKHR;
    exprType = QualType();
    break;
  case IntrinsicOp::MOP_CommittedGeometryIndex:
    spvCode = spv::Op::OpRayQueryGetIntersectionGeometryIndexKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedInstanceContributionToHitGroupIndex:
    spvCode = spv::Op::
        OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedInstanceID:
    spvCode = spv::Op::OpRayQueryGetIntersectionInstanceCustomIndexKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedInstanceIndex:
    spvCode = spv::Op::OpRayQueryGetIntersectionInstanceIdKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedObjectRayDirection:
    spvCode = spv::Op::OpRayQueryGetIntersectionObjectRayDirectionKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedObjectRayOrigin:
    spvCode = spv::Op::OpRayQueryGetIntersectionObjectRayOriginKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedObjectToWorld3x4:
    spvCode = spv::Op::OpRayQueryGetIntersectionObjectToWorldKHR;
    traceArgs.push_back(committedIntersection);
    transposeMatrix = true;
    break;
  case IntrinsicOp::MOP_CommittedObjectToWorld4x3:
    spvCode = spv::Op::OpRayQueryGetIntersectionObjectToWorldKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedPrimitiveIndex:
    spvCode = spv::Op::OpRayQueryGetIntersectionPrimitiveIndexKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedRayT:
    spvCode = spv::Op::OpRayQueryGetIntersectionTKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedStatus:
    spvCode = spv::Op::OpRayQueryGetIntersectionTypeKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedTriangleBarycentrics:
    spvCode = spv::Op::OpRayQueryGetIntersectionBarycentricsKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedTriangleFrontFace:
    spvCode = spv::Op::OpRayQueryGetIntersectionFrontFaceKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_CommittedWorldToObject3x4:
    spvCode = spv::Op::OpRayQueryGetIntersectionWorldToObjectKHR;
    traceArgs.push_back(committedIntersection);
    transposeMatrix = true;
    break;
  case IntrinsicOp::MOP_CommittedWorldToObject4x3:
    spvCode = spv::Op::OpRayQueryGetIntersectionWorldToObjectKHR;
    traceArgs.push_back(committedIntersection);
    break;
  case IntrinsicOp::MOP_RayFlags:
    spvCode = spv::Op::OpRayQueryGetRayFlagsKHR;
    break;
  case IntrinsicOp::MOP_RayTMin:
    spvCode = spv::Op::OpRayQueryGetRayTMinKHR;
    break;
  case IntrinsicOp::MOP_WorldRayDirection:
    spvCode = spv::Op::OpRayQueryGetWorldRayDirectionKHR;
    break;
  case IntrinsicOp::MOP_WorldRayOrigin:
    spvCode = spv::Op::OpRayQueryGetWorldRayOriginKHR;
    break;
  default:
    emitError("intrinsic '%0' method unimplemented",
              expr->getCallee()->getExprLoc())
        << getFunctionOrOperatorName(expr->getDirectCallee(), true);
    return nullptr;
  }

  if (transposeMatrix) {
    assert(hlsl::IsHLSLMatType(exprType) && "intrinsic should be matrix");
    const clang::Type *type = exprType.getCanonicalType().getTypePtr();
    const RecordType *RT = cast<RecordType>(type);
    const ClassTemplateSpecializationDecl *templateSpecDecl =
        cast<ClassTemplateSpecializationDecl>(RT->getDecl());
    ClassTemplateDecl *templateDecl =
        templateSpecDecl->getSpecializedTemplate();
    exprType = getHLSLMatrixType(astContext, theCompilerInstance.getSema(),
                                 templateDecl, astContext.FloatTy, 4, 3);
  }

  const auto loc = expr->getExprLoc();
  const auto range = expr->getSourceRange();
  SpirvInstruction *retVal = spvBuilder.createRayQueryOpsKHR(
      spvCode, exprType, traceArgs, false, loc, range);

  if (transposeMatrix) {
    retVal = spvBuilder.createUnaryOp(spv::Op::OpTranspose, expr->getType(),
                                      retVal, loc, range);
  }

  if (logicalNot) {
    retVal = spvBuilder.createUnaryOp(spv::Op::OpLogicalNot, expr->getType(),
                                      retVal, loc, range);
  }

  retVal->setRValue();
  return retVal;
}

SpirvInstruction *
SpirvEmitter::createSpirvIntrInstExt(llvm::ArrayRef<const Attr *> attrs,
                                     QualType retType,
                                     llvm::ArrayRef<SpirvInstruction *> spvArgs,
                                     bool isInstr, SourceLocation loc) {
  llvm::SmallVector<uint32_t, 2> capabilities;
  llvm::SmallVector<llvm::StringRef, 2> extensions;
  llvm::StringRef instSet = "";
  // For [[vk::ext_type_def]], we use dummy OpNop with no semantic meaning,
  // with possible extension and capabilities.
  uint32_t op = static_cast<unsigned>(spv::Op::OpNop);
  for (auto &attr : attrs) {
    if (auto capAttr = dyn_cast<VKCapabilityExtAttr>(attr)) {
      capabilities.push_back(capAttr->getCapability());
    } else if (auto extAttr = dyn_cast<VKExtensionExtAttr>(attr)) {
      extensions.push_back(extAttr->getName());
    }
    if (!isInstr)
      continue;
    if (auto instAttr = dyn_cast<VKInstructionExtAttr>(attr)) {
      op = instAttr->getOpcode();
      instSet = instAttr->getInstruction_set();
    }
  }

  SpirvInstruction *retVal = spvBuilder.createSpirvIntrInstExt(
      op, retType, spvArgs, extensions, instSet, capabilities, loc);
  if (!retVal)
    return nullptr;

  // TODO: Revisit this r-value setting when handling vk::ext_result_id<T> ?
  retVal->setRValue();

  return retVal;
}

SpirvInstruction *SpirvEmitter::invertYIfRequested(SpirvInstruction *position,
                                                   SourceLocation loc,
                                                   SourceRange range) {
  // Negate SV_Position.y if requested and supported

  bool supportsInvertY = spvContext.isVS() || spvContext.isGS() ||
                         spvContext.isDS() || spvContext.isMS();

  if (spirvOptions.invertY && supportsInvertY) {
    const auto oldY = spvBuilder.createCompositeExtract(
        astContext.FloatTy, position, {1}, loc, range);
    const auto newY = spvBuilder.createUnaryOp(
        spv::Op::OpFNegate, astContext.FloatTy, oldY, loc, range);
    position = spvBuilder.createCompositeInsert(
        astContext.getExtVectorType(astContext.FloatTy, 4), position, {1}, newY,
        loc, range);
  }
  return position;
}

SpirvInstruction *
SpirvEmitter::processSpvIntrinsicCallExpr(const CallExpr *expr) {
  const auto *funcDecl = expr->getDirectCallee();
  llvm::SmallVector<SpirvInstruction *, 8> spvArgs;
  const auto args = expr->getArgs();
  for (uint32_t i = 0; i < expr->getNumArgs(); ++i) {
    const auto *param = funcDecl->getParamDecl(i);
    const Expr *arg = args[i]->IgnoreParenLValueCasts();
    SpirvInstruction *argInst = doExpr(arg);
    if (param->hasAttr<VKReferenceExtAttr>()) {
      if (argInst->isRValue()) {
        emitError("argument for a parameter with vk::ext_reference attribute "
                  "must be a reference",
                  arg->getExprLoc());
        return nullptr;
      }
      spvArgs.push_back(argInst);
    } else if (param->hasAttr<VKLiteralExtAttr>()) {
      auto constArg = dyn_cast<SpirvConstant>(argInst);
      if (constArg == nullptr) {
        constArg = constEvaluator.tryToEvaluateAsConst(arg, isSpecConstantMode);
      }
      if (constArg == nullptr) {
        emitError("vk::ext_literal may only be applied to parameters that can "
                  "be evaluated to a literal value",
                  expr->getExprLoc());
        return nullptr;
      }
      constArg->setLiteral();
      spvArgs.push_back(constArg);
    } else {
      spvArgs.push_back(loadIfGLValue(arg, argInst));
    }
  }

  return createSpirvIntrInstExt(funcDecl->getAttrs(), funcDecl->getReturnType(),
                                spvArgs,
                                /*isInstr*/ true, expr->getExprLoc());
}

uint32_t SpirvEmitter::getRawBufferAlignment(const Expr *expr) {
  llvm::APSInt value;
  if (expr->EvaluateAsInt(value, astContext) && value.isNonNegative()) {
    return static_cast<uint32_t>(value.getZExtValue());
  }

  // Unable to determine a valid alignment at compile time
  emitError("alignment argument must be a constant unsigned integer",
            expr->getExprLoc());
  return 0;
}

SpirvInstruction *SpirvEmitter::processRawBufferLoad(const CallExpr *callExpr) {
  if (callExpr->getNumArgs() > 2) {
    emitError("number of arguments for vk::RawBufferLoad() must be 1 or 2",
              callExpr->getExprLoc());
    return nullptr;
  }

  uint32_t alignment = callExpr->getNumArgs() == 1
                           ? 4
                           : getRawBufferAlignment(callExpr->getArg(1));
  if (alignment == 0)
    return nullptr;

  SpirvInstruction *address = doExpr(callExpr->getArg(0));
  QualType bufferType = callExpr->getCallReturnType(astContext);
  SourceLocation loc = callExpr->getExprLoc();
  if (!isBoolOrVecMatOfBoolType(bufferType)) {
    return loadDataFromRawAddress(address, bufferType, alignment, loc);
  }

  // If callExpr is `vk::RawBufferLoad<bool>(..)`, we have to load 'uint' and
  // convert it to boolean data, because a physical pointer cannot have boolean
  // type in Vulkan.
  if (alignment % 4 != 0) {
    emitWarning("Since boolean is a logical type, we use a unsigned integer "
                "type to read/write boolean from a buffer. Therefore "
                "alignment for the data with a boolean type must be aligned "
                "with 4 bytes",
                loc);
  }
  QualType boolType = bufferType;
  bufferType = getUintTypeForBool(astContext, theCompilerInstance, boolType);
  SpirvInstruction *load =
      loadDataFromRawAddress(address, bufferType, alignment, loc);
  auto *loadAsBool = castToBool(load, bufferType, boolType, loc);
  if (!loadAsBool)
    return nullptr;
  loadAsBool->setRValue();
  return loadAsBool;
}

SpirvInstruction *
SpirvEmitter::loadDataFromRawAddress(SpirvInstruction *addressInUInt64,
                                     QualType bufferType, uint32_t alignment,
                                     SourceLocation loc) {
  // Summary:
  //   %address = OpBitcast %ptrTobufferType %addressInUInt64
  //   %loadInst = OpLoad %bufferType %address alignment %alignment

  const HybridPointerType *bufferPtrType =
      spvBuilder.getPhysicalStorageBufferType(bufferType);

  SpirvUnaryOp *address = spvBuilder.createUnaryOp(
      spv::Op::OpBitcast, bufferPtrType, addressInUInt64, loc);
  address->setStorageClass(spv::StorageClass::PhysicalStorageBuffer);
  address->setLayoutRule(spirvOptions.sBufferLayoutRule);

  SpirvLoad *loadInst =
      dyn_cast<SpirvLoad>(spvBuilder.createLoad(bufferType, address, loc));
  assert(loadInst);
  loadInst->setAlignment(alignment);
  loadInst->setRValue();
  return loadInst;
}

SpirvInstruction *
SpirvEmitter::storeDataToRawAddress(SpirvInstruction *addressInUInt64,
                                    SpirvInstruction *value,
                                    QualType bufferType, uint32_t alignment,
                                    SourceLocation loc, SourceRange range) {
  // Summary:
  //   %address = OpBitcast %ptrTobufferType %addressInUInt64
  //   %storeInst = OpStore %address %value alignment %alignment
  if (!value || !addressInUInt64)
    return nullptr;

  const HybridPointerType *bufferPtrType =
      spvBuilder.getPhysicalStorageBufferType(bufferType);

  SpirvUnaryOp *address = spvBuilder.createUnaryOp(
      spv::Op::OpBitcast, bufferPtrType, addressInUInt64, loc);
  if (!address)
    return nullptr;
  address->setStorageClass(spv::StorageClass::PhysicalStorageBuffer);
  address->setLayoutRule(spirvOptions.sBufferLayoutRule);

  // If the source value has a different layout, it is not safe to directly
  // store it. It needs to be component-wise reconstructed to the new layout.
  SpirvInstruction *source = value;
  if (value->getStorageClass() != address->getStorageClass()) {
    source = reconstructValue(value, bufferType, address->getLayoutRule(), loc,
                              range);
  }
  if (!source)
    return nullptr;

  SpirvStore *storeInst = spvBuilder.createStore(address, source, loc);
  storeInst->setAlignment(alignment);
  storeInst->setStorageClass(spv::StorageClass::PhysicalStorageBuffer);
  return nullptr;
}

SpirvInstruction *
SpirvEmitter::processRawBufferStore(const CallExpr *callExpr) {
  if (callExpr->getNumArgs() != 2 && callExpr->getNumArgs() != 3) {
    emitError("number of arguments for vk::RawBufferStore() must be 2 or 3",
              callExpr->getExprLoc());
    return nullptr;
  }

  uint32_t alignment = callExpr->getNumArgs() == 2
                           ? 4
                           : getRawBufferAlignment(callExpr->getArg(2));
  if (alignment == 0)
    return nullptr;

  SpirvInstruction *address = doExpr(callExpr->getArg(0));
  SpirvInstruction *value = doExpr(callExpr->getArg(1));
  if (!address || !value)
    return nullptr;

  QualType bufferType = value->getAstResultType();
  clang::SourceLocation loc = callExpr->getExprLoc();
  if (!isBoolOrVecMatOfBoolType(bufferType)) {
    return storeDataToRawAddress(address, value, bufferType, alignment, loc,
                                 callExpr->getLocStart());
  }

  // If callExpr is `vk::RawBufferLoad<bool>(..)`, we have to load 'uint' and
  // convert it to boolean data, because a physical pointer cannot have boolean
  // type in Vulkan.
  if (alignment % 4 != 0) {
    emitWarning("Since boolean is a logical type, we use a unsigned integer "
                "type to read/write boolean from a buffer. Therefore "
                "alignment for the data with a boolean type must be aligned "
                "with 4 bytes",
                loc);
  }
  QualType boolType = bufferType;
  bufferType = getUintTypeForBool(astContext, theCompilerInstance, boolType);
  auto *storeAsInt = castToInt(value, boolType, bufferType, loc);
  return storeDataToRawAddress(address, storeAsInt, bufferType, alignment, loc,
                               callExpr->getLocStart());
}

SpirvInstruction *
SpirvEmitter::processCooperativeMatrixGetLength(const CallExpr *call) {
  auto *declaration = dyn_cast<FunctionDecl>(call->getCalleeDecl());
  assert(declaration);

  const auto *templateSpecializationInfo =
      declaration->getTemplateSpecializationInfo();
  assert(templateSpecializationInfo);
  const clang::TemplateArgumentList &templateArgs =
      *templateSpecializationInfo->TemplateArguments;
  assert(templateArgs.size() == 1);
  const clang::TemplateArgument &arg = templateArgs[0];
  assert(arg.getKind() == clang::TemplateArgument::Type);
  const clang::QualType &type = arg.getAsType();

  // Create an undef for `type`.
  SpirvInstruction *undef = spvBuilder.getUndef(type);

  // Create an OpCooperativeMatrixLengthKHR instruction. However, we cannot
  // make a type a parameter at this point in the code. We will use an Undef of
  // the type that will become the parameter, and then adjust the instruction
  // in EmitVisitor.
  SpirvInstruction *inst = getSpirvBuilder().createUnaryOp(
      spv::Op::OpCooperativeMatrixLengthKHR, call->getType(), undef,
      call->getLocStart(), call->getSourceRange());
  inst->setRValue();
  return inst;
}

SpirvInstruction *
SpirvEmitter::processIntrinsicExecutionMode(const CallExpr *expr) {
  llvm::SmallVector<uint32_t, 2> execModesParams;
  uint32_t exeMode = 0;
  const auto args = expr->getArgs();
  for (uint32_t i = 0; i < expr->getNumArgs(); ++i) {
    uint32_t argInteger;

    Expr::EvalResult evalResult;
    if (args[i]->EvaluateAsRValue(evalResult, astContext) &&
        !evalResult.HasSideEffects && evalResult.Val.isInt()) {
      argInteger = evalResult.Val.getInt().getZExtValue();
    } else {
      emitError("argument should be constant integer", expr->getExprLoc());
      return nullptr;
    }

    if (i > 0)
      execModesParams.push_back(argInteger);
    else
      exeMode = argInteger;
  }
  assert(entryFunction != nullptr);
  assert(exeMode != 0);

  return spvBuilder.addExecutionMode(entryFunction,
                                     static_cast<spv::ExecutionMode>(exeMode),
                                     execModesParams, expr->getExprLoc());
}

SpirvInstruction *
SpirvEmitter::processIntrinsicExecutionModeId(const CallExpr *expr) {
  assert(expr->getNumArgs() > 0);
  uint32_t exeMode = 0;
  const Expr *modeExpr = expr->getArg(0);
  Expr::EvalResult evalResult;
  if (modeExpr->EvaluateAsRValue(evalResult, astContext) &&
      !evalResult.HasSideEffects && evalResult.Val.isInt()) {
    exeMode = evalResult.Val.getInt().getZExtValue();
  } else {
    emitError("The execution mode must be constant integer",
              expr->getExprLoc());
    return nullptr;
  }

  llvm::SmallVector<SpirvInstruction *, 2> execModesParams;
  const auto args = expr->getArgs();
  for (uint32_t i = 1; i < expr->getNumArgs(); ++i) {
    const Expr *argExpr = args[i];
    SpirvInstruction *argInst = doExpr(argExpr);
    execModesParams.push_back(argInst);
  }

  assert(entryFunction != nullptr);
  return spvBuilder.addExecutionModeId(entryFunction,
                                       static_cast<spv::ExecutionMode>(exeMode),
                                       execModesParams, expr->getExprLoc());
}

SpirvInstruction *
SpirvEmitter::processSpvIntrinsicTypeDef(const CallExpr *expr) {
  auto funcDecl = expr->getDirectCallee();
  SmallVector<SpvIntrinsicTypeOperand, 3> operands;
  const auto args = expr->getArgs();
  for (uint32_t i = 0; i < expr->getNumArgs(); ++i) {
    auto param = funcDecl->getParamDecl(i);
    const Expr *arg = args[i]->IgnoreParenLValueCasts();
    if (param->hasAttr<VKReferenceExtAttr>()) {
      auto *recType = param->getType()->getAs<RecordType>();
      if (recType && recType->getDecl()->getName() == "ext_type") {
        auto typeId = hlsl::GetHLSLResourceTemplateUInt(arg->getType());
        auto *typeArg = spvContext.getCreatedSpirvIntrinsicType(typeId);
        operands.emplace_back(typeArg);
      } else {
        operands.emplace_back(doExpr(arg));
      }
    } else if (param->hasAttr<VKLiteralExtAttr>()) {
      SpirvInstruction *argInst = doExpr(arg);
      auto constArg = dyn_cast<SpirvConstant>(argInst);
      assert(constArg != nullptr);
      constArg->setLiteral();
      operands.emplace_back(constArg);
    } else {
      operands.emplace_back(loadIfGLValue(arg));
    }
  }

  auto typeDefAttr = funcDecl->getAttr<VKTypeDefExtAttr>();
  spvContext.getOrCreateSpirvIntrinsicType(typeDefAttr->getId(),
                                           typeDefAttr->getOpcode(), operands);

  return createSpirvIntrInstExt(
      funcDecl->getAttrs(), QualType(),
      /*spvArgs*/ llvm::SmallVector<SpirvInstruction *, 1>{},
      /*isInstr*/ false, expr->getExprLoc());
}

bool SpirvEmitter::spirvToolsValidate(std::vector<uint32_t> *mod,
                                      std::string *messages) {
  spvtools::SpirvTools tools(featureManager.getTargetEnv());

  tools.SetMessageConsumer(
      [messages](spv_message_level_t /*level*/, const char * /*source*/,
                 const spv_position_t & /*position*/,
                 const char *message) { *messages += message; });

  spvtools::ValidatorOptions options;
  options.SetBeforeHlslLegalization(beforeHlslLegalization);
  // GL: strict block layout rules
  // VK: relaxed block layout rules
  // DX: Skip block layout rules
  if (spirvOptions.useScalarLayout || spirvOptions.useDxLayout) {
    options.SetScalarBlockLayout(true);
  } else if (spirvOptions.useGlLayout) {
    // spirv-val by default checks this.
  } else {
    options.SetRelaxBlockLayout(true);
  }
  options.SetUniversalLimit(spv_validator_limit_max_id_bound,
                            spirvOptions.maxId);

  return tools.Validate(mod->data(), mod->size(), options);
}

static bool canUseDerivativeGroupExecutionMode(SpirvContext::ShaderModelKind sm,
                                               bool usingEXTMeshShader) {
  switch (sm) {
  case SpirvContext::ShaderModelKind::Compute:
  case SpirvContext::ShaderModelKind::Node:
    return true;

  // The KHR extension that allows derivative instruction in mesh and task
  // (amplification) shader does not work with SPV_NV_mesh_shader extesion.
  case SpirvContext::ShaderModelKind::Mesh:
  case SpirvContext::ShaderModelKind::Amplification:
    return usingEXTMeshShader;
  default:
    return false;
  }
}

void SpirvEmitter::addDerivativeGroupExecutionMode() {
  bool usingEXTMeshShader =
      featureManager.isExtensionEnabled(Extension::EXT_mesh_shader);
  SpirvContext::ShaderModelKind sm = spvContext.getCurrentShaderModelKind();
  if (!canUseDerivativeGroupExecutionMode(sm, usingEXTMeshShader))
    return;

  SpirvExecutionMode *numThreadsEm =
      cast<SpirvExecutionMode>(spvBuilder.getModule()->findExecutionMode(
          entryFunction, spv::ExecutionMode::LocalSize));
  auto numThreads = numThreadsEm->getParams();

  // The layout of the quad is determined by the numer of threads in each
  // dimention. From the HLSL spec
  // (https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_Derivatives.html):
  //
  // Where numthreads has an X value divisible by 4 and Y and Z are both 1, the
  // quad layouts are determined according to 1D quad rules. Where numthreads X
  // and Y values are divisible by 2, the quad layouts are determined according
  // to 2D quad rules. Using derivative operations in any numthreads
  // configuration not matching either of these is invalid and will produce an
  // error.
  static_assert(spv::ExecutionMode::DerivativeGroupQuadsNV ==
                spv::ExecutionMode::DerivativeGroupQuadsKHR);
  static_assert(spv::ExecutionMode::DerivativeGroupLinearNV ==
                spv::ExecutionMode::DerivativeGroupLinearKHR);
  spv::ExecutionMode em = spv::ExecutionMode::DerivativeGroupQuadsNV;
  if (numThreads[0] % 4 == 0 && numThreads[1] == 1 && numThreads[2] == 1) {
    em = spv::ExecutionMode::DerivativeGroupLinearNV;
  } else {
    assert(numThreads[0] % 2 == 0 && numThreads[1] % 2 == 0);
  }

  spvBuilder.addExecutionMode(entryFunction, em, {}, SourceLocation());
}

SpirvVariable *SpirvEmitter::createPCFParmVarAndInitFromStageInputVar(
    const ParmVarDecl *param) {
  const QualType type = param->getType();
  std::string tempVarName = "param.var." + param->getNameAsString();
  auto paramLoc = param->getLocation();
  auto *tempVar = spvBuilder.addFnVar(
      type, paramLoc, tempVarName, param->hasAttr<HLSLPreciseAttr>(),
      param->hasAttr<HLSLNoInterpolationAttr>());
  SpirvInstruction *loadedValue = nullptr;
  declIdMapper.createStageInputVar(param, &loadedValue, /*forPCF*/ true);
  spvBuilder.createStore(tempVar, loadedValue, paramLoc);
  return tempVar;
}

SpirvVariable *
SpirvEmitter::createFunctionScopeTempFromParameter(const ParmVarDecl *param) {
  const QualType type = param->getType();
  std::string tempVarName = "param.var." + param->getNameAsString();
  auto paramLoc = param->getLocation();
  auto *tempVar = spvBuilder.addFnVar(
      type, paramLoc, tempVarName, param->hasAttr<HLSLPreciseAttr>(),
      param->hasAttr<HLSLNoInterpolationAttr>());
  return tempVar;
}

bool SpirvEmitter::spirvToolsRunPass(std::vector<uint32_t> *mod,
                                     spvtools::Optimizer::PassToken token,
                                     std::string *messages) {
  spvtools::Optimizer optimizer(featureManager.getTargetEnv());
  optimizer.SetMessageConsumer(
      [messages](spv_message_level_t /*level*/, const char * /*source*/,
                 const spv_position_t & /*position*/,
                 const char *message) { *messages += message; });

  string::RawOstreamBuf printAllBuf(llvm::errs());
  std::ostream printAllOS(&printAllBuf);
  if (spirvOptions.printAll)
    optimizer.SetPrintAll(&printAllOS);

  spvtools::OptimizerOptions options;
  options.set_run_validator(false);
  options.set_preserve_bindings(spirvOptions.preserveBindings);
  options.set_max_id_bound(spirvOptions.maxId);

  optimizer.RegisterPass(std::move(token));
  return optimizer.Run(mod->data(), mod->size(), mod, options);
}

bool SpirvEmitter::spirvToolsFixupOpExtInst(std::vector<uint32_t> *mod,
                                            std::string *messages) {
  spvtools::Optimizer::PassToken token =
      spvtools::CreateOpExtInstWithForwardReferenceFixupPass();
  return spirvToolsRunPass(mod, std::move(token), messages);
}

bool SpirvEmitter::spirvToolsTrimCapabilities(std::vector<uint32_t> *mod,
                                              std::string *messages) {
  spvtools::Optimizer::PassToken token = spvtools::CreateTrimCapabilitiesPass();
  return spirvToolsRunPass(mod, std::move(token), messages);
}

bool SpirvEmitter::spirvToolsUpgradeToVulkanMemoryModel(
    std::vector<uint32_t> *mod, std::string *messages) {
  spvtools::Optimizer::PassToken token =
      spvtools::CreateUpgradeMemoryModelPass();
  return spirvToolsRunPass(mod, std::move(token), messages);
}

bool SpirvEmitter::spirvToolsOptimize(std::vector<uint32_t> *mod,
                                      std::string *messages) {
  spvtools::Optimizer optimizer(featureManager.getTargetEnv());
  optimizer.SetMessageConsumer(
      [messages](spv_message_level_t /*level*/, const char * /*source*/,
                 const spv_position_t & /*position*/,
                 const char *message) { *messages += message; });

  string::RawOstreamBuf printAllBuf(llvm::errs());
  std::ostream printAllOS(&printAllBuf);
  if (spirvOptions.printAll)
    optimizer.SetPrintAll(&printAllOS);

  spvtools::OptimizerOptions options;
  options.set_run_validator(false);
  options.set_preserve_bindings(spirvOptions.preserveBindings);
  options.set_max_id_bound(spirvOptions.maxId);

  if (spirvOptions.optConfig.empty()) {
    // Add performance passes.
    optimizer.RegisterPerformancePasses(spirvOptions.preserveInterface);

    // Add propagation of volatile semantics passes.
    optimizer.RegisterPass(spvtools::CreateSpreadVolatileSemanticsPass());

    // Add compact ID pass.
    optimizer.RegisterPass(spvtools::CreateCompactIdsPass());
  } else {
    // Command line options use llvm::SmallVector and llvm::StringRef, whereas
    // SPIR-V optimizer uses std::vector and std::string.
    std::vector<std::string> stdFlags;
    for (const auto &f : spirvOptions.optConfig)
      stdFlags.push_back(f.str());
    if (!optimizer.RegisterPassesFromFlags(stdFlags))
      return false;
  }

  return optimizer.Run(mod->data(), mod->size(), mod, options);
}

bool SpirvEmitter::spirvToolsLegalize(std::vector<uint32_t> *mod,
                                      std::string *messages,
                                      const std::vector<DescriptorSetAndBinding>
                                          *dsetbindingsToCombineImageSampler) {
  spvtools::Optimizer optimizer(featureManager.getTargetEnv());
  optimizer.SetMessageConsumer(
      [messages](spv_message_level_t /*level*/, const char * /*source*/,
                 const spv_position_t & /*position*/,
                 const char *message) { *messages += message; });

  string::RawOstreamBuf printAllBuf(llvm::errs());
  std::ostream printAllOS(&printAllBuf);
  if (spirvOptions.printAll)
    optimizer.SetPrintAll(&printAllOS);

  spvtools::OptimizerOptions options;
  options.set_run_validator(false);
  options.set_preserve_bindings(spirvOptions.preserveBindings);
  options.set_max_id_bound(spirvOptions.maxId);

  // Add interface variable SROA if the signature packing is enabled.
  if (spirvOptions.signaturePacking) {
    optimizer.RegisterPass(
        spvtools::CreateInterfaceVariableScalarReplacementPass());
  }
  optimizer.RegisterLegalizationPasses(spirvOptions.preserveInterface);
  // Add flattening of resources if needed.
  if (spirvOptions.flattenResourceArrays) {
    optimizer.RegisterPass(
        spvtools::CreateReplaceDescArrayAccessUsingVarIndexPass());
    optimizer.RegisterPass(
        spvtools::CreateAggressiveDCEPass(spirvOptions.preserveInterface));
    optimizer.RegisterPass(
        spvtools::CreateDescriptorArrayScalarReplacementPass());
    optimizer.RegisterPass(
        spvtools::CreateAggressiveDCEPass(spirvOptions.preserveInterface));
  }
  if (declIdMapper.requiresFlatteningCompositeResources()) {
    optimizer.RegisterPass(
        spvtools::CreateDescriptorCompositeScalarReplacementPass());
    // ADCE should be run after desc_sroa in order to remove potentially
    // illegal types such as structures containing opaque types.
    optimizer.RegisterPass(
        spvtools::CreateAggressiveDCEPass(spirvOptions.preserveInterface));
  }
  if (dsetbindingsToCombineImageSampler &&
      !dsetbindingsToCombineImageSampler->empty()) {
    optimizer.RegisterPass(spvtools::CreateConvertToSampledImagePass(
        *dsetbindingsToCombineImageSampler));
    // ADCE should be run after combining images and samplers in order to
    // remove potentially illegal types such as structures containing opaque
    // types.
    optimizer.RegisterPass(
        spvtools::CreateAggressiveDCEPass(spirvOptions.preserveInterface));
  }
  if (spirvOptions.reduceLoadSize) {
    // The threshold must be bigger than 1.0 to reduce all possible loads.
    optimizer.RegisterPass(spvtools::CreateReduceLoadSizePass(1.1));
    // ADCE should be run after reduce-load-size pass in order to remove
    // dead instructions.
    optimizer.RegisterPass(
        spvtools::CreateAggressiveDCEPass(spirvOptions.preserveInterface));
  }
  optimizer.RegisterPass(spvtools::CreateCompactIdsPass());
  optimizer.RegisterPass(spvtools::CreateSpreadVolatileSemanticsPass());
  if (spirvOptions.fixFuncCallArguments) {
    optimizer.RegisterPass(spvtools::CreateFixFuncCallArgumentsPass());
  }

  return optimizer.Run(mod->data(), mod->size(), mod, options);
}

SpirvInstruction *
SpirvEmitter::doUnaryExprOrTypeTraitExpr(const UnaryExprOrTypeTraitExpr *expr) {
  // TODO: We support only `sizeof()`. Support other kinds.
  if (expr->getKind() != clang::UnaryExprOrTypeTrait::UETT_SizeOf) {
    emitError("expression class '%0' unimplemented", expr->getExprLoc())
        << expr->getStmtClassName();
    return nullptr;
  }

  if (auto *constExpr =
          constEvaluator.tryToEvaluateAsConst(expr, isSpecConstantMode)) {
    constExpr->setRValue();
    return constExpr;
  }

  AlignmentSizeCalculator alignmentCalc(astContext, spirvOptions);
  uint32_t size = 0, stride = 0;
  std::tie(std::ignore, size) = alignmentCalc.getAlignmentAndSize(
      expr->getArgumentType(), SpirvLayoutRule::Scalar,
      /*isRowMajor*/ llvm::None, &stride);
  auto *sizeConst = spvBuilder.getConstantInt(astContext.UnsignedIntTy,
                                              llvm::APInt(32, size));
  sizeConst->setRValue();
  return sizeConst;
}

std::vector<SpirvInstruction *>
SpirvEmitter::decomposeToScalars(SpirvInstruction *inst) {
  QualType elementType;
  uint32_t elementCount = 0;
  uint32_t numOfRows = 0;
  uint32_t numOfCols = 0;

  QualType resultType = inst->getAstResultType();
  if (hlsl::IsHLSLResourceType(resultType)) {
    resultType = hlsl::GetHLSLResourceResultType(resultType);
  }

  if (isScalarType(resultType)) {
    return {inst};
  }

  if (isVectorType(resultType, &elementType, &elementCount)) {
    std::vector<SpirvInstruction *> result;
    for (uint32_t i = 0; i < elementCount; i++) {
      auto *element = spvBuilder.createCompositeExtract(
          elementType, inst, {i}, inst->getSourceLocation());
      element->setLayoutRule(inst->getLayoutRule());
      result.push_back(element);
    }
    return result;
  }

  if (isMxNMatrix(resultType, &elementType, &numOfRows, &numOfCols)) {
    std::vector<SpirvInstruction *> result;
    for (uint32_t i = 0; i < numOfRows; i++) {
      for (uint32_t j = 0; j < numOfCols; j++) {
        auto *element = spvBuilder.createCompositeExtract(
            elementType, inst, {i, j}, inst->getSourceLocation());
        element->setLayoutRule(inst->getLayoutRule());
        result.push_back(element);
      }
    }
    return result;
  }

  if (isArrayType(resultType, &elementType, &elementCount)) {
    std::vector<SpirvInstruction *> result;
    for (uint32_t i = 0; i < elementCount; i++) {
      auto *element = spvBuilder.createCompositeExtract(
          elementType, inst, {i}, inst->getSourceLocation());
      element->setLayoutRule(inst->getLayoutRule());
      auto decomposedElement = decomposeToScalars(element);

      // See how we can improve the performance by avoiding this copy.
      result.insert(result.end(), decomposedElement.begin(),
                    decomposedElement.end());
    }
    return result;
  }

  if (const RecordType *recordType = resultType->getAs<RecordType>()) {
    std::vector<SpirvInstruction *> result;

    const SpirvType *type = nullptr;
    LowerTypeVisitor lowerTypeVisitor(astContext, spvContext, spirvOptions,
                                      spvBuilder);
    type = lowerTypeVisitor.lowerType(resultType, inst->getLayoutRule(), false,
                                      inst->getSourceLocation());

    forEachSpirvField(
        recordType, dyn_cast<StructType>(type),
        [this, inst, &result](size_t spirvFieldIndex, const QualType &fieldType,
                              const StructType::FieldInfo &fieldInfo) {
          auto *field = spvBuilder.createCompositeExtract(
              fieldType, inst, {fieldInfo.fieldIndex},
              inst->getSourceLocation());
          field->setLayoutRule(inst->getLayoutRule());
          auto decomposedField = decomposeToScalars(field);

          // See how we can improve the performance by avoiding this copy.
          result.insert(result.end(), decomposedField.begin(),
                        decomposedField.end());
          return true;
        },
        true);
    return result;
  }

  llvm_unreachable("Trying to decompose a type that we cannot decompose");
  return {};
}

SpirvInstruction *
SpirvEmitter::generateFromScalars(QualType type,
                                  std::vector<SpirvInstruction *> &scalars,
                                  SpirvLayoutRule layoutRule) {
  QualType elementType;
  uint32_t elementCount = 0;
  uint32_t numOfRows = 0;
  uint32_t numOfCols = 0;

  assert(!scalars.empty());
  auto sourceLocation = scalars[0]->getSourceLocation();

  if (isScalarType(type)) {
    // If the type is bool with a non-void layout rule, then it should be
    // treated as a uint.
    assert(layoutRule == SpirvLayoutRule::Void &&
           "If the layout type is not void, then we should cast to an int when "
           "type is a boolean.");
    QualType sourceType = scalars[0]->getAstResultType();
    if (sourceType->isBooleanType() &&
        scalars[0]->getLayoutRule() != SpirvLayoutRule::Void) {
      sourceType = astContext.UnsignedIntTy;
    }

    SpirvInstruction *result =
        castToType(scalars[0], sourceType, type, sourceLocation);
    scalars.erase(scalars.begin());
    return result;
  } else if (isVectorType(type, &elementType, &elementCount)) {
    assert(elementCount <= scalars.size());
    std::vector<SpirvInstruction *> elements;
    for (uint32_t i = 0; i < elementCount; ++i) {
      elements.push_back(castToType(scalars[i], scalars[i]->getAstResultType(),
                                    elementType,
                                    scalars[i]->getSourceLocation()));
    }
    SpirvInstruction *result =
        spvBuilder.createCompositeConstruct(type, elements, sourceLocation);
    result->setLayoutRule(layoutRule);
    scalars.erase(scalars.begin(), scalars.begin() + elementCount);
    return result;
  } else if (isMxNMatrix(type, &elementType, &numOfRows, &numOfCols)) {
    std::vector<SpirvInstruction *> rows;
    QualType rowType = astContext.getExtVectorType(elementType, numOfCols);
    for (uint32_t i = 0; i < numOfRows; i++) {
      std::vector<SpirvInstruction *> row;
      for (uint32_t j = 0; j < numOfCols; j++) {
        row.push_back(castToType(scalars[j], scalars[j]->getAstResultType(),
                                 elementType, scalars[j]->getSourceLocation()));
      }
      scalars.erase(scalars.begin(), scalars.begin() + numOfCols);
      SpirvInstruction *r =
          spvBuilder.createCompositeConstruct(rowType, row, sourceLocation);
      r->setLayoutRule(layoutRule);
      rows.push_back(r);
    }
    SpirvInstruction *result =
        spvBuilder.createCompositeConstruct(type, rows, sourceLocation);
    result->setLayoutRule(layoutRule);
    return result;
  } else if (isArrayType(type, &elementType, &elementCount)) {
    std::vector<SpirvInstruction *> elements;
    for (uint32_t i = 0; i < elementCount; i++) {
      elements.push_back(generateFromScalars(elementType, scalars, layoutRule));
    }
    SpirvInstruction *result =
        spvBuilder.createCompositeConstruct(type, elements, sourceLocation);
    result->setLayoutRule(layoutRule);
    return result;
  } else if (const RecordType *recordType = dyn_cast<RecordType>(type)) {
    std::vector<SpirvInstruction *> elements;
    LowerTypeVisitor lowerTypeVisitor(astContext, spvContext, spirvOptions,
                                      spvBuilder);
    const SpirvType *spirvType =
        lowerTypeVisitor.lowerType(type, layoutRule, false, sourceLocation);

    forEachSpirvField(recordType, dyn_cast<StructType>(spirvType),
                      [this, &elements, &scalars, layoutRule](
                          size_t spirvFieldIndex, const QualType &fieldType,
                          const StructType::FieldInfo &fieldInfo) {
                        elements.push_back(generateFromScalars(
                            fieldType, scalars, layoutRule));
                        return true;
                      });
    SpirvInstruction *result =
        spvBuilder.createCompositeConstruct(type, elements, sourceLocation);
    result->setLayoutRule(layoutRule);
    return result;
  } else {
    llvm_unreachable("Trying to generate a type that we cannot generate");
  }
  return {};
}

SpirvInstruction *
SpirvEmitter::splatScalarToGenerate(QualType type, SpirvInstruction *scalar,
                                    SpirvLayoutRule layoutRule) {
  QualType elementType;
  uint32_t elementCount = 0;
  uint32_t numOfRows = 0;
  uint32_t numOfCols = 0;

  SourceLocation sourceLocation = scalar->getSourceLocation();

  if (isScalarType(type)) {
    // If the type is bool with a non-void layout rule, then it should be
    // treated as a uint.
    assert(layoutRule == SpirvLayoutRule::Void &&
           "If the layout type is not void, then we should cast to an int when "
           "type is a boolean.");
    QualType sourceType = scalar->getAstResultType();
    if (sourceType->isBooleanType() &&
        scalar->getLayoutRule() != SpirvLayoutRule::Void) {
      sourceType = astContext.UnsignedIntTy;
    }

    SpirvInstruction *result =
        castToType(scalar, sourceType, type, scalar->getSourceLocation());
    return result;
  } else if (isVectorType(type, &elementType, &elementCount)) {
    SpirvInstruction *element =
        castToType(scalar, scalar->getAstResultType(), elementType,
                   scalar->getSourceLocation());
    std::vector<SpirvInstruction *> elements(elementCount, element);
    SpirvInstruction *result = spvBuilder.createCompositeConstruct(
        type, elements, scalar->getSourceLocation());
    result->setLayoutRule(layoutRule);
    return result;
  } else if (isMxNMatrix(type, &elementType, &numOfRows, &numOfCols)) {
    SpirvInstruction *element =
        castToType(scalar, scalar->getAstResultType(), elementType,
                   scalar->getSourceLocation());
    assert(element);
    std::vector<SpirvInstruction *> row(numOfCols, element);

    QualType rowType = astContext.getExtVectorType(elementType, numOfCols);
    SpirvInstruction *r =
        spvBuilder.createCompositeConstruct(rowType, row, sourceLocation);
    r->setLayoutRule(layoutRule);
    std::vector<SpirvInstruction *> rows(numOfRows, r);
    SpirvInstruction *result =
        spvBuilder.createCompositeConstruct(type, rows, sourceLocation);
    result->setLayoutRule(layoutRule);
    return result;
  } else if (isArrayType(type, &elementType, &elementCount)) {
    SpirvInstruction *element =
        splatScalarToGenerate(elementType, scalar, layoutRule);
    std::vector<SpirvInstruction *> elements(elementCount, element);
    SpirvInstruction *result = spvBuilder.createCompositeConstruct(
        type, elements, scalar->getSourceLocation());
    result->setLayoutRule(layoutRule);
    return result;
  } else if (const RecordType *recordType = dyn_cast<RecordType>(type)) {
    std::vector<SpirvInstruction *> elements;
    LowerTypeVisitor lowerTypeVisitor(astContext, spvContext, spirvOptions,
                                      spvBuilder);
    const SpirvType *spirvType = lowerTypeVisitor.lowerType(
        type, SpirvLayoutRule::Void, false, sourceLocation);

    forEachSpirvField(recordType, dyn_cast<StructType>(spirvType),
                      [this, &elements, &scalar, layoutRule](
                          size_t spirvFieldIndex, const QualType &fieldType,
                          const StructType::FieldInfo &fieldInfo) {
                        elements.push_back(splatScalarToGenerate(
                            fieldType, scalar, layoutRule));
                        return true;
                      });
    SpirvInstruction *result =
        spvBuilder.createCompositeConstruct(type, elements, sourceLocation);
    result->setLayoutRule(layoutRule);
    return result;
  } else {
    llvm_unreachable("Trying to generate a type that we cannot generate");
  }
  return {};
}

bool SpirvEmitter::UpgradeToVulkanMemoryModelIfNeeded(
    std::vector<uint32_t> *module) {
  // DXC generates code assuming the vulkan memory model is not used. However,
  // if a feature is used that requires the Vulkan memory model, then some code
  // may need to be rewritten.
  if (!spirvOptions.useVulkanMemoryModel &&
      !spvBuilder.hasCapability(spv::Capability::VulkanMemoryModel))
    return true;

  std::string messages;
  if (!spirvToolsUpgradeToVulkanMemoryModel(module, &messages)) {
    emitFatalError("failed to use the vulkan memory model: %0", {}) << messages;
    emitNote("please file a bug report on "
             "https://github.com/Microsoft/DirectXShaderCompiler/issues "
             "with source code if possible",
             {});
    return false;
  }
  return true;
}

} // end namespace spirv
} // end namespace clang
