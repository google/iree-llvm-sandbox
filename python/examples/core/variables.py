"""Utilities for search space exploration over linalg operations."""

import typing as tp


class Variable:
  'Abstract class used as a base for all search variables.'

  def __init__(self, name):
    self.name = name

  def assign(self, assignments, value):
    'Assigns variable to a given value in assignments dictionary.'

    assignments[self.name] = value


class TypeVariable(Variable):
  'Linalg operation-specific type variable that defines a scalar component.'

  def __init__(self, name, scalar_types):
    Variable.__init__(self, name)
    self.scalar_types = scalar_types

  def __repr__(self):
    return f'TypeVariable({self.name})'


class IntVariable(Variable):
  'Linalg operation-specific integer dimension variable.'

  def __init__(self, name, value_range):
    Variable.__init__(self, name)
    self.value_range = value_range

  def __repr__(self):
    return f'IntVariable({self.name}, {self.value_range})'


class BoolVariable(Variable):
  'Boolean flag variable.'

  def __repr__(self):
    return f'BoolVariable({self.name})'


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


class ChoiceVariableBase(Variable):
  """Base class for choice variables.

  Subclasses must define an `options` attribute with the sequence of options to
  choose from.
  """
  options: tp.Sequence

  def __repr__(self):
    return f'{self.__class__.__name__}({self.name}, {self.options})'
