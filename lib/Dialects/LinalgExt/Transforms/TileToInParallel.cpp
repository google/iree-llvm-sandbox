//===- TileToInParallel.cpp.cpp - Rewrite TileOp as InParallel -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <mlir/IR/BuiltinOps.h>

#include "Dialects/LinalgExt/LinalgExtOps.h"
#include "Dialects/LinalgExt/PassDetail.h"
#include "Dialects/LinalgExt/Passes.h"
#include "Transforms/Utils.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/Identifier.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/STLExtras.h"

using namespace mlir;
using namespace mlir::linalg_ext;

namespace {

struct TileOpToInParallelRewriter
    : public OpRewritePattern<linalg_ext::TileOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(linalg_ext::TileOp tileOp,
                                PatternRewriter &rewriter) const override {
    // TODO: verifier.
    assert(tileOp.getNumResults() > 0 &&
           tileOp.outs().size() == tileOp.getNumResults());

    // TODO: when supported, iterate over the tensor of sizes. This will be
    // iterating through a level of indirection.

    // Construct the loop bounds based on the canonical arithmetic progression.
    Location loc = tileOp.getLoc();
    Value zero = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value one = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Value totalSize =
        rewriter.create<tensor::DimOp>(loc, tileOp.outs().front(), zero);
    Value step = tileOp.tile_sizes();
    assert(step.getType().isa<IndexType>() && "NYI: not an index type");

    using AV = AffineValueExpr;
    AffineBuilder ab(rewriter, loc);
    AffineExpr i, j, M;
    bindDims(rewriter.getContext(), i, j);
    bindSymbols(rewriter.getContext(), M);
    Value numThreads = ab.ceil(AV(i).bind(totalSize), AV(M).bind(step));

    // Construct the op without a body builder: we need to clone the ops in the
    // body explicitly after having access to the new bbArgs.
    // As a consequence, `ensureTerminator` is not called and the body has no
    // terminator.
    linalg_ext::InParallelOp inParallelOp =
        rewriter.create<linalg_ext::InParallelOp>(loc, tileOp->getResultTypes(),
                                                  numThreads);

    // At the beginning of the InParallelOp, compute offset and sizes.
    rewriter.setInsertionPointToStart(inParallelOp.getBody());

    // Materialize the implicit subtensors as explicit subset_extract.
    // TODO: generalize to multiple offset/chunk_size bbargs if needed.
    // TODO: generalize the subset op.
    Value offset =
        ab.mul(AV(i).bind(inParallelOp.getThreadIndex()), AV(M).bind(step));
    // clang-format off
    Value size = ab.min(
      ValueRange{ab.sub(AV(i).bind(totalSize), AV(j).bind(offset)), 
      step});
    // clang-format on

    SmallVector<Value> implicitSubtensorExtracts;
    for (Value tensor : tileOp.outs()) {
      implicitSubtensorExtracts.push_back(
          rewriter.createOrFold<tensor::ExtractSliceOp>(loc, tensor, offset,
                                                        size, one));
    }

    // Get a reference to the TileOp terminator before the body is merged and it
    // becomes too hard to get to the terminator.
    auto tileYieldOp = cast<TileYieldOp>(tileOp.getBody()->getTerminator());

    // Regroup the values that replace the tileOp's bbArg and move the body.
    SmallVector<Value> bbArgsTranslated{offset, size};
    llvm::append_range(bbArgsTranslated, implicitSubtensorExtracts);
    rewriter.mergeBlockBefore(&tileOp.region().front(),
                              inParallelOp.getBody()->getTerminator(),
                              bbArgsTranslated);

    // tileOp's terminator is not the terminator, insert explicit subset_insert
    // ops and feed them to a new scf.yield terminator that we can now add.
    PerformConcurrentlyOp performConcurrentlyOp = inParallelOp.getTerminator();

    rewriter.setInsertionPointToStart(performConcurrentlyOp.getBody());
    for (auto it : llvm::zip(tileYieldOp->getOperands(), tileOp.outs())) {
      rewriter.createOrFold<ParallelInsertSliceOp>(
          loc, std::get<0>(it), std::get<1>(it), offset, size, one);
    }

    // // Cleanup and replace.
    rewriter.eraseOp(tileYieldOp);
    rewriter.replaceOp(tileOp, inParallelOp.getResults());

    return success();
  }
};

struct TileToInParallelPass
    : public TileToInParallelBase<TileToInParallelPass> {
  void runOnOperation() override;
};
} // namespace

void TileToInParallelPass::runOnOperation() {
  FuncOp funcOp = getOperation();
  MLIRContext *context = funcOp.getContext();
  RewritePatternSet patterns(context);
  patterns.insert<TileOpToInParallelRewriter>(context);
  (void)applyPatternsAndFoldGreedily(funcOp, std::move(patterns));
}

std::unique_ptr<OperationPass<FuncOp>>
mlir::linalg_ext::createTileToInParallelPass() {
  return std::make_unique<TileToInParallelPass>();
}
