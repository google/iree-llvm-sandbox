#!/bin/bash

set -ex

###############################################################################
# Ran on a machine with the following characteristics.
# Note the SMT pair of CPU 4 (i.e. CPU 40) is disabled.
###############################################################################
# > lscpu
# Architecture:            x86_64
#   CPU op-mode(s):        32-bit, 64-bit
#   Address sizes:         46 bits physical, 48 bits virtual
#   Byte Order:            Little Endian
# CPU(s):                  72
#   On-line CPU(s) list:   0-39,41-71
#   Off-line CPU(s) list:  40
# Vendor ID:               GenuineIntel
#   BIOS Vendor ID:        Intel(R) Corporation
#   Model name:            Intel(R) Xeon(R) Gold 6154 CPU @ 3.00GHz
#     BIOS Model name:     Intel(R) Xeon(R) Gold 6154 CPU @ 3.00GHz
#     CPU family:          6
#     Model:               85
#     Thread(s) per core:  2
#     Core(s) per socket:  18
#     Socket(s):           2
#     Stepping:            4
#     CPU max MHz:         3700.0000
#     CPU min MHz:         1200.0000
#     BogoMIPS:            6000.00
#     Flags:               fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush dts acpi mmx fxsr sse sse2 ss ht tm pbe syscall nx pdpe1gb rdtscp lm constant_tsc art arch_perfmon pebs bts rep_good nopl xtopology nonstop_tsc cpuid aperfmperf pni pclmulqdq dtes64 monitor ds_cpl vmx smx est tm2 ssse3 sdbg fma cx16 xtpr p
#                          dcm pcid dca sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes xsave avx f16c rdrand lahf_lm abm 3dnowprefetch cpuid_fault epb cat_l3 cdp_l3 invpcid_single pti intel_ppin ssbd mba ibrs ibpb stibp tpr_shadow vnmi flexpriority ept vpid ept_ad fsgsbase tsc_adjust bmi1 hle avx2 smep bmi2 erms invpcid rtm cqm mpx rd
#                          t_a avx512f avx512dq rdseed adx smap clflushopt clwb intel_pt avx512cd avx512bw avx512vl xsaveopt xsavec xgetbv1 xsaves cqm_llc cqm_occup_llc cqm_mbm_total cqm_mbm_local dtherm ida arat pln pts hwp hwp_act_window hwp_epp hwp_pkg_req pku ospke md_clear flush_l1d
# Virtualization features:
#   Virtualization:        VT-x
# Caches (sum of all):
#   L1d:                   1.1 MiB (36 instances)
#   L1i:                   1.1 MiB (36 instances)
#   L2:                    36 MiB (36 instances)
#   L3:                    49.5 MiB (2 instances)
# NUMA:
#   NUMA node(s):          2
#   NUMA node0 CPU(s):     0-17,36-39,41-53
#   NUMA node1 CPU(s):     18-35,54-71
# Vulnerabilities:

###############################################################################
# The benchmarks below assume the setup described in the 'Benchmark commands'
# section in the README.md. Instructions are reproduced here for convenience.
###############################################################################
# ################################################################
# # Prepare to run on CPU 4 only
# ################################################################
# # Disable address space randomization.
# echo 0 > /proc/sys/kernel/randomize_va_space

# # Disable the sibling of CPU 4.
# cat /sys/devices/system/cpu/cpu4/topology/thread_siblings_list
# # E.g. this may return 4,40
# echo 0 > /sys/devices/system/cpu/cpu40/online

# ################################################################
# Perform cpuset manipulation.
# ################################################################
# # For reference, cset shield does not seem to run as expected on at least 2 systems.
# # cset shield -c 4 --user=${RUN_AS_USER} -k on --userset=${RUN_AS_USER}
# # Instead, reproduce the follwing:
# # https://documentation.suse.com/sle-rt/15-SP2/html/SLE-RT-all/cha-shielding-cpuset.html
# #
# cset set -c 0-3,5-39,41-71 -s system -s system
# cset set -s sandbox -c 4 -m 0 --cpu_exclusive
# cset proc -m -f root -t system

# ################################################################
# # Freq control (note, cloud VM instances do not allow).
# ################################################################

# echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
# echo performance > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
###############################################################################

export BASE_SCRIPT_PATH="$( cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P )"
export IREE_LLVM_SANDBOX_BUILD_DIR=${BASE_SCRIPT_PATH}/build
export MLIR_RUNNER_UTILS_LIB=${IREE_LLVM_SANDBOX_BUILD_DIR}/lib/libmlir_runner_utils.so
export MLIR_C_RUNNER_UTILS_LIB=${IREE_LLVM_SANDBOX_BUILD_DIR}/lib/libmlir_c_runner_utils.so
export PYTHONPATH=${PYTHONPATH}:${IREE_LLVM_SANDBOX_BUILD_DIR}/tools/sandbox/python_package
export PATH=$PATH:$(dirname ~ntv)/.venv/mlirdev/bin/

function prepare_data_collection() {
  DUMP_DATA_FLAG=""
  PLOT_COMMAND_LINE=""
  if !(test -z "$1" )
  then
    DUMP_DATA_FLAG="--dump_data $1"
  fi
}

###############################################################################
# Copy benchmarks.
###############################################################################
# On my machine (theoretical peak 384 GB/s L1 BW) I see:
# [100,  32],    # sweet spot for prefetchers, seems to maximize L1 BW @ 281GB/s
#
# [ 50, 272],    # 10% L2 load, L2 BW @ 71.6GB/s
# [100, 272],    # 20% L2 load, L2 BW @ 80.8GB/s
# [150, 272],    # 30% L2 load, L2 BW @ 69.3GB/s
# [200, 272],    # 40% L2 load, L2 BW @ 82GB/s
# [250, 272],    # 50% L2 load, L2 BW @ 81GB/s
# [300, 272],    # 60% L2 load, L2 BW @ 76GB/s
# [350, 272],    # 70% L2 load, L2 BW @ 65.3GB/s
# [400, 272],    # 80% L2 load, L2 BW @ 56.5GB/s
# [450, 272],    # 90% L2 load, L2 BW @ 54.8GB/s
# [500, 272],    # 100% L2 load, L2 BW @ 47.7GB/s
#
# [5000, 272],   # 40% L3 load, L3 BW @ 25.7GB/s
# [10000, 272],  # 80% L3 load, L3 BW @ 17.2GB/s
# [15000, 272],  # 120% L3 load, L3 BW @ 15.8GB/s
#
# [30000, 272], # DRAM (2.4x L3 load), L3 BW @ 12.2GB/s
# [300000, 272], # DRAM (24x L3 load), L3 BW @ 10.8GB/s
function copy_bandwidth_benchmark() {
  cset proc -s sandbox -e python -- -m python.examples.copy.custom_copy_2d_bench
}

###############################################################################
# Static 1D copy benchmarks.
###############################################################################
# Careful here, static problem size smaller than the tile sizes completely folds
# away since the result tensor is not used.
# TODO: add a fake noop use after the timer in the timing loop to avoid this.
function copy_1d_static_small() {
  COMMAND="cset proc -s sandbox -e python -- -m python.examples.copy.copy_1d_bench ${DUMP_DATA_FLAG} --dynamic_at_compile_time_list []"

  (${COMMAND} --problem_sizes_list 32)
  (${COMMAND} --problem_sizes_list 128)
  (${COMMAND} --problem_sizes_list 1024)
  (${COMMAND} --problem_sizes_list 2000)
  (${COMMAND} --problem_sizes_list 2040)
  (${COMMAND} --problem_sizes_list 4000)
  (${COMMAND} --problem_sizes_list 4096)
}

###############################################################################
# Static 1D reduction benchmarks.
###############################################################################
function reduction_1d_static_l1_repro() {
  export SANDBOX_INLINING='alwaysinline'
  COMMAND="cset proc -s sandbox -e python -- -m python.examples.reduction.reduction_1d_bench ${DUMP_DATA_FLAG} --dynamic_at_compile_time_list []"

  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 100)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 1000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 2048)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 3333)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 4567)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 8000)
}
function reduction_1d_static_l2_repro() {
  export SANDBOX_INLINING='alwaysinline'
  COMMAND="cset proc -s sandbox -e python -- -m python.examples.reduction.reduction_1d_bench ${DUMP_DATA_FLAG} --dynamic_at_compile_time_list []"

  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 10000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 20000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 50000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 100000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 200000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 250000)
}
function reduction_1d_static_l3_repro() {
  export SANDBOX_INLINING='alwaysinline'
  COMMAND="cset proc -s sandbox -e python -- -m python.examples.reduction.reduction_1d_bench ${DUMP_DATA_FLAG} --dynamic_at_compile_time_list []"

  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 1000000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 2000000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 3000000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 4000000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 5000000)
}
function reduction_1d_static_dram_repro() {
  export SANDBOX_INLINING='alwaysinline'
  COMMAND="cset proc -s sandbox -e python -- -m python.examples.reduction.reduction_1d_bench ${DUMP_DATA_FLAG} --dynamic_at_compile_time_list []"

  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 10000000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 20000000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 30000000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 40000000)
  (${COMMAND} --expert_list Tile1DPeel --problem_sizes_list 50000000)
}



###############################################################################
# Entry point.
###############################################################################

function run_one_and_append_results_to_data() {
  benchmark=$1

  BENCH_ROOT_DIR=${BASE_SCRIPT_PATH}/benchmarks
  if !(test -z "$2" )
  then
    BENCH_DIR=${BENCH_ROOT_DIR}/$2
  else
    BENCH_DIR=${BENCH_ROOT_DIR}/results_$(ls -l ${BENCH_ROOT_DIR} | wc -l)
  fi
  mkdir -p ${BENCH_DIR}

  unset SANDBOX_INLINING

  prepare_data_collection ${BENCH_DIR}/${benchmark}.data
  ${benchmark}
}

function run_all_benchmarks() {
  (source benchmarks/bandwidth_bound_l1.sh; run_l1_benchmarks_n_times l1_benchmarks 3)
  (source benchmarks/bandwidth_bound_l2.sh; run_l2_benchmarks_n_times l2_benchmarks 3)
  (source benchmarks/bandwidth_bound_l3.sh; run_l3_benchmarks_n_times l3_benchmarks 3)

  (source benchmarks/matmul.sh; run_matmul_benchmarks_n_times matmul 3)

  (source benchmarks/conv.sh; run_conv_1d_benchmarks_n_times conv_1d 3)
  (source benchmarks/conv.sh; run_conv_2d_benchmarks_n_times conv_2d 3)

  (source benchmarks/depthwise_conv.sh; run_depthwise_conv_1d_l1_benchmarks_n_times depthwiseconv_1d_l1 3)
  (source benchmarks/depthwise_conv.sh; run_depthwise_conv_1d_l2_benchmarks_n_times depthwiseconv_1d_l2 3)
  (source benchmarks/depthwise_conv.sh; run_depthwise_conv_1d_l3_benchmarks_n_times depthwiseconv_1d_l3 3)

  (source benchmarks/depthwise_conv.sh; run_depthwise_conv_2d_mobilenet_benchmarks_n_times depthwise_conv_2d_mobilenet 3)
}

# export BASE_EXP_DIR=$(pwd)/benchmarks

# sed inject some newlines
# find ${BASE_EXP_DIR} -name "*data" | xargs -i sed -i s:{\"function_name\":\\n{\"function_name\":g {}

# L1 bandwidth bound.
# python ./tools/plot_benchmark.py --inputs ${BASE_EXP_DIR}/l1_benchmarks/copy_2d_static_l1_repro.data,${BASE_EXP_DIR}/l1_benchmarks/transpose_2d_static_l1_repro.data,${BASE_EXP_DIR}/l1_benchmarks/row_reduction_2d_static_l1_repro.data,${BASE_EXP_DIR}/l1_benchmarks/column_reduction_2d_static_l1_repro.data --output bandwidth-bound-l1.pdf --plot_name "Bandwidth-bound Experiments (L1-bound)" --metric_to_plot "gbyte_per_s_per_iter"  --sizes_to_plot="m=10,n=32;m=10,n=48;m=20,n=32;m=8,n=96;m=20,n=48;m=12,n=96;m=10,n=128;m=16,n=96;m=12,n=144;m=110,n=16;m=16,n=128;m=64,n=32;m=20,n=128;m=20,n=144;m=90,n=32;m=190,n=16" --benchmarks_to_plot "copy_2d,transpose_2d,column_reduction_2d,row_reduction_2d"

# L2 bandwidth bound
# python ./tools/plot_benchmark.py --inputs ${BASE_EXP_DIR}/l2_benchmarks/copy_2d_static_l2_repro.data,${BASE_EXP_DIR}/l2_benchmarks/transpose_2d_static_l2_repro.data,${BASE_EXP_DIR}/l2_benchmarks/row_reduction_2d_static_l2_repro.data,${BASE_EXP_DIR}/l2_benchmarks/column_reduction_2d_static_l2_repro.data --output bandwidth-bound-l2.pdf --plot_name "Bandwidth-bound Experiments (L1-bound)" --metric_to_plot "gbyte_per_s_per_iter"  --sizes_to_plot="m=12,n=960;m=16,n=960;m=50,n=320;m=12,n=1440;m=1100,n=16;m=20,n=960;m=160,n=160;m=900,n=32;m=200,n=160;m=160,n=240;m=200,n=240;m=160,n=320;m=200,n=320;m=6000,n=16;m=200,n=640;m=8000,n=16" --benchmarks_to_plot "copy_2d,transpose_2d,column_reduction_2d,row_reduction_2d"

# L3 bandwidth bound
# python ./tools/plot_benchmark.py --inputs ${BASE_EXP_DIR}/l3_benchmarks/copy_2d_static_l3_repro.data,${BASE_EXP_DIR}/l3_benchmarks/transpose_2d_static_l3_repro.data,${BASE_EXP_DIR}/l3_benchmarks/row_reduction_2d_static_l3_repro.data,${BASE_EXP_DIR}/l3_benchmarks/column_reduction_2d_static_l3_repro.data --output bandwidth-bound-l3.pdf --plot_name "Bandwidth-bound Experiments (L1-bound)" --metric_to_plot "gbyte_per_s_per_iter"  --sizes_to_plot="m=250,n=960;m=5500,n=64;m=100,n=3840;m=3000,n=128;m=250,n=1920;m=100,n=5760;m=9500,n=64;m=250,n=2560;m=1000,n=640;m=250,n=3840;m=1000,n=960;m=800,n=1280;m=9500,n=128;m=1000,n=1280;m=12000,n=128;m=16000,n=128" --benchmarks_to_plot "copy_2d,transpose_2d,column_reduction_2d,row_reduction_2d"

# Matmul
# python ./tools/plot_benchmark.py --inputs ${BASE_EXP_DIR}/matmul/matmul_kmkn_repro.data,${BASE_EXP_DIR}/matmul/matmul_mkkn_repro.data,${BASE_EXP_DIR}/matmul/matmul_mknk_repro.data --output matmul.pdf --plot_name "Matrix Multiplication" --metric_to_plot "gflop_per_s_per_iter"

# Conv1D
# python ./tools/plot_benchmark.py --inputs ${BASE_EXP_DIR}/conv_1d/conv_1d_repro.data --output conv_1d.pdf --plot_name "Conv1D" --metric_to_plot "gflop_per_s_per_iter" --group_by_strides_and_dilations True

# Conv2D
# python ./tools/plot_benchmark.py --inputs ${BASE_EXP_DIR}/conv_2d/conv_2d_repro.data --output conv_2d.pdf --plot_name "Conv2D" --metric_to_plot "gflop_per_s_per_iter" --group_by_strides_and_dilations True

# DepthwiseConv1D
# python ./tools/plot_benchmark.py --inputs ${BASE_EXP_DIR}/depthwiseconv_1d_l1/depthwise_conv_1d_l1_repro.data,${BASE_EXP_DIR}/depthwiseconv_1d_l2/depthwise_conv_1d_l2_repro.data,${BASE_EXP_DIR}/depthwiseconv_1d_l3/depthwise_conv_1d_l3_repro.data --output depthwise_conv_1d.pdf --plot_name "DepthwiseConv1D" --metric_to_plot "gbyte_per_s_per_iter" --num_sizes_to_plot=16 --group_by_strides_and_dilations True

# DepthwiseConv2D
# python ./tools/plot_benchmark.py --inputs ${BASE_EXP_DIR}/depthwise_conv_2d_mobilenet/depthwise_conv_2d_mobilenet.data --output depthwise_conv_2d.pdf --plot_name "DepthwiseConv2D" --metric_to_plot "gbyte_per_s_per_iter"
