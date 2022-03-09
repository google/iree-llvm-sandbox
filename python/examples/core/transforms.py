from mlir.ir import *
from mlir.passmanager import PassManager
import mlir.dialects.linalg_transform as tx
from mlir.dialects import builtin, pdl

from .variables import *
from .transform import Transform

import mlir.all_passes_registration

import typing as tp


def _get_size_list_as_str(name: str, sizes: tp.List[int]) -> str:
  """Compute the textual tile size flag for the given `transform`."""
  if not sizes or len(sizes) == 0:
    return ''
  return f'{name}={",".join([str(ts) for ts in sizes])}'


def _get_pad_str(transform: Transform) -> str:
  """Compute the textual padding flags for the given `transform`."""
  if not transform.pad:
    return ''
  pad_str = f'pad'
  pack_paddings = [str(pp) for pp in transform.pack_paddings]
  hoist_paddings = [str(hd) for hd in transform.hoist_paddings]
  transpose_paddings = [
      ':'.join([str(dim) for dim in ip]) for ip in transform.transpose_paddings
  ]

  if pack_paddings:
    pad_str = pad_str + f' pack-paddings={",".join(pack_paddings)}'
  if hoist_paddings:
    pad_str = pad_str + f' hoist-paddings={",".join(hoist_paddings)}'
  if transpose_paddings:
    pad_str = pad_str + f' transpose-paddings={",".join(transpose_paddings)}'
  return pad_str


def make_pattern_name(fun_name: str, op_name: str):
  return "match_" + op_name.replace('.', '_') + "_in_" + fun_name


def emit_transform_matcher(fun_name: str, op_name: str):
  pattern = pdl.PatternOp(benefit=1, name=make_pattern_name(fun_name, op_name))
  with InsertionPoint(pattern.body):
    args = pdl.OperandsOp()
    types = pdl.TypesOp()
    pdl_op = pdl.OperationOp(op_name, args=[args], types=[types])
    pdl.ApplyNativeConstraintOp('nestedInFunc',
                                args=[pdl_op],
                                params=[FlatSymbolRefAttr.get(fun_name)])
    pdl.RewriteOp(pdl_op, 'linalg_transform.apply')


def emit_pattern_if_not_present(fun_name: str, op_name: str):
  parent = InsertionPoint.current.block.owner.operation
  while not isinstance(parent.opview, builtin.ModuleOp) and parent:
    parent = parent.parent
  assert parent, "Expected to find a ModuleOp as parent"
  symbol_table = SymbolTable(parent)
  pattern_name = make_pattern_name(fun_name, op_name)
  if pattern_name not in symbol_table:
    with InsertionPoint(parent.opview.body):
      emit_transform_matcher(fun_name, op_name)
  return pattern_name


class Inject(Transform):
  """Inject intermediate IR.

  Replace the module by the provided IR. The transform can be configured as
  follows:
  * `ir_to_inject`: Textual IR to inject.
  """

  def __init__(self, ir_to_inject: str, **kwargs):
    self.ir_to_inject = ir_to_inject

  def __call__(self, module: Module, fun_name: str, **kwargs):
    return Module.parse(self.ir_to_inject)


class Fuse(Transform):
  """Tile a linalg op and fuse its producers.

  This transform can be configured as follows:
  * `tile_sizes`: Tile sizes used for tiling.
  * `tile_interchange`: Interchange used for tiling.
  * `pad`: Pad the operands.
  * `pack_paddings`: Pack the padded operand if the packing flag is set. `pad`
     must also be specified.
  * `hoist_paddings`: Hoist the padded operand by the specified number of loops.
     `pad` must also be specified.
  * `transpose_paddings`: Transpose the padded operands by the specified
    interchange vectors:
    transpose_paddings=[[1, 0, 2], [0, 1], [0, 1]]
    It defines the interchange [1, 0, 2] for operand one and the
    interchange [0, 1] (no transpose) for the remaining operands.
    An interchange vector has to be a permutation matching the
    operand rank. `pad` must also be specified.
  * `vectorize`: Vectorize the fused operations.
  * `vectorize_padding`: Vectorize the pad tensor operations.
  """

  variables = {
      'tile_sizes': (TilingSizesVariable, []),
      'tile_interchange': (InterchangeVariable, []),
      'pad': (BoolVariable, False),
      'pack_paddings': (PackPaddingVariable, []),
      'hoist_paddings': (HoistPaddingVariable, []),
      'transpose_paddings': (TransposePaddingVariable, []),
      'vectorize': (BoolVariable, False),
      'vectorize_paddings': (BoolVariable, False),
  }

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)
    tile_str = _get_size_list_as_str(name="tile-sizes", sizes=self.tile_sizes)
    interchange_str = _get_size_list_as_str(name="tile-interchange",
                                            sizes=self.tile_interchange)
    pad_str = _get_pad_str(self)
    vectorize_str = ''
    if self.vectorize:
      vectorize_str = f'vectorize'
      if self.vectorize_paddings:
        vectorize_str = vectorize_str + f' vectorize-padding'
    pipeline = (f'linalg-fuse{{'
                f'     anchor-func={fun_name} '
                f'     anchor-op={op_name} '
                f'     {tile_str} '
                f'     {interchange_str} '
                f'     {pad_str} '
                f'     {vectorize_str}}},'
                f'canonicalize,'
                f'cse')
    self.pipeline = (f'builtin.func({pipeline})')


class Tile(Transform):
  """Tile a linalg op with `tile_sizes`.

  This transform can be configured as follows:
  * `tile_sizes`: Tile sizes used for tiling.
  * `tile_interchange`: Interchange used for tiling.
  * `peel`: Peel the specified loops generated by the tiling pattern. Cannot be
    used together with `pad`.
  * `pad`: Pad the operands.
  * `pack_paddings`: Pack the padded operand if the packing flag is set. `pad`
    must also be specified.
  * `hoist_paddings`: Hoist the padded operand by the specified number of loops.
    pad` must also be specified.
  * `transpose_paddings`: Transpose the padded operands by the specified
    interchange vectors:
    transpose_paddings=[[1, 0, 2], [0, 1], [0, 1]]
    It defines the interchange [1, 0, 2] for operand one and the
    interchange [0, 1] (no transpose) for the remaining operands.
    An interchange vector has to be a permutation matching the
    operand rank. `pad` must also be specified.
  * `scalarize_dyn_dims`: Scalarize all dimensions that have statically
    unknown size. Either `tile_sizes` or `scalarize_dyn_dims` must be specified.
    Cannot use both at the same time. Cannot be used together with `pad` or
    `peel`.
  """

  variables = {
      'tile_sizes': (TilingSizesVariable, []),
      'tile_interchange': (InterchangeVariable, []),
      'pad': (BoolVariable, False),
      'peel': (PeelingVariable, []),
      'pack_paddings': (PackPaddingVariable, []),
      'hoist_paddings': (HoistPaddingVariable, []),
      'transpose_paddings': (TransposePaddingVariable, []),
      'scalarize_dyn_dims': (BoolVariable, False),
  }

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)
    self.fun_name = fun_name
    self.op_name = op_name

  def build_transform_ir(self):
    target = tx.MatchOp(emit_pattern_if_not_present(self.fun_name,
                                                    self.op_name))
    tile_only = tx.TileOp(target,
                          sizes=self.tile_sizes,
                          interchange=self.tile_interchange,
                          peel=self.peel,
                          scalarize_dyn_dims=self.scalarize_dyn_dims)
    # This is necessary to ensure the enabler transformations run between
    # tiling and padding. In the interpreter, they can only run between
    # transformations. In the strategy, they run between passes that
    # constitute the conflated tile command that actually corresponds to
    # SingleTilingExpertPass
    if self.pad:
      tx.TileOp(tile_only,
                pad=self.pad,
                pack_paddings=self.pack_paddings,
                hoist_paddings=self.hoist_paddings,
                transpose_paddings=self.transpose_paddings)


class Vectorize(Transform):
  """Vectorize named operations.

  This transform can be configured as follows:
  * `vectorize_paddings`: Vectorize pad tensor operations.
  * `vectorize_only_tiled`: Vectorize only tiled operations.
  """

  variables = {
      'vectorize_paddings': (BoolVariable, True),
      'vectorize_only_tiled': (BoolVariable, False),
  }

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)
    self.fun_name = fun_name
    self.op_name = op_name

  def build_transform_ir(self):
    # Emit the untargeted version if requested.
    if not self.op_name:
      tx.VectorizeOp(vectorize_padding=self.vectorize_paddings)
      return

    target = tx.MatchOp(emit_pattern_if_not_present(self.fun_name,
                                                    self.op_name))
    tx.VectorizeOp(target, vectorize_padding=self.vectorize_paddings)


class Generalize(Transform):
  """Transform a named operation to its generic form.

  This transform can be configured as follows:
  * `iterator_interchange`: Interchange the iterators of the generic operation.

  Note: After generalization the anchor op name changes to 'linalg.generic'.
  """

  variables = {
      'iterator_interchange': (InterchangeVariable, []),
  }

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self.fun_name = fun_name
    self.op_name = op_name

  def build_transform_ir(self):
    target = tx.MatchOp(emit_pattern_if_not_present(self.fun_name,
                                                    self.op_name))
    tx.GeneralizeOp(target)


class Interchange(Transform):
  """Transform a named operation to its generic form.

  This transform can be configured as follows:
  * `iterator_interchange`: Interchange the iterators of the generic operation.

  Note: After generalization the anchor op name changes to 'linalg.generic'.
  """

  variables = {
      'iterator_interchange': (InterchangeVariable, []),
  }

  def __init__(self, fun_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)
    self.fun_name = fun_name

  def build_transform_ir(self):
    target = tx.MatchOp(emit_pattern_if_not_present(self.fun_name, 'generic'))
    tx.InterchangeOp(target, iterator_interchange=self.iterator_interchange)


class DecomposeToLowerDimensionalNamedOp(Transform):
  """Rewrite all known named ops to a lower-dimensional form suitable for
  vectorization.

  TODO: atm this is applied to all supported ops, add finer-grained control.
  """

  def __init__(self, **kwargs):
    pass

  def build_transform_ir(self):
    tx.DecomposeOp()


class Bufferize(Transform):
  """Trigger one-shot bufferization on the whole module.
  """

  def __init__(self, **kwargs):
    pass

  def build_transform_ir(self):
    tx.BufferizeOp()


class LowerVectors(Transform):

  class ContractionLoweringChoice(ChoiceVariableBase):
    options = ("outerproduct", "dot", "matrixintrinsics")

  class MultiReductionLoweringChoice(ChoiceVariableBase):
    options = ("innerparallel", "innerreduction")

  class TransposeLoweringChoice(ChoiceVariableBase):
    options = ("eltwise", "flat_transpose", "shuffle")

  class VectorTransferSplitChoice(ChoiceVariableBase):
    options = ("none", "linalg-copy", "vector-transfers")

  variables = {
      'contraction_lowering': (ContractionLoweringChoice, 'outerproduct'),
      'max_transfer_rank': (IntVariable, 1),
      'multi_reduction_lowering':
          (MultiReductionLoweringChoice, 'innerparallel'),
      'split_transfers': (VectorTransferSplitChoice, 'linalg-copy'),
      'transpose_lowering': (TransposeLoweringChoice, 'eltwise'),
      'transpose_avx2_lowering': (BoolVariable, False),
      'unroll_vector_transfers': (BoolVariable, True),
      'print_after_all': (BoolVariable, False),
  }

  def __init__(self,
               stages: tp.Union[int, tp.Sequence[int]] = range(7),
               **kwargs):
    self._parse_variables_in_kwargs(kwargs)
    if isinstance(stages, int):
      stages = [stages]

    self.stages = stages

  def build_transform_ir(self):
    for name in ('max_transfer_rank', 'print_after_all'):
      if getattr(self, name) != LowerVectors.variables[name][1]:
        raise NotImplementedError(name +
                                  " not supported by the transform dialect")

    for stage in sorted(self.stages):
      tx.LowerVectorsOp(stages=[s + 1 for s in range(stage + 1)],
                        contraction_lowering=self.contraction_lowering,
                        multireduction_lowering=self.multi_reduction_lowering,
                        split_transfers=self.split_transfers,
                        unroll_vector_transfers=self.unroll_vector_transfers,
                        transpose_lowering=self.transpose_lowering,
                        transpose_avx2_lowering=self.transpose_avx2_lowering)


class LowerToLLVM(Transform):
  """Trigger lowering to LLVM on the whole module.
  """

  def __init__(self, **kwargs):
    pass

  def build_transform_ir(self):
    tx.LowerToLLVMOp()


class UnrollOneParentLoop(Transform):

  variables = {
      'parent_loop_num': (IntVariable, 1),
      'unroll_factor': (IntVariable, 1),
  }

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)
    self.fun_name = fun_name
    self.op_name = op_name

  def build_transform_ir(self):
    target = tx.MatchOp(emit_pattern_if_not_present(self.fun_name,
                                                    self.op_name))
    loop = tx.GetParentLoopOp(target, num_loops=self.parent_loop_num)
    tx.UnrollLoopOp(loop, factor=self.unroll_factor)


class PipelineOneParentLoop(Transform):

  variables = {
      'parent_loop_num': (IntVariable, 1),
      'II': (IntVariable, 1),
      'read_latency': (IntVariable, 10),
  }

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)
    self.fun_name = fun_name
    self.op_name = op_name

  def build_transform_ir(self):
    target = tx.MatchOp(emit_pattern_if_not_present(self.fun_name,
                                                    self.op_name))
    loop = tx.GetParentLoopOp(target, num_loops=self.parent_loop_num)
    tx.PipelineLoopOp(loop,
                      iteration_interval=self.II,
                      read_latency=self.read_latency)


class OutlineOneParentLoop(Transform):

  variables = {
      'parent_loop_num': (IntVariable, 1),
  }

  def __init__(self, fun_name: str, op_name: str, result_func_name: str,
               **kwargs):
    self.fun_name = fun_name
    self.op_name = op_name

  def build_transform_ir(self):
    target = tx.MatchOp(emit_pattern_if_not_present(self.fun_name,
                                                    self.op_name))
    loop = tx.GetParentLoopOp(target, num_loops=self.parent_loop_num)
    tx.OutlineLoopOp(loop, func_name=self.result_func_name)


class ApplySchedule(Transform):

  def __init__(self):
    pass

  def __call__(self, module: Module, **kwargs):
    PassManager.parse('linalg-interp-transforms').run(module)
    self.drop_schedule_from_module(module)
    return module

  def drop_schedule_from_module(self, module):
    for op in module.body.operations:
      op_name = op.operation.name
      if op_name == 'pdl.pattern' or op_name == 'linalg_transform.sequence':
        op.operation.erase()


###############################################################################
# TODO: Port to the transform dialect
###############################################################################
class LinalgExtTile(Transform):
  """Tile a linalg op with using the linalg_ext.tile op and a single
  entry tile_sizes.

  This transform can be configured as follows:
  * `tile_sizes`: The 1-D tile size used for tiling.
  """

  variables = {
      'tile_sizes': (TilingSizesVariable, []),
  }

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)
    count_non_zero = 0
    for ts in self.tile_sizes:
      if ts != 0:
        count_non_zero = count_non_zero + 1
    assert count_non_zero, 'only a single element may have count non zero'
    tile_str = _get_size_list_as_str(name="tile-sizes", sizes=self.tile_sizes)
    pipeline = (
        f'linalg-ext-tiling-to-tile-op{{'
        #f'     anchor-func={fun_name} '
        #f'     anchor-op={op_name} '
        f'     {tile_str}}}'
        #f'canonicalize,'
        #f'cse'
    )
    self.pipeline = (f'builtin.func({pipeline})')


class LinalgExtTileToSequentialFor(Transform):
  """Rewrite linalg_ext.tile op to scf.for.
  """

  variables = {}

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)

    pipeline = (f'linalg-tile-to-sequential-for,'
                f'canonicalize,'
                f'cse')
    self.pipeline = (f'builtin.func({pipeline})')


class LinalgExtTileToInParallel(Transform):
  """Rewrite linalg_ext.tile op to linalg_ext.in_parallel.
  """

  variables = {}

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)

    pipeline = (f'linalg-tile-to-in-parallel,'
                f'canonicalize,'
                f'cse')
    self.pipeline = (f'builtin.func({pipeline})')


class LinalgExtInParallelToSequentialFor(Transform):
  """Rewrite linalg_ext.in_parallel op to scf.for.
  """

  variables = {}

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)

    pipeline = (f'linalg-in-parallel-to-sequential-for,'
                f'canonicalize,'
                f'cse')
    self.pipeline = (f'builtin.func({pipeline})')


class LinalgExtInParallelToAsync(Transform):
  """Rewrite linalg_ext.in_parallel op to async.
  """

  variables = {}

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)

    pipeline = (f'linalg-in-parallel-to-async,'
                f'canonicalize,'
                f'cse')
    self.pipeline = (f'builtin.func({pipeline})')


class UnrollOneVectorOp(Transform):

  variables = {
      # Vector unrolling is similar to tiling but using unrolling instead of
      # loops. Use TilingSizesVariable as a searchable type.
      'source_shape': (TilingSizesVariable, []),
      'target_shape': (TilingSizesVariable, []),
  }

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    pass
