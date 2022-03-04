# RUN: %PYTHON %s 2>&1 | FileCheck %s

# This file contains small benchmarks with reasonably-sized problem/tiling sizes
# and codegen options.

from ..core.experts import *
from ..core.harness import *
from ..core.transforms import *

from ..contraction.definitions import *

fun_name = 'column_reduction_2d'
op_name = 'linalg.generic'

################################################################################
### Compilation strategies.
################################################################################

# Note: `\` char at the end of next line prevents formatter reflows, keep it.
all_names = [  \
  "Tile4x8PeelInnerParallel", \
  "Tile6x8PeelInnerParallel", \
  "Tile8x8PeelInnerParallel", \
  "Tile4x16PeelInnerParallel", \
  "Tile6x16PeelInnerParallel", \
  "Tile8x16PeelInnerParallel", \
  "Tile4x32PeelInnerParallel", \
  "Tile6x32PeelInnerParallel", \
  "Tile8x32PeelInnerParallel", \
  "Tile16x32PeelInnerParallel", \
  "Tile4x64PeelInnerParallel", \
  "Tile6x64PeelInnerParallel", \
  "Tile8x64PeelInnerParallel", \
  "Tile16x64PeelInnerParallel", \
            ]


def all_experts(problem_sizes: List[int]):
  tile_sizes = [
    [4, 8], [6, 8], [8, 8], \
    [4, 16], [6, 16], [8, 16], \
    [4, 32], [6, 32], [8, 32], [16, 32], \
    [4, 64], [6, 64], [8, 64], [16, 64], \
  ]

  res = []
  for ts in tile_sizes:
    res.append(
    # Note: `\` char at the end of next line prevents formatter reflows, keep it.
      Tile(fun_name=fun_name, \
           op_name=op_name,
           # Don't tile too small dimensions.
           tile_sizes=[ts[0], ts[1]] if problem_sizes[1] > ts[1] else [ts[0]],
           peel=[0, 1] if problem_sizes[1] > ts[1] else [0])
        .then(Vectorize(fun_name, op_name))
        .then(LoweringOnlyExpert(fun_name,
                                 op_name,
                                 multi_reduction_lowering='innerparallel')),
    )
  return [e.print_ir(after_all=False, at_begin=False, llvm=False) for e in res]


################################################################################
### Problem instantiations.
################################################################################

keys = ['m', 'n']


# CHECK-NOT: FAILURE
def main():
  # Specify default configuration and parse command line.
  # Note: `\` char at the end of next line prevents formatter reflows, keep it.
  args = test_argparser(  \
    "column reduction 2d benchmark",
    default_n_iters=100,
    default_problem_sizes_list=[
      [128, 256],
      [104, 128],
      [256, 256],
      [1000, 1024],
      [8000, 6144],
    ],
    default_expert_list=all_names,
    default_dynamic_at_compile_time_list=[
      [],
      ['m', 'n']
    ],
    default_spec_list=[])

  def numpy_kernel(args, sizes, types):
    A, B = args
    B.fill(0.)
    np.sum(A, axis=0, out=B)

  def pytorch_kernel(args, sizes, types):
    A, B = args
    B.fill_(0.)
    torch.sum(A, dim=0, out=B)

  for dynamic_at_compile_time in args.dynamic_at_compile_time_list:
    for problem_sizes in args.problem_sizes_list:
      test_harness(lambda s, t: EinsumProblem('mn->n', 'mn', 1),
                   [[np.float32] * 2],
                   test_sizes(keys, [problem_sizes]),
                   test_experts(all_experts(problem_sizes), all_names,
                                args.expert_list),
                   n_iters=args.n_iters,
                   dynamic_at_compile_time_sizes=set(
                       dynamic_at_compile_time).intersection(keys),
                   function_name=fun_name,
                   dump_ir_to_file='/tmp/abcd.mlir',
                   dump_obj_to_file='/tmp/abcd.o',
                   dump_data_to_file=args.dump_data,
                   backends=['dialect'])


if __name__ == '__main__':
  main()
