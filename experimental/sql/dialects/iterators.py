# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from dataclasses import dataclass
from typing import Any, Union, List
from xdsl.ir import Block, Region, Operation, SSAValue, ParametrizedAttribute, Data, MLContext, Attribute
from xdsl.dialects.builtin import StringAttr, ArrayAttr, ArrayOfConstraint, IntegerAttr, IntegerType, TupleType, FlatSymbolRefAttr
from xdsl.irdl import AttributeDef, OperandDef, ResultDef, RegionDef, SingleBlockRegionDef, irdl_attr_definition, irdl_op_definition, ParameterDef, AnyAttr, VarOperandDef, builder

# This mirrors a subset of the MLIR iterators dialect in a one to one way
#
# (https://github.com/google/iree-llvm-sandbox/tree/main/experimental/iterators)

#===------------------------------------------------------------------------===#
# Data types
#===------------------------------------------------------------------------===#

#===------------------------------------------------------------------------===#
# Query types
#===------------------------------------------------------------------------===#


@irdl_attr_definition
class Stream(ParametrizedAttribute):
  """
  Stream of elements of the given type.
  """
  name = "iterators.stream"

  types: ParameterDef[Attribute]

  @builder
  @staticmethod
  def get(elem_type: Attribute) -> 'Stream':
    return Stream([elem_type])  #type: ignore


@irdl_attr_definition
class ReduceFuncRefAttr(FlatSymbolRefAttr):
  name = "iterators.reduceFuncRefAttr"

  data = ParameterDef(StringAttr)


#===------------------------------------------------------------------------===#
# Operations
#===------------------------------------------------------------------------===#


@irdl_op_definition
class SampleInputOp(Operation):
  """
  Create some sample input.
  """
  name = "iterators.sampleInput"

  result = ResultDef(Stream)

  @builder
  @staticmethod
  def get(type: Attribute) -> 'SampleInputOp':
    return SampleInputOp.create(result_types=[type])


@irdl_op_definition
class ReduceOp(Operation):
  """
  Reads the elements of its operand stream and reduces them to a single
  element using the provided reduce function. The result stream is empty iff
  the operand stream is empty. Otherwise, the elements are reduced pairwise in
  an implementation-defined order until a single element is left, which
  constitutes the result stream. This is only deterministic if the reduce
  function is associative.

  Example:
  ```mlir
  %reduced = "iterators.reduce"(%input) {reduceFuncRef = @sum} :
                 (!iterators.stream<i32>) -> (!iterators.stream<i32>)
  ```
  """
  name = "iterators.reduce"

  input = OperandDef(Stream)
  reduceFuncRef = AttributeDef(ReduceFuncRefAttr)

  result = ResultDef(Stream)

  @builder
  @staticmethod
  def get(input: Operation, func: StringAttr,
          res_type: Attribute) -> 'ReduceOp':
    return ReduceOp.build(
        operands=[input],
        attributes={"reduceFuncRef": ReduceFuncRefAttr([func])},
        result_types=[res_type])


@irdl_op_definition
class ConstantStreamOp(Operation):
  """
  Produces a stream of LLVM structs given in the array of arrays attribute
  (each inner array being returned as a literal LLVM struct with the values
  and types of the elements of that array). The inner arrays all have to have
  matching types, i.e., the element at position i has to be the same for all
  inner arrays, and the element type of the return Stream has to be the
  corresponding literal LLVM struct. An empty array is allowed (in which case
  the return Stream does not need to match anything).

  Example:
  ```mlir
  %0 = "iterators.constantstream"() { value = [[42 : i32]] } :
          () -> (!iterators.stream<!llvm.struct<(i32)>>)
  ```
  """
  name = "iterators.constantstream"

  value = AttributeDef(ArrayOfConstraint(ArrayOfConstraint(AnyAttr())))
  result = ResultDef(Stream)

  @builder
  @staticmethod
  def get(values: List[List[Attribute]],
          res_type: Attribute) -> 'ConstantstreamOp':
    return ConstantstreamOp.build(attributes={
        "value": ArrayAttr.from_list([ArrayAttr.from_list(v) for v in values])
    },
                                  result_types=[res_type])


@irdl_op_definition
class SinkOp(Operation):
  """
  Consume tuples from an iterator.
  """
  name = "iterators.sink"

  arguments = OperandDef(Stream)

  @builder
  @staticmethod
  def get(arguments: Operation) -> 'SinkOp':
    return SinkOp.create(operands=[arguments.result])


@dataclass
class Iterators:
  ctx: MLContext

  def __post_init__(self: 'Iterators'):
    self.ctx.register_attr(Stream)
    self.ctx.register_attr(ReduceFuncRefAttr)

    self.ctx.register_op(SampleInputOp)
    self.ctx.register_op(ReduceOp)
    self.ctx.register_op(SinkOp)
    self.ctx.register_op(ConstantStreamOp)
