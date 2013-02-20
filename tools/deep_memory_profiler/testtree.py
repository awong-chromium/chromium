# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import copy
import os
import sys

BASE_PATH = os.path.dirname(os.path.abspath(__file__))
BINTREES_PATH = os.path.join(
    BASE_PATH, os.pardir, os.pardir, 'third_party', 'bintrees')
sys.path.insert(0, BINTREES_PATH)

from bintrees import FastRBTree


class ExclusiveRangeDict(object):
  class RangeAttribute(object):
    def __init__(self):
      pass

    def __str__(self):
      return '<RangeAttribute>'

    def __repr__(self):
      return '<RangeAttribute>'

    def copy(self):
      return RangeAttribute()

  def __init__(self, attr=RangeAttribute):
    self._tree = FastRBTree()
    self._attr = attr

  def iter_range(self, begin, end, entire=False):
    # Assume that self._tree has at least one element.
    if self._tree.is_empty():
      self._tree[begin] = (end, self._attr())

    # Create a beginning range (border)
    try:
      bound_begin, bound_value = self._tree.floor_item(begin)
      bound_end = bound_value[0]
      if begin >= bound_end:
        # Create a blank range.
        try:
          new_end, _ = self._tree.succ_item(bound_begin)
        except KeyError:
          new_end = end
        self._tree[begin] = (min(end, new_end), self._attr())
      elif bound_begin < begin and begin < bound_end:
        # Split the existing range.
        new_end = bound_value[0]
        new_value = bound_value[1]
        self._tree[bound_begin] = (begin, new_value.copy())
        self._tree[begin] = (new_end, new_value.copy())
      else: # bound_begin == begin
        # Do nothing (just saying it clearly since this part is confusing)
        pass
    except KeyError: # begin is less than the smallest element.
      # Crate a blank range.
      # Note that we can assume self._tree has at least one element.
      self._tree[begin] = (min(end, self._tree.min_key()), self._attr())

    # Create an ending range (border)
    try:
      bound_begin, bound_value = self._tree.floor_item(end)
      bound_end = bound_value[0]
      if end > bound_end:
        # Create a blank range.
        new_begin = bound_end
        self._tree[new_begin] = (end, self._attr())
      elif bound_begin < end and end < bound_end:
        # Split the existing range.
        new_end = bound_value[0]
        new_value = bound_value[1]
        self._tree[bound_begin] = (end, new_value.copy())
        self._tree[end] = (new_end, new_value.copy())
      else: # bound_begin == begin
        # Do nothing (just saying it clearly since this part is confusing)
        pass
    except KeyError: # end is less than the smallest element.
      # It must not happen.  A blank range [begin,end) has already been created
      # even if [begin,end) is less than the smallest range.
      pass

    missing_ranges = []

    prev_end = None
    if entire:
      for range_begin, range_value in self._tree.items():
        range_end = range_value[0]
        # Note that we can assume that we have a range beginning with |begin|
        # and a range ending with |end| (they may be the same range).
        if prev_end and prev_end != range_begin:
          missing_ranges.append((prev_end, range_begin))
        prev_end = range_end
    else:
      for range_begin, range_value in self._tree.itemslice(begin, end):
        range_end = range_value[0]
        # Note that we can assume that we have a range beginning with |begin|
        # and a range ending with |end| (they may be the same range).
        if prev_end and prev_end != range_begin:
          missing_ranges.append((prev_end, range_begin))
        prev_end = range_end

    for missing_begin, missing_end in missing_ranges:
      self._tree[missing_begin] = (missing_end, self._attr())

    if entire:
      for range_begin, range_value in self._tree.items():
        range_end = range_value[0]
        in_range = begin <= range_begin and range_end <= end
        yield range_begin, range_value[0], range_value[1], in_range
    else:
      for range_begin, range_value in self._tree.itemslice(begin, end):
        yield range_begin, range_value[0], range_value[1]

  def __str__(self):
    return str(self._tree)


class ListAttribute(ExclusiveRangeDict.RangeAttribute):
  def __init__(self):
    self._list = []

  def __str__(self):
    return str(self._list)

  def __repr__(self):
    return 'ListAttribute' + str(self._list)

  def __iter__(self):
    for x in self._list:
      yield x

  def append(self, x):
    self._list.append(x)

  def copy(self):
    new = ListAttribute()
    new._replace(copy.copy(self._list))
    return new

  def _replace(self, new_list):
    self._list = new_list


def main():
  regions = ExclusiveRangeDict(ListAttribute)

  for begin, end, attr, in_range in regions.iter_range(20, 500, True):
    attr.append('hoge')
    print '%d-%d:%s' % (begin, end, attr)
  print '---'

  for begin, end, attr, in_range in regions.iter_range(545, 600, True):
    if in_range:
      attr.append('in')
    else:
      attr.append('out')
    print '%d-%d:%s' % (begin, end, attr)
  print '---'

  for begin, end, attr in regions.iter_range(500, 520):
    print '%d-%d:%s' % (begin, end, attr)
  print '---'

  for begin, end, attr in regions.iter_range(510, 530):
    print '%d-%d:%s' % (begin, end, attr)
  print '---'

  for begin, end, attr in regions.iter_range(10, 15):
    print '%d-%d:%s' % (begin, end, attr)
  print '---'

  for begin, end, attr in regions.iter_range(15, 530):
    print '%d-%d:%s' % (begin, end, attr)
  print '---'

  for begin, end, attr in regions.iter_range(15, 530):
    print '%d-%d:%s' % (begin, end, attr)
  print '---'

  for begin, end, attr in regions.iter_range(15, 529):
    print '%d-%d:%s' % (begin, end, attr)
  print '---'

  for begin, end, attr in regions.iter_range(15, 530):
    print '%d-%d:%s' % (begin, end, attr)
  print '---'

  for begin, end, attr in regions.iter_range(5, 610):
    print '%d-%d:%s' % (begin, end, attr)
  print '---'


def testFastRBTree():
  tree = FastRBTree()
  print tree.is_empty()
  tree[120] = 'foo'
  print tree.is_empty()
  tree[150] = 'bar'
  print tree.get(120)
  print tree.get(150)
  try:
    print tree.floor_item(119)
  except KeyError, e:
    print "Not found: " + str(e)
  print tree.floor_item(120)
  print tree.floor_item(121)
  print tree.floor_item(149)
  print tree.floor_item(150)
  print tree.floor_item(151)
  print tree.get(150)
  return 0


if __name__ == '__main__':
  sys.exit(main())
