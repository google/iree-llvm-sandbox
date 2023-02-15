//===-- IteratorsDialect.h - Iterators dialect ------------------*- C++ -*-===//
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef ITERATORS_DIALECT_ITERATORS_IR_ITERATORS_H
#define ITERATORS_DIALECT_ITERATORS_IR_ITERATORS_H

#include "iterators/Dialect/Tabular/IR/Tabular.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"

#include "iterators/Dialect/Iterators/IR/IteratorsOpsDialect.h.inc"

namespace mlir {
namespace LLVM {
class LLVMPointerType;
} // namespace LLVM
} // namespace mlir

namespace mlir {
namespace iterators {
#include "iterators/Dialect/Iterators/IR/IteratorsOpInterfaces.h.inc"
#include "iterators/Dialect/Iterators/IR/IteratorsTypeInterfaces.h.inc"

} // namespace iterators
} // namespace mlir

#define GET_TYPEDEF_CLASSES
#include "iterators/Dialect/Iterators/IR/IteratorsOpsTypes.h.inc"

#define GET_OP_CLASSES
#include "iterators/Dialect/Iterators/IR/IteratorsOps.h.inc"

#endif // ITERATORS_DIALECT_ITERATORS_IR_ITERATORS_H
