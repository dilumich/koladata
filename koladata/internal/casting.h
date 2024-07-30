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
#ifndef KOLADATA_INTERNAL_CASTING_H_
#define KOLADATA_INTERNAL_CASTING_H_

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "koladata/internal/data_bag.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/object_id.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/expr/quote.h"
#include "arolla/qexpr/operators/core/cast_operator.h"
#include "arolla/qexpr/operators/strings/strings.h"
#include "arolla/qtype/qtype.h"
#include "arolla/qtype/qtype_traits.h"
#include "arolla/util/bytes.h"
#include "arolla/util/meta.h"
#include "arolla/util/text.h"
#include "arolla/util/unit.h"
#include "arolla/util/view_types.h"
#include "arolla/util/status_macros_backport.h"

namespace koladata::schema {
namespace schema_internal {

using kNumericsCompatible =
    arolla::meta::type_list<int, int64_t, float, double, bool>;
using kStrings = arolla::meta::type_list<arolla::Text, arolla::Bytes>;

std::string GetQTypeName(arolla::QTypePtr qtype);

// Casts the given item/slice to the provided type T ("self") without any data
// conversion. Asserts that the provided data is empty-and-unknown or only holds
// values of type T.
template <typename T>
struct ToSelf {
  absl::StatusOr<internal::DataItem> operator()(
      const internal::DataItem& item) const {
    if (!item.has_value() || item.holds_value<T>()) {
      return item;
    }
    return absl::InvalidArgumentError(absl::StrFormat(
        "cannot cast %s to %v", schema_internal::GetQTypeName(item.dtype()),
        schema_internal::GetQTypeName(arolla::GetQType<T>())));
  }

  absl::StatusOr<internal::DataSliceImpl> operator()(
      const internal::DataSliceImpl& slice) const {
    if (slice.is_empty_and_unknown() ||
        slice.dtype() == arolla::GetQType<T>()) {
      return slice;
    }
    RETURN_IF_ERROR(
        slice.VisitValues([&]<class T2>(const arolla::DenseArray<T2>& values) {
          if constexpr (std::is_same_v<T, T2>) {
            return absl::OkStatus();
          } else {
            return absl::InvalidArgumentError(absl::StrFormat(
                "cannot cast %s to %v",
                schema_internal::GetQTypeName(arolla::GetQType<T2>()),
                schema_internal::GetQTypeName(arolla::GetQType<T>())));
          }
        }));
    return absl::UnknownError(
        absl::StrCat("unexpected DataSlice state", slice));
  }
};

// Casts the given item/slice to the provided type DST ("self") with potential
// data conversion using `CastOp`. The provided data is expected to be
// empty-and-unknown or hold (potentially mixed) values of the types listed in
// `SRCs`.
template <typename CastOp, typename DST, typename SRCs>
struct ToDST {
  absl::StatusOr<internal::DataItem> operator()(
      const internal::DataItem& item) const {
    if (!item.has_value() || item.holds_value<DST>()) {
      return item;
    }
    ASSIGN_OR_RETURN(
        auto res,
        item.VisitValue([&]<class T>(const T& value) -> absl::StatusOr<DST> {
          if constexpr (std::is_same_v<DST, T>) {
            return DST(value);
          } else if constexpr (arolla::meta::contains_v<SRCs, T>) {
            return CastOp()(value);
          } else {
            return absl::InvalidArgumentError(
                absl::StrFormat("cannot cast %s to %v",
                                GetQTypeName(item.dtype()), GetDType<DST>()));
          }
        }));
    return internal::DataItem(res);
  }

  absl::StatusOr<internal::DataSliceImpl> operator()(
      const internal::DataSliceImpl& slice) const {
    // NOTE: We may wish to create an empty DenseArray when it's empty and
    // unknown to enforce the type.
    if (slice.is_empty_and_unknown() ||
        slice.dtype() == arolla::GetQType<DST>()) {
      return slice;
    }
    arolla::DenseArrayBuilder<DST> bldr(slice.size());
    RETURN_IF_ERROR(
        slice.VisitValues([&]<class T>(const arolla::DenseArray<T>& values) {
          if constexpr (arolla::meta::contains_v<SRCs, T>) {
            absl::Status status = absl::OkStatus();
            auto cast_op = CastOp();
            values.ForEachPresent([&](int64_t id, arolla::view_type_t<T> v) {
              if constexpr (std::is_same_v<DST, T>) {
                bldr.Set(id, v);
              } else if constexpr (std::is_same_v<decltype(cast_op(v)), DST>) {
                bldr.Set(id, cast_op(v));
              } else {
                auto res = cast_op(v);
                if (!res.ok()) {
                  status = res.status();
                  return;
                }
                bldr.Set(id, *res);
              }
            });
            return status;
          } else {
            return absl::InvalidArgumentError(absl::StrFormat(
                "cannot cast %s to %v", GetQTypeName(arolla::GetQType<T>()),
                GetDType<DST>()));
          }
        }));
    return internal::DataSliceImpl::Create(std::move(bldr).Build());
  }
};

}  // namespace schema_internal

// Casts the given item/slice to int32.
//
// The following cases are supported:
// - {INT32, INT64, FLOAT32, FLOAT64, BOOL} QType -> INT32.
// - Empty -> empty.
// - Mixed types -> INT32 if all items are in {INT32, INT64, FLOAT32, FLOAT64,
// BOOL}.
struct ToInt32 : schema_internal::ToDST<arolla::CastOp<int>, int,
                                        schema_internal::kNumericsCompatible> {
};

// Casts the given item/slice to int64.
//
// The following cases are supported:
// - {INT32, INT64, FLOAT32, FLOAT64, BOOL} QType -> INT64.
// - Empty -> empty.
// - Mixed types -> INT64 if all items are in {INT32, INT64, FLOAT32, FLOAT64,
// BOOL}.
struct ToInt64 : schema_internal::ToDST<arolla::CastOp<int64_t>, int64_t,
                                        schema_internal::kNumericsCompatible> {
};

// Casts the given item/slice to float.
//
// The following cases are supported:
// - {INT32, INT64, FLOAT32, FLOAT64, BOOL} QType -> FLOAT32.
// - Empty -> empty.
// - Mixed types -> FLOAT32 if all items are in {INT32, INT64, FLOAT32,
// FLOAT64, BOOL}.
struct ToFloat32
    : schema_internal::ToDST<arolla::CastOp<float>, float,
                             schema_internal::kNumericsCompatible> {};

// Casts the given item/slice to double.
//
// The following cases are supported:
// - {INT32, INT64, FLOAT32, FLOAT64, BOOL} QType -> FLOAT64.
// - Empty -> empty.
// - Mixed types -> FLOAT64 if all items are in {INT32, INT64, FLOAT32,
// FLOAT64, BOOL}.
struct ToFloat64
    : schema_internal::ToDST<arolla::CastOp<double>, double,
                             schema_internal::kNumericsCompatible> {};

// Casts the given item/slice to None.
//
// Requires that the provided slice / item is empty.
struct ToNone {
  absl::StatusOr<internal::DataItem> operator()(
      const internal::DataItem& item) const;
  absl::StatusOr<internal::DataSliceImpl> operator()(
      const internal::DataSliceImpl& slice) const;
};

// Casts the given item/slice to ExprQuote.
//
// The following cases are supported:
// - EXPR -> EXPR.
// - Empty -> empty.
struct ToExpr : schema_internal::ToSelf<arolla::expr::ExprQuote> {};

// Casts the given item/slice to Text.
//
// The following cases are supported:
// - TEXT -> TEXT.
// - BYTES -> TEXT, by `b'foo'` -> `"b'foo'"`.
// - MASK -> TEXT.
// - BOOL -> TEXT.
// - INT32 -> TEXT.
// - INT64 -> TEXT.
// - FLOAT32 -> TEXT.
// - FLOAT64 -> TEXT.
// - Empty -> empty.
struct ToText
    : schema_internal::ToDST<
          arolla::AsTextOp, arolla::Text,
          arolla::meta::type_list<arolla::Text, arolla::Bytes, arolla::Unit,
                                  bool, int, int64_t, float, double>> {};

// Casts the given item/slice to Bytes.
//
// The following cases are supported:
// - BYTES -> BYTES.
// - Empty -> empty.
struct ToBytes : schema_internal::ToSelf<arolla::Bytes> {};

// Decodes the given item/slice to Text.
//
// The following cases are supported:
// - TEXT -> TEXT.
// - BYTES -> TEXT, using UTF-8 decoding.
// - Empty -> empty.
struct Decode : schema_internal::ToDST<arolla::DecodeOp, arolla::Text,
                                       schema_internal::kStrings> {};

// Encodes the given item/slice to Text.
//
// The following cases are supported:
// - BYTES -> BYTES.
// - TEXT -> BYTES, using UTF-8 encoding.
// - Empty -> empty.
struct Encode : schema_internal::ToDST<arolla::EncodeOp, arolla::Bytes,
                                       schema_internal::kStrings> {};

// Casts the given item/slice to Unit.
//
// The following cases are supported:
// - MASK -> MASK.
// - Empty -> empty.
struct ToMask : schema_internal::ToSelf<arolla::Unit> {};

// Casts the given item/slice to bool.
//
// - {INT32, INT64, FLOAT32, FLOAT64, BOOL} QType -> BOOL.
// - Empty -> empty.
// - Mixed types -> BOOL if all items are in {INT32, INT64, FLOAT32, FLOAT64,
//   BOOL}.
struct ToBool : schema_internal::ToDST<arolla::ToBoolOp, bool,
                                       schema_internal::kNumericsCompatible> {};

// Casts the given item/slice to ItemId.
//
// The following cases are supported:
// - OBJECT_ID -> OBJECT_ID.
// - Empty -> empty.
struct ToItemId : schema_internal::ToSelf<internal::ObjectId> {};

// Casts the given item/slice to schema.
//
// The following cases are supported:
// - DTYPE -> DTYPE.
// - OBJECT_ID -> OBJECT_ID. Requires the object to be a schema.
// - Empty -> empty.
struct ToSchema {
  absl::StatusOr<internal::DataItem> operator()(
      const internal::DataItem& item) const;
  absl::StatusOr<internal::DataSliceImpl> operator()(
      const internal::DataSliceImpl& slice) const;
};

// Casts the given item/slice to Object.
//
// The `schema` indicates the schema of the provided data. If it is an entity
// schema, the schema attributes for all items are set to it in the provided
// DataBag. If validate_schema is true, any existing schema attribute is
// additionally verified to be identical. If the provided schema is not an
// entity schema, or if it is empty, schema attributes are not set.
//
// `validate_schema` indicates whether provided schema is validated to match
// existing schema attributes. This is a no-op for primitive items/slices.
//
// Note that it is assumed that the provided schema matches the provided data.
class ToObject {
 public:
  static absl::StatusOr<ToObject> Make(
      internal::DataItem schema, bool validate_schema = true,
      absl::Nullable<internal::DataBagImpl*> db_impl = nullptr);

  static absl::StatusOr<ToObject> Make(
      bool validate_schema = true,
      absl::Nullable<internal::DataBagImpl*> db_impl = nullptr);

  absl::Status operator()(const internal::DataItem& item) const;
  absl::Status operator()(const internal::DataSliceImpl& slice) const;

 private:
  ToObject(internal::DataItem entity_schema, bool validate_schema,
           absl::Nullable<internal::DataBagImpl*> db_impl)
      : entity_schema_(entity_schema),
        validate_schema_(validate_schema),
        db_impl_(std::move(db_impl)) {}

  // Empty DataItem is used to represent a slice without Entity schema, i.e.
  // primitive, ANY, OBJECT, etc.
  internal::DataItem entity_schema_;
  bool validate_schema_;
  absl::Nullable<internal::DataBagImpl*> db_impl_;
};

}  // namespace koladata::schema

#endif  // KOLADATA_INTERNAL_CASTING_H_
