// RUN: mlir-proto-opt %s -linalg-bufferization-driver -canonicalize | FileCheck %s

// CHECK-LABEL: func @parallel_insert_slice_no_conflict(
//  CHECK-SAME:     %[[idx:.*]]: index, %[[idx2:.*]]: index,
//  CHECK-SAME:     %[[arg1:.*]]: memref<?xf32, #{{.*}}>,
//  CHECK-SAME:     %[[arg2:.*]]: memref<?xf32, #{{.*}}>
func @parallel_insert_slice_no_conflict(
    %idx: index, %idx2: index,
    %arg1: tensor<?xf32> {linalg.inplaceable=true},
    %arg2: tensor<?xf32> {linalg.inplaceable=true}) -> (tensor<?xf32>, f32)
{
  %cst = arith.constant 4.200000e+01 : f32
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index

  // CHECK: linalg_ext.in_parallel %[[idx2]]  -> ()
  %2 = linalg_ext.in_parallel %idx2  -> (tensor<?xf32>) {
    ^bb0(%arg3: index):  // no predecessors
      // CHECK: %[[subview:.*]] = memref.subview %[[arg2]][5] [%[[idx]]] [1]
      %6 = tensor.extract_slice %arg2[5] [%idx] [%c1] : tensor<?xf32> to tensor<?xf32>
      // CHECK: linalg.fill(%{{.*}}, %[[subview]])
      %8 = linalg.fill (%cst, %6) : f32, tensor<?xf32> -> tensor<?xf32>

      // CHECK: linalg_ext.perform_concurrently
      // CHECK-NOT: parallel_insert_slice
      linalg_ext.perform_concurrently {
        linalg_ext.parallel_insert_slice %8 into %arg2[5] [%idx] [%c1] : tensor<?xf32> into tensor<?xf32>
      }
  }

  // CHECK: %[[load:.*]] = memref.load %[[arg2]]
  %f = tensor.extract %2[%c0] : tensor<?xf32>

  // CHECK: return %[[load]] : f32
  return %2, %f : tensor<?xf32>, f32
}

// -----

// CHECK-LABEL: func @parallel_insert_slice_with_conflict(
//  CHECK-SAME:     %[[idx:.*]]: index, %[[idx2:.*]]: index,
//  CHECK-SAME:     %[[arg1:.*]]: memref<?xf32, #{{.*}}>,
//  CHECK-SAME:     %[[arg2:.*]]: memref<?xf32, #{{.*}}>
func @parallel_insert_slice_with_conflict(
    %idx: index, %idx2: index,
    %arg1: tensor<?xf32> {linalg.inplaceable=true},
    %arg2: tensor<?xf32> {linalg.inplaceable=true}) -> (f32, f32)
{
  %cst = arith.constant 4.200000e+01 : f32
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index

  // The parallel_insert_slice_op bufferizes out-of-place, so we need an allocation.
  // CHECK: %[[alloc1:.*]] = memref.alloc
  // CHECK: linalg_ext.in_parallel %[[idx2]]  -> ()
  %2 = linalg_ext.in_parallel %idx2  -> (tensor<?xf32>) {
    ^bb0(%arg3: index):  // no predecessors
      // Another alloc for the extract_slice op.
      // CHECK: %[[alloc2:.*]] = memref.alloc
      // CHECK: %[[subview2:.*]] = memref.subview %[[arg2]][5] [%[[idx]]] [1]
      // CHECK: memref.copy %[[subview2]], %[[alloc2]]
      %6 = tensor.extract_slice %arg2[5] [%idx] [%c1] : tensor<?xf32> to tensor<?xf32>

      // CHECK: linalg.fill(%{{.*}}, %[[alloc2]])
      %8 = linalg.fill (%cst, %6) : f32, tensor<?xf32> -> tensor<?xf32>

      // parallel_insert_slice buffer was already allocated but not copied yet.
      // CHECK: memref.copy %[[arg2]], %[[alloc1]]

      // Now the copy of the actual insert_slice.
      // CHECK: %[[subview1:.*]] = memref.subview %[[alloc1]][5] [%[[idx]]] [1]
      // CHECK: memref.copy %[[alloc2]], %[[subview1]]
      // CHECK: memref.dealloc %[[alloc2]]

      // The terminator is empty.
      // CHECK: linalg_ext.perform_concurrently
      // CHECK-NOT: parallel_insert_slice
      linalg_ext.perform_concurrently {
        linalg_ext.parallel_insert_slice %8 into %arg2[5] [%idx] [%c1] : tensor<?xf32> into tensor<?xf32>
      }
  }

  // CHECK: %[[load:.*]] = memref.load %[[arg2]]
  // CHECK: %[[load2:.*]] = memref.load %[[alloc1]]
  // CHECK: memref.dealloc %[[alloc1]]
  %f = tensor.extract %arg2[%c0] : tensor<?xf32>
  %f2 = tensor.extract %2[%c0] : tensor<?xf32>

  // CHECK: return %[[load2]], %[[load]] : f32, f32
  return %f2, %f : f32, f32
}
