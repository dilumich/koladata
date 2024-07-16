// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "py/koladata/types/wrap_utils.h"

#include <string>
#include <utility>

#include "absl/base/nullability.h"
#include "koladata/data_bag.h"
#include "koladata/data_slice.h"
#include "koladata/data_slice_qtype.h"
#include "py/arolla/abc/py_qvalue.h"
#include "py/arolla/abc/py_qvalue_specialization.h"
#include "arolla/jagged_shape/array/qtype/qtype.h"
#include "arolla/jagged_shape/dense_array/qtype/qtype.h"
#include "arolla/qtype/qtype_traits.h"
#include "arolla/qtype/typed_value.h"

namespace koladata::python {

namespace {

absl::Nullable<const DataSlice*> NotDataSliceError(PyObject* py_obj) {
  PyErr_Format(PyExc_TypeError, "expected DataSlice, got %s",
               Py_TYPE(py_obj)->tp_name);
  return nullptr;
}

absl::Nullable<DataBagPtr> NotDataBagError(PyObject* py_obj) {
  PyErr_Format(PyExc_TypeError, "expected DataBag, got %s",
               Py_TYPE(py_obj)->tp_name);
  return nullptr;
}

absl::Nullable<const DataSlice::JaggedShape*> NotJaggedShapeError(
    PyObject* py_obj) {
  PyErr_Format(PyExc_TypeError, "expected JaggedShape, got %s",
               Py_TYPE(py_obj)->tp_name);
  return nullptr;
}

}  // namespace

absl::Nullable<const DataSlice*> UnwrapDataSlice(PyObject* py_obj) {
  if (!arolla::python::IsPyQValueInstance(py_obj)) {
    return NotDataSliceError(py_obj);
  }
  const auto& typed_value = arolla::python::UnsafeUnwrapPyQValue(py_obj);
  if (typed_value.GetType() != arolla::GetQType<DataSlice>()) {
    return NotDataSliceError(py_obj);
  }
  return &typed_value.UnsafeAs<DataSlice>();
}

absl::Nullable<PyObject*> WrapPyDataSlice(DataSlice&& ds) {
  return arolla::python::WrapAsPyQValue(
      arolla::TypedValue::FromValue(std::move(ds)));
}

const DataSlice& UnsafeDataSliceRef(PyObject* py_obj) {
  return arolla::python::UnsafeUnwrapPyQValue(py_obj).UnsafeAs<DataSlice>();
}

absl::Nullable<PyObject*> WrapDataBagPtr(DataBagPtr db) {
  return arolla::python::WrapAsPyQValue(
      arolla::TypedValue::FromValue(std::move(db)));
}

absl::Nullable<DataBagPtr> UnwrapDataBagPtr(PyObject* py_obj) {
  if (!arolla::python::IsPyQValueInstance(py_obj)) {
    return NotDataBagError(py_obj);
  }
  const auto& db_typed_value = arolla::python::UnsafeUnwrapPyQValue(py_obj);
  if (db_typed_value.GetType() != arolla::GetQType<DataBagPtr>()) {
    return NotDataBagError(py_obj);
  }
  return db_typed_value.UnsafeAs<DataBagPtr>();
}

const DataBagPtr& UnsafeDataBagPtr(PyObject* py_obj) {
  return arolla::python::UnsafeUnwrapPyQValue(py_obj).UnsafeAs<DataBagPtr>();
}

absl::Nullable<const DataSlice::JaggedShape*> UnwrapJaggedShape(
    PyObject* py_obj) {
  if (!arolla::python::IsPyQValueInstance(py_obj)) {
    return NotJaggedShapeError(py_obj);
  }
  const auto& shape_typed_value = arolla::python::UnsafeUnwrapPyQValue(py_obj);
  if (shape_typed_value.GetType() !=
      arolla::GetQType<DataSlice::JaggedShape>()) {
    return NotJaggedShapeError(py_obj);
  }
  return &shape_typed_value.UnsafeAs<DataSlice::JaggedShape>();
}

absl::Nullable<PyObject*> WrapPyJaggedShape(DataSlice::JaggedShape shape) {
  return arolla::python::WrapAsPyQValue(
      arolla::TypedValue::FromValue(std::move(shape)));
}

}  // namespace koladata::python
