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

"""Tests for kde.core.no_db."""

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

I = input_container.InputContainer('I')
bag = data_bag.DataBag.empty
ds = data_slice.DataSlice.from_vals
kde = kde_operators.kde


class CoreNoDbTest(parameterized.TestCase):

  @parameterized.parameters(
      bag().obj(),
      bag().list([1, 2, 3]),
      ds([bag().obj(a=1)]),
      ds([1, 2, 3]),
  )
  def test_eval(self, x):
    testing.assert_equal(expr_eval.eval(kde.core.no_db(x)), x.with_db(None))

  def test_qtype_signatures(self):
    arolla.testing.assert_qtype_signatures(
        kde.core.no_db,
        [(qtypes.DATA_SLICE, qtypes.DATA_SLICE)],
        possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES,
    )

  def test_view(self):
    self.assertTrue(view.has_data_slice_view(kde.core.no_db(I.x)))

  def test_alias(self):
    self.assertTrue(optools.equiv_to_op(kde.core.no_db, kde.no_db))


if __name__ == '__main__':
  absltest.main()
