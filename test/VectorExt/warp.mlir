// RUN: mlir-proto-opt %s -allow-unregistered-dialect -split-input-file -test-vector-warp-distribute=propagate-distribution -canonicalize | FileCheck %s
// RUN: mlir-proto-opt %s -allow-unregistered-dialect -split-input-file -test-vector-warp-distribute=rewrite-warp-ops-to-scf-if -canonicalize | FileCheck %s --check-prefix=CHECK-SCF-IF

// CHECK-LABEL:   func @warp_dead_result(
func @warp_dead_result(%laneid: index) -> (vector<1xf32>) {
  // CHECK: %[[R:.*]] = vector_ext.warp_execute_on_lane_0(%{{.*}}) -> (vector<1xf32>)
    %r:3 = vector_ext.warp_execute_on_lane_0(%laneid) ->
    (vector<1xf32>, vector<1xf32>, vector<1xf32>) {
    %2 = "some_def"() : () -> (vector<32xf32>)
    %3 = "some_def"() : () -> (vector<32xf32>)
    %4 = "some_def"() : () -> (vector<32xf32>)
  // CHECK:   vector_ext.yield %{{.*}} : vector<32xf32>
    vector_ext.yield %2, %3, %4 : vector<32xf32>, vector<32xf32>, vector<32xf32>
  }
  // CHECK: return %[[R]] : vector<1xf32>
  return %r#1 : vector<1xf32>
}

// -----

// CHECK-LABEL:   func @warp_propagate_operand(
//  CHECK-SAME:   %[[ID:.*]]: index, %[[V:.*]]: vector<4xf32>)
func @warp_propagate_operand(%laneid: index, %v0: vector<4xf32>)
  -> (vector<4xf32>) {
  %r = vector_ext.warp_execute_on_lane_0(%laneid)
     args(%v0 : vector<4xf32>) -> (vector<4xf32>) {
     ^bb0(%arg0 : vector<128xf32>) :
    vector_ext.yield %arg0 : vector<128xf32>
  }
  // CHECK: return %[[V]] : vector<4xf32>
  return %r : vector<4xf32>
}

// -----

#map0 = affine_map<()[s0] -> (s0 * 2)>

// CHECK-LABEL:   func @warp_propagate_elementwise(
func @warp_propagate_elementwise(%laneid: index, %dest: memref<1024xf32>) {
  %c0 = arith.constant 0 : index
  %c32 = arith.constant 0 : index
  %cst = arith.constant 0.000000e+00 : f32
  // CHECK: %[[R:.*]]:4 = vector_ext.warp_execute_on_lane_0(%{{.*}}) -> (vector<1xf32>, vector<1xf32>, vector<2xf32>, vector<2xf32>)
  %r:2 = vector_ext.warp_execute_on_lane_0(%laneid) ->
    (vector<1xf32>, vector<2xf32>) {
    // CHECK: %[[V0:.*]] = "some_def"() : () -> vector<32xf32>
    // CHECK: %[[V1:.*]] = "some_def"() : () -> vector<32xf32>
    // CHECK: %[[V2:.*]] = "some_def"() : () -> vector<64xf32>
    // CHECK: %[[V3:.*]] = "some_def"() : () -> vector<64xf32>
    // CHECK: vector_ext.yield %[[V0]], %[[V1]], %[[V2]], %[[V3]] : vector<32xf32>, vector<32xf32>, vector<64xf32>, vector<64xf32>
    %2 = "some_def"() : () -> (vector<32xf32>)
    %3 = "some_def"() : () -> (vector<32xf32>)
    %4 = "some_def"() : () -> (vector<64xf32>)
    %5 = "some_def"() : () -> (vector<64xf32>)
    %6 = arith.addf %2, %3 : vector<32xf32>
    %7 = arith.addf %4, %5 : vector<64xf32>
    vector_ext.yield %6, %7 : vector<32xf32>, vector<64xf32>
  }
  // CHECK: %[[A0:.*]] = arith.addf %[[R]]#2, %[[R]]#3 : vector<2xf32>
  // CHECK: %[[A1:.*]] = arith.addf %[[R]]#0, %[[R]]#1 : vector<1xf32>
  %id2 = affine.apply #map0()[%laneid]
  // CHECK: vector.transfer_write %[[A1]], {{.*}} : vector<1xf32>, memref<1024xf32>
  // CHECK: vector.transfer_write %[[A0]], {{.*}} : vector<2xf32>, memref<1024xf32>
  vector.transfer_write %r#0, %dest[%laneid] : vector<1xf32>, memref<1024xf32>
  vector.transfer_write %r#1, %dest[%id2] : vector<2xf32>, memref<1024xf32>
  return
}

// -----

#map0 = affine_map<()[s0] -> (s0 * 2)>

//  CHECK-DAG: #[[MAP0:.*]] = affine_map<()[s0] -> (s0 * 2)>

// CHECK:   func @warp_propagate_read
//  CHECK-SAME:     (%[[ID:.*]]: index
func @warp_propagate_read(%laneid: index, %src: memref<1024xf32>, %dest: memref<1024xf32>) {
// CHECK-NOT: warp_execute_on_lane_0
// CHECK-DAG: %[[R0:.*]] = vector.transfer_read %arg1[%[[ID]]], %{{.*}} : memref<1024xf32>, vector<1xf32>
// CHECK-DAG: %[[ID2:.*]] = affine.apply #[[MAP0]]()[%[[ID]]]
// CHECK-DAG: %[[R1:.*]] = vector.transfer_read %arg1[%[[ID2]]], %{{.*}} : memref<1024xf32>, vector<2xf32>
// CHECK: vector.transfer_write %[[R0]], {{.*}} : vector<1xf32>, memref<1024xf32>
// CHECK: vector.transfer_write %[[R1]], {{.*}} : vector<2xf32>, memref<1024xf32>
  %c0 = arith.constant 0 : index
  %c32 = arith.constant 0 : index
  %cst = arith.constant 0.000000e+00 : f32
  %r:2 = vector_ext.warp_execute_on_lane_0(%laneid) ->(vector<1xf32>, vector<2xf32>) {
    %2 = vector.transfer_read %src[%c0], %cst : memref<1024xf32>, vector<32xf32>
    %3 = vector.transfer_read %src[%c32], %cst : memref<1024xf32>, vector<64xf32>
    vector_ext.yield %2, %3 : vector<32xf32>, vector<64xf32>
  }
  %id2 = affine.apply #map0()[%laneid]
  vector.transfer_write %r#0, %dest[%laneid] : vector<1xf32>, memref<1024xf32>
  vector.transfer_write %r#1, %dest[%id2] : vector<2xf32>, memref<1024xf32>
  return
}

// -----

// CHECK-SCF-IF-LABEL: func @rewrite_warp_op_to_scf_if(
//  CHECK-SCF-IF-SAME:     %[[laneid:.*]]: index,
//  CHECK-SCF-IF-SAME:     %[[v0:.*]]: vector<4xf32>, %[[v1:.*]]: vector<8xf32>)
func @rewrite_warp_op_to_scf_if(%laneid: index,
                                %v0: vector<4xf32>, %v1: vector<8xf32>) {
//   CHECK-SCF-IF-DAG:   %[[c0:.*]] = arith.constant 0 : index
//   CHECK-SCF-IF-DAG:   %[[c2:.*]] = arith.constant 2 : index
//   CHECK-SCF-IF-DAG:   %[[c4:.*]] = arith.constant 4 : index
//   CHECK-SCF-IF-DAG:   %[[c8:.*]] = arith.constant 8 : index
//       CHECK-SCF-IF:   %[[is_lane_0:.*]] = arith.cmpi eq, %[[laneid]], %[[c0]]

//       CHECK-SCF-IF:   %[[buffer_v0:.*]] = memref.alloc() : memref<128xf32>
//       CHECK-SCF-IF:   %[[s0:.*]] = arith.muli %[[laneid]], %[[c4]]
//       CHECK-SCF-IF:   vector.store %[[v0]], %[[buffer_v0]][%[[s0]]]
//       CHECK-SCF-IF:   %[[buffer_v1:.*]] = memref.alloc() : memref<256xf32>
//       CHECK-SCF-IF:   %[[s1:.*]] = arith.muli %[[laneid]], %[[c8]]
//       CHECK-SCF-IF:   vector.store %[[v1]], %[[buffer_v1]][%[[s1]]]

//       CHECK-SCF-IF:   %[[buffer_def_0:.*]] = memref.alloc() : memref<32xf32>
//       CHECK-SCF-IF:   %[[buffer_def_1:.*]] = memref.alloc() : memref<64xf32>

//       CHECK-SCF-IF:   scf.if %[[is_lane_0]] {
  %r:2 = vector_ext.warp_execute_on_lane_0(%laneid)
      args(%v0, %v1 : vector<4xf32>, vector<8xf32>) -> (vector<1xf32>, vector<2xf32>) {
    ^bb0(%arg0: vector<128xf32>, %arg1: vector<256xf32>):
//       CHECK-SCF-IF:     %[[arg1:.*]] = vector.load %[[buffer_v1]][%[[c0]]] : memref<256xf32>, vector<256xf32>
//       CHECK-SCF-IF:     %[[arg0:.*]] = vector.load %[[buffer_v0]][%[[c0]]] : memref<128xf32>, vector<128xf32>
//       CHECK-SCF-IF:     %[[def_0:.*]] = "some_def"(%[[arg0]]) : (vector<128xf32>) -> vector<32xf32>
//       CHECK-SCF-IF:     %[[def_1:.*]] = "some_def"(%[[arg1]]) : (vector<256xf32>) -> vector<64xf32>
    %2 = "some_def"(%arg0) : (vector<128xf32>) -> vector<32xf32>
    %3 = "some_def"(%arg1) : (vector<256xf32>) -> vector<64xf32>
//       CHECK-SCF-IF:     vector.store %[[def_0]], %[[buffer_def_0]][%[[c0]]]
//       CHECK-SCF-IF:     vector.store %[[def_1]], %[[buffer_def_1]][%[[c0]]]
    vector_ext.yield %2, %3 : vector<32xf32>, vector<64xf32>
  }
//       CHECK-SCF-IF:   }
//       CHECK-SCF-IF:   %[[o1:.*]] = arith.muli %[[laneid]], %[[c2]]
//       CHECK-SCF-IF:   %[[r1:.*]] = vector.load %[[buffer_def_1]][%[[o1]]] : memref<64xf32>, vector<2xf32>
//       CHECK-SCF-IF:   %[[r0:.*]] = vector.load %[[buffer_def_0]][%[[laneid]]] : memref<32xf32>, vector<1xf32>
//       CHECK-SCF-IF:   vector.print %[[r0]]
//       CHECK-SCF-IF:   vector.print %[[r1]]
  vector.print %r#0 : vector<1xf32>
  vector.print %r#1 : vector<2xf32>
  return
}

// -----

// CHECK-SCF-IF-LABEL: func @vector_reduction(
//  CHECK-SCF-IF-SAME:     %[[laneid:.*]]: index)
//       CHECK-SCF-IF:   %[[c0:.*]] = arith.constant 0 : index
//       CHECK-SCF-IF:   %[[is_lane_0:.*]] = arith.cmpi eq, %[[laneid]]
//       CHECK-SCF-IF:   %[[buffer:.*]] = memref.alloc() : memref<1xf32>
//       CHECK-SCF-IF:   scf.if %[[is_lane_0]] {
//       CHECK-SCF-IF:     %[[reduction:.*]] = vector.reduction
//       CHECK-SCF-IF:     memref.store %[[reduction]], %[[buffer]][%[[c0]]]
//       CHECK-SCF-IF:   }
//       CHECK-SCF-IF:   %[[broadcasted:.*]] = memref.load %[[buffer]][%[[c0]]]
//       CHECK-SCF-IF:   vector.print %[[broadcasted]] : f32

// CHECK-LABEL: func @vector_reduction(
//  CHECK-SAME:     %[[laneid:.*]]: index)
//   CHECK-DAG:   %[[c0:.*]] = arith.constant 0 : i32
//   CHECK-DAG:   %[[c1:.*]] = arith.constant 1 : i32
//   CHECK-DAG:   %[[c2:.*]] = arith.constant 2 : i32
//   CHECK-DAG:   %[[c4:.*]] = arith.constant 4 : i32
//   CHECK-DAG:   %[[c8:.*]] = arith.constant 8 : i32
//   CHECK-DAG:   %[[c16:.*]] = arith.constant 16 : i32
//   CHECK-DAG:   %[[c32:.*]] = arith.constant 32 : i32
//       CHECK:   %[[warp_op:.*]] = vector_ext.warp_execute_on_lane_0(%[[laneid]]) -> (vector<1xf32>) {
//       CHECK:     vector_ext.yield %{{.*}} : vector<32xf32>
//       CHECK:   }
//       CHECK:   %[[a:.*]] = vector.extract %[[warp_op]][0] : vector<1xf32>
//       CHECK:   %[[r0:.*]], %{{.*}} = gpu.shuffle  down %[[a]], %[[c16]], %[[c32]]
//       CHECK:   %[[a0:.*]] = arith.addf %[[a]], %[[r0]]
//       CHECK:   %[[r1:.*]], %{{.*}} = gpu.shuffle  down %[[a0]], %[[c8]], %[[c32]]
//       CHECK:   %[[a1:.*]] = arith.addf %[[a0]], %[[r1]]
//       CHECK:   %[[r2:.*]], %{{.*}} = gpu.shuffle  down %[[a1]], %[[c4]], %[[c32]]
//       CHECK:   %[[a2:.*]] = arith.addf %[[a1]], %[[r2]]
//       CHECK:   %[[r3:.*]], %{{.*}} = gpu.shuffle  down %[[a2]], %[[c2]], %[[c32]]
//       CHECK:   %[[a3:.*]] = arith.addf %[[a2]], %[[r3]]
//       CHECK:   %[[r4:.*]], %{{.*}} = gpu.shuffle  down %[[a3]], %[[c1]], %[[c32]]
//       CHECK:   %[[a4:.*]] = arith.addf %[[a3]], %[[r4]]
//       CHECK:   %[[broadcasted:.*]], %{{.*}} = gpu.shuffle  idx %[[a4]], %[[c0]], %[[c32]]
//       CHECK:   vector.print %[[broadcasted]] : f32
func @vector_reduction(%laneid: index) {
  %r = vector_ext.warp_execute_on_lane_0(%laneid) -> (f32) {
    %0 = "some_def"() : () -> (vector<32xf32>)
    %1 = vector.reduction <add>, %0 : vector<32xf32> into f32
    vector_ext.yield %1 : f32
  }
  vector.print %r : f32
  return
}

// -----

// CHECK-LABEL: func @fold_vector_broadcast(
//       CHECK:   %[[r:.*]] = vector_ext.warp_execute_on_lane_0{{.*}} -> (vector<1xf32>)
//       CHECK:     %[[some_def:.*]] = "some_def"
//       CHECK:     vector_ext.yield %[[some_def]] : vector<1xf32>
//       CHECK:   vector.print %[[r]] : vector<1xf32>
func @fold_vector_broadcast(%laneid: index) {
  %r = vector_ext.warp_execute_on_lane_0(%laneid) -> (vector<1xf32>) {
    %0 = "some_def"() : () -> (vector<1xf32>)
    %1 = vector.broadcast %0 : vector<1xf32> to vector<32xf32>
    vector_ext.yield %1 : vector<32xf32>
  }
  vector.print %r : vector<1xf32>
  return
}

// -----

// CHECK-LABEL: func @extract_vector_broadcast(
//       CHECK:   %[[r:.*]] = vector_ext.warp_execute_on_lane_0{{.*}} -> (vector<1xf32>)
//       CHECK:     %[[some_def:.*]] = "some_def"
//       CHECK:     vector_ext.yield %[[some_def]] : vector<1xf32>
//       CHECK:   %[[broadcasted:.*]] = vector.broadcast %[[r]] : vector<1xf32> to vector<2xf32>
//       CHECK:   vector.print %[[broadcasted]] : vector<2xf32>
func @extract_vector_broadcast(%laneid: index) {
  %r = vector_ext.warp_execute_on_lane_0(%laneid) -> (vector<2xf32>) {
    %0 = "some_def"() : () -> (vector<1xf32>)
    %1 = vector.broadcast %0 : vector<1xf32> to vector<64xf32>
    vector_ext.yield %1 : vector<64xf32>
  }
  vector.print %r : vector<2xf32>
  return
}
