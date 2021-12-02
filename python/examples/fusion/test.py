# RUN: %PYTHON %s 2>&1 | FileCheck %s

# This file contains test to compile fusion examples.

from ..core.experts import *
from ..core.harness import *
from ..core.transforms import *
from ..core.transform import Print

from .definitions import *

################################################################################
### Expert for running the fusion tests.
################################################################################


fusion_test_expert = Fuse.then(Bufferize).then(Print).then(
    LowerVectors).then(LowerToLLVM)


# 1 linalg.fill -> linalg.matmul fusion.
def fill_matmul_fusion():

  expert = fusion_test_expert(
      'matmul_on_tensors',
      'linalg.matmul',
      tile_sizes=[4, 8, 6],
      tile_interchange=[0, 1, 2],
      pad=True,
      pack_paddings=[1, 1, 0],
      hoist_paddings=[0, 0, 0],
      vectorize=True,
      vectorize_paddings=True)
  problem_sizes_dict = {'M': 24, 'N': 32, 'K': 48}
  problem = ProblemInstance(
      problem_definition=MatmulProblem(),
      np_types=[np.float32, np.float32, np.float32])

  ## These lit tests are not actually run, but can be checked locally using
  ## $ bazel run ${IREE_LLVM_SANDBOX_DIR}:fusion_test | \
  ##     bazel run ${LLVM_DIR}/llvm:FileCheck ${IREE_LLVM_SANDBOX_DIR}/python/fusion/test.py

  #      CHECK: func @matmul_on_tensors(
  # CHECK-SAME:     %[[ARG0:.+]]: memref<24x48xf32>
  # CHECK-SAME:     %[[ARG1:.+]]: memref<48x32xf32>
  # CHECK-SAME:     %[[ARG2:.+]]: memref<24x32xf32>)
  #      CHECK:   %[[ZERO:.+]] = arith.constant dense<0.000000e+00> : vector<4x8xf32>
  #      CHECK:   scf.for %{{.+}} =
  #      CHECK:     scf.for %{{.+}} =
  #      CHECK:       %[[REDUCTION:.+]] = scf.for %[[IV1:[a-zA-Z0-9]+]]
  # CHECK-SAME:           iter_args(%[[PHI:.+]] = %[[ZERO]]
  #      CHECK:         %[[LHS_VEC:.+]] = vector.transfer_read %[[ARG0]]
  #      CHECK:         %[[RHS_VEC:.+]] = vector.transfer_read %[[ARG1]]
  #      CHECK:         %[[CONTRACT:.+]] = vector.contract
  # CHECK-SAME:            %[[LHS_VEC]], %[[RHS_VEC]], %[[PHI]]
  #      CHECK:          scf.yield %[[CONTRACT]]
  #      CHECK:       vector.transfer_write %[[REDUCTION]], %[[ARG2]]
  problem.compile(
      entry_point_name='matmul_main',
      fun_to_benchmark_name='matmul_on_tensors',
      compile_time_problem_sizes_dict=problem_sizes_dict,
      transform=expert)


def fill_matmul_bias_add_fusion():
  expert = fusion_test_expert(
      'matmul_bias_add_on_tensors',
      'linalg.generic',
      tile_sizes=[4, 8, 6],
      tile_interchange=[0, 1, 2],
      pad=True,
      pack_paddings=[1, 1, 0],
      hoist_paddings=[0, 0, 0],
      vectorize=True,
      vectorize_paddings=True)
  problem_sizes_dict = {'M': 24, 'N': 32, 'K': 48}
  problem = ProblemInstance(
      problem_definition=MatmulBiasAddProblem(),
      np_types=[np.float32, np.float32, np.float32, np.float32])

  ## These lit tests are not actually run, but can be checked locally using
  ## $ bazel run ${IREE_LLVM_SANDBOX_DIR}:fusion_test | \
  ##     bazel run ${LLVM_DIR}/llvm:FileCheck ${IREE_LLVM_SANDBOX_DIR}/python/fusion/test.py
  #
  #      CHECK: func @matmul_bias_add_on_tensors(
  # CHECK-SAME:     %[[ARG0:.+]]: memref<24x48xf32>
  # CHECK-SAME:     %[[ARG1:.+]]: memref<48x32xf32>
  # CHECK-SAME:     %[[ARG2:.+]]: memref<32xf32>
  # CHECK-SAME:     %[[ARG3:[a-zA-Z0-9]+]]: memref<24x32xf32>
  # CHECK-SAME:     %[[ARG4:[a-zA-Z0-9]+]]: memref<24x32xf32>)
  #      CHECK:   scf.for %{{.+}} =
  #      CHECK:     %[[LHS_VEC:.+]] = vector.transfer_read %[[ARG0]]
  #      CHECK:     scf.for %{{.+}} =
  #      CHECK:       %[[RHS_VEC:.+]] = vector.transfer_read %[[ARG1]]
  #      CHECK:       %[[INIT_VEC:.+]] = vector.transfer_read %[[ARG2]]
  #      CHECK:       %[[BCAST:.+]] = vector.broadcast %[[INIT_VEC]]
  #      CHECK:       %[[CONTRACT:.+]] = vector.contract
  # CHECK-SAME:          %[[LHS_VEC]], %[[RHS_VEC]], %[[BCAST]]
  #      CHECK:       vector.transfer_write %[[CONTRACT]], %[[ARG4]]
  problem.compile(
      entry_point_name='matmul_bias_add_main',
      fun_to_benchmark_name='matmul_bias_add_on_tensors',
      compile_time_problem_sizes_dict=problem_sizes_dict,
      transform=expert)


def main():
  fill_matmul_fusion()
  fill_matmul_bias_add_fusion()


if __name__ == '__main__':
  main()
