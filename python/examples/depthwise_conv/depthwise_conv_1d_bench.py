# RUN: %PYTHON %s 2>&1 | FileCheck %s

# This file contains simple test cases that combine various codegen options.

from ..core.experts import *
from ..core.harness import *
from ..core.transforms import *

from .definitions import *

fun_name = 'depthwise_conv_1d_nwc_wc'
op_name = 'linalg.depthwise_conv_1d_nwc_wc'

################################################################################
# Compilation strategies.
################################################################################

# Note: `\` char at the end of next line prevents formatter reflows, keep it.
all_names = [                 \
  "SingleTiling3D4x16x3Peel", \
  "SingleTiling3D5x16x3Peel", \
  "SingleTiling3D6x16x3Peel", \
  "SingleTiling3D8x16x3Peel", \
]

all_experts = [
    # Note: `\` char at the end of next line prevents formatter reflows, keep it.
    e.print_ir(after_all=False, at_begin=False, llvm=False) for e in [ \
        Tile(fun_name=fun_name,
             op_name=op_name,
             #           N  W   C  KW
             tile_sizes=[1, 4, 16, 3],
             peel=[0,1,2,3])
          .then(Vectorize(fun_name, ''))
          .then(LoweringOnlyExpert(fun_name, op_name)),
        Tile(fun_name=fun_name,
             op_name=op_name,
             #           N  W   C  KW
             tile_sizes=[1, 5, 16, 3],
             peel=[0,1,2,3])
          .then(Vectorize(fun_name, ''))
          .then(LoweringOnlyExpert(fun_name, op_name)),
        Tile(fun_name=fun_name,
             op_name=op_name,
             #           N  W   C  KW
             tile_sizes=[1, 6, 16, 3],
             peel=[0,1,2,3])
          .then(Vectorize(fun_name, ''))
          .then(LoweringOnlyExpert(fun_name, op_name)),
        Tile(fun_name=fun_name,
             op_name=op_name,
             #           N  W   C  KW
             tile_sizes=[1, 8, 16, 3],
             peel=[0,1,2,3])
          .then(Vectorize(fun_name, ''))
          .then(LoweringOnlyExpert(fun_name, op_name)),
    ]
]

################################################################################
# Problem instantiation
################################################################################

keys = ['N', 'W', 'C', 'KW', 'strides', 'dilations']


# CHECK-NOT: FAILURE
def main():
  # Specify default configuration and parse command line.
  args = test_argparser(
    "depthwise conv 1d benchmark",
    default_n_iters=100,
    #  N   W   C  KW   st  dil
    default_problem_sizes_list=[ \
      [8, 16, 32, 3, [1], [1]],
      [8, 16, 32, 3, [1], [2]],
      [8, 16, 32, 3, [2], [1]],
      [8, 16, 32, 3, [2], [2]],
    ],
    default_expert_list=all_names,
    default_dynamic_at_compile_time_list=[
          # case 1: static at compile time
          [],
          # case 2: partially dynamic at compile time
          ['W'],
          # case 3: partially dynamic at compile time
          ['C'],
          # case 4: fully dynamic at compile time (except KW)
          ['N', 'W', 'C'],
    ],
    default_spec_list=[])

  for dynamic_at_compile_time in args.dynamic_at_compile_time_list:

    def numpy_kernel(args, sizes, types):
      DepthwiseConvolutionProblem(
          'NWC', 'WC', strides=sizes['strides'],
          dilations=sizes['dilations']).reference_np(*args)

    def pytorch_kernel(args, sizes, types):
      DepthwiseConvolutionProblem(
          'NWC', 'WC', strides=sizes['strides'],
          dilations=sizes['dilations']).reference_pt(*args)

    test_harness(lambda sizes, t: DepthwiseConvolutionProblem(
        'NWC', 'WC', strides=sizes['strides'], dilations=sizes['dilations']),
                 [[np.float32] * 3],
                 test_sizes(keys, args.problem_sizes_list),
                 test_experts(all_experts, all_names, args.expert_list),
                 n_iters=args.n_iters,
                 dynamic_at_compile_time_sizes=set(
                     dynamic_at_compile_time).intersection(keys),
                 function_name=fun_name,
                 dump_ir_to_file='/tmp/abcd.mlir',
                 dump_obj_to_file='/tmp/abcd.o',
                 dump_data_to_file=args.dump_data,
                 numpy_benchmark=numpy_kernel,
                 pytorch_benchmark=pytorch_kernel)


if __name__ == '__main__':
  main()
