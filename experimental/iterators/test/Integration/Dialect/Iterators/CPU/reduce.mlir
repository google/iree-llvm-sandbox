// RUN: mlir-proto-opt %s \
// RUN:   -convert-iterators-to-llvm \
// RUN:   -convert-func-to-llvm \
// RUN:   -convert-scf-to-cf -convert-cf-to-llvm \
// RUN: | mlir-cpu-runner -e main -entry-point-result=void \
// RUN: | FileCheck %s

!tupleType = type !llvm.struct<(i32)>

func @main() {
  %input = "iterators.sampleInput"() : () -> (!iterators.stream<!tupleType>)
  %reduce = "iterators.reduce"(%input) : (!iterators.stream<!tupleType>) -> (!iterators.stream<!tupleType>)
  "iterators.sink"(%reduce) : (!iterators.stream<!tupleType>) -> ()
  // CHECK:      (6)
  return
}
