from mlir.ir import *
from mlir.passmanager import PassManager

from .variables import *
from .transform import Transform, TransformationList

import mlir.all_passes_registration

import typing as tp


def _get_tile_sizes_str(transform: Transform) -> str:
  """Compute the textual tile size flag for the given `transform`."""
  if not transform.tile_sizes:
    return ''
  return f'tile-sizes={",".join([str(ts) for ts in transform.tile_sizes])}'


def _get_tile_interchange_str(transform: Transform) -> str:
  """Compute the textual tile interchange flag for the given `transform`."""
  if not transform.tile_interchange:
    return ''
  tile_interchange = [str(ti) for ti in transform.tile_interchange]
  return f'tile-interchange={",".join(tile_interchange)}'


def _get_pad_str(transform: Transform) -> str:
  """Compute the textual padding flags for the given `transform`."""
  if not transform.pad:
    return ''
  pad_str = f'pad'
  pack_paddings = [str(pp) for pp in transform.pack_paddings]
  hoist_paddings = [str(hd) for hd in transform.hoist_paddings]
  if pack_paddings:
    pad_str = pad_str + f' pack-paddings={",".join(pack_paddings)}'
  if hoist_paddings:
    pad_str = pad_str + f' hoist-paddings={",".join(hoist_paddings)}'
  return pad_str


class ExperimentalSplitAndFuseFillOp(Transform):
  """Tile and fuse FillOp into the output of reduction.

  This transform can be configured as follows:
  * `tile_sizes`: Tile sizes used for tiling.
  """

  def __init__(self, fun_name: str, op_name: str, tile_sizes=[], **kwargs):
    if tile_sizes:
      tile_str = f'tile-sizes={",".join([str(ts) for ts in tile_sizes])}'
    pipeline = (f'linalg-fuse-fill-into-reduction{{'
                f'     anchor-func={fun_name} '
                f'     anchor-op={op_name} '
                f'     {tile_str}}},'
                f'canonicalize,'
                f'cse')
    self.pipeline = (f'builtin.func({pipeline})')


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
     pad` must also be specified.
  * `vectorize`: Vectorize the fused operations.
  * `vectorize_padding`: Vectorize the pad tensor operations.
  """

  variables = {
      'tile_sizes': (TilingSizesVariable, []),
      'tile_interchange': (InterchangeVariable, []),
      'pad': (BoolVariable, False),
      'pack_paddings': (PackPaddingVariable, []),
      'hoist_paddings': (HoistPaddingVariable, []),
      'vectorize': (BoolVariable, False),
      'vectorize_paddings': (BoolVariable, False),
  }

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)
    tile_str = _get_tile_sizes_str(self)
    interchange_str = _get_tile_interchange_str(self)
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
      'scalarize_dyn_dims': (BoolVariable, False),
  }

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)
    tile_str = _get_tile_sizes_str(self)
    interchange_str = _get_tile_interchange_str(self)
    pad_str = _get_pad_str(self)
    peeled_loops_str = ''
    scalarize_dyn_dims_str = ''
    if self.peel:
      loop_indices = [str(l) for l in self.peel]
      peeled_loops_str = f'peeled-loops={",".join(loop_indices)}'
    if self.scalarize_dyn_dims:
      scalarize_dyn_dims_str = 'scalarize-dynamic-dims'

    pipeline = (f'linalg-single-tiling-expert-driver{{'
                f'     anchor-func={fun_name} '
                f'     anchor-op={op_name} '
                f'     {tile_str} '
                f'     {interchange_str} '
                f'     {peeled_loops_str} '
                f'     {scalarize_dyn_dims_str} '
                f'     {pad_str}}},'
                f'canonicalize,'
                f'cse')
    self.pipeline = (f'builtin.func({pipeline})')


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
    assert len(self.tile_sizes) == 1, "expected single tile size, got: " + \
      str(self.tile_sizes)

    pipeline = (
        f'linalg-ext-tiling-to-tile-op{{'
        #f'     anchor-func={fun_name} '
        #f'     anchor-op={op_name} '
        f'     tile-size={self.tile_sizes[0]}}}'
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
                f'linalg-in-parallel-to-sequential-for,'
                f'canonicalize,'
                f'cse')
    self.pipeline = (f'builtin.func({pipeline})')


class Vectorize(Transform):
  """Vectorize named operations.

  This transform can be configured as follows:
  * `vectorize_paddings`: Vectorize pad tensor operations.
  """

  variables = {
      'vectorize_paddings': (BoolVariable, True),
  }

  def __init__(self, fun_name: str, op_name: str, **kwargs):
    self._parse_variables_in_kwargs(kwargs)
    vectorize_paddings_str = ''
    if self.vectorize_paddings:
      vectorize_paddings_str = 'vectorize-padding'
    pipeline = (f'linalg-single-tiling-expert-driver{{'
                f'     anchor-func={fun_name} '
                f'     anchor-op={op_name} '
                f'     vectorize '
                f'     {vectorize_paddings_str}}},'
                f'canonicalize,'
                f'cse')
    self._parse_variables_in_kwargs(kwargs)
    self.pipeline = (f'builtin.func({pipeline})')


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
    self._parse_variables_in_kwargs(kwargs)
    interchange_str = ''

    if self.iterator_interchange:
      dims = [str(ic) for ic in self.iterator_interchange]
      interchange_str = f'iterator-interchange={",".join(dims)}'

    pipeline = (f'linalg-single-tiling-expert-driver{{'
                f'     anchor-func={fun_name} '
                f'     anchor-op={op_name} '
                f'     generalize '
                f'     {interchange_str}}}')
    self.pipeline = (f'builtin.func({pipeline})')


class DecomposeToLowerDimensionalNamedOp(Transform):
  """Rewrite all known named ops to a lower-dimensional form suitable for

     vectorization.

    TODO: atm this is applied to all supported ops. If/when we need finer
    control this should be exposed with an opName + filter and a proper
    pattern.
  """

  def __init__(self, **kwargs):
    pipeline = (f'linalg-single-tiling-expert-driver{{'
                f'     decompose-to-lower-dim }}')
    self.pipeline = (f'builtin.func({pipeline})')


class Bufferize(Transform):

  def __init__(self, **kwargs):
    pipeline = (f'linalg-bufferization-driver,'
                f'canonicalize,'
                f'cse')
    self.pipeline = pipeline


class LowerVectors(Transform):

  class ContractionLoweringChoice(ChoiceVariableBase):
    options = ("outerproduct", "dot", "matrixintrinsics")

  class MultiReductionLoweringChoice(ChoiceVariableBase):
    options = ("innerparallel", "innerreduction")

  class TransposeLoweringChoice(ChoiceVariableBase):
    options = ("eltwise", "flat_transpose", "shuffle")

  variables = {
      'contraction_lowering':
          (ContractionLoweringChoice, ContractionLoweringChoice.options[0]),
      'multi_reduction_lowering': (MultiReductionLoweringChoice,
                                   MultiReductionLoweringChoice.options[0]),
      'transpose_lowering':
          (TransposeLoweringChoice, TransposeLoweringChoice.options[0]),
      'transpose_avx2_lowering': (BoolVariable, False)
  }

  def __init__(self,
               stages: tp.Union[int, tp.Sequence[int]] = range(7),
               **kwargs):
    if isinstance(stages, int):
      stages = [stages]

    self._parse_variables_in_kwargs(kwargs)

    pipelines = [
        (f'linalg-vector-lowering{{'
         f'    lower-vector-stage={stage}'
         f'    max-transfer-rank=1 '
         f'    split-transfers=linalg-copy '
         f'    lower-vector-transpose-to={self.transpose_lowering} '
         f'    lower-vector-transpose-to-avx2={self.transpose_avx2_lowering} '
         f'    lower-vector-multi-reduction-to={self.multi_reduction_lowering} '
         f'    lower-vector-contraction-to={self.contraction_lowering} '
         f'    unroll-vector-transfers=true}},'
         f'canonicalize,'
         f'cse') for stage in stages
    ]
    self.pipelines = [f'builtin.func({pipeline})' for pipeline in pipelines]

  def __call__(self, module: Module, fun_name: str):
    for pipeline in self.pipelines:
      PassManager.parse(pipeline).run(module)
    return module


class LowerToLLVM(Transform):

  def __init__(self, **kwargs):
    pipeline = (f'llvm-lowering,'
                f'canonicalize,'
                f'cse')
    self.pipeline = pipeline


class Sparsify(Transform):

  def __init__(self, options: str):
    pipeline = (
        f'sparsification{{{options}}},'
        f'sparse-tensor-conversion,'
        f'builtin.func(convert-linalg-to-loops,convert-vector-to-scf),'
        f'convert-scf-to-std,'
        f'func-bufferize,'
        f'tensor-constant-bufferize,'
        f'builtin.func(tensor-bufferize,std-bufferize,finalizing-bufferize),'
        f'convert-vector-to-llvm{{reassociate-fp-reductions=1 enable-index-optimizations=1}},'
        f'lower-affine,'
        f'convert-memref-to-llvm,'
        f'convert-std-to-llvm,'
        f'reconcile-unrealized-casts')
    self.pipeline = pipeline
