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
#include "koladata/internal/data_item.h"

#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "koladata/internal/expr_quote_utils.h"
#include "koladata/internal/missing_value.h"
#include "koladata/internal/stable_fingerprint.h"
#include "koladata/internal/types.h"
#include "arolla/expr/quote.h"
#include "arolla/memory/optional_value.h"
#include "arolla/qtype/base_types.h"
#include "arolla/qtype/optional_qtype.h"
#include "arolla/qtype/qtype.h"
#include "arolla/qtype/qtype_traits.h"
#include "arolla/qtype/typed_ref.h"
#include "arolla/qtype/typed_value.h"
#include "arolla/util/bytes.h"
#include "arolla/util/fingerprint.h"
#include "arolla/util/meta.h"
#include "arolla/util/text.h"
#include "arolla/util/unit.h"

namespace koladata::internal {

using ::arolla::GetOptionalQType;
using ::arolla::GetQType;
using ::arolla::QTypePtr;

QTypePtr DataItem::dtype() const {
  return std::visit(
      [](const auto& arg) -> arolla::QTypePtr {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, MissingValue>) {
          return arolla::GetNothingQType();
        } else {
          return arolla::GetQType<T>();
        }
      },
      data_);
}

absl::StatusOr<DataItem> DataItem::Create(const arolla::TypedRef& value) {
  QTypePtr dtype = value.GetType();
  std::optional<DataItem> result;
  arolla::meta::foreach_type(supported_primitives_list(), [&](auto tpe) {
    using T = typename decltype(tpe)::type;
    if (dtype == GetQType<T>()) {
      result = DataItem(value.UnsafeAs<T>());
    } else if (dtype == GetOptionalQType<T>()) {
      auto& optional_value = value.UnsafeAs<arolla::OptionalValue<T>>();
      if (optional_value.present) {
        result = DataItem(optional_value.value);
      } else {
        result = DataItem();
      }
    }
  });
  if (!result.has_value()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("DataItem cannot be created from value with type %s",
                        value.GetType()->name()));
  }
  return std::move(*result);
}

absl::StatusOr<DataItem> DataItem::Create(const arolla::TypedValue& value) {
  return Create(value.AsRef());
}

arolla::Fingerprint DataItem::StableFingerprint() const {
  StableFingerprintHasher hasher("data_item");
  std::visit([&hasher](const auto& value) { hasher.Combine(value); }, data_);
  return std::move(hasher).Finish();
}

void DataItem::ArollaFingerprint(arolla::FingerprintHasher* hasher) const {
  std::visit(
      [&hasher, this](const auto& value) {
        if constexpr (!std::is_same_v<std::decay_t<decltype(value)>,
                                      MissingValue>) {
          hasher->Combine(value);
          hasher->Combine(data_.index());
        } else {
          hasher->Combine(data_.index());
        }
      },
      data_);
}

std::string DataItem::DebugString() const {
  return std::visit(
      [](const auto& val) -> std::string {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, MissingValue>) {
          return "None";
        } else if constexpr (std::is_same_v<T, arolla::Unit>) {
          return "present";
        } else if constexpr (std::is_same_v<T, arolla::Text>) {
          return absl::StrCat("'", absl::string_view(val), "'");
        } else if constexpr (std::is_same_v<T, arolla::Bytes>) {
          return absl::StrCat("b'", absl::CHexEscape(absl::string_view(val)),
                              "'");
        } else if constexpr (std::is_same_v<T, arolla::expr::ExprQuote>) {
          return ExprQuoteDebugString(val);
        } else if constexpr (std::is_same_v<T, bool>) {
          return val ? "True" : "False";
        } else if constexpr (std::is_same_v<T, float> ||
                             std::is_same_v<T, double>) {
          std::string str = absl::StrCat(val);
          return absl::StrContains(str, ".") ? str : absl::StrCat(str, ".0");
        } else {
          return absl::StrCat(val);
        }
      },
      data_);
}

}  // namespace koladata::internal
