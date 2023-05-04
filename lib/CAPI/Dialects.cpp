//===- Dialects.cpp - CAPI for dialects -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "structured-c/Dialects.h"

#include "mlir-c/BuiltinTypes.h"
#include "mlir-c/IR.h"
#include "mlir/CAPI/IR.h"
#include "mlir/CAPI/Registration.h"
#include "mlir/CAPI/Support.h"
#include "mlir/IR/Types.h"
#include "structured/Dialect/Indexing/IR/Indexing.h"
#include "structured/Dialect/Iterators/IR/Iterators.h"
#include "structured/Dialect/Tabular/IR/Tabular.h"
#include "structured/Dialect/Tuple/IR/Tuple.h"

using namespace mlir;
using namespace mlir::indexing;
using namespace mlir::iterators;
using namespace mlir::tabular;
using namespace mlir::tuple;

//===----------------------------------------------------------------------===//
// Indexing dialect and attributes
//===----------------------------------------------------------------------===//

MLIR_DEFINE_CAPI_DIALECT_REGISTRATION(Indexing, indexing, IndexingDialect)

bool mlirTypeIsAIndexingCustom(MlirType type) {
  return unwrap(type).isa<CustomType>();
}

MlirType mlirIndexingCustomTypeGet(MlirContext ctx, MlirStringRef str) {
  return wrap(CustomType::get(unwrap(ctx), unwrap(str)));
}

bool mlirIsATensorValue(MlirValue value) {
  return mlirTypeIsATensor(mlirValueGetType(value));
}

// Collects together checking for all conventional floats, integers,
// and index types. Roughly corresponds to the union of
// _is_floating_point_type, _is_integer_type, and _is_index_type in
// mlir/dialects/linalg/opdsl/lang/emitter.py.
bool mlirIsAScalarValue(MlirValue value) {
  MlirType type = mlirValueGetType(value);
  return mlirTypeIsABF16(type) || mlirTypeIsAComplex(type) ||
         mlirTypeIsAF16(type) || mlirTypeIsAF32(type) || mlirTypeIsAF64(type) ||
         mlirTypeIsAInteger(type) || mlirTypeIsAIndex(type);
}

//===----------------------------------------------------------------------===//
// Iterators dialect and types
//===----------------------------------------------------------------------===//

MLIR_DEFINE_CAPI_DIALECT_REGISTRATION(Iterators, iterators, IteratorsDialect)

bool mlirTypeIsAIteratorsStreamType(MlirType type) {
  return unwrap(type).isa<StreamType>();
}

MlirType mlirIteratorsStreamTypeGet(MlirContext context, MlirType elementType) {
  return wrap(StreamType::get(unwrap(context), unwrap(elementType)));
}

//===----------------------------------------------------------------------===//
// Tabular dialect and types
//===----------------------------------------------------------------------===//

MLIR_DEFINE_CAPI_DIALECT_REGISTRATION(Tabular, tabular, TabularDialect)

/// Checks whether the given type is a tabular view type.
bool mlirTypeIsATabularView(MlirType type) {
  return unwrap(type).isa<TabularViewType>();
}

/// Creates a tabular view type that consists of the given list of column types.
/// The type is owned by the context.
MlirType mlirTabularViewTypeGet(MlirContext ctx, intptr_t numColumns,
                                MlirType const *columnTypes) {
  SmallVector<Type, 4> types;
  ArrayRef<Type> typesRef = unwrapList(numColumns, columnTypes, types);
  return wrap(TabularViewType::get(unwrap(ctx), typesRef));
}

/// Returns the number of types contained in a tabular view.
intptr_t mlirTabularViewTypeGetNumColumnTypes(MlirType type) {
  return unwrap(type).cast<TabularViewType>().getColumnTypes().size();
}

/// Returns the pos-th type in the tabular view type.
MlirType mlirTabularViewTypeGetColumnType(MlirType type, intptr_t pos) {
  return wrap(unwrap(type)
                  .cast<TabularViewType>()
                  .getColumnTypes()[static_cast<size_t>(pos)]);
}

MlirType mlirTabularViewTypeGetRowType(MlirType type) {
  return wrap(unwrap(type).cast<TabularViewType>().getRowType());
}

//===----------------------------------------------------------------------===//
// Tuple dialect and attributes
//===----------------------------------------------------------------------===//

MLIR_DEFINE_CAPI_DIALECT_REGISTRATION(Tuple, tuple, TupleDialect)
