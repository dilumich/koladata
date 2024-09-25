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

"""Tests for from_py."""

from absl.testing import absltest
from koladata.exceptions import exceptions
from koladata.functions import functions as fns
from koladata.operators import kde_operators
from koladata.testing import testing
from koladata.types import data_slice
from koladata.types import schema_constants

kde = kde_operators.kde
ds = data_slice.DataSlice.from_vals


class FromPyTest(absltest.TestCase):

  # More detailed tests for conversions to Koda OBJECT are located in
  # obj_test.py.
  def test_object(self):
    obj = fns.from_py({'a': {'b': [1, 2, 3]}})
    testing.assert_equal(
        obj.get_schema().with_db(None), schema_constants.OBJECT
    )
    testing.assert_dicts_keys_equal(obj, ds(['a']))
    values = obj['a']
    testing.assert_equal(
        values.get_schema().with_db(None), schema_constants.OBJECT
    )
    testing.assert_dicts_keys_equal(values, ds(['b']))
    nested_values = values['b']
    testing.assert_equal(
        nested_values.get_schema().with_db(None), schema_constants.OBJECT
    )
    testing.assert_equal(nested_values[:], ds([1, 2, 3]).with_db(obj.db))

  # More detailed tests for conversions to Koda Entities for Lists are located
  # in new_test.py.
  def test_list_with_schema(self):
    # Python list items can be various Python / Koda objects that are normalized
    # to Koda Items.
    l = fns.from_py([1, 2, 3], schema=fns.list_schema(schema_constants.FLOAT32))
    testing.assert_allclose(l[:].no_db(), ds([1., 2., 3.]))

    l = fns.from_py(
        [[1, 2], [ds(42, schema_constants.INT64)]],
        schema=fns.list_schema(fns.list_schema(schema_constants.FLOAT64)),
    )
    testing.assert_allclose(
        l[:][:].no_db(), ds([[1., 2.], [42.]], schema_constants.FLOAT64)
    )

  # More detailed tests for conversions to Koda Entities for Dicts are located
  # in new_test.py.
  def test_dict_with_schema(self):
    # Python dictionary keys and values can be various Python / Koda objects
    # that are normalized to Koda Items.
    d = fns.from_py(
        {ds('a'): [1, 2], 'b': [42]},
        schema=fns.dict_schema(
            schema_constants.TEXT, fns.list_schema(schema_constants.INT32)
        ),
    )
    testing.assert_dicts_keys_equal(d, ds(['a', 'b']))
    testing.assert_equal(d[ds(['a', 'b'])][:].no_db(), ds([[1, 2], [42]]))

  def test_dict_as_obj_object(self):
    obj = fns.from_py(
        {'a': 42, 'b': {'x': 'abc'}, 'c': ds(b'xyz')}, dict_as_obj=True,
    )
    testing.assert_equal(obj.get_schema().no_db(), schema_constants.OBJECT)
    self.assertCountEqual(dir(obj), ['a', 'b', 'c'])
    testing.assert_equal(obj.a.no_db(), ds(42))
    b = obj.b
    testing.assert_equal(b.get_schema().no_db(), schema_constants.OBJECT)
    testing.assert_equal(b.x.no_db(), ds('abc'))
    testing.assert_equal(obj.c.no_db(), ds(b'xyz'))

  def test_dict_as_obj_entity_with_schema(self):
    schema = fns.new_schema(
        a=schema_constants.FLOAT32,
        b=fns.new_schema(x=schema_constants.TEXT),
        c=schema_constants.BYTES,
    )
    entity = fns.from_py(
        {'a': 42, 'b': {'x': 'abc'}, 'c': ds(b'xyz')}, dict_as_obj=True,
        schema=schema,
    )
    testing.assert_equal(entity.get_schema().no_db(), schema.no_db())
    self.assertCountEqual(dir(entity), ['a', 'b', 'c'])
    testing.assert_equal(entity.a.no_db(), ds(42.))
    b = entity.b
    testing.assert_equal(b.get_schema().no_db(), schema.b.no_db())
    testing.assert_equal(b.x.no_db(), ds('abc'))
    testing.assert_equal(entity.c.no_db(), ds(b'xyz'))

  def test_dict_as_obj_entity_with_nested_object(self):
    schema = fns.new_schema(
        a=schema_constants.INT64,
        b=schema_constants.OBJECT,
        c=schema_constants.BYTES,
    )
    entity = fns.from_py(
        {'a': 42, 'b': {'x': 'abc'}, 'c': ds(b'xyz')}, dict_as_obj=True,
        schema=schema,
    )
    testing.assert_equal(entity.get_schema().no_db(), schema.no_db())
    self.assertCountEqual(dir(entity), ['a', 'b', 'c'])
    testing.assert_equal(entity.a.no_db(), ds(42, schema_constants.INT64))
    obj_b = entity.b
    testing.assert_equal(obj_b.get_schema().no_db(), schema_constants.OBJECT)
    testing.assert_equal(obj_b.x.no_db(), ds('abc'))
    testing.assert_equal(entity.c.no_db(), ds(b'xyz'))

  def test_dict_as_obj_entity_incomplete_schema(self):
    schema = fns.new_schema(b=schema_constants.OBJECT)
    entity = fns.from_py(
        {'a': 42, 'b': {'x': 'abc'}, 'c': ds(b'xyz')}, dict_as_obj=True,
        schema=schema,
    )
    testing.assert_equal(entity.get_schema().no_db(), schema.no_db())
    self.assertCountEqual(dir(entity), ['b'])
    testing.assert_equal(entity.b.get_schema().no_db(), schema_constants.OBJECT)
    testing.assert_equal(entity.b.x.no_db(), ds('abc'))

  def test_dict_as_obj_entity_empty_schema(self):
    schema = fns.new_schema()
    entity = fns.from_py(
        {'a': 42, 'b': {'x': 'abc'}, 'c': ds(b'xyz')}, dict_as_obj=True,
        schema=schema,
    )
    testing.assert_equal(entity.get_schema().no_db(), schema.no_db())
    self.assertCountEqual(dir(entity), [])

  def test_dict_as_obj_db_adoption(self):
    obj_b = fns.from_py({'x': 'abc'}, dict_as_obj=True)
    obj = fns.from_py({'a': 42, 'b': obj_b}, dict_as_obj=True)
    testing.assert_equal(obj.b.x.no_db(), ds('abc'))

  def test_dict_as_obj_entity_incompatible_schema(self):
    schema = fns.new_schema(
        a=schema_constants.INT64,
        b=fns.new_schema(x=schema_constants.FLOAT32),
        c=schema_constants.FLOAT32,
    )
    with self.assertRaisesRegex(
        exceptions.KodaError, 'schema for attribute \'x\' is incompatible'
    ):
      fns.from_py(
          {'a': 42, 'b': {'x': 'abc'}, 'c': ds(b'xyz')}, dict_as_obj=True,
          schema=schema,
      )

  def test_dict_as_obj_dict_key_is_data_item(self):
    # Object.
    obj = fns.from_py({ds('a'): 42}, dict_as_obj=True)
    self.assertCountEqual(dir(obj), ['a'])
    testing.assert_equal(obj.a.no_db(), ds(42))
    # Entity - non TEXT schema with TEXT item.
    entity = fns.from_py(
        {ds('a').as_any(): 42}, dict_as_obj=True,
        schema=fns.new_schema(a=schema_constants.INT32)
    )
    self.assertCountEqual(dir(entity), ['a'])
    testing.assert_equal(entity.a.no_db(), ds(42))

  def test_dict_as_obj_non_unicode_key(self):
    with self.assertRaisesRegex(
        ValueError,
        'dict_as_obj requires keys to be valid unicode objects, got bytes'
    ):
      fns.from_py({b'xyz': 42}, dict_as_obj=True)

  def test_dict_as_obj_non_text_data_item(self):
    with self.assertRaisesRegex(TypeError, 'unhashable type'):
      fns.from_py({ds(['abc']): 42}, dict_as_obj=True)
    with self.assertRaisesRegex(
        ValueError, 'dict keys cannot be non-TEXT DataItems, got b\'abc\''
    ):
      fns.from_py({ds(b'abc'): 42}, dict_as_obj=True)

  def test_alias(self):
    obj = fns.from_pytree({'a': 42})
    testing.assert_equal(
        obj.get_schema().with_db(None), schema_constants.OBJECT
    )
    testing.assert_dicts_keys_equal(obj, ds(['a']))
    values = obj['a']
    testing.assert_equal(
        values.get_schema().with_db(None), schema_constants.INT32
    )
    testing.assert_equal(values, ds(42).with_db(values.db))

  def test_not_yet_implemented(self):
    with self.subTest('itemid'):
      with self.assertRaises(NotImplementedError):
        fns.from_py({'a': {'b': [1, 2, 3]}}, itemid=kde.uuid._eval(a=1))  # pylint: disable=protected-access

    with self.subTest('from_dim'):
      with self.assertRaises(NotImplementedError):
        fns.from_py({'a': {'b': [1, 2, 3]}}, from_dim=1)

  def test_arg_errors(self):
    with self.assertRaisesRegex(
        TypeError, 'expecting schema to be a DataSlice, got int'
    ):
      fns.from_py([1, 2], schema=42)
    with self.assertRaisesRegex(
        TypeError, 'expecting dict_as_obj to be a bool, got int'
    ):
      fns.from_py([1, 2], dict_as_obj=42)  # pytype: disable=wrong-arg-types

  # pylint: disable=protected-access
  # TODO: Migrate these to test_arg_errors.
  def test_internal_arg_errors(self):
    with self.assertRaisesRegex(
        ValueError, '_from_py_impl accepts exactly 5 arguments, got 2'
    ):
      fns.bag()._from_py_impl([1, 2], 42)
    with self.assertRaisesRegex(
        TypeError, 'expecting itemid to be a DataSlice, got int'
    ):
      fns.bag()._from_py_impl([1, 2], False, 42, fns.new_schema(), 0)
    with self.assertRaisesRegex(
        TypeError, 'expecting from_dim to be an int, got str'
    ):
      fns.bag()._from_py_impl([1, 2], False, fns.new(), fns.new_schema(), 'abc')
    with self.assertRaisesRegex(OverflowError, 'Python int too large'):
      fns.bag()._from_py_impl(
          [1, 2], False, fns.new(), fns.new_schema(), 1 << 100
      )
  # pylint: enable=protected-access


if __name__ == '__main__':
  absltest.main()
