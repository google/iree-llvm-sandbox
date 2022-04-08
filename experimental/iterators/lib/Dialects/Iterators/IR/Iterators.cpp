//===-- IteratorsDialect.cpp - Iterators dialect ----------------*- C++ -*-===//
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "iterators/Dialect/Iterators/IR/Iterators.h"

#include "mlir/IR/DialectImplementation.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace mlir::iterators;

//===----------------------------------------------------------------------===//
// Iterators dialect
//===----------------------------------------------------------------------===//

#include "iterators/Dialect/Iterators/IR/IteratorsOpsDialect.cpp.inc"

void IteratorsDialect::initialize() {
#define GET_OP_LIST
  addOperations<
#include "iterators/Dialect/Iterators/IR/IteratorsOps.cpp.inc"
      >();
  addTypes<
#define GET_TYPEDEF_LIST
#include "iterators/Dialect/Iterators/IR/IteratorsOpsTypes.cpp.inc"
      >();
}

//===----------------------------------------------------------------------===//
// Iterators interfaces
//===----------------------------------------------------------------------===//

#include "iterators/Dialect/Iterators/IR/IteratorsOpInterfaces.cpp.inc"
#include "iterators/Dialect/Iterators/IR/IteratorsTypeInterfaces.cpp.inc"

//===----------------------------------------------------------------------===//
// Iterators operations
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "iterators/Dialect/Iterators/IR/IteratorsOps.cpp.inc"

LogicalResult OpenOp::verify() {
  if (inputState().getType() != resultState().getType()) {
    return emitOpError() << "Type mismatch: Opening iterator of type "
                         << inputState().getType()
                         << " should return the same type but returns "
                         << resultState().getType();
  }
  return success();
}

LogicalResult NextOp::verify() {
  // Check matching state types
  if (inputState().getType() != resultState().getType()) {
    return emitOpError()
           << "Type mismatch: Consuming an element of an iterator of type "
           << inputState().getType()
           << " should return in an iterator of the same type but returns "
           << resultState().getType();
  }

  // Check matching tuple type
  IteratorInterface iteratorType =
      inputState().getType().dyn_cast<IteratorInterface>();
  assert(iteratorType);
  if (iteratorType.getElementType() != nextElement().getType()) {
    return emitOpError()
           << "Type mismatch: Element returned by iterator of type "
           << inputState().getType() << " should be "
           << iteratorType.getElementType() << " but is "
           << nextElement().getType();
  }

  return success();
}

LogicalResult CloseOp::verify() {
  if (inputState().getType() != resultState().getType()) {
    return emitOpError() << "Type mismatch: Closing iterator of type "
                         << inputState().getType()
                         << " should return the same type but returns "
                         << resultState().getType();
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Iterators types
//===----------------------------------------------------------------------===//

#define GET_TYPEDEF_CLASSES
#include "iterators/Dialect/Iterators/IR/IteratorsOpsTypes.cpp.inc"
