//===-- TritonSPMDToFuncArgs.cpp - Triton SPMD ops to func args -*- C++ -*-===//
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "structured/Conversion/TritonSPMDToFuncArgs/TritonSPMDToFuncArgs.h"

#include "../PassDetail.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Transforms/DialectConversion.h"
#include "triton/Dialect/Triton/IR/Dialect.h"

namespace mlir {
class MLIRContext;
} // namespace mlir

using namespace mlir;
using namespace mlir::func;
using namespace triton;

namespace {
struct ConvertTritonSPMDToFuncArgsPass
    : public ConvertTritonSPMDToFuncArgsBase<ConvertTritonSPMDToFuncArgsPass> {
  void runOnOperation() override;
};

struct GetNumProgramsOpConversion
    : public OpRewritePattern<triton::GetNumProgramsOp> {
  GetNumProgramsOpConversion(MLIRContext *context, PatternBenefit benefit = 1)
      : OpRewritePattern(context, benefit) {}

  LogicalResult matchAndRewrite(triton::GetNumProgramsOp op,
                                PatternRewriter &rewriter) const override {
    auto funcOp = op->getParentOfType<FunctionOpInterface>();
    Block::BlockArgListType funcArgs =
        funcOp.getFunctionBody().front().getArguments();
    uint32_t dim = op.getAxis();
    Value programIdArg = funcArgs[dim + 3];
    rewriter.replaceOp(op, programIdArg);
    return success();
  }
};

struct GetProgramIdOpConversion
    : public OpRewritePattern<triton::GetProgramIdOp> {
  GetProgramIdOpConversion(MLIRContext *context, PatternBenefit benefit = 1)
      : OpRewritePattern(context, benefit) {}

  LogicalResult matchAndRewrite(triton::GetProgramIdOp op,
                                PatternRewriter &rewriter) const override {
    auto funcOp = op->getParentOfType<FunctionOpInterface>();
    Block::BlockArgListType funcArgs =
        funcOp.getFunctionBody().front().getArguments();
    uint32_t dim = op.getAxis();
    Value programIdArg = funcArgs[dim];
    rewriter.replaceOp(op, programIdArg);
    return success();
  }
};

struct TrivialPatternRewriter : public PatternRewriter {
public:
  explicit TrivialPatternRewriter(MLIRContext *context)
      : PatternRewriter(context) {}
};

void addGridArguments(FunctionOpInterface op, PatternRewriter &rewriter) {
  MLIRContext *context = rewriter.getContext();

  // New argument types: add two arguments for program_id and num_programs.
  Type i32 = rewriter.getI32Type();
  ArrayRef<Type> gridArgs{i32, i32, i32,  // program_id per dimension
                          i32, i32, i32}; // num_programs per dimension

  // Compute new function type: add new arguments at the beginning.
  TypeRange oldArgTypes = op.getArgumentTypes();
  SmallVector<Type> newArgTypes(gridArgs.begin(), gridArgs.end());
  newArgTypes.append(oldArgTypes.begin(), oldArgTypes.end());
  TypeRange resultTypes = op->getResultTypes();
  auto newType =
      FunctionType::get(rewriter.getContext(), newArgTypes, resultTypes);

  // Compute new argument attributes: add two empty dicts for the new args.
  SmallVector<DictionaryAttr> newArgAttrs(gridArgs.size(),
                                          DictionaryAttr::get(context));
  op.getAllArgAttrs(newArgAttrs);

  // Update function type and argument attributes in-place.
  op.setType(newType);
  op.setAllArgAttrs(newArgAttrs);

  // Compute new arguments for the entry block of the body.
  Block *oldBlock = &op.getFunctionBody().front();
  SmallVector<Location> locs(gridArgs.size(), op.getLoc());
  for (BlockArgument arg : oldBlock->getArguments()) {
    locs.push_back(arg.getLoc());
  }

  // Create a new block with the new signature and merge the old one into it.
  llvm::errs() << "locs: "
               << "\n"
               << "size: " << locs.size() << "\n";
  for (auto loc : locs) {
    llvm::errs() << "  ";
    llvm::errs().flush();
    loc.dump();
  }
  llvm::errs() << "newArgTypes: "
               << "\n"
               << "size: " << newArgTypes.size() << "\n";
  for (auto type : newArgTypes) {
    llvm::errs() << "  ";
    llvm::errs().flush();
    type.dump();
  }
  llvm::errs() << "oldBlock: " << oldBlock << "\n";
  oldBlock->dump();

  Block *newBlock = rewriter.createBlock(oldBlock, newArgTypes, locs);

  llvm::errs() << "newBlock: " << newBlock << "\n";
  newBlock->dump();

  oldBlock->replaceAllUsesWith(newBlock);
  ArrayRef<BlockArgument> forwardArgs =
      newBlock->getArguments().drop_front(gridArgs.size());
  rewriter.mergeBlocks(oldBlock, newBlock, forwardArgs);
}
} // namespace

void mlir::populateTritonSPMDToFuncArgsConversionPatterns(
    RewritePatternSet &patterns) {
  patterns.add<
      // clang-format off
      GetNumProgramsOpConversion,
      GetProgramIdOpConversion
      // clang-format on
      >(patterns.getContext());
}

void ConvertTritonSPMDToFuncArgsPass::runOnOperation() {
  auto module = getOperation();
  MLIRContext *context = &getContext();

  // Add grid arguments to all functions.
  SmallVector<FunctionOpInterface> funcOps;
  TrivialPatternRewriter rewriter(context);
  for (auto &op : module.getBodyRegion().front()) {
    if (auto funcOp = llvm::dyn_cast<FunctionOpInterface>(&op)) {
      if (!funcOp.isExternal()) {
        addGridArguments(funcOp, rewriter);
        funcOps.push_back(funcOp);
      }
    }
  }

  for (auto op : funcOps) {

    if (!llvm::isa<func::FuncOp>(op))
      return;
    rewriter.setInsertionPointAfter(op);

    auto oldArgTypes = op.getArgumentTypes().drop_front(6);
    auto resultTypes = op->getResultTypes();
    SmallVector<DictionaryAttr> newArgAttrs;
    op.getAllArgAttrs(newArgAttrs);

    Location loc = op->getLoc();
    auto oldFuncType = FunctionType::get(context, oldArgTypes, resultTypes);
    auto gridFuncOp =
        rewriter.create<func::FuncOp>(loc, StringRef{"grid"}, oldFuncType);
    auto oldArgAttrs = ArrayRef<DictionaryAttr>(newArgAttrs).drop_front(6);
    gridFuncOp.setAllArgAttrs(oldArgAttrs);

    Block *entryBlock = gridFuncOp.addEntryBlock();
    rewriter.setInsertionPointToStart(entryBlock);
    rewriter.create<func::ReturnOp>(loc);
  }

  // Convert the SPMD ops in the Triton dialect to accesses to the corresponding
  // function arguments.
  RewritePatternSet patterns(&getContext());
  populateTritonSPMDToFuncArgsConversionPatterns(patterns);

  // Mark the SPMD ops as illegal; everything else is legal.
  ConversionTarget target(getContext());
  target.addLegalDialect<TritonDialect>();
  target.addIllegalOp<triton::GetNumProgramsOp, triton::GetProgramIdOp>();
  target.addLegalOp<ModuleOp>();

  if (failed(applyPartialConversion(module, target, std::move(patterns))))
    signalPassFailure();
}

std::unique_ptr<OperationPass<ModuleOp>>
mlir::createConvertTritonSPMDToFuncArgsPass() {
  return std::make_unique<ConvertTritonSPMDToFuncArgsPass>();
}
