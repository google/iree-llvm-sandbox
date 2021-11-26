!elem_type_a = type f32
!elem_type_b = type f32
!elem_type_c = type f32
!row_major_A = type tensor<${M}x${K}x!elem_type_a>
!row_major_B = type tensor<${K}x${N}x!elem_type_b>
!row_major_C = type tensor<${M}x${N}x!elem_type_c>

func @init_and_matmul(
  %a: !row_major_A {linalg.buffer_layout = affine_map<(i, j)[s0, s1] -> (i, j)>},
  %b: !row_major_B {linalg.buffer_layout = affine_map<(i, j)[s0, s1] -> (i, j)>},
  %c: !row_major_C {linalg.buffer_layout = affine_map<(i, j)[s0, s1] -> (i, j)>}) -> !row_major_C
// TODO: activate manually for now.
// attributes { passthrough = [["target-cpu", "skylake-avx512"], ["prefer-vector-width", "512"]]}
{
  %v0 = arith.constant 0.0 : !elem_type_c
  %d = linalg.fill(%v0, %c) : !elem_type_c, !row_major_C -> !row_major_C
  %e = linalg.matmul ins(%a, %b : !row_major_A, !row_major_B)
    outs(%d: !row_major_C) -> !row_major_C
  return %e : !row_major_C
}

func @print_perf(%iters: index, %total_time: f64) {
  %c2 = arith.constant 2 : index
  %cM = arith.constant ${M} : index
  %cN = arith.constant ${N} : index
  %cK = arith.constant ${K} : index

  %mn = arith.muli %cM, %cN : index
  %mnk = arith.muli %mn, %cK : index

  // 2*M*N*K.
  %flops_per_iter = arith.muli %c2, %mnk : index
  %flops = arith.muli %iters, %flops_per_iter : index
  %flops_i64 = arith.index_cast %flops : index to i64
  %flops_f = arith.sitofp %flops_i64 : i64 to f64
  %flops_per_s = arith.divf %flops_f, %total_time : f64
  vector.print %flops_per_s : f64

  return
}

func @exec(%iters : index) {
  %v0 = arith.constant 0.0 : !elem_type_c
  %v1 = arith.constant 1.0 : !elem_type_a
  %v2 = arith.constant 2.0 : !elem_type_b

  %A = linalg.init_tensor [${M}, ${K}] : !row_major_A
  %B = linalg.init_tensor [${K}, ${N}] : !row_major_B
  %C = linalg.init_tensor [${M}, ${N}] : !row_major_C
  %AA = linalg.fill(%v1, %A) : !elem_type_a, !row_major_A -> !row_major_A
  %BB = linalg.fill(%v2, %B) : !elem_type_b, !row_major_B -> !row_major_B
  %CC = linalg.fill(%v0, %C) : !elem_type_c, !row_major_C -> !row_major_C

  %c0 = arith.constant 0: index
  %c1 = arith.constant 1: index

  /// Run and dump performance for matmul.
  %t_start_matmul = call @rtclock() : () -> f64
  %res = scf.for %arg0 = %c0 to %iters step %c1 iter_args(%bbarg = %CC) -> (!row_major_C) {
    %r = call @init_and_matmul(%AA, %BB, %bbarg) : (!row_major_A, !row_major_B, !row_major_C) -> (!row_major_C)
    scf.yield %r : !row_major_C
  }
  %t_end_matmul = call @rtclock() : () -> f64
  %tmatmul = arith.subf %t_end_matmul, %t_start_matmul: f64
  call @print_perf(%iters, %tmatmul) : (index, f64) -> ()

  // %res2 = tensor.cast %res: !row_major_C to tensor<*xf32>
  // call @print_memref_f32(%res2) : (tensor<*xf32>) -> ()
  %val = vector.transfer_read %res[%c0, %c0], %v0: !row_major_C, vector<1x1x!elem_type_c>
  vector.print %val: vector<1x1x!elem_type_c>

  return
}

func @main() {
  %iters = arith.constant ${ITERS} : index
  call @exec(%iters) : (index) -> ()
  return
}

func private @rtclock() -> f64
func private @print_memref_f32(tensor<*xf32>) attributes { llvm.emit_c_interface }
