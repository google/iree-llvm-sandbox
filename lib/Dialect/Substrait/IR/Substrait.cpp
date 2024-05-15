//===-- Substrait.cpp - Substrait dialect -----------------------*- C++ -*-===//
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "structured/Dialect/Substrait/IR/Substrait.h"

#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/Support/LogicalResult.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;
using namespace mlir::substrait;

//===----------------------------------------------------------------------===//
// Substrait dialect
//===----------------------------------------------------------------------===//

#include "structured/Dialect/Substrait/IR/SubstraitOpsDialect.cpp.inc"

void SubstraitDialect::initialize() {
#define GET_OP_LIST
  addOperations<
#include "structured/Dialect/Substrait/IR/SubstraitOps.cpp.inc"
      >();
  addTypes<
#define GET_TYPEDEF_LIST
#include "structured/Dialect/Substrait/IR/SubstraitOpsTypes.cpp.inc"
      >();
}

//===----------------------------------------------------------------------===//
// Substrait interfaces
//===----------------------------------------------------------------------===//

#include "structured/Dialect/Substrait/IR/SubstraitOpInterfaces.cpp.inc"
#include "structured/Dialect/Substrait/IR/SubstraitTypeInterfaces.cpp.inc"

//===----------------------------------------------------------------------===//
// Substrait operations
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "structured/Dialect/Substrait/IR/SubstraitOps.cpp.inc"

namespace mlir {
namespace substrait {

LogicalResult
CrossOp::inferReturnTypes(MLIRContext *context, std::optional<Location> loc,
                          ValueRange operands, DictionaryAttr attributes,
                          OpaqueProperties properties, RegionRange regions,
                          llvm::SmallVectorImpl<Type> &inferredReturnTypes) {
  Value leftInput = operands[0];
  Value rightInput = operands[0];

  TypeRange leftFieldTypes = cast<TupleType>(leftInput.getType()).getTypes();
  TypeRange rightFieldTypes = cast<TupleType>(rightInput.getType()).getTypes();

  SmallVector<mlir::Type> fieldTypes;
  llvm::append_range(fieldTypes, leftFieldTypes);
  llvm::append_range(fieldTypes, rightFieldTypes);
  auto resultType = TupleType::get(context, fieldTypes);

  inferredReturnTypes.emplace_back(resultType);

  return success();
}

/// Verifies that the provided field names match the provided field types. While
/// the field types are potentially nested, the names are given in a single,
/// flat list and correspond to the field types in depth first order (where each
/// nested tuple-typed field has a name and its nested field have names on their
/// own). Furthermore, the names on each nesting level need to be unique. For
/// details, see
/// https://substrait.io/tutorial/sql_to_substrait/#types-and-schemas.
FailureOr<int> verifyNamedStruct(Location loc,
                                 llvm::ArrayRef<Attribute> fieldNames,
                                 TypeRange fieldTypes) {
  int numConsumedNames = 0;
  llvm::SmallSet<llvm::StringRef, 8> currentLevelNames;
  for (Type type : fieldTypes) {
    // Check name of current field.
    if (numConsumedNames >= static_cast<int>(fieldNames.size()))
      return emitError(loc, "not enough field names provided");
    auto currentName = llvm::cast<StringAttr>(fieldNames[numConsumedNames]);
    if (!currentLevelNames.insert(currentName).second)
      return emitError(loc, llvm::Twine("duplicate field name: '") +
                                currentName.getValue() + "'");
    numConsumedNames++;

    // Recurse for nested structs/tuples.
    if (auto tupleType = llvm::dyn_cast<TupleType>(type)) {
      llvm::ArrayRef<Type> nestedFieldTypes = tupleType.getTypes();
      llvm::ArrayRef<Attribute> remainingNames =
          fieldNames.drop_front(numConsumedNames);
      FailureOr<int> res =
          verifyNamedStruct(loc, remainingNames, nestedFieldTypes);
      if (failed(res))
        return failure();
      numConsumedNames += res.value();
    }
  }
  return numConsumedNames;
}

LogicalResult NamedTableOp::verify() {
  Location loc = getLoc();
  llvm::ArrayRef<Attribute> fieldNames = getFieldNames().getValue();
  auto tupleType = llvm::cast<TupleType>(getResult().getType());
  TypeRange fieldTypes = tupleType.getTypes();

  // Emits error message with context on failure.
  auto emitErrorMessage = [&]() {
    InFlightDiagnostic error = ::emitError(loc)
                               << "mismatching 'field_names' ([";
    llvm::interleaveComma(fieldNames, error);
    error << "]) and result type (" << tupleType << ")";
    return error;
  };

  // Call recursive verification function.
  FailureOr<int> numConsumedNames =
      verifyNamedStruct(loc, fieldNames, fieldTypes);

  // Relay any failure.
  if (failed(numConsumedNames))
    return emitErrorMessage();

  // If we haven't consumed all names, we got too many of them, so report.
  if (numConsumedNames.value() != static_cast<int>(getFieldNames().size())) {
    InFlightDiagnostic error = emitErrorMessage();
    error.attachNote(loc) << "too many field names provided";
    return error;
  }

  return success();
}

} // namespace substrait
} // namespace mlir

//===----------------------------------------------------------------------===//
// Substrait types
//===----------------------------------------------------------------------===//

#define GET_TYPEDEF_CLASSES
#include "structured/Dialect/Substrait/IR/SubstraitOpsTypes.cpp.inc"
