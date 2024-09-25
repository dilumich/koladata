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

"""Tests for kde.core.list_size."""

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
from koladata.types import jagged_shape
from koladata.types import list_item as _  # pylint: disable=unused-import
from koladata.types import qtypes
from koladata.types import schema_constants

I = input_container.InputContainer('I')
kde = kde_operators.kde
ds = data_slice.DataSlice.from_vals
bag = data_bag.DataBag.empty
DATA_SLICE = qtypes.DATA_SLICE


QTYPES = frozenset([
    (DATA_SLICE, DATA_SLICE),
])


class CoreListSizeTest(parameterized.TestCase):

  @parameterized.parameters(
      (bag().list(), ds(0, schema_constants.INT64)),
      (bag().list([1, 2, 3]), ds(3, schema_constants.INT64)),
      (
          bag().list_shaped(jagged_shape.create_shape([3])),
          ds([0, 0, 0], schema_constants.INT64),
      ),
      (
          bag().list_shaped(
              jagged_shape.create_shape([3]),
              ds([[1, 2, 3], [4], [5, 6]]),
          ),
          ds([3, 1, 2], schema_constants.INT64),
      ),
  )
  def test_eval(self, d, sizes):
    testing.assert_equal(expr_eval.eval(kde.core.list_size(I.d), d=d), sizes)

  def test_qtype_signatures(self):
    self.assertCountEqual(
        arolla.testing.detect_qtype_signatures(
            kde.core.list_size,
            possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES,
        ),
        QTYPES,
    )

  def test_view(self):
    self.assertTrue(view.has_data_slice_view(kde.core.list_size(I.x)))

  def test_alias(self):
    self.assertTrue(optools.equiv_to_op(kde.core.list_size, kde.list_size))


if __name__ == '__main__':
  absltest.main()
