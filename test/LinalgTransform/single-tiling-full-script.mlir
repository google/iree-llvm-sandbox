// RUN: mlir-proto-opt -linalg-interp-transforms %s | FileCheck %s

// CHECK-LABEL: func @matmul_tensors
// CHECK-NOT: linalg
// CHECK: llvm
func @matmul_tensors(
  %arg0: tensor<128x128xf32>, %arg1: tensor<128x128xf32>, %arg2: tensor<128x128xf32> { linalg.inplaceable = true})
    -> tensor<128x128xf32> {
  %0 = linalg.matmul  ins(%arg0, %arg1: tensor<128x128xf32>, tensor<128x128xf32>)
                     outs(%arg2: tensor<128x128xf32>)
    -> tensor<128x128xf32>

  return %0 : tensor<128x128xf32>
}


pdl.pattern @pdl_target : benefit(1) {
  %args = pdl.operands
  %results = pdl.types
  %0 = pdl.operation "linalg.matmul"(%args : !pdl.range<value>) -> (%results : !pdl.range<type>)
  pdl.apply_native_constraint "nestedInFunc"[@matmul_tensors](%0 : !pdl.operation)
  // TODO: this should be implicit, injected by the driver or have PDL only do the matching
  // FIXME: not putting [] here crashes the pdl-to-pdl-interp
  pdl.apply_native_constraint "notTagged"[](%0 : !pdl.operation)
  // TODO: we don't want this, but it is the required terminator for pdl.pattern
  pdl.rewrite %0 with "linalg_transform.apply"
}

linalg_transform.apply {
^bb0(%arg0: !pdl.operation):
  linalg_transform.sequence {
    %0 = tile %arg0 {sizes = [4, 4, 4]}
    %1 = vectorize %0 {vectorize_padding = true}
    bufferize
    lower_vectors { multireduction_lowering = "innerreduce"}
    lower_to_llvm
  }
} when {
  linalg_transform.pdl_match @pdl_target
}
