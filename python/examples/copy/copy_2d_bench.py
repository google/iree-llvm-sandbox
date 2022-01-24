# RUN: %PYTHON %s 2>&1 | FileCheck %s

# This file contains small benchmarks with reasonably-sized problem/tiling sizes
# and codegen options.

from ..core.experts import *
from ..core.harness import *
from ..core.transforms import *
from ..core.utils import *

from ..contraction.definitions import EinsumProblem

from typing import List

fun_name = 'copy_2d'
op_name = 'linalg.generic'

################################################################################
### Compilation strategies.
################################################################################

all_names = [  \
  "ColumnReduction2DExpert"
            ]


# Problem size-specific transformation parameters: the tile size is the max
# divisible entry that fits within
def all_experts(problem_sizes: List[int]):
  # Before bufferization, the IR only has a tensor.extract_slice /
  #   tensor.insert_slice pair.
  # Bufferization then properly introduces copy ops (implemented with
  # linalg.generic)
  # We want to make more these copies more efficient.
  # In the case of a single copy benchmark it is the one true thing to optimize.
  return [
    # Note: `\` char at the end of next line prevents formatter reflows, keep it.
    e.print_ir(after_all=False, at_begin=False, llvm=False) for e in [ \
      Tile(fun_name=fun_name,
            op_name=op_name,
            tile_sizes=[4, 16],
            peel=[0, 1])
        # Bufferize first
        .then(Bufferize())
        # Then vectorize and lower.
        .then(Vectorize(fun_name=fun_name, op_name=op_name))
        .then(UnrollOneParentLoop(fun_name=fun_name,
                                  op_name='vector.transfer_read',
                                  parent_loop_num=1,
                                  unroll_factor=1))
        .then(UnrollOneParentLoop(fun_name=fun_name,
                                  op_name='vector.transfer_read',
                                  parent_loop_num=2,
                                  unroll_factor=1))
        .then(LowerVectors())
        .then(LowerToLLVM())
    ]
  ]


################################################################################
### Problem instantiations.
################################################################################

keys = ['m', 'n']


# CHECK-NOT: FAILURE
def main():
  # Specify default configuration and parse command line.
  # Note: `\` char at the end of next line prevents formatter reflows, keep it.
  args = test_argparser(  \
    "copy 2d benchmark",
    default_n_iters=100,
    default_problem_sizes_list=[
      [128, 256],
      [104, 128],
      [256, 256],
      [1000, 1024],
      [8000, 6144],
    ],
    default_expert_list=all_names,
    default_dynamic_at_compile_time_list=[],
    default_spec_list=[])

  for problem_sizes in args.problem_sizes_list:
    test_harness(lambda s, t: EinsumProblem('mn->mn', 'mn', 0), [[np.float32] * 2],
                 test_sizes(keys, [problem_sizes]),
                 test_experts(all_experts(problem_sizes), all_names,
                              args.expert_list),
                 n_iters=args.n_iters,
                 function_name=fun_name,
                 dump_data_to_file=args.dump_data,
                 dump_ir_to_file='/tmp/abc.mlir',
                 dump_obj_to_file='/tmp/abc.o')


if __name__ == '__main__':
  main()
