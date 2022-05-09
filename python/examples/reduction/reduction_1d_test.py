# RUN: %PYTHON %s 2>&1 | FileCheck %s

# This file contains simple test cases that combine various codegen options.

from mlir.sandbox.experts import *
from mlir.sandbox.harness import *
from mlir.sandbox.problem_definition import *
from mlir.sandbox.transforms import *

from ..contraction.definitions import *

################################################################################
### Compilation strategies.
################################################################################

# No tiling.
expert_no_tiling = LoweringOnlyExpert('reduction_1d', 'linalg.generic')

all_experts = [expert_no_tiling]

################################################################################
### Problem instantiations.
################################################################################

keys = ['m']


# CHECK-NOT: FAILURE
def main():
  n_iters = 1
  problem_size_list = [[48], [49]]

  test_harness(lambda s, t: EinsumProblem('m->', 'm', 1), [[np.float32] * 2],
               test_sizes(keys, problem_size_list),
               all_experts,
               n_iters=n_iters,
               function_name='reduction_1d')


if __name__ == '__main__':
  main()
