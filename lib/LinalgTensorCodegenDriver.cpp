//===- LinalgTensorCodegenDriver.cpp - Linalg transformation driver--------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Transforms/PassDetail.h"
#include "Transforms/Passes.h"
#include "Transforms/Transforms.h"
#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Conversion/LinalgToLLVM/LinalgToLLVM.h"
#include "mlir/Conversion/MathToLLVM/MathToLLVM.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/Passes.h"
#include "mlir/Conversion/VectorToLLVM/ConvertVectorToLLVM.h"
#include "mlir/Conversion/VectorToSCF/VectorToSCF.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/Dialect/Linalg/ComprehensiveBufferize/ComprehensiveBufferize.h"
#include "mlir/Dialect/Linalg/ComprehensiveBufferize/LinalgInterfaceImpl.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/Linalg/Transforms/CodegenStrategy.h"
#include "mlir/Dialect/Linalg/Transforms/Hoisting.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/Linalg/Utils/Utils.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/MemRef/Transforms/Passes.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Vector/VectorRewritePatterns.h"
#include "mlir/Dialect/Vector/VectorTransforms.h"
#include "mlir/Dialect/X86Vector/Transforms.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/LoopUtils.h"
#include "mlir/Transforms/Passes.h"

using namespace mlir;
using namespace mlir::linalg;

namespace {
struct LinalgTensorCodegenDriverPass
    : public LinalgTensorCodegenDriverBase<LinalgTensorCodegenDriverPass> {
  LinalgTensorCodegenDriverPass() = default;
  LinalgTensorCodegenDriverPass(const LinalgTensorCodegenDriverPass &pass) {}

  /// Function pass entry point.
  void runOnOperation() override;

 private:
  void fuseOutputIntoReduction(FuncOp funcOp);
  void fuseAll(FuncOp funcOp);
  void runOpAnchoredStrategy(FuncOp funcOp);
  void runComprehensiveBufferization();
  void runVectorLowering();
  void runLowerToLLVM();

  void getDependentDialects(DialectRegistry &registry) const override;
};
}  // namespace

void LinalgTensorCodegenDriverPass::runLowerToLLVM() {
  OpPassManager dynamicPM("builtin.module");
  // This is a failsafe catchall, if it does something performance opportunities
  // have been missed previously.
  dynamicPM.addNestedPass<FuncOp>(createConvertVectorToSCFPass());
  dynamicPM.addNestedPass<FuncOp>(createConvertLinalgToLoopsPass());
  dynamicPM.addPass(createCanonicalizerPass());
  dynamicPM.addPass(createLowerAffinePass());
  dynamicPM.addPass(createLowerToCFGPass());
  dynamicPM.addPass(createConvertLinalgToLLVMPass());
  dynamicPM.addPass(createConvertVectorToLLVMPass(
      // clang-format off
      LowerVectorToLLVMOptions()
        .enableReassociateFPReductions(reassociateFPReductions)
        .enableIndexOptimizations(indexOptimizations)
        .enableArmNeon(armNeon)
        .enableArmSVE(armSVE)
        .enableAMX(amx)
        .enableX86Vector(x86Vector)));
  // clang-format on
  dynamicPM.addNestedPass<FuncOp>(createConvertMathToLLVMPass());
  dynamicPM.addPass(createMemRefToLLVMPass());
  dynamicPM.addPass(createLowerToLLVMPass());
  dynamicPM.addPass(createCanonicalizerPass());
  dynamicPM.addPass(createCSEPass());
  if (failed(runPipeline(dynamicPM, getOperation())))
    return signalPassFailure();

  // Make all arguments noalias for now.
  getOperation().walk([](LLVM::LLVMFuncOp funcOp) {
    for (int64_t i = 0; i < funcOp.getNumArguments(); ++i) {
      if (!funcOp.getType().getParamType(i).isa<LLVM::LLVMPointerType>())
        continue;
      funcOp.setArgAttr(i, "llvm.noalias", UnitAttr::get(funcOp.getContext()));
    }
  });
}

/// Return the neutral element as a new Value.
/// For now, just assume it is the zero of type.
/// In the future, it should be the zero of type + op.
static Value getNeutralOfLinalgOp(OpBuilder &b, OpOperand &op) {
  auto t = getElementTypeOrSelf(op.get().getType());
  return b.create<ConstantOp>(op.getOwner()->getLoc(), t, b.getZeroAttr(t));
}

/// Collect all Linalg ops, they must all have tensor semantics.
/// For now this just fuses everything.
// TODO: finer control.
void LinalgTensorCodegenDriverPass::fuseAll(FuncOp funcOp) {
  SmallVector<LinalgOp> linalgOps;
  auto walkResult = funcOp.walk([&](LinalgOp op) {
    if (!op.hasTensorSemantics()) return WalkResult::interrupt();
    linalgOps.push_back(op);
    return WalkResult::advance();
  });
  if (walkResult.wasInterrupted()) return signalPassFailure();

  // Compute the tile sizes and the interchange.
  LinalgOp rootOp = linalgOps.back();
  assert(tileSizes.size() >= rootOp.getNumLoops() &&
         "expect one tile sizes per root op loop dimension");
  assert(tileInterchange.empty() ||
         tileInterchange.size() == tileSizes.size() &&
             "expect the number of tile sizes and interchange dims to match");
  SmallVector<int64_t> rootTileSizes(tileSizes.begin(),
                                     tileSizes.begin() + rootOp.getNumLoops());
  SmallVector<int64_t> rootInterchange =
      tileInterchange.empty()
          ? llvm::to_vector<6>(llvm::seq<int64_t>(0, rootOp.getNumLoops()))
          : SmallVector<int64_t>(
                tileInterchange.begin(),
                tileInterchange.begin() + rootOp.getNumLoops());

  // Tile the root operation and fuse it with its producers.
  OpBuilder b(funcOp.getContext());
  FailureOr<TileLoopNest> tileLoopNest =
      tileConsumerAndFuseProducers(b, rootOp, rootTileSizes, rootInterchange);
  if (failed(tileLoopNest)) return signalPassFailure();
  rootOp->replaceAllUsesWith(tileLoopNest->getRootOpReplacementResults());
}

void LinalgTensorCodegenDriverPass::fuseOutputIntoReduction(FuncOp funcOp) {
  LinalgTilingOptions tiling_options;
  tiling_options.setTileSizes(tileSizes);

  auto *context = funcOp.getContext();

  auto patterns =
      mlir::linalg::getLinalgTilingCanonicalizationPatterns(context);
  mlir::memref::populateResolveRankedShapeTypeResultDimsPatterns(patterns);
  populateFuseFillIntoReductionPatterns(patterns, tiling_options);
  (void)mlir::applyPatternsAndFoldGreedily(funcOp, std::move(patterns));

  // Ensure we drop the marker in the end.
  funcOp.walk([](mlir::linalg::LinalgOp op) {
    op->removeAttr(mlir::linalg::LinalgTransforms::kLinalgTransformMarker);
  });
}

void LinalgTensorCodegenDriverPass::runOpAnchoredStrategy(FuncOp funcOp) {
  if (anchorOpName.empty()) return;

  if (fuse) return fuseAll(funcOp);
  if (fuseFillIntoReduction) return fuseOutputIntoReduction(funcOp);

  // Set up tiling and vectorization options.
  LinalgTilingOptions tilingOptions;
  if (!tileSizes.empty()) tilingOptions = tilingOptions.setTileSizes(tileSizes);
  if (!tileInterchange.empty())
    tilingOptions = tilingOptions.setInterchange(
        SmallVector<unsigned>(tileInterchange.begin(), tileInterchange.end()));
  if (scalarizeDynamicDims)
    tilingOptions = tilingOptions.scalarizeDynamicDims();
  tilingOptions = tilingOptions.setPeeledLoops(peeledLoops);

  // Set up padding options.
  // TODO: Replace the lambdas by either functions defined in MLIR core or even
  // adapt the LinalgPaddingOptions to take the `hoistPaddings` and
  // `packPaddings` arrays directly.
  auto packFunc = [&](OpOperand &opOperand) {
    return opOperand.getOperandNumber() < packPaddings.size()
               ? packPaddings[opOperand.getOperandNumber()]
               : false;
  };
  auto hoistingFunc = [&](OpOperand &opOperand) {
    return opOperand.getOperandNumber() < hoistPaddings.size()
               ? hoistPaddings[opOperand.getOperandNumber()]
               : 0;
  };
  LinalgPaddingOptions paddingOptions;
  paddingOptions.setPaddingValueComputationFunction(getNeutralOfLinalgOp);
  paddingOptions.setPaddingNoFoldComputationFunction(packFunc);
  paddingOptions.setPaddingHoistComputationFunction(hoistingFunc);

  CodegenStrategy strategy;
  StringRef genericOpName = GenericOp::getOperationName();
  strategy
      .tileIf(!tileSizes.empty() || scalarizeDynamicDims, anchorOpName,
              tilingOptions)
      .padIf(pad, anchorOpName, paddingOptions)
      .generalizeIf(generalize, anchorOpName)
      // TODO: decomposeToLowerDimIf when the need arises.
      .interchangeIf(!iteratorInterchange.empty(), iteratorInterchange)
      .vectorizeIf(vectorize, generalize ? genericOpName : anchorOpName,
                   nullptr, vectorizePadding);

  // Created a nested OpPassManager and run.
  OpPassManager dynamicPM("builtin.func");
  strategy.configurePassPipeline(dynamicPM, funcOp.getContext());
  if (failed(runPipeline(dynamicPM, funcOp))) return signalPassFailure();
}

void LinalgTensorCodegenDriverPass::runComprehensiveBufferization() {
  OpPassManager dynamicPM("builtin.module");
  dynamicPM.addPass(createCanonicalizerPass());
  dynamicPM.addPass(createCSEPass());
  dynamicPM.addPass(createLinalgComprehensiveModuleBufferizePass());
  if (failed(runPipeline(dynamicPM, getOperation())))
    return signalPassFailure();
}

void LinalgTensorCodegenDriverPass::runVectorLowering() {
  vector::VectorTransposeLowering vectorTransposeLowering =
      llvm::StringSwitch<vector::VectorTransposeLowering>(
          lowerVectorTransposeTo.getValue())
          .Case("eltwise", vector::VectorTransposeLowering::EltWise)
          .Case("flat_transpose", vector::VectorTransposeLowering::Flat)
          .Case("shuffle", vector::VectorTransposeLowering::Shuffle)
          .Default(vector::VectorTransposeLowering::EltWise);
  vector::VectorMultiReductionLowering vectorMultiReductionLowering =
      llvm::StringSwitch<vector::VectorMultiReductionLowering>(
          lowerVectorMultiReductionTo.getValue())
          .Case("innerreduction",
                vector::VectorMultiReductionLowering::InnerReduction)
          .Default(vector::VectorMultiReductionLowering::InnerParallel);
  vector::VectorContractLowering vectorContractLowering =
      llvm::StringSwitch<vector::VectorContractLowering>(
          lowerVectorContractionTo.getValue())
          .Case("matrixintrinsics", vector::VectorContractLowering::Matmul)
          .Case("dot", vector::VectorContractLowering::Dot)
          .Case("outerproduct", vector::VectorContractLowering::OuterProduct)
          .Default(vector::VectorContractLowering::OuterProduct);
  vector::VectorTransferSplit vectorTransferSplit =
      llvm::StringSwitch<vector::VectorTransferSplit>(
          splitVectorTransfersTo.getValue())
          .Case("none", vector::VectorTransferSplit::None)
          .Case("linalg-copy", vector::VectorTransferSplit::LinalgCopy)
          .Case("vector-transfers", vector::VectorTransferSplit::VectorTransfer)
          .Default(vector::VectorTransferSplit::None);

  // Per-function lowering pipeline.
  getOperation().walk([&](FuncOp funcOp) {
    vector::VectorTransformsOptions vectorTransformOptions =
        vector::VectorTransformsOptions()
            .setVectorTransposeLowering(vectorTransposeLowering)
            .setVectorTransformsOptions(vectorContractLowering)
            .setVectorMultiReductionLowering(vectorMultiReductionLowering)
            .setVectorTransferSplit(vectorTransferSplit);
    VectorTransferToSCFOptions vectorTransferToSCFOptions =
        VectorTransferToSCFOptions()
            .enableFullUnroll(unrollVectorTransfers)
            .enableLowerPermutationMaps();

    LinalgVectorLoweringOptions vectorLoweringOptions =
        LinalgVectorLoweringOptions()
            // Lowering of vector contractions.
            .enableContractionLowering(vectorLoweringStage >= 0)
            // Lowering of vector multi_reduction.
            .enableMultiReductionLowering(vectorLoweringStage >= 1)
            // Whether to split full/partial vector.transfer ops.
            .enableTransferPartialRewrite(vectorLoweringStage >= 2 &&
                                          vectorTransferSplit !=
                                              vector::VectorTransferSplit::None)
            // Set the maximum vector load / store rank.
            .setMaxTransferRank(maxTransferRank)
            // Lower vector.transfer to vector.transfer of max rank.
            .enableTransferLowering(vectorLoweringStage >= 3)
            // Conversion to scf.
            .enableTransferToSCFConversion(vectorLoweringStage >= 4)
            .setVectorTransferToSCFOptions(vectorTransferToSCFOptions)
            // Lowering of vector.shape_cast.
            .enableShapeCastLowering(vectorLoweringStage >= 5)
            // Lowering of vector.transpose.
            .enableVectorTransposeLowering(vectorLoweringStage >= 6)
            .setVectorTransformsOptions(vectorTransformOptions)
            .enableAVX2Lowering(lowerVectorTransposeToAVX2)
            .setAVX2LoweringOptions(
                x86vector::avx2::LoweringOptions().setTransposeOptions(
                    x86vector::avx2::TransposeLoweringOptions()
                        .lower4x8xf32(lowerVectorTransposeToAVX2)
                        .lower8x8xf32(lowerVectorTransposeToAVX2)));

    CodegenStrategy strategy;
    strategy.vectorLowering(vectorLoweringOptions);
    // Created a nested OpPassManager and run.
    OpPassManager dynamicPM("builtin.func");
    strategy.configurePassPipeline(dynamicPM, funcOp.getContext());
    if (failed(runPipeline(dynamicPM, funcOp))) return signalPassFailure();
  });
}

void LinalgTensorCodegenDriverPass::runOnOperation() {
  if (!anchorFuncOpName.empty()) {
    getOperation().walk([&](FuncOp funcOp) {
      if (funcOp.getName() != anchorFuncOpName) return;

      // Run transforms that require anchoring on a particular op. This only
      // applies if !anchorOpName.empty().
      runOpAnchoredStrategy(funcOp);
    });
  }

  // TODO: atm this is applied to all supported ops. If/when we need finer
  // control this should be exposed with an opName + filter and a proper
  // pattern.
  if (decomposeToLowerDimOp) {
    OpPassManager dynamicPM("builtin.module");
    OpPassManager &nestedDynamicPM = dynamicPM.nest<FuncOp>();
    nestedDynamicPM.addPass(createLinalgStrategyDecomposePass());
    if (failed(runPipeline(dynamicPM, getOperation())))
      return signalPassFailure();
  }

  if (bufferize) {
    runComprehensiveBufferization();
    // Perform buffer-level hoistings.
    getOperation().walk(
        [&](FuncOp funcOp) { hoistRedundantVectorTransfers(funcOp); });
  }

  if (vectorLowering) runVectorLowering();

  if (llvmLowering) runLowerToLLVM();
}

/// Return the dialect that must be loaded in the context before this pass.
void LinalgTensorCodegenDriverPass::getDependentDialects(
    DialectRegistry &registry) const {
  registry.insert<arith::ArithmeticDialect>();
  registry.insert<AffineDialect>();
  registry.insert<linalg::LinalgDialect>();
  registry.insert<memref::MemRefDialect>();
  registry.insert<scf::SCFDialect>();
  registry.insert<StandardOpsDialect>();
  registry.insert<tensor::TensorDialect>();
  registry.insert<vector::VectorDialect>();

  linalg::comprehensive_bufferize::
      registerBufferizableOpInterfaceExternalModels(registry);
  linalg::comprehensive_bufferize::linalg_ext::
      registerBufferizableOpInterfaceExternalModels(registry);
}

std::unique_ptr<OperationPass<ModuleOp>>
mlir::createLinalgTensorCodegenDriverPass() {
  return std::make_unique<LinalgTensorCodegenDriverPass>();
}
