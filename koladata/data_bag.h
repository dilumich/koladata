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
#ifndef KOLADATA_DATA_BAG_H_
#define KOLADATA_DATA_BAG_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "koladata/internal/data_bag.h"
#include "arolla/qtype/simple_qtype.h"
#include "arolla/util/fingerprint.h"
#include "arolla/util/repr.h"

namespace koladata {

class DataBag;

using DataBagPtr = std::shared_ptr<DataBag>;

// This abstraction implements the API of all public DataBag functionality
// users can access. It is used as the main entry point to business logic
// implementation and all the processing is delegated to it from C Python
// bindings for DataBag.
//
// C Python bindings for DataBag is processing only the minimum part necessary
// to extract information from PyObject(s) and propagate it to appropriate
// methods of this class.
//
// In addition, it provides indirection from the low-level DataBagImpl, so that
// the underlying Object storage can be changed for many DataSlice(s). This way
// full persistency can be achieved with partially persistent DataBagImpl.
class DataBag {
 public:
  // Tag for creating immutable DataBag.
  struct immutable_t {};

  // Returns a newly created empty DataBag.
  static DataBagPtr Empty() {
    return std::make_shared<DataBag>();
  }

  DataBag() : DataBag(/*is_mutable=*/true) {}
  explicit DataBag(immutable_t) : DataBag(/*is_mutable=*/false) {}

  bool IsMutable() const { return is_mutable_; }

  const internal::DataBagImpl& GetImpl() const { return *impl_; }
  absl::StatusOr<std::reference_wrapper<internal::DataBagImpl>>
  GetMutableImpl() {
    if (!is_mutable_) {
      return absl::InvalidArgumentError("DataBag is immutable.");
    }
    return *impl_;
  }

  // Returns fallbacks in priority order.
  const std::vector<DataBagPtr>& GetFallbacks() const {
    return fallbacks_;
  }

  // Returns a newly created immutable DataBag with fallbacks.
  static DataBagPtr ImmutableEmptyWithFallbacks(
      std::vector<DataBagPtr> fallbacks);

  // Returns a DataBag that contains all the data its input contain.
  // * If they are all the same or only 1 DataBag is non-nullptr, that DataBag
  //   is returned.
  // * Otherwise, an immutable DataBag with all the inputs as fallbacks is
  //   created and returned.
  // * In case of no DataBags, nullptr is returned.
  static DataBagPtr CommonDataBag(absl::Span<const DataBagPtr> databags);

  // Returns a mutable DataBag that wraps provided low-level DataBagImpl.
  static DataBagPtr FromImpl(internal::DataBagImplPtr impl);

  // Returns an id of this DataBag. On each call it returns the same id for that
  // DataBag. Different DataBags have different ids.
  // Both the address of this DataBag and a random number are included in
  // computing this id.
  uint64_t GetRandomizedDataBagId();

 private:
  explicit DataBag(bool is_mutable)
      : impl_(internal::DataBagImpl::CreateEmptyDatabag()),
        is_mutable_(is_mutable) {}

  internal::DataBagImplPtr impl_;
  std::vector<DataBagPtr> fallbacks_;
  bool is_mutable_ = true;
  std::optional<uint64_t> randomized_data_bag_id_;
};

class FlattenFallbackFinder {
 public:
  // Constructs empty fallback list.
  FlattenFallbackFinder() = default;

  // Constructs fallback list from the provided databag.
  FlattenFallbackFinder(const DataBag& bag) {
    const auto& fallbacks = bag.GetFallbacks();
    if (fallbacks.empty()) {
      return;
    }
    CollectFlattenFallbacks(bag, fallbacks);
  }

  // Returns DatBagImpl fallbacks in the decreasing priority order.
  // All duplicates are removed.
  internal::DataBagImpl::FallbackSpan GetFlattenFallbacks() const {
    return fallback_span_;
  }

 private:
  // Collect fallbacks in pre order using Depth First Search.
  void CollectFlattenFallbacks(
      const DataBag& bag, const std::vector<DataBagPtr>& fallbacks);

  absl::InlinedVector<const internal::DataBagImpl*, 2> fallback_holder_;
  internal::DataBagImpl::FallbackSpan fallback_span_;
};

}  // namespace koladata

namespace arolla {

AROLLA_DECLARE_FINGERPRINT_HASHER_TRAITS(::koladata::DataBagPtr);
AROLLA_DECLARE_REPR(::koladata::DataBagPtr);
AROLLA_DECLARE_SIMPLE_QTYPE(DATA_BAG, ::koladata::DataBagPtr);

}  // namespace arolla

#endif  // KOLADATA_DATA_BAG_H_
