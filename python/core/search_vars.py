"""Utilities for search space exploration over linalg operations."""

from itertools import chain
from mlir.dialects.linalg.opdsl.lang import OperandKind
import random
import math


def rand_in_range(value_range):
  return random.randrange(value_range.start, value_range.stop, value_range.step)


class Variable:
  'Abstract class used as a base for all search variables.'

  def __init__(self, name):
    self.name = name

  def assign(self, assignments, value):
    'Assigns variable to a given value in assignments dictionary.'

    assignments[self.name] = value

  def random_value(self):
    'Abstract method that returns a valid random value for this variable.'


class TypeVariable(Variable):
  'Linalg operation-specific type variable that defines a scalar component.'

  def __init__(self, name, scalar_types):
    Variable.__init__(self, name)
    self.scalar_types = scalar_types

  def __repr__(self):
    return f'TypeVariable({self.name})'

  def random_value(self):
    return random.choice(self.scalar_types)


class IntVariable(Variable):
  'Linalg operation-specific integer dimension variable.'

  def __init__(self, name, value_range):
    Variable.__init__(self, name)
    self.value_range = value_range

  def __repr__(self):
    return f'IntVariable({self.name}, {self.value_range})'

  def random_value(self):
    return rand_in_range(self.value_range)


class BoolVariable(Variable):
  'Boolean flag variable.'

  def __repr__(self):
    return f'BoolVariable({self.name})'

  def random_value(self):
    return random.randint(0, 1) == 0


class DimensionVariable(IntVariable):
  'Variable that corresponds to the operation dimensions.'

  def __init__(self, name, value_range):
    IntVariable.__init__(self, name, value_range)

  def __repr__(self):
    return f'DimensionVariable({self.name}, {self.value_range})'


class TilingSizesVariable(Variable):
  'Variable that corresponds to tiling sizes.'

  def __init__(self, name, length_ranges, value_ranges):
    Variable.__init__(self, name)
    if name in length_ranges:
      self.length_range = length_ranges[name]
    else:
      self.length_range = length_ranges['default']
    if name in value_ranges:
      self.value_range = value_ranges[name]
    else:
      self.value_range = value_ranges['default']

  def __repr__(self):
    return f'TilingSizesVariable({self.name}, {self.length_range}, {self.value_range})'

  def random_value(self):
    result = []
    for x in range(rand_in_range(self.length_range)):
      result.append(rand_in_range(self.value_range))
    return result


class InterchangeVariable(Variable):
  'Variable that corresponds to a dimension interchange.'

  def __init__(self, name, length_ranges):
    Variable.__init__(self, name)
    if name in length_ranges:
      self.length_range = length_ranges[name]
    else:
      self.length_range = length_ranges['default']

  def __repr__(self):
    return f'InterchangeVariable({self.name}, {self.length_range})'

  def random_value(self):
    result = list(range(rand_in_range(self.length_range)))
    random.shuffle(result)
    return result


class PeelingVariable(Variable):
  'Variable that corresponds to loop peeling.'

  def __init__(self, name, length_ranges):
    Variable.__init__(self, name)
    if name in length_ranges:
      self.length_range = length_ranges[name]
    else:
      self.length_range = length_ranges['default']

  def __repr__(self):
    return f'PeelingVariable({self.name}, {self.length_range})'

  def random_value(self):
    options = list(range(rand_in_range(self.length_range)))
    k = random.randint(0, len(options))
    return sorted(random.sample(options, k))


class PackPaddingVariable(Variable):
  'Variable that corresponds to pack padding.'

  def __init__(self, name, length_ranges):
    Variable.__init__(self, name)
    if name in length_ranges:
      self.length_range = length_ranges[name]
    else:
      self.length_range = length_ranges['default']

  def __repr__(self):
    return f'PackPaddingVariable({self.name}, {self.length_range})'

  def random_value(self):
    result = []
    for x in range(rand_in_range(self.length_range)):
      result.append(random.randint(0, 1))
    return result


class HoistPaddingVariable(IntVariable):
  'Variable that corresponds to hoist padding.'

  def __init__(self, name, length_ranges, value_ranges):
    Variable.__init__(self, name)
    if name in length_ranges:
      self.length_range = length_ranges[name]
    else:
      self.length_range = length_ranges['default']
    if name in value_ranges:
      self.value_range = value_ranges[name]
    else:
      self.value_range = value_ranges['default']

  def __repr__(self):
    return f'HoistPaddingVariable({self.name}, {self.length_range}, {self.value_range})'

  def random_value(self):
    result = []
    for x in range(rand_in_range(self.length_range)):
      result.append(rand_in_range(self.value_range))
    return result


def collect_variables(op):
  type_vars = set()
  syms = set()
  for odef in op.model.registered_operands.values():
    type_vars.add(odef.type_var)
    if (odef.kind == OperandKind.InputTensor or
        odef.kind == OperandKind.OutputTensor):
      for sym in odef.size_exprs:
        syms.add(sym)

  variables = {}
  for type_var in type_vars:
    variables[type_var.name] = TypeVariable
  for sym in syms:
    variables[sym.symname] = DimensionVariable
  return variables


def create_variable(name, variable_type, **settings):
  if variable_type is TypeVariable:
    return TypeVariable(name, settings['types'])
  elif variable_type is IntVariable:
    return IntVariable(name, settings['int_range'])
  elif variable_type is BoolVariable:
    return BoolVariable(name)
  elif variable_type is DimensionVariable:
    return DimensionVariable(name, settings['dim_range'])
  elif variable_type is TilingSizesVariable:
    return TilingSizesVariable(name, settings['tsize_length_range'],
                               settings['tsize_value_range'])
  elif variable_type is InterchangeVariable:
    return InterchangeVariable(name, settings['tsize_length_range'])
  elif variable_type is PackPaddingVariable:
    return PackPaddingVariable(name, settings['ppad_length_range'])
  elif variable_type is HoistPaddingVariable:
    return HoistPaddingVariable(name, settings['hpad_length_range'],
                                settings['hpad_value_range'])
  elif variable_type is PeelingVariable:
    return PeelingVariable(name, settings['tsize_length_range'])
  else:
    raise Exception(f'unknown variable type: {variable_type}')

def _get_variable_classes(variables):
  return ((name, variable[0] if isinstance(variable, tuple) else variable)
          for name, variable in variables.items())


def instantiate_variables(variables, **settings):
  assignments = {}
  for name, variablecls in _get_variable_classes(variables):
    variable = create_variable(name, variablecls, **settings)
    variable.assign(assignments, variable.random_value())
  return assignments


def are_constraints_satisfied(assignment, variables, **settings):
  tile_sizes_names = [
      name for name, variablecls in _get_variable_classes(variables)
      if variablecls is TilingSizesVariable
  ]
  interchange_names = [
      name for name, variablecls in _get_variable_classes(variables)
      if variablecls is InterchangeVariable
  ]
  # Check the tile sizes and interchange lengths match.
  for tile_sizes, interchange in zip(tile_sizes_names, interchange_names):
    if len(assignment[interchange]) == 0:
      continue
    if len(assignment[interchange]) != len(assignment[tile_sizes]):
      return False
  dims = max([len(assignment[name]) for name in tile_sizes_names])
  # Check the tile sizes increase monotonically from inner to outer tile loops.
  for dim in range(dims):
    last = math.inf
    for name in tile_sizes_names:
      current = assignment[name][dim] if dim < len(assignment[name]) else 0
      if current == 0:
        continue
      if current >= last:
        return False
      last = current
    # Check the size of the innermost tile is bound.
    if last > settings['tsize_register_tile_bound']:
      return False
  return True
