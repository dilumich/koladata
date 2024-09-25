# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Tests for allocation.new_listid_shaped_as."""

from absl.testing import absltest
from absl.testing import parameterized
from arolla import arolla
from koladata.expr import expr_eval
from koladata.expr import input_container
from koladata.expr import view
from koladata.operators import kde_operators
from koladata.operators import optools
from koladata.operators.tests.util import qtypes as test_qtypes
from koladata.testing import testing
from koladata.types import data_bag
from koladata.types import data_slice
from koladata.types import qtypes
from koladata.types import schema_constants

I = input_container.InputContainer('I')
bag = data_bag.DataBag.empty
ds = data_slice.DataSlice.from_vals
kde = kde_operators.kde


# TODO: Test non-determinism when it gets implemented.
class AllocationNewListIdShapedAsTest(parameterized.TestCase):

  @parameterized.parameters(
      (ds([1, 1]), ds([[42], [42]])),
      (ds([[1, 1], [1]]), ds([[[42], [42]], [[42]]])),
      (ds(1), ds([42])),
  )
  def test_eval(self, shape, items):
    listid = expr_eval.eval(kde.allocation.new_listid_shaped_as(shape))
    testing.assert_equal(listid.get_schema(), schema_constants.ITEMID)
    lst = listid.with_db(bag())
    lst = lst.with_schema(lst.db.list_schema(schema_constants.INT32))
    lst.append(42)
    testing.assert_equal(lst[:], items.with_db(lst.db))

  def test_new_alloc_ids(self):
    listid = expr_eval.eval(kde.allocation.new_listid_shaped_as(ds([1, 1])))
    testing.assert_equal(
        listid, expr_eval.eval(kde.allocation.new_listid_shaped_as(ds([1, 1])))
    )
    arolla.abc.clear_caches()
    with self.assertRaises(AssertionError):
      testing.assert_equal(
          listid,
          expr_eval.eval(kde.allocation.new_listid_shaped_as(ds([1, 1])))
      )

  def test_qtype_signatures(self):
    self.assertCountEqual(
        arolla.testing.detect_qtype_signatures(
            kde.allocation.new_listid_shaped_as,
            possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES,
        ),
        frozenset([(qtypes.DATA_SLICE, qtypes.DATA_SLICE)]),
    )

  def test_view(self):
    self.assertTrue(
        view.has_data_slice_view(kde.allocation.new_listid_shaped_as(I.x))
    )

  def test_alias(self):
    self.assertTrue(
        optools.equiv_to_op(
            kde.allocation.new_listid_shaped_as, kde.new_listid_shaped_as,
        )
    )


if __name__ == '__main__':
  absltest.main()
