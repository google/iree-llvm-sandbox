//===-- VectorExtOps.h - Vector Extension dialect ops ------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dialect/VectorExt/VectorExtOps.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OpImplementation.h"

using namespace mlir;
using namespace mlir::vector_ext;

//===----------------------------------------------------------------------===//
// PredicateOp
//===----------------------------------------------------------------------===//

/// Default callback for PredicateOp builders. Inserts a yield without
/// arguments.
void mlir::vector_ext::buildTerminatedBody(OpBuilder &builder, Location loc) {
  builder.create<vector_ext::YieldOp>(loc);
}

void PredicateOp::build(OpBuilder &builder, OperationState &result,
                        Value predicateMask, ValueRange indices,
                        Value incomingMask) {
  build(builder, result, /*resultTypes=*/llvm::None, predicateMask, indices,
        incomingMask);
}

void PredicateOp::build(
    OpBuilder &builder, OperationState &result, TypeRange resultTypes,
    Value predicateMask, ValueRange indices, Value incomingMask,
    function_ref<void(OpBuilder &, Location)> truePredicateBuilder) {
  assert(truePredicateBuilder &&
         "the builder callback for 'truePredicate' must be present");

  result.addOperands(predicateMask);
  result.addOperands(indices);
  result.addOperands(incomingMask);
  result.addTypes(resultTypes);

  OpBuilder::InsertionGuard guard(builder);
  Region *truePredicateRegion = result.addRegion();
  Block *bodyBlock = builder.createBlock(truePredicateRegion);
  bodyBlock->addArgument(predicateMask.getType(), result.location);
  truePredicateBuilder(builder, result.location);
}

ParseResult mlir::vector_ext::PredicateOp::parse(OpAsmParser &parser,
                                                 OperationState &result) {
  // Create the regions for 'truePredicate'.
  result.regions.reserve(1);
  Region *truePredicateRegion = result.addRegion();

  auto &builder = parser.getBuilder();

  // Parse all the operands.
  OpAsmParser::UnresolvedOperand predicateMask;
  OpAsmParser::UnresolvedOperand incomingMask;
  SmallVector<OpAsmParser::UnresolvedOperand> indices;
  if (parser.parseLParen() || parser.parseRegionArgument(predicateMask) ||
      parser.parseComma() ||
      parser.parseOperandList(indices, AsmParser::Delimiter::Square) ||
      parser.parseComma() || parser.parseRegionArgument(incomingMask) ||
      parser.parseRParen())
    return failure();

  // Parse predicate type.
  Type maskType;
  if (parser.parseColonType(maskType))
    return failure();

  if (parser.resolveOperand(predicateMask, maskType, result.operands) ||
      parser.resolveOperands(indices, IndexType::get(builder.getContext()),
                             result.operands) ||
      parser.resolveOperand(incomingMask, maskType, result.operands))
    return failure();

  // Parse optional results type list.
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Parse the 'truePredicate' region.
  if (parser.parseRegion(*truePredicateRegion, /*arguments=*/{},
                         /*argTypes=*/{}))
    return failure();
  PredicateOp::ensureTerminator(*truePredicateRegion, builder, result.location);

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

void mlir::vector_ext::PredicateOp::print(OpAsmPrinter &p) {
  bool printBlockTerminators = false;

  p << "(" << predicateMask() << ", [" << indices() << "], " << incomingMask()
    << ") : " << predicateMask().getType();
  if (!results().empty()) {
    p << " -> (" << getResultTypes() << ")";
    // Print yield explicitly if the op defines values.
    printBlockTerminators = true;
  }
  p << " ";
  p.printRegion(truePredicateRegion(),
                /*printEntryBlockArgs=*/true,
                /*printBlockTerminators=*/printBlockTerminators);

  p.printOptionalAttrDict(getOperation()->getAttrs());
}

/// Given the region at `index`, or the parent operation if `index` is None,
/// return the successor regions. These are the regions that may be selected
/// during the flow of control. `operands` is a set of optional attributes that
/// correspond to a constant value for each operand, or null if that operand is
/// not a constant.
void PredicateOp::getSuccessorRegions(
    Optional<unsigned> index, ArrayRef<Attribute> operands,
    SmallVectorImpl<RegionSuccessor> &regions) {
  // The `truePredicate` region branch back to the parent operation.
  if (index.hasValue()) {
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }

  // The `truePredicate` (and the future `falsePredicate` region)  will always
  // be executed regardless of the condition since they are not modeling control
  // but data flow.
  regions.push_back(RegionSuccessor(&truePredicateRegion()));
}

//===----------------------------------------------------------------------===//
// WarpExecuteOnLane0Op
//===----------------------------------------------------------------------===//

// TODO: Implement me.
bool WarpExecuteOnLane0Op::areTypesCompatible(Type lhs, Type rhs) {
  return true;
}

constexpr StringRef getWarpSizeAttrName() { return "warp_size"; }

void mlir::vector_ext::WarpExecuteOnLane0Op::print(OpAsmPrinter &p) {
  p << "(" << laneid() << ")";

  SmallVector<StringRef> coreAttr = {getWarpSizeAttrName()};
  auto warpSizeAttr = getOperation()->getAttr(getWarpSizeAttrName());
  p << "[" << warpSizeAttr.cast<IntegerAttr>().getInt() << "]";

  if (!args().empty())
    p << " args(" << args() << " : " << args().getTypes() << ")";
  if (!results().empty())
    p << " -> (" << results().getTypes() << ')';
  p << " ";
  p.printRegion(getRegion(),
                /*printEntryBlockArgs=*/true,
                /*printBlockTerminators=*/!results().empty());
  p.printOptionalAttrDict(getOperation()->getAttrs(), coreAttr);
}

ParseResult
mlir::vector_ext::WarpExecuteOnLane0Op::parse(OpAsmParser &parser,
                                              OperationState &result) {
  // Create the region.
  result.regions.reserve(1);
  Region *warpRegion = result.addRegion();

  auto &builder = parser.getBuilder();
  OpAsmParser::UnresolvedOperand laneId;

  // Parse predicate operand.
  if (parser.parseLParen() || parser.parseRegionArgument(laneId) ||
      parser.parseRParen())
    return failure();

  int64_t warpSize;
  if (parser.parseLSquare() || parser.parseInteger(warpSize) ||
      parser.parseRSquare())
    return failure();
  result.addAttribute(getWarpSizeAttrName(),
                      builder.getI64IntegerAttr(warpSize));

  if (parser.resolveOperand(laneId, builder.getIndexType(), result.operands))
    return failure();

  llvm::SMLoc inputsOperandsLoc;
  SmallVector<OpAsmParser::UnresolvedOperand> inputsOperands;
  SmallVector<Type> inputTypes;
  if (succeeded(parser.parseOptionalKeyword("args"))) {
    if (parser.parseLParen())
      return failure();

    inputsOperandsLoc = parser.getCurrentLocation();
    if (parser.parseOperandList(inputsOperands) ||
        parser.parseColonTypeList(inputTypes) || parser.parseRParen())
      return failure();
  }
  if (parser.resolveOperands(inputsOperands, inputTypes, inputsOperandsLoc,
                             result.operands))
    return failure();

  // Parse optional results type list.
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();
  // Parse the region.
  if (parser.parseRegion(*warpRegion, /*arguments=*/{},
                         /*argTypes=*/{}))
    return failure();
  WarpExecuteOnLane0Op::ensureTerminator(*warpRegion, builder, result.location);

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

void WarpExecuteOnLane0Op::getSuccessorRegions(
    Optional<unsigned> index, ArrayRef<Attribute> operands,
    SmallVectorImpl<RegionSuccessor> &regions) {
  if (index.hasValue()) {
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }

  // The warp region is always executed
  regions.push_back(RegionSuccessor(&warpRegion()));
}

void WarpExecuteOnLane0Op::build(OpBuilder &builder, OperationState &result,
                                 TypeRange resultTypes, Value laneId,
                                 int64_t warpSize) {
  build(builder, result, resultTypes, laneId, warpSize,
        /*operands=*/llvm::None, /*argTypes=*/llvm::None);
}

void WarpExecuteOnLane0Op::build(OpBuilder &builder, OperationState &result,
                                 TypeRange resultTypes, Value laneId,
                                 int64_t warpSize, ValueRange args,
                                 TypeRange blockArgTypes) {
  result.addOperands(laneId);
  result.addAttribute(getAttributeNames()[0],
                      builder.getI64IntegerAttr(warpSize));
  result.addTypes(resultTypes);
  result.addOperands(args);
  assert(args.size() == blockArgTypes.size());
  OpBuilder::InsertionGuard guard(builder);
  Region *warpRegion = result.addRegion();
  Block *block = builder.createBlock(warpRegion);
  for (auto it : llvm::zip(blockArgTypes, args))
    block->addArgument(std::get<0>(it), std::get<1>(it).getLoc());
}

/// Helper check if the distributed vector type is consistent with the expanded
/// type and distributed size.
static LogicalResult verifyDistributedType(Type expanded, Type distributed,
                                           int64_t warpSize, Operation *op) {
  // If the types matches there is no distribution.
  if (expanded == distributed)
    return success();
  auto expandedVecType = expanded.dyn_cast<VectorType>();
  auto distributedVecType = distributed.dyn_cast<VectorType>();
  if (!expandedVecType || !distributedVecType)
    return op->emitOpError("expected vector type for distributed operands.");
  if (expandedVecType.getRank() != distributedVecType.getRank() ||
      expandedVecType.getElementType() != distributedVecType.getElementType())
    return op->emitOpError(
        "expected distributed vectors to have same rank and element type.");
  bool foundDistributedDim = false;
  for (int64_t i = 0, e = expandedVecType.getRank(); i < e; i++) {
    if (expandedVecType.getDimSize(i) == distributedVecType.getDimSize(i))
      continue;
    if (expandedVecType.getDimSize(i) ==
        distributedVecType.getDimSize(i) * warpSize) {
      if (foundDistributedDim)
        return op->emitOpError()
               << "expected only one dimension to be distributed from "
               << expandedVecType << " to " << distributedVecType;
      foundDistributedDim = true;
      continue;
    }
    return op->emitOpError() << "incompatible distribution dimensions from "
                             << expandedVecType << " to " << distributedVecType;
  }
  return success();
}

LogicalResult WarpExecuteOnLane0Op::verify() {
  if (args().size() != warpRegion().getNumArguments())
    return emitOpError(
        "expected same number op arguments and block arguments.");
  auto yield = cast<vector_ext::YieldOp>(
      warpRegion().getBlocks().begin()->getTerminator());
  if (yield.getNumOperands() != getNumResults())
    return emitOpError(
        "expected same number of yield operands and return values.");
  int64_t warpSize = warp_size();
  for (auto it : llvm::zip(warpRegion().getArguments(), args())) {
    if (failed(verifyDistributedType(std::get<0>(it).getType(),
                                     std::get<1>(it).getType(), warpSize,
                                     getOperation())))
      return failure();
  }
  for (auto it : llvm::zip(yield.getOperands(), getResults())) {
    if (failed(verifyDistributedType(std::get<0>(it).getType(),
                                     std::get<1>(it).getType(), warpSize,
                                     getOperation())))
      return failure();
  }
  return success();
}

#define GET_OP_CLASSES
#include "Dialect/VectorExt/VectorExtOps.cpp.inc"

using namespace mlir;
using namespace mlir::vector_ext;
