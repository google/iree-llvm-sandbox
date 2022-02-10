// RUN: mlir-proto-opt %s -split-input-file -verify-diagnostics

linalg_transform.sequence {
  %0 = match @match
  // expected-error@below {{result #0 has more than one use}}
  %1 = tile %0
  // expected-note@below {{used here as operand #0}}
  tile %1
  // expected-note@below {{used here as operand #0}}
  vectorize %1
}

// -----

linalg_transform.sequence {
  %0 = match @match
  // expected-error@below {{expects transpose paddings to be a permutation, found [2, 0]}}
  tile %0 {pad = true, transpose_paddings = [[0, 1], [2, 0]]}
}

// -----

linalg_transform.sequence {
  %0 = match @match
  // expected-error@below {{"sizes" and "scalarize_dyn_dims" attributes are mutually exclusive}}
  tile %0 {sizes = [1,2,3], scalarize_dyn_dims = true}
}
