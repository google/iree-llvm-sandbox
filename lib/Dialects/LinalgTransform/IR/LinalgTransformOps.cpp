//===-- LinalgTransformOps.cpp - Linalg Transform dialect -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Dialects/LinalgTransform/LinalgTransformOps.h"
#include "Dialects/LinalgTransform/ScopedTransform.h"
#include "Dialects/LinalgTransform/TrackingListener.h"
#include "Dialects/LinalgTransform/TrackingRewriteDriver.h"
#include "Dialects/LinalgTransform/TransformOpInterface.h"
#include "Transforms/Listener.h"
#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Conversion/LinalgToLLVM/LinalgToLLVM.h"
#include "mlir/Conversion/LinalgToStandard/LinalgToStandard.h"
#include "mlir/Conversion/MathToLLVM/MathToLLVM.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Conversion/StandardToLLVM/ConvertStandardToLLVMPass.h"
#include "mlir/Conversion/VectorToLLVM/ConvertVectorToLLVM.h"
#include "mlir/Dialect/Bufferization/IR/BufferizableOpInterface.h"
#include "mlir/Dialect/Bufferization/Transforms/Bufferize.h"
#include "mlir/Dialect/Bufferization/Transforms/OneShotAnalysis.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Linalg/ComprehensiveBufferize/AffineInterfaceImpl.h"
#include "mlir/Dialect/Linalg/ComprehensiveBufferize/ModuleBufferization.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/Linalg/Transforms/Hoisting.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/PDL/IR/PDLTypes.h"
#include "mlir/Dialect/SCF/Transforms.h"
#include "mlir/Dialect/Utils/StaticValueUtils.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/InliningUtils.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
#include <algorithm>

#include "FunctionHelpers.h"
#include "PDL.h"

#include "Dialects/LinalgTransform/LinalgTransformOpsDialect.cpp.inc"

#define DEBUG_TYPE "linalg-transform-dialect"
#define DBGS() (llvm::dbgs() << "[" DEBUG_TYPE << "]: ")

using namespace mlir;
using namespace mlir::linalg;

void transform::LinalgTransformDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "Dialects/LinalgTransform/LinalgTransformOps.cpp.inc"
      >();
}

//===----------------------------------------------------------------------===//
// Functional Rewrite Helpers
//===----------------------------------------------------------------------===//

using FunctionalLinalgTransform =
    std::function<FailureOr<LinalgOp>(LinalgOp, PatternRewriter &)>;

/// Fallback "pattern" for simply forwarding a result when an interpreter op is
/// a no-op.
static FailureOr<LinalgOp> forwardOp(LinalgOp op, PatternRewriter &rewriter) {
  return op;
}

/// Extracts a vector of int64_t from an array attribute. Asserts if the
/// attribute contains values other than integers.
static SmallVector<int64_t> extractI64Array(ArrayAttr attr) {
  SmallVector<int64_t> result;
  result.reserve(attr.size());
  for (APInt value : attr.getAsValueRange<IntegerAttr>())
    result.push_back(value.getSExtValue());
  return result;
}

/// Extracts a vector of unsigned from an array attribute. Asserts if the
/// attribute contains values other than intergers. May truncate.
static SmallVector<unsigned> extractUIntArray(ArrayAttr attr) {
  SmallVector<unsigned> result;
  result.reserve(attr.size());
  for (APInt value : attr.getAsValueRange<IntegerAttr>())
    result.push_back(value.getZExtValue());
  return result;
}

//===---------------------------------------------------------------------===//
// ScopeOp
//===---------------------------------------------------------------------===//

void transform::ScopeOp::getSuccessorRegions(
    Optional<unsigned> index, ArrayRef<Attribute> operands,
    SmallVectorImpl<RegionSuccessor> &regions) {
  if (index)
    regions.emplace_back(getResults());
  else
    regions.emplace_back(&body());
}

//===---------------------------------------------------------------------===//
// SequenceOp
//===---------------------------------------------------------------------===//

static LogicalResult verifySequenceOp(transform::SequenceOp op) {
  WalkResult result = op.walk([](Operation *child) {
    for (OpResult result : child->getResults()) {
      if (llvm::hasNItemsOrLess(result.getUses(), 1))
        continue;
      InFlightDiagnostic diag = child->emitError()
                                << "result #" << result.getResultNumber()
                                << " has more than one use";
      for (OpOperand &use : result.getUses()) {
        diag.attachNote(use.getOwner()->getLoc())
            << "used here as operand #" << use.getOperandNumber();
      }
      return WalkResult::interrupt();
    }
    return WalkResult::advance();
  });
  return failure(result.wasInterrupted());
}

//===---------------------------------------------------------------------===//
// MatchOp
//===---------------------------------------------------------------------===//

LogicalResult transform::MatchOp::apply(TransformResults &results,
                                        TransformState &state) {
  FailureOr<SmallVector<Operation *>> ops =
      findMatchingOps(*this, cast<ModuleOp>(state.getTopLevel()));
  if (failed(ops))
    return failure();
  LLVM_DEBUG(DBGS() << "matched " << ops->size() << " ops\n");
  results.set(getResult().cast<OpResult>(), *ops);
  return success();
}

//===---------------------------------------------------------------------===//
// TileOp
//===---------------------------------------------------------------------===//

/// Returns the neutral value for a Linalg operation that produces the given
/// operand, construct using the provided builder. Currently assumes the
/// reduction in the Linalg operation is an addition and, therefore, the neutral
/// value is zero.
static Value getNeutralOfLinalgOp(OpBuilder &b, OpOperand &op) {
  auto t = getElementTypeOrSelf(op.get().getType());
  return b.create<arith::ConstantOp>(op.getOwner()->getLoc(), t,
                                     b.getZeroAttr(t));
}

/// Applies the pad pattern to the given target operation as indicated by the
/// tile op that subsumes padding. Populates `nextTargets` with transformable
/// operations for further transformations (currently, the single padded op).
static FunctionalLinalgTransform
buildPadFromTileOpPattern(transform::TileOp tileOp) {
  if (!tileOp.pad())
    return forwardOp;

  // Capture `tileOp` by-copy because it lives on the stack of the current
  // function but lambdas outlive it. They are marked as mutable because op
  // accessors are non-const.
  auto packFunc = [tileOp](OpOperand &opOperand) mutable {
    return opOperand.getOperandNumber() < tileOp.pack_paddings().size()
               ? !tileOp.pack_paddings()[opOperand.getOperandNumber()]
                      .cast<IntegerAttr>()
                      .getValue()
                      .isZero()
               : false;
  };
  auto hoistingFunc = [tileOp](OpOperand &opOperand) mutable {
    return opOperand.getOperandNumber() < tileOp.hoist_paddings().size()
               ? tileOp.hoist_paddings()[opOperand.getOperandNumber()]
                     .cast<IntegerAttr>()
                     .getValue()
                     .getSExtValue()
               : 0;
  };
  auto transposeFunc = [tileOp](OpOperand &opOperand) mutable {
    if (opOperand.getOperandNumber() >= tileOp.transpose_paddings().size())
      return SmallVector<int64_t>();

    auto transposePaddings =
        tileOp.transpose_paddings()[opOperand.getOperandNumber()]
            .cast<ArrayAttr>();
    return extractI64Array(transposePaddings);
  };
  LinalgPaddingOptions paddingOptions;
  paddingOptions.setPaddingValueComputationFunction(getNeutralOfLinalgOp);
  paddingOptions.setPaddingNoFoldComputationFunction(packFunc);
  paddingOptions.setPaddingHoistComputationFunction(hoistingFunc);
  paddingOptions.setPaddingTransposeComputationFunction(transposeFunc);

  return callLinalgPattern<LinalgPaddingPattern>(tileOp.getContext(),
                                                 paddingOptions);
}

/// Applies the generalization pattern to the given target operation as
/// indicated by the tile op that subsumes padding. Populates `nextTargets` with
/// transformable operations for further transformations (currently, the single
/// generalized op).
static FunctionalLinalgTransform
buildGeneralizeFromTileOpPattern(transform::TileOp tileOp) {
  if (!tileOp.generalize())
    return forwardOp;
  return callLinalgPattern<LinalgGeneralizationPattern>(tileOp.getContext());
}

FailureOr<LinalgOp> transform::TileOp::applyToOne(LinalgOp target) {
  LinalgTilingOptions tilingOptions;
  SmallVector<int64_t> tileSizes = extractI64Array(sizes());
  // "scalarize_dyn_dims" actually sets the same lambda as the tile sizes and
  // asserts that it is not already set.
  if (!tileSizes.empty() || !scalarize_dyn_dims())
    tilingOptions.setTileSizes(tileSizes);
  tilingOptions.setInterchange(extractUIntArray(interchange()));
  tilingOptions.setPeeledLoops(extractI64Array(peel()));
  if (scalarize_dyn_dims())
    tilingOptions.scalarizeDynamicDims();

  LinalgTilingPattern pattern(getContext(), tilingOptions);
  auto functionalTile = [&](LinalgOp op,
                            PatternRewriter &rewriter) -> FailureOr<LinalgOp> {
    auto result = pattern.returningMatchAndRewrite(op, rewriter);
    if (failed(result))
      return failure();
    return result->op;
  };

  auto tileSeq = functional::SequenceBuilder()
                     .begin(std::move(functionalTile))
                     .then(buildPadFromTileOpPattern(*this))
                     .then(buildGeneralizeFromTileOpPattern(*this));

  return functional::applyAt(target, tileSeq);
}

LogicalResult transform::TileOp::verify() {
  if (!sizes().empty() && scalarize_dyn_dims()) {
    return emitOpError() << sizesAttrName() << " and "
                         << scalarize_dyn_dimsAttrName()
                         << " attributes are mutually exclusive";
  }

  ArrayAttr transposes = transpose_paddings();
  for (Attribute attr : transposes) {
    SmallVector<int64_t> transpose = extractFromI64ArrayAttr(attr);
    auto sequence = llvm::seq<int64_t>(0, transpose.size());
    if (!std::is_permutation(sequence.begin(), sequence.end(),
                             transpose.begin(), transpose.end())) {
      return emitOpError()
             << "expects transpose paddings to be a permutation, found "
             << attr;
    }
  }
  return success();
}

//===---------------------------------------------------------------------===//
// DecomposeOp
//===---------------------------------------------------------------------===//

LogicalResult
transform::DecomposeOp::apply(transform::TransformResults &results,
                              transform::TransformState &state) {
  RewritePatternSet patterns(getContext());
  // TODO: make this targetable.
  populateDecomposeConvolutionPatterns(patterns, LinalgTransformationFilter());
  if (failed(applyPatternsAndFoldGreedily(state.getTopLevel(),
                                          std::move(patterns))))
    return failure();

  // TODO: make this chainable, it isn't in the original codegenstrategy.
  return success();
}

//===---------------------------------------------------------------------===//
// VectorizeOp
//===---------------------------------------------------------------------===//

static void configureVectorizationPatterns(transform::VectorizeOp vectorizeOp,
                                           RewritePatternSet &patterns) {
  MLIRContext *ctx = vectorizeOp->getContext();
  vector::populateVectorTransferPermutationMapLoweringPatterns(patterns);
  vector::populateVectorReductionToContractPatterns(patterns);
  patterns.add<linalg::LinalgCopyVTRForwardingPattern,
               linalg::LinalgCopyVTWForwardingPattern>(ctx,
                                                       /*benefit=*/2);
  vector::TransferReadOp::getCanonicalizationPatterns(patterns, ctx);
  vector::TransferWriteOp::getCanonicalizationPatterns(patterns, ctx);
  if (vectorizeOp.vectorize_padding())
    linalg::populatePadOpVectorizationPatterns(patterns);
}

/// Applies the transformation specified by the given vectorize operation to the
/// given target operation AND some related operations.Populates `results` with
/// transformation operations for further transformations if the pattern applied
/// successfully (currently, the main "contraction" op after vectorization).
static FailureOr<LinalgOp>
executeTargetedVectorizeOp(LinalgOp target,
                           linalg::transform::VectorizeOp vectorizeOp) {
  // TODO: this is copy-pasta from LinalgStrategyVectorizePass, it shouldn't be.
  MLIRContext *ctx = target->getContext();
  RewritePatternSet patterns(ctx);
  configureVectorizationPatterns(vectorizeOp, patterns);
  LinalgVectorizationPattern pattern(vectorizeOp.getContext());
  auto functionalVectorize = [&](LinalgOp op, PatternRewriter &rewriter) {
    return pattern.matchAndRewrite(op, rewriter);
  };

  /// Apply the transformations in a scope.
  return transform::scoped(
      target,
      [&](transform::ScopeOp scope, Operation *op) -> FailureOr<LinalgOp> {
        if (failed(functional::applyAt(op, functionalVectorize)) ||
            failed(applyPatternsAndFoldGreedily(scope, std::move(patterns))))
          return failure();
        // FIXME: Vectorization doesn't return anything.
        return LinalgOp();
      });

  // TODO: vectorization may fail because the op is not vectorizable, unclear
  // what to do here. We should probably report it somehow, but we may also
  // want to go on and keep the original for continuation. Should we have
  // some notion of transformation optionality vs. mandatory (like lowering)?
  // How to find ops that were not replaced?
}

LogicalResult
transform::VectorizeOp::apply(transform::TransformResults &results,
                              transform::TransformState &state) {
  if (target()) {
    SmallVector<Operation *> resultVector;
    LogicalResult res = applyTransformToEach(
        state.getPayloadOps(target()), resultVector, [&](LinalgOp target) {
          return executeTargetedVectorizeOp(target, *this);
        });

    if (failed(res))
      return failure();

    results.set(getResult(0).cast<OpResult>(), resultVector);
    return success();
  }

  MLIRContext *ctx = getContext();
  RewritePatternSet patterns(ctx);
  patterns.add<LinalgVectorizationPattern>(ctx);
  configureVectorizationPatterns(*this, patterns);
  return applyPatternsTrackAndFoldGreedily(
      state.getTopLevel(), state.getMapping(), std::move(patterns));
}

ParseResult transform::VectorizeOp::parse(OpAsmParser &parser,
                                          OperationState &result) {
  auto operationType = pdl::OperationType::get(parser.getContext());
  OpAsmParser::OperandType target;
  OptionalParseResult parseResult = parser.parseOptionalOperand(target);
  if (parseResult.hasValue()) {
    if (parseResult.getValue().failed() ||
        parser.parseOptionalAttrDict(result.attributes) ||
        parser.resolveOperand(target, operationType, result.operands) ||
        parser.addTypeToList(operationType, result.types)) {
      return failure();
    }
  } else {
    if (parser.parseOptionalAttrDict(result.attributes)) {
      return failure();
    }
  }
  return success();
}

void transform::VectorizeOp::print(OpAsmPrinter &printer) {
  if (target())
    printer << " " << target() << " ";
  printer.printOptionalAttrDict(getOperation()->getAttrs());
}

//===---------------------------------------------------------------------===//
// LowerVectorsOp
//===---------------------------------------------------------------------===//

/// Returns true of the numbered vector lowering stage is included into the list
/// of stages specified on the given lowerVectors operation.
static bool stageIncluded(int stage, transform::LowerVectorsOp lowerVectorsOp) {
  for (auto s : lowerVectorsOp.stages().getAsValueRange<IntegerAttr>()) {
    if (s.getSExtValue() == stage)
      return true;
  }
  return false;
}

// Applies the transformation specified by the given lower vectors operation
/// to the given function.
LogicalResult
transform::LowerVectorsOp::apply(transform::TransformResults &results,
                                 transform::TransformState &state) {
  MLIRContext *ctx = getContext();
  RewritePatternSet patterns(ctx);

  vector::VectorTransposeLowering vectorTransposeLowering =
      llvm::StringSwitch<vector::VectorTransposeLowering>(transpose_lowering())
          .Case("eltwise", vector::VectorTransposeLowering::EltWise)
          .Case("flat_transpose", vector::VectorTransposeLowering::Flat)
          .Case("shuffle", vector::VectorTransposeLowering::Shuffle)
          .Default(vector::VectorTransposeLowering::EltWise);
  vector::VectorMultiReductionLowering vectorMultiReductionLowering =
      llvm::StringSwitch<vector::VectorMultiReductionLowering>(
          multireduction_lowering())
          .Case("innerreduction",
                vector::VectorMultiReductionLowering::InnerReduction)
          .Default(vector::VectorMultiReductionLowering::InnerParallel);
  vector::VectorContractLowering vectorContractLowering =
      llvm::StringSwitch<vector::VectorContractLowering>(contraction_lowering())
          .Case("matrixintrinsics", vector::VectorContractLowering::Matmul)
          .Case("dot", vector::VectorContractLowering::Dot)
          .Case("outerproduct", vector::VectorContractLowering::OuterProduct)
          .Default(vector::VectorContractLowering::OuterProduct);
  // TODO: fix the annoying name mismatch (vector-transfers vs vector-transfer).
  vector::VectorTransferSplit vectorTransferSplit =
      llvm::StringSwitch<vector::VectorTransferSplit>(split_transfers())
          .Case("none", vector::VectorTransferSplit::None)
          .Case("linalg-copy", vector::VectorTransferSplit::LinalgCopy)
          .Case("vector-transfers", vector::VectorTransferSplit::VectorTransfer)
          .Default(vector::VectorTransferSplit::None);

  vector::VectorTransformsOptions vectorTransformOptions;
  vectorTransformOptions.setVectorTransformsOptions(vectorContractLowering)
      .setVectorMultiReductionLowering(vectorMultiReductionLowering)
      .setVectorTransposeLowering(vectorTransposeLowering)
      .setVectorTransferSplit(vectorTransferSplit);

  VectorTransferToSCFOptions vectorTransferToSCFOptions =
      VectorTransferToSCFOptions()
          .enableFullUnroll(unroll_vector_transfers())
          .enableLowerPermutationMaps();

  int maxTransferRank = 1;

  auto avx2LoweringOptions =
      x86vector::avx2::LoweringOptions().setTransposeOptions(
          x86vector::avx2::TransposeLoweringOptions()
              .lower4x8xf32(transpose_avx2_lowering())
              .lower8x8xf32(transpose_avx2_lowering()));

  // TODO: this is copy-pasta from LinalgStrategyLowerVectorsPass, shouldn't be.
  vector::populateVectorToVectorCanonicalizationPatterns(patterns);
  if (stageIncluded(1, *this)) {
    patterns.add<mlir::vector::ContractionOpToOuterProductOpLowering,
                 mlir::vector::ContractionOpToMatmulOpLowering,
                 mlir::vector::ContractionOpLowering>(vectorTransformOptions,
                                                      ctx);
    vector::populateVectorTransferPermutationMapLoweringPatterns(patterns);
  }
  if (stageIncluded(2, *this)) {
    vector::populateVectorMultiReductionLoweringPatterns(
        patterns, vectorTransformOptions.vectorMultiReductionLowering);
  }
  if (stageIncluded(3, *this)) {
    patterns.add<vector::VectorTransferFullPartialRewriter>(
        ctx, vectorTransformOptions);
  }
  if (stageIncluded(4, *this)) {
    vector::populateVectorTransferLoweringPatterns(patterns, maxTransferRank);
  }
  if (stageIncluded(5, *this)) {
    populateVectorToSCFConversionPatterns(
        patterns, vectorTransferToSCFOptions.setTargetRank(maxTransferRank));
  }
  if (stageIncluded(6, *this)) {
    vector::populateVectorShapeCastLoweringPatterns(patterns);
  }
  if (stageIncluded(7, (*this))) {
    vector::populateVectorTransposeLoweringPatterns(patterns,
                                                    vectorTransformOptions);
    if (transpose_avx2_lowering())
      x86vector::avx2::populateSpecializedTransposeLoweringPatterns(
          patterns, avx2LoweringOptions, /*benefit=*/10);
  }

  // TODO: these transformations are currently not targeted at concrete ops.
  // LinalgTransformationFilter filter = makeTransformationFilter(target);
  if (failed(applyPatternsAndFoldGreedily(state.getTopLevel(),
                                          std::move(patterns))))
    return failure();

  // TODO: make composable...
  return success();
}

//===---------------------------------------------------------------------===//
// BufferizeOp
//===---------------------------------------------------------------------===//

LogicalResult transform::BufferizeOp::apply(transform::TransformResults &result,
                                            transform::TransformState &state) {
  PassManager pm(getContext());

  bufferization::AnalysisBufferizationOptions options;
  options.memCpyFn = [](OpBuilder &builder, Location loc, Value from,
                        Value to) {
    return success(linalg::makeMemRefCopyOp(builder, loc, from, to));
  };
  pm.addPass(createLinalgComprehensiveModuleBufferizePass(options));
  if (failed(pm.run(state.getTopLevel())))
    return failure();

  // Perform buffer-level hoistings.
  state.getTopLevel()->walk(
      [&](FuncOp funcOp) { hoistRedundantVectorTransfers(funcOp); });
  return success();
}

//===---------------------------------------------------------------------===//
// LowerToLLVMOp
//===---------------------------------------------------------------------===//

LogicalResult
transform::LowerToLLVMOp::apply(transform::TransformResults &result,
                                transform::TransformState &state) {
  // TODO: it is feasible to scope lowering at arbitrary level and introduce
  // unrealized casts, but there needs to be the final module-wise cleanup in
  // the end. Keep module-level for now.
  PassManager pm(getContext());

  pm.addNestedPass<FuncOp>(createConvertVectorToSCFPass());
  pm.addNestedPass<FuncOp>(createConvertLinalgToLoopsPass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createLowerAffinePass());
  pm.addPass(createConvertSCFToCFPass());
  pm.addPass(createConvertLinalgToLLVMPass());
  pm.addPass(createConvertVectorToLLVMPass(
      // clang-format off
      LowerVectorToLLVMOptions()
        .enableReassociateFPReductions(false)
        .enableIndexOptimizations(false)
        .enableArmNeon(false)
        .enableArmSVE(false)
        .enableAMX(false)
        .enableX86Vector(false)));
  // clang-format on
  pm.addNestedPass<FuncOp>(createConvertMathToLLVMPass());
  pm.addPass(createMemRefToLLVMPass());
  pm.addPass(createLowerToLLVMPass());
  pm.addPass(createReconcileUnrealizedCastsPass());
  if (failed(pm.run(state.getTopLevel())))
    return failure();

  // Make all arguments noalias for now.
  // FIXME: this is a terrible hack!
  state.getTopLevel()->walk([](LLVM::LLVMFuncOp funcOp) {
    for (int64_t i = 0; i < funcOp.getNumArguments(); ++i) {
      if (!funcOp.getType().getParamType(i).isa<LLVM::LLVMPointerType>())
        continue;
      funcOp.setArgAttr(i, "llvm.noalias", UnitAttr::get(funcOp.getContext()));
    }
  });
  return success();
}

//===---------------------------------------------------------------------===//
// GetParentLoopOp
//===---------------------------------------------------------------------===//

FailureOr<scf::ForOp>
transform::GetParentLoopOp::applyToOne(Operation *source) {
  int64_t nLoops = num_loops();
  for (int64_t i = 0; i < nLoops; ++i) {
    source = source->getParentOfType<scf::ForOp>();
    if (!source) {
      emitError() << "the transformed op is enclosed by " << i << " loops, but "
                  << nLoops << " expected";
      return failure();
    }
  }
  return cast<scf::ForOp>(source);
}

//===---------------------------------------------------------------------===//
// UnrollLoopOp
//===---------------------------------------------------------------------===//

LogicalResult transform::UnrollLoopOp::applyToOne(scf::ForOp loop) {
  return loopUnrollByFactor(loop, factor());
}

//===---------------------------------------------------------------------===//
// PipelineLoopOp
//===---------------------------------------------------------------------===//

static void
loopScheduling(scf::ForOp forOp,
               std::vector<std::pair<Operation *, unsigned>> &schedule,
               unsigned iterationInterval, unsigned readLatency) {
  auto getLatency = [&](Operation *op) {
    if (isa<vector::TransferReadOp>(op))
      return readLatency;
    return unsigned(1);
  };

  DenseMap<Operation *, unsigned> opCycles;
  std::map<unsigned, std::vector<Operation *>> wrappedSchedule;
  for (Operation &op : forOp.getBody()->getOperations()) {
    if (isa<scf::YieldOp>(op))
      continue;
    unsigned earlyCycle = 0;
    for (Value operand : op.getOperands()) {
      Operation *def = operand.getDefiningOp();
      if (!def)
        continue;
      earlyCycle = std::max(earlyCycle, opCycles[def] + getLatency(def));
    }
    opCycles[&op] = earlyCycle;
    wrappedSchedule[earlyCycle % iterationInterval].push_back(&op);
  }
  for (auto it : wrappedSchedule) {
    for (Operation *op : it.second) {
      unsigned cycle = opCycles[op];
      schedule.push_back(std::make_pair(op, cycle / iterationInterval));
    }
  }
}

FailureOr<scf::ForOp> transform::PipelineLoopOp::applyToOne(scf::ForOp loop) {
  // TODO: make the pipelining pattern return the transformed loop.
  if (!getOperation()->getUses().empty()) {
    InFlightDiagnostic diag = emitError()
                              << "NYI: cannot target the result of pipelining";
    diag.attachNote(getOperation()->use_begin()->getOwner()->getLoc())
        << "use here";
    return failure();
  }

  scf::PipeliningOption schedule;
  schedule.getScheduleFn =
      [this](scf::ForOp forOp,
             std::vector<std::pair<Operation *, unsigned>> &schedule) mutable {
        loopScheduling(forOp, schedule, iteration_interval(), read_latency());
      };

  RewritePatternSet patterns(loop->getContext());
  scf::populateSCFLoopPipeliningPatterns(patterns, schedule);
  assert(patterns.getNativePatterns().size() == 1 &&
         "expected one pipelining pattern");
  auto functionalPattern = [&patterns](scf::ForOp forOp,
                                       PatternRewriter &rewriter) {
    RewritePattern *pattern = patterns.getNativePatterns().front().get();
    return pattern->matchAndRewrite(forOp, rewriter);
  };
  if (failed(functional::applyAt(loop, std::move(functionalPattern))))
    return failure();

  return scf::ForOp();
}

//===---------------------------------------------------------------------===//
// OutlineLoopOp
//===---------------------------------------------------------------------===//

static scf::ExecuteRegionOp outlineInExecuteRegion(RewriterBase &b,
                                                   Operation *op) {
  if (op->getNumRegions() != 1)
    return nullptr;
  OpBuilder::InsertionGuard g(b);
  b.setInsertionPoint(op);
  scf::ExecuteRegionOp executeRegionOp =
      b.create<scf::ExecuteRegionOp>(op->getLoc(), op->getResultTypes());
  {
    OpBuilder::InsertionGuard g(b);
    b.setInsertionPointToStart(&executeRegionOp.getRegion().emplaceBlock());
    Operation *clonedOp = b.cloneWithoutRegions(*op);
    Region &clonedRegion = clonedOp->getRegions().front();
    assert(clonedRegion.empty() && "expected empty region");
    b.inlineRegionBefore(op->getRegions().front(), clonedRegion,
                         clonedRegion.end());
    b.create<scf::YieldOp>(op->getLoc(), clonedOp->getResults());
  }
  b.replaceOp(op, executeRegionOp.getResults());
  return executeRegionOp;
}

static FailureOr<FuncOp> outlineLoop(scf::ForOp loop, StringRef funcName,
                                     transform::TransformState &state) {
  PatternRewriterListener rewriter(loop->getContext());
  TrackingListener listener(state.getMapping());
  rewriter.addListener(&listener);
  Location loc = loop.getLoc();
  scf::ExecuteRegionOp exec = outlineInExecuteRegion(rewriter, loop);
  assert(exec && "failed to produce execute_region");
  FailureOr<FuncOp> outlined =
      outlineSingleBlockRegion(rewriter, loc, exec.getRegion(), funcName);
  if (failed(listener.checkErrorState()))
    return failure();
  return outlined;
}

LogicalResult
transform::OutlineLoopOp::apply(transform::TransformResults &results,
                                transform::TransformState &state) {
  SmallVector<Operation *> resultVector;
  auto res =
      applyTransformToEach(state.getPayloadOps(target()), resultVector,
                           [&](scf::ForOp loop) -> FailureOr<FuncOp> {
                             return outlineLoop(loop, func_name(), state);
                           });
  if (failed(res))
    return failure();
  results.set(getResult().cast<OpResult>(), resultVector);
  return success();
}

#define GET_OP_CLASSES
#include "Dialects/LinalgTransform/LinalgTransformOps.cpp.inc"
