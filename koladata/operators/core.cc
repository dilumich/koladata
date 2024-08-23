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
#include "koladata/operators/core.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/base/attributes.h"
#include "absl/container/flat_hash_map.h"
#include "absl/hash/hash.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "koladata/adoption_utils.h"
#include "koladata/casting.h"
#include "koladata/data_bag.h"
#include "koladata/data_slice.h"
#include "koladata/data_slice_qtype.h"
#include "koladata/extract_utils.h"
#include "koladata/internal/data_bag.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/ellipsis.h"
#include "koladata/internal/op_utils/at.h"
#include "koladata/internal/op_utils/collapse.h"
#include "koladata/internal/op_utils/extract.h"
#include "koladata/internal/op_utils/itemid.h"
#include "koladata/internal/op_utils/reverse.h"
#include "koladata/internal/op_utils/reverse_select.h"
#include "koladata/internal/op_utils/select.h"
#include "koladata/internal/schema_utils.h"
#include "koladata/object_factories.h"
#include "koladata/operators/arolla_bridge.h"
#include "koladata/operators/utils.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/dense_array/edge.h"
#include "arolla/dense_array/qtype/types.h"
#include "arolla/expr/quote.h"
#include "arolla/jagged_shape/dense_array/util/concat.h"
#include "arolla/jagged_shape/util/concat.h"
#include "arolla/memory/buffer.h"
#include "arolla/memory/frame.h"
#include "arolla/qexpr/bound_operators.h"
#include "arolla/qexpr/eval_context.h"
#include "arolla/qexpr/operator_errors.h"
#include "arolla/qexpr/operators.h"
#include "arolla/qexpr/qexpr_operator_signature.h"
#include "arolla/qtype/optional_qtype.h"
#include "arolla/qtype/qtype.h"
#include "arolla/qtype/qtype_traits.h"
#include "arolla/qtype/slice_qtype.h"
#include "arolla/qtype/tuple_qtype.h"
#include "arolla/qtype/typed_slot.h"
#include "arolla/qtype/unspecified_qtype.h"
#include "arolla/util/repr.h"
#include "arolla/util/text.h"
#include "arolla/util/view_types.h"
#include "arolla/util/status_macros_backport.h"

namespace koladata::ops {
namespace {

class AlignOperator : public arolla::QExprOperator {
 public:
  explicit AlignOperator(absl::Span<const arolla::QTypePtr> input_types)
      : arolla::QExprOperator(
            "kde.core.align",
            arolla::QExprOperatorSignature::Get(
                input_types, arolla::MakeTupleQType(input_types))) {}

 private:
  absl::StatusOr<std::unique_ptr<arolla::BoundOperator>> DoBind(
      absl::Span<const arolla::TypedSlot> input_slots,
      arolla::TypedSlot output_slot) const final override {
    DCHECK_EQ(input_slots.size(), output_slot.SubSlotCount());
    std::vector<arolla::FrameLayout::Slot<DataSlice>> ds_input_slots;
    ds_input_slots.reserve(input_slots.size());
    for (const auto& input_slot : input_slots) {
      ds_input_slots.push_back(input_slot.UnsafeToSlot<DataSlice>());
    }
    return arolla::MakeBoundOperator(
        [ds_input_slots(std::move(ds_input_slots)), output_slot = output_slot](
            arolla::EvaluationContext* ctx, arolla::FramePtr frame) {
          std::optional<DataSlice::JaggedShape> largest_shape;
          for (const auto& input_slot : ds_input_slots) {
            const DataSlice& input = frame.Get(input_slot);
            if (!largest_shape.has_value() ||
                input.GetShape().rank() > largest_shape->rank()) {
              largest_shape = input.GetShape();
            }
          }

          for (size_t i = 0; i < ds_input_slots.size(); ++i) {
            const auto& input_slot = ds_input_slots[i];
            const DataSlice& input = frame.Get(input_slot);
            ASSIGN_OR_RETURN(DataSlice output,
                             BroadcastToShape(input, largest_shape.value()),
                             ctx->set_status(std::move(_)));
            const auto& output_subslot =
                output_slot.SubSlot(i).UnsafeToSlot<DataSlice>();
            frame.Set(output_subslot, std::move(output));
          }
        });
  }
};

absl::StatusOr<DataSlice> ConcatOrStack(bool stack, int64_t ndim,
                                        std::vector<DataSlice> args) {
  if (args.empty()) {
    // Special case: no arguments returns kd.slice([]).
    return DataSlice::Create(
        internal::DataSliceImpl::CreateEmptyAndUnknownType(0),
        DataSlice::JaggedShape::FlatFromSize(0),
        internal::DataItem(schema::kObject), nullptr);
  }

  const int64_t rank = args[0].GetShape().rank();
  for (const auto& ds : args) {
    if (ds.GetShape().rank() != rank) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "all concat/stack args must have the same rank, got %d and %d", rank,
          ds.GetShape().rank()));
    }
  }

  if (!stack) {  // concat
    if (ndim < 1 || ndim > rank) {
      return absl::InvalidArgumentError(
          absl::StrFormat("invalid ndim=%d for rank=%d concat", ndim, rank));
    }
  } else {  // stack
    if (ndim < 0 || ndim > rank) {
      return absl::InvalidArgumentError(
          absl::StrFormat("invalid ndim=%d for rank=%d stack", ndim, rank));
    }
  }

  if (args.size() == 1 && !stack) {
    return args[0];
  }

  // Compute result schema.
  ASSIGN_OR_RETURN(auto aligned_schemas, AlignSchemas(std::move(args)));
  args = std::move(aligned_schemas.slices);
  internal::DataItem result_schema = std::move(aligned_schemas.common_schema);

  // Compute result data bag.
  ASSIGN_OR_RETURN(auto result_db, [&]() -> absl::StatusOr<DataBagPtr> {
    AdoptionQueue adoption_queue;
    for (const DataSlice& ds : args) {
      adoption_queue.Add(ds);
    }
    return adoption_queue.GetCommonOrMergedDb();
  }());

  if (rank == 0) {
    // Special case: rank == 0 iff all inputs are DataItems.
    DCHECK(stack);  // Implied by error checking above.
    internal::DataSliceImpl::Builder impl_builder(args.size());
    for (int i = 0; i < args.size(); ++i) {
      impl_builder.Insert(i, args[i].item());
    }
    return DataSlice::Create(std::move(impl_builder).Build(),
                             DataSlice::JaggedShape::FlatFromSize(args.size()),
                             std::move(result_schema), std::move(result_db));
  }

  std::vector<DataSlice::JaggedShape> shapes;
  shapes.reserve(args.size());
  for (const auto& ds : args) {
    shapes.push_back(ds.GetShape());
  }

  // Check whether all input slices have the same single dtype. If the result
  // dtype will be unknown/mixed, we convert all args to DenseArray<DataItem>
  // for uniform handling (at some performance cost).
  const bool has_mixed_result_dtype = [&]() -> bool {
    std::optional<arolla::QTypePtr> result_dtype;
    for (const auto& ds : args) {
      const auto& impl = ds.impl<internal::DataSliceImpl>();
      if (!impl.is_single_dtype()) {
        return true;
      }
      if (result_dtype.has_value() && result_dtype.value() != impl.dtype()) {
        return true;
      }
      if (!result_dtype.has_value()) {
        result_dtype = impl.dtype();
      }
    }
    return false;
  }();

  const auto process_arrays =
      [&]<typename T>(absl::Span<const arolla::DenseArray<T>> arrays)
      -> absl::StatusOr<DataSlice> {
    arolla::DenseArray<T> result_array;
    DataSlice::JaggedShape result_shape;
    if (stack) {
      ASSIGN_OR_RETURN(std::tie(result_array, result_shape),
                       arolla::StackJaggedArraysAlongDimension(
                           arrays, absl::MakeConstSpan(shapes), rank - ndim));
    } else {
      ASSIGN_OR_RETURN(std::tie(result_array, result_shape),
                       arolla::ConcatJaggedArraysAlongDimension(
                           arrays, absl::MakeConstSpan(shapes), rank - ndim));
    }
    return DataSlice::Create(
        internal::DataSliceImpl::Create(std::move(result_array)),
        std::move(result_shape), std::move(result_schema),
        std::move(result_db));
  };

  if (has_mixed_result_dtype) {
    std::vector<arolla::DenseArray<internal::DataItem>> arrays;
    arrays.reserve(args.size());
    for (const auto& ds : args) {
      arrays.push_back(
          ds.impl<internal::DataSliceImpl>().AsDataItemDenseArray());
    }
    return process_arrays(absl::MakeConstSpan(arrays));
  } else {
    // Note: VisitValues calls its callback exactly once, because args[0] has
    // a single dtype.
    DCHECK(args[0].impl<internal::DataSliceImpl>().is_single_dtype());
    std::optional<DataSlice> result;  // To avoid constructing empty DataSlice.
    RETURN_IF_ERROR(args[0].impl<internal::DataSliceImpl>().VisitValues(
        [&]<typename T>(const arolla::DenseArray<T>&) -> absl::Status {
          std::vector<arolla::DenseArray<T>> arrays;
          arrays.reserve(args.size());
          for (const auto& ds : args) {
            arrays.push_back(ds.impl<internal::DataSliceImpl>().values<T>());
          }
          ASSIGN_OR_RETURN(result, process_arrays(absl::MakeConstSpan(arrays)));
          return absl::OkStatus();
        }));
    DCHECK(result.has_value());  // Always populated by callback.
    return std::move(result).value();
  }
}

class ConcatOrStackOperator : public arolla::QExprOperator {
 public:
  explicit ConcatOrStackOperator(absl::Span<const arolla::QTypePtr> input_types)
      : arolla::QExprOperator("kde.core._concat_or_stack",
                              arolla::QExprOperatorSignature::Get(
                                  input_types, arolla::GetQType<DataSlice>())) {
  }

 private:
  absl::StatusOr<std::unique_ptr<arolla::BoundOperator>> DoBind(
      absl::Span<const arolla::TypedSlot> input_slots,
      arolla::TypedSlot output_slot) const final override {
    DCHECK_GE(input_slots.size(), 2);
    std::vector<arolla::FrameLayout::Slot<DataSlice>> ds_input_slots;
    ds_input_slots.reserve(input_slots.size() - 2);
    for (const auto& input_slot : input_slots.subspan(2)) {
      ds_input_slots.push_back(input_slot.UnsafeToSlot<DataSlice>());
    }
    return arolla::MakeBoundOperator(
        [stack_slot = input_slots[0].UnsafeToSlot<bool>(),
         ndim_slot = input_slots[1].UnsafeToSlot<int64_t>(),
         ds_input_slots(std::move(ds_input_slots)),
         output_slot = output_slot.UnsafeToSlot<DataSlice>()](
            arolla::EvaluationContext* ctx, arolla::FramePtr frame) {
          const bool stack = frame.Get(stack_slot);
          const int64_t ndim = frame.Get(ndim_slot);
          std::vector<DataSlice> args;
          args.reserve(ds_input_slots.size());
          for (const auto& ds_input_slot : ds_input_slots) {
            args.push_back(frame.Get(ds_input_slot));
          }
          ASSIGN_OR_RETURN(DataSlice output,
                           ConcatOrStack(stack, ndim, std::move(args)),
                           ctx->set_status(std::move(_)));
          frame.Set(output_slot, std::move(output));
        });
  }
};

absl::StatusOr<absl::string_view> GetAttrNameAsStr(const DataSlice& attr_name) {
  if (attr_name.GetShape().rank() != 0 ||
      attr_name.dtype() != schema::kText.qtype()) {
    return absl::InvalidArgumentError(
        absl::StrCat("attr_name in kd.get_attr expects TEXT, got: ",
                     arolla::Repr(attr_name)));
  }
  return attr_name.item().value<arolla::Text>().view();
}

absl::Status VerifyGroupByIndicesInputs(
    absl::Span<const arolla::QTypePtr> input_types) {
  if (input_types.empty()) {
    return absl::InvalidArgumentError("requires at least 1 argument");
  }

  for (const auto& args_type : input_types) {
    if (args_type != arolla::GetQType<DataSlice>()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "arguments must be DataSlices, but got ", args_type->name()));
    }
  }
  return absl::OkStatus();
}

static constexpr size_t kUndefinedGroup = ~size_t{};

struct DataItemPairHash {
  size_t operator()(const std::pair<size_t, internal::DataItem>& p) const {
    return absl::HashOf(p.first, internal::DataItem::Hash()(p.second));
  }
};

struct DataItemPairEq {
  size_t operator()(const std::pair<size_t, internal::DataItem>& a,
                    const std::pair<size_t, internal::DataItem>& b) const {
    return a.first == b.first && internal::DataItem::Eq()(a.second, b.second);
  }
};

// Helper class to process key data slices and find group indices.
class GroupByIndicesProcessor {
 public:
  GroupByIndicesProcessor(const arolla::DenseArrayEdge& edge_to_parent,
                          bool sort)
      : split_points_(edge_to_parent.edge_values().values.span()),
        group_id_(edge_to_parent.child_size(), 0),
        sort_(sort) {}

  // Update groups with a new key data slice. The shape must correspond to
  // the `edge_to_parent` passed to the constructor.
  void ProcessGroupKey(const internal::DataSliceImpl& ds) {
    if (ds.is_empty_and_unknown()) {
      std::fill(group_id_.begin(), group_id_.end(), kUndefinedGroup);
      return;
    }
    if (ds.is_mixed_dtype()) {
      DCHECK(!sort_) << "sort is not supported for mixed dtype";
      ProcessMixedType(ds);
      return;
    }
    ds.VisitValues([this](const auto& value) { ProcessSingleType(value); });
  }

  // Returns the data to construct the final DataSlice.
  // 1) Indices array.
  // 2) Split points for groups within the parent.
  // 3) Split points for items within the groups.
  std::tuple<arolla::DenseArray<int64_t>, arolla::DenseArrayEdge,
             arolla::DenseArrayEdge>
  CreateFinalDataSlice() {
    arolla::DenseArrayBuilder<int64_t> group_split_points_builder(
        split_points_.size());
    group_split_points_builder.Set(0, 0);

    size_t total_size =
        group_id_.size() - absl::c_count(group_id_, kUndefinedGroup);
    arolla::DenseArrayBuilder<int64_t> idx_builder(total_size);

    std::vector<int64_t> item_split_points;
    // we assume at least one group exists.
    item_split_points.reserve(split_points_.size());
    item_split_points.push_back(0);

    std::vector<size_t> group_id_count(group_id_.size(), 0);
    size_t output_index = 0;
    size_t local_group_prefix_sum = 0;
    for (size_t split_id = 1; split_id < split_points_.size(); ++split_id) {
      size_t begin = split_points_[split_id - 1];
      size_t end = split_points_[split_id];

      // Count the number of groups and elements in groups in the [beging, end).
      // Note that groups are numerated consequently within the [begin, end) by
      // construction. This loop also finds the range of existent groups.
      // Note that different [begin, end) couldn't have common groups.
      size_t local_group_count = 0;
      size_t start_local_group = group_id_.size() + 1;
      size_t end_local_group = 0;
      for (size_t i = begin; i < end; ++i) {
        size_t group = group_id_[i];
        if (group != kUndefinedGroup) {
          size_t& cnt = group_id_count[group];
          local_group_count += (cnt == 0);
          ++cnt;
          end_local_group = std::max(end_local_group, group + 1);
          start_local_group = std::min(start_local_group, group);
        }
      }
      local_group_prefix_sum += local_group_count;
      group_split_points_builder.Set(split_id, local_group_prefix_sum);
      if (local_group_count == 0) {
        continue;
      }

      // Converts `group_id_count` to be the starting position of the group
      // within the output indices array.
      // This loop also updates item_split_points.
      for (size_t i = start_local_group, prefix_sum = output_index;
           i < end_local_group; ++i) {
        size_t& cnt = group_id_count[i];
        if (cnt != 0) {
          prefix_sum += cnt;
          item_split_points.push_back(prefix_sum);
        }
        cnt = prefix_sum - cnt;
      }

      for (size_t i = begin; i < end; ++i) {
        size_t group = group_id_[i];
        if (group != kUndefinedGroup) {
          size_t idx = group_id_count[group]++;
          idx_builder.Set(idx, i - begin);
          ++output_index;
        }
      }
    }

    return {std::move(idx_builder).Build(),
            arolla::DenseArrayEdge::UnsafeFromSplitPoints(
                std::move(group_split_points_builder).Build()),
            // Transfer the ownership of the `std::vector` to the edge.
            arolla::DenseArrayEdge::UnsafeFromSplitPoints(
                arolla::DenseArray<int64_t>{arolla::Buffer<int64_t>::Create(
                    std::move(item_split_points))})};
  }

 private:
  template <typename T>
  class SortingData {
   public:
    explicit SortingData(bool sort) : sort_(sort) {}

    void Clear() {
      if (!sort_) {
        return;
      }
      keys_to_sort_.clear();
    }

    void AddUnique(size_t group, const T& value) {
      if (!sort_) {
        return;
      }
      keys_to_sort_.emplace_back(group, value);
    }

    void Sort(size_t start_group_id, absl::Span<size_t> group_ids) {
      if (!sort_) {
        return;
      }
      group_index_.resize(keys_to_sort_.size());
      group_to_sorted_index_.resize(keys_to_sort_.size());
      for (size_t i = 0; i < keys_to_sort_.size(); ++i) {
        group_index_[i] = i;
      }
      absl::c_sort(group_index_, [this](size_t a, size_t b) {
        if constexpr (internal::IsKodaScalarSortable<T>()) {
          return this->keys_to_sort_[a] < this->keys_to_sort_[b];
        } else {
          (void)this;  // suppress unused lambda capture warning.
          LOG(FATAL) << "sort for mixed type and ExprQuote is not allowed";
          return false;
        }
      });
      for (size_t i = 0; i < group_index_.size(); ++i) {
        group_to_sorted_index_[group_index_[i]] = i;
      }
      for (size_t& group_id : group_ids) {
        if (group_id != kUndefinedGroup) {
          group_id = group_to_sorted_index_[group_id - start_group_id] +
                     start_group_id;
        }
      }
    }

   private:
    bool sort_;
    std::vector<std::pair<size_t, T>> keys_to_sort_;
    std::vector<size_t> group_index_;
    std::vector<size_t> group_to_sorted_index_;
  };

  void ProcessMixedType(const internal::DataSliceImpl& ds) {
    using Key = std::pair<size_t, internal::DataItem>;
    absl::flat_hash_map<Key, size_t, DataItemPairHash, DataItemPairEq>
        key_to_group_id;
    ProcessArray(ds.AsDataItemDenseArray(), key_to_group_id);
  }

  template <typename T>
  void ProcessSingleType(const arolla::DenseArray<T>& value) {
    using Key = std::pair<size_t, arolla::view_type_t<T>>;
    absl::flat_hash_map<Key, size_t> key_to_group_id;
    ProcessArray(value, key_to_group_id);
  }

  template <typename T, typename Map>
  void ProcessArray(const arolla::DenseArray<T>& value, Map& key_to_group_id) {
    using Key = typename Map::key_type;

    SortingData<arolla::view_type_t<T>> sorting_data(sort_);

    size_t new_group_id = 0;
    for (size_t split_id = 1; split_id < split_points_.size(); ++split_id) {
      size_t begin = split_points_[split_id - 1];
      size_t end = split_points_[split_id];
      // avoid clear to keep the memory.
      key_to_group_id.erase(key_to_group_id.begin(), key_to_group_id.end());
      sorting_data.Clear();
      size_t start_group_id = new_group_id;
      for (size_t i = begin; i < end; ++i) {
        size_t& group = group_id_[i];
        if (!value.present(i) || group == kUndefinedGroup) {
          group = kUndefinedGroup;
          continue;
        }
        auto [it, inserted] =
            key_to_group_id.emplace(Key{group, value.values[i]}, new_group_id);
        if (inserted) {
          sorting_data.AddUnique(group, value.values[i]);
          ++new_group_id;
        }
        group_id_[i] = it->second;
      }
      sorting_data.Sort(start_group_id,
                        absl::MakeSpan(group_id_).subspan(begin, end - begin));
    }
  }

  absl::Span<const int64_t> split_points_;
  std::vector<size_t> group_id_;
  bool sort_;
};

class GroupByIndicesQExprOperator : public arolla::QExprOperator {
 public:
  GroupByIndicesQExprOperator(absl::Span<const arolla::QTypePtr> types,
                              bool sort)
      : arolla::QExprOperator("kde.group_by_indices",
                              arolla::QExprOperatorSignature::Get(
                                  types, arolla::GetQType<DataSlice>())),
        sort_(sort) {
    DCHECK(!types.empty());
  }

 private:
  absl::StatusOr<std::unique_ptr<arolla::BoundOperator>> DoBind(
      absl::Span<const arolla::TypedSlot> input_slots,
      arolla::TypedSlot output_slot) const final {
    std::vector<arolla::FrameLayout::Slot<DataSlice>> ds_slots;
    ds_slots.reserve(input_slots.size());
    for (const auto& input_slot : input_slots) {
      ds_slots.push_back(input_slot.UnsafeToSlot<DataSlice>());
    }
    return arolla::MakeBoundOperator(
        [sort(sort_), output_slot = output_slot.UnsafeToSlot<DataSlice>(),
         ds_slots(std::move(ds_slots))](arolla::EvaluationContext* ctx,
                                        arolla::FramePtr frame) {
          const auto& shape = frame.Get(ds_slots[0]).GetShape();
          if (shape.rank() == 0) {
            ctx->set_status(absl::FailedPreconditionError(
                "group_by is not supported for scalar data"));
            return;
          }
          GroupByIndicesProcessor processor(shape.edges().back(),
                                            /*sort=*/sort);
          for (const auto& ds_slot : ds_slots) {
            const auto& ds = frame.Get(ds_slot);
            if (!ds.GetShape().IsEquivalentTo(shape)) {
              ctx->set_status(absl::FailedPreconditionError(
                  "all arguments must have the same shape"));
              return;
            }
            if (sort) {
              if (ds.slice().is_mixed_dtype()) {
                ctx->set_status(absl::FailedPreconditionError(
                    "sort is not supported for mixed dtype"));
                return;
              }
              if (!internal::IsKodaScalarQTypeSortable(ds.slice().dtype())) {
                ctx->set_status(absl::FailedPreconditionError(absl::StrCat(
                    "sort is not supported for ", ds.slice().dtype()->name())));
                return;
              }
            }
            processor.ProcessGroupKey(ds.slice());
          }
          auto [indices_array, group_split_points, item_split_points] =
              processor.CreateFinalDataSlice();
          ASSIGN_OR_RETURN(
              auto new_shape,
              shape.RemoveDims(/*from=*/shape.rank() - 1)
                  .AddDims({group_split_points, item_split_points}),
              ctx->set_status(std::move(_)));
          ASSIGN_OR_RETURN(
              auto result,
              DataSlice::Create(
                  internal::DataSliceImpl::Create(std::move(indices_array)),
                  std::move(new_shape), internal::DataItem(schema::kInt64)),
              ctx->set_status(std::move(_)));
          frame.Set(output_slot, result);
        });
  }

  bool sort_;
};

struct Slice {
  int64_t start;
  std::optional<int64_t> stop;
};

using SlicingArgType = std::variant<Slice, const DataSlice*>;

// TODO: remove this to Expr operator constraints.
absl::Status IsSliceQTypeValid(const arolla::QTypePtr& qtype, int64_t curr_pos,
                               std::optional<int64_t>& ellipsis_pos) {
  if (qtype == arolla::GetQType<DataSlice>()) {
    return absl::OkStatus();
  } else if (qtype == arolla::GetQType<internal::Ellipsis>()) {
    if (ellipsis_pos.has_value()) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "ellipsis ... can appear at most once in the slicing arguments, "
          "found at least two at positions: %d and %d",
          *ellipsis_pos, curr_pos));
    }
    ellipsis_pos = curr_pos;
    return absl::OkStatus();
  } else if (arolla::IsSliceQType(qtype)) {
    const auto& subfields = qtype->type_fields();
    DCHECK_EQ(subfields.size(), 3);

    auto start_qtype = subfields[0].GetType();
    if (start_qtype != arolla::GetQType<int32_t>() &&
        start_qtype != arolla::GetQType<int64_t>() &&
        start_qtype != arolla::GetUnspecifiedQType()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "'start' argument of a Slice must be integer or unspecified, got: ",
          start_qtype->name()));
    }
    auto end_qtype = subfields[1].GetType();
    if (end_qtype != arolla::GetQType<int32_t>() &&
        end_qtype != arolla::GetQType<int64_t>() &&
        end_qtype != arolla::GetUnspecifiedQType()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "'end' argument of a Slice must be integer or unspecified, got: ",
          end_qtype->name()));
    }
    auto step_qtype = subfields[2].GetType();
    if (step_qtype != arolla::GetUnspecifiedQType()) {
      return absl::InvalidArgumentError(
          absl::StrCat("'step' argument of a Slice is not supported, got: ",
                       end_qtype->name()));
    }
    return absl::OkStatus();
  }
  return absl::InvalidArgumentError(
      absl::StrCat("unsupported input type: ", qtype->name()));
  ;
}

std::optional<int64_t> GetSliceArg(const arolla::TypedSlot& field,
                                   arolla::FramePtr frame) {
  if (field.GetType() == arolla::GetUnspecifiedQType()) {
    return std::nullopt;
  } else if (field.GetType() == arolla::GetQType<int32_t>()) {
    return frame.Get(field.UnsafeToSlot<int32_t>());
  } else if (field.GetType() == arolla::GetQType<int64_t>()) {
    return frame.Get(field.UnsafeToSlot<int64_t>());
  } else {
    DCHECK(false);
    return std::nullopt;
  }
}

absl::StatusOr<std::vector<SlicingArgType>> ExtractSlicingArgs(
    const std::vector<arolla::TypedSlot>& slots, arolla::FramePtr frame,
    const int64_t x_rank) {
  std::vector<SlicingArgType> slices;
  std::optional<int64_t> ellipsis_pos;
  for (auto i = 0; i < slots.size(); ++i) {
    const auto qtype = slots[i].GetType();
    if (qtype == arolla::GetQType<DataSlice>()) {
      slices.push_back(&frame.Get(slots[i].UnsafeToSlot<DataSlice>()));
    } else if (arolla::IsSliceQType(qtype)) {
      auto start = GetSliceArg(slots[i].SubSlot(0), frame);
      auto end = GetSliceArg(slots[i].SubSlot(1), frame);
      slices.emplace_back(Slice{start.has_value() ? *start : 0, end});
    } else if (qtype == arolla::GetQType<koladata::internal::Ellipsis>()) {
      ellipsis_pos = i;
    }
  }

  if (ellipsis_pos.has_value()) {
    if (slices.size() > x_rank) {
      return absl::InvalidArgumentError(
          absl::StrFormat("cannot subslice DataSlice 'x' as the number of "
                          "provided non-ellipsis slicing arguments is larger "
                          "than x.ndim: %d > %d",
                          slices.size(), x_rank));
    }
    // Insert full slices (e.g. slice(0, None)) so that slices have the same
    // size as x_rank.
    // There is an optimization: when ellipsis is the first slicing argument,
    // only implode and explode the last N dimensions where N is the number of
    // non-ellipsis slicing arguments.
    if (*ellipsis_pos != 0) {
      slices.insert(slices.begin() + *ellipsis_pos, x_rank - slices.size(),
                    Slice{0, std::nullopt});
    }
  } else if (slices.size() != x_rank) {
    return absl::InvalidArgumentError(
        absl::StrFormat("cannot subslice DataSlice 'x' as the number of "
                        "provided slicing arguments is different from x.ndim: "
                        "%d != %d",
                        slices.size(), x_rank));
  }
  return slices;
}

class SubsliceOperator : public arolla::InlineOperator {
 public:
  SubsliceOperator(absl::Span<const arolla::QTypePtr> types)
      : InlineOperator("kde.core.subslice",
                       arolla::QExprOperatorSignature::Get(
                           types, arolla::GetQType<DataSlice>())) {}

 private:
  absl::StatusOr<std::unique_ptr<arolla::BoundOperator>> DoBind(
      absl::Span<const arolla::TypedSlot> input_slots,
      arolla::TypedSlot output_slot) const override {
    return arolla::MakeBoundOperator(
        [x_slot = input_slots[0].UnsafeToSlot<DataSlice>(),
         slice_slots = std::vector(input_slots.begin() + 1, input_slots.end()),
         result_slot = output_slot.UnsafeToSlot<DataSlice>()](
            arolla::EvaluationContext* ctx, arolla::FramePtr frame) {
          const auto& x = frame.Get(x_slot);
          ASSIGN_OR_RETURN(
              auto slice_args,
              ExtractSlicingArgs(slice_slots, frame, x.GetShape().rank()),
              ctx->set_status(std::move(_)));

          // TODO: improve the performance by avoiding list
          // creation.
          auto temp_db = DataBag::Empty();
          auto new_x = x.WithDb(temp_db);
          for (size_t i = 0; i < slice_args.size(); ++i) {
            ASSIGN_OR_RETURN(new_x,
                             CreateListsFromLastDimension(temp_db, new_x),
                             ctx->set_status(std::move(_)));
          }

          for (const auto& slice_arg : slice_args) {
            if (std::holds_alternative<const DataSlice*>(slice_arg)) {
              ASSIGN_OR_RETURN(
                  new_x,
                  new_x.GetFromList(*std::get<const DataSlice*>(slice_arg)),
                  ctx->set_status(std::move(_)));
            } else {
              auto slice = std::get<Slice>(slice_arg);
              ASSIGN_OR_RETURN(new_x,
                               new_x.ExplodeList(slice.start, slice.stop),
                               ctx->set_status(std::move(_)));
            }
          }
          frame.Set(result_slot, new_x.WithDb(x.GetDb()));
        });
  }
};

absl::StatusOr<DataSlice> AtImpl(const DataSlice& x, const DataSlice& indices) {
  const auto& x_shape = x.GetShape();
  const auto& indices_shape = indices.GetShape();
  // If ndim(indices) == ndim(x) - 1, insert a unit dimension to the end,
  // which is needed by internal::AtOp().
  // If ndim(indices) > ndim(x) - 1, flatten the last ndim(indices) - ndim(x)
  // + 1 dimensions.
  // The flattened_shape always has the same rank and the same N-1 dimensions
  // as the shape of x.
  auto flattened_shape =
      indices_shape.FlattenDims(x_shape.rank() - 1, indices_shape.rank());

  std::optional<arolla::DenseArrayEdge> indices_to_common =
      flattened_shape.edges().empty()
          ? std::nullopt
          : std::make_optional(flattened_shape.edges().back());
  auto x_to_common = x_shape.edges().back();
  ASSIGN_OR_RETURN(auto index_array, ToArollaDenseArrayInt64(indices));

  return DataSlice::Create(
      internal::AtOp(x.slice(), index_array, x_to_common, indices_to_common),
      indices_shape, x.GetSchemaImpl(), x.GetDb());
}

class UuSchemaOperator : public arolla::QExprOperator {
 public:
  explicit UuSchemaOperator(absl::Span<const arolla::QTypePtr> input_types)
      : QExprOperator("kde.schema._uu_schema",
                      arolla::QExprOperatorSignature::Get(
                          input_types, arolla::GetQType<DataSlice>())) {}

  absl::StatusOr<std::unique_ptr<arolla::BoundOperator>> DoBind(
      absl::Span<const arolla::TypedSlot> input_slots,
      arolla::TypedSlot output_slot) const final {
    return arolla::MakeBoundOperator(
        [seed_slot = input_slots[0].UnsafeToSlot<DataSlice>(),
         named_tuple_slot = input_slots[1],
         output_slot = output_slot.UnsafeToSlot<DataSlice>()](
            arolla::EvaluationContext* ctx, arolla::FramePtr frame) {
          const auto& seed_data_slice = frame.Get(seed_slot);
          if (seed_data_slice.GetShape().rank() != 0 ||
              !seed_data_slice.item().holds_value<arolla::Text>()) {
            ctx->set_status(absl::InvalidArgumentError(absl::StrFormat(
                "requires seed to be DataItem holding Text, got %s",
                arolla::Repr(seed_data_slice))));
            return;
          }
          auto seed = seed_data_slice.item().value<arolla::Text>();
          auto attr_names = GetAttrNames(named_tuple_slot);
          auto values = GetValueDataSlices(named_tuple_slot, attr_names, frame);
          auto db = koladata::DataBag::Empty();
          koladata::AdoptionQueue adoption_queue;
          for (const auto& ds : values) {
            adoption_queue.Add(ds);
          }
          auto status = adoption_queue.AdoptInto(*db);
          if (!status.ok()) {
            ctx->set_status(std::move(status));
            return;
          }
          ASSIGN_OR_RETURN(auto result,
                           CreateUuSchema(db, seed, attr_names, values),
                           ctx->set_status(std::move(_)));
          frame.Set(output_slot, std::move(result));
        });
  }
};

class UuidOperator : public arolla::QExprOperator {
 public:
  explicit UuidOperator(absl::Span<const arolla::QTypePtr> input_types)
      : QExprOperator("kde.core._uuid",
                      arolla::QExprOperatorSignature::Get(
                          input_types, arolla::GetQType<DataSlice>())) {}

  absl::StatusOr<std::unique_ptr<arolla::BoundOperator>> DoBind(
      absl::Span<const arolla::TypedSlot> input_slots,
      arolla::TypedSlot output_slot) const final {
    return arolla::MakeBoundOperator(
        [seed_slot = input_slots[0].UnsafeToSlot<DataSlice>(),
         named_tuple_slot = input_slots[1],
         output_slot = output_slot.UnsafeToSlot<DataSlice>()](
            arolla::EvaluationContext* ctx, arolla::FramePtr frame) {
          const auto& seed_data_slice = frame.Get(seed_slot);
          if (seed_data_slice.GetShape().rank() != 0 ||
              !seed_data_slice.item().holds_value<arolla::Text>()) {
            ctx->set_status(absl::InvalidArgumentError(absl::StrFormat(
                "requires seed to be DataItem holding Text, got %s",
                arolla::Repr(seed_data_slice))));
            return;
          }
          auto seed = seed_data_slice.item().value<arolla::Text>();
          auto attr_names = GetAttrNames(named_tuple_slot);
          auto values = GetValueDataSlices(named_tuple_slot, attr_names, frame);
          ASSIGN_OR_RETURN(
              auto result,
              koladata::CreateUuidFromFields(seed, attr_names, values),
              ctx->set_status(std::move(_)));
          frame.Set(output_slot, std::move(result));
        });
  }
};

class UuObjOperator : public arolla::QExprOperator {
 public:
  explicit UuObjOperator(absl::Span<const arolla::QTypePtr> input_types)
      : QExprOperator("kde.core._uuobj",
                      arolla::QExprOperatorSignature::Get(
                          input_types, arolla::GetQType<DataSlice>())) {}

  absl::StatusOr<std::unique_ptr<arolla::BoundOperator>> DoBind(
      absl::Span<const arolla::TypedSlot> input_slots,
      arolla::TypedSlot output_slot) const final {
    return arolla::MakeBoundOperator(
        [seed_slot = input_slots[0].UnsafeToSlot<DataSlice>(),
         named_tuple_slot = input_slots[1],
         output_slot = output_slot.UnsafeToSlot<DataSlice>()](
            arolla::EvaluationContext* ctx, arolla::FramePtr frame) {
          const auto& seed_data_slice = frame.Get(seed_slot);
          if (seed_data_slice.GetShape().rank() != 0 ||
              !seed_data_slice.item().holds_value<arolla::Text>()) {
            ctx->set_status(absl::InvalidArgumentError(absl::StrFormat(
                "requires seed to be DataItem holding Text, got %s",
                arolla::Repr(seed_data_slice))));
            return;
          }
          auto seed = seed_data_slice.item().value<arolla::Text>();
          auto attr_names = GetAttrNames(named_tuple_slot);
          auto values = GetValueDataSlices(named_tuple_slot, attr_names, frame);
          auto db = koladata::DataBag::Empty();
          koladata::AdoptionQueue adoption_queue;
          for (const auto& ds : values) {
            adoption_queue.Add(ds);
          }
          auto status = adoption_queue.AdoptInto(*db);
          if (!status.ok()) {
            ctx->set_status(std::move(status));
            return;
          }
          ASSIGN_OR_RETURN(auto result,
                           CreateUuObject(db, seed, attr_names, values),
                           ctx->set_status(std::move(_)));
          frame.Set(output_slot, std::move(result));
        });
  }
};

}  // namespace

absl::StatusOr<DataSlice> Add(const DataSlice& x, const DataSlice& y) {
  return SimplePointwiseEval("kde.core._add_impl", {x, y});
}

DataSlice NoDb(const DataSlice& ds) { return ds.WithDb(nullptr); }

absl::StatusOr<DataBagPtr> GetDb(const DataSlice& ds) {
  if (auto result = ds.GetDb()) {
    return result;
  }
  return absl::InvalidArgumentError("DataSlice has no associated DataBag");
}

DataSlice WithDb(const DataSlice& ds, const DataBagPtr& db) {
  return ds.WithDb(db);
}

absl::StatusOr<DataSlice> InverseMapping(const DataSlice& x) {
  return SimpleAggOverEval("array.inverse_mapping", {x});
}

absl::StatusOr<DataSlice> OrdinalRank(const DataSlice& x,
                                      const DataSlice& tie_breaker,
                                      const DataSlice& descending) {
  if (descending.GetShape().rank() != 0 ||
      !descending.item().holds_value<bool>()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "expected `descending` to be a scalar boolean value, got %s",
        arolla::Repr(descending)));
  }
  ASSIGN_OR_RETURN(auto tie_breaker_primitive_schema,
                   GetPrimitiveArollaSchema(tie_breaker));
  if (tie_breaker_primitive_schema.has_value()) {
    return SimpleAggOverEval(
        "array.ordinal_rank", {x, tie_breaker, descending},
        /*output_schema=*/internal::DataItem(schema::kInt64), /*edge_index=*/2);
  } else {
    // `tie_breaker` _must_ be an integral, while the other data can be of other
    // types. We therefore fix the schema of `tie_breaker` to be INT64 to avoid
    // type errors.
    ASSIGN_OR_RETURN(
        auto tie_breaker_int64,
        tie_breaker.WithSchema(internal::DataItem(schema::kInt64)));
    return SimpleAggOverEval(
        "array.ordinal_rank", {x, std::move(tie_breaker_int64), descending},
        /*output_schema=*/internal::DataItem(schema::kInt64), /*edge_index=*/2);
  }
}

absl::StatusOr<arolla::OperatorPtr> AlignOperatorFamily::DoGetOperator(
    absl::Span<const arolla::QTypePtr> input_types,
    arolla::QTypePtr output_type) const {
  for (const auto& args_type : input_types) {
    if (args_type != arolla::GetQType<DataSlice>()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "arguments must be DataSlices, but got ", args_type->name()));
    }
  }

  return arolla::EnsureOutputQTypeMatches(
      std::make_shared<AlignOperator>(input_types), input_types, output_type);
}

absl::StatusOr<DataSlice> Clone(const DataSlice& ds, const DataSlice& schema) {
  const auto& db = ds.GetDb();
  if (db == nullptr) {
    return absl::InvalidArgumentError("cannot clone without a DataBag");
  }
  ASSIGN_OR_RETURN(DataSlice shallow_clone, ShallowClone(ds, schema));
  DataSlice shallow_clone_with_fallback = shallow_clone.WithDb(
      DataBag::ImmutableEmptyWithFallbacks({shallow_clone.GetDb(), db}));
  return Extract(std::move(shallow_clone_with_fallback), schema);
}

absl::StatusOr<DataSlice> Collapse(const DataSlice& ds) {
  const auto& shape = ds.GetShape();
  size_t rank = shape.rank();
  if (rank == 0) {
    return absl::InvalidArgumentError(
        "kd.collapse is not supported for DataItem.");
  }
  return DataSlice::Create(
      internal::CollapseOp()(ds.slice(), shape.edges().back()),
      shape.RemoveDims(rank - 1), ds.GetSchemaImpl(), ds.GetDb());
}

absl::StatusOr<arolla::OperatorPtr> ConcatOrStackOperatorFamily::DoGetOperator(
    absl::Span<const arolla::QTypePtr> input_types,
    arolla::QTypePtr output_type) const {
  if (input_types.size() < 2) {
    return absl::InvalidArgumentError(
        absl::StrCat("_concat_or_stack expected at least 2 arguments, but got ",
                     input_types.size()));
  }

  if (input_types[0] != arolla::GetQType<bool>()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "`stack` argument must be BOOLEAN, but got ", input_types[0]->name()));
  }
  if (input_types[1] != arolla::GetQType<int64_t>()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "`ndim` argument must be INT64, but got ", input_types[1]->name()));
  }

  for (const auto& args_type : input_types.subspan(2)) {
    if (args_type != arolla::GetQType<DataSlice>()) {
      return absl::InvalidArgumentError(absl::StrCat(
          "arguments must be DataSlices, but got ", args_type->name()));
    }
  }

  return arolla::EnsureOutputQTypeMatches(
      std::make_shared<ConcatOrStackOperator>(input_types), input_types,
      output_type);
}

absl::StatusOr<DataSlice> DictSize(const DataSlice& dicts) {
  const auto& db = dicts.GetDb();
  if (db == nullptr) {
    return absl::InvalidArgumentError(
        "Not possible to get Dict size without a DataBag");
  }
  FlattenFallbackFinder fb_finder(*db);
  internal::DataItem schema(schema::kInt64);
  return dicts.VisitImpl([&](const auto& impl) -> absl::StatusOr<DataSlice> {
    return DataSlice::Create(
        db->GetImpl().GetDictSize(impl, fb_finder.GetFlattenFallbacks()),
        dicts.GetShape(), std::move(schema), /*db=*/nullptr);
  });
}

absl::StatusOr<DataSlice> Explode(const DataSlice& x, const int64_t ndim) {
  if (ndim == 0) {
    return x;
  }

  DataSlice result = x;
  if (ndim < 0) {
    // Explode until items are no longer lists.
    while (true) {
      if (result.GetSchemaImpl() == schema::kAny ||
          result.GetSchemaImpl() == schema::kItemId) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "cannot fully explode 'x' with %v schema", result.GetSchemaImpl()));
      }

      if (result.GetSchemaImpl() == schema::kObject &&
          result.present_count() == 0) {
        return absl::InvalidArgumentError(
            "cannot fully explode 'x' with OBJECT schema and all-missing items,"
            " because the correct number of times to explode is ambiguous");
      }

      if (!result.ContainsOnlyLists()) break;
      ASSIGN_OR_RETURN(result, result.ExplodeList(0, std::nullopt));
    }
  } else {
    for (int i = 0; i < ndim; ++i) {
      if (!result.ContainsOnlyLists()) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "cannot explode 'x' to have additional %d dimension(s), the "
            "maximum number of additional dimension(s) is %d",
            ndim, i));
      }

      ASSIGN_OR_RETURN(result, result.ExplodeList(0, std::nullopt));
    }
  }
  return result;
}

absl::StatusOr<DataSlice> Extract(const DataSlice& ds,
                                  const DataSlice& schema) {
  return koladata::extract_utils_internal::ExtractWithSchema(ds, schema);
}

absl::StatusOr<DataSlice> GetAttr(const DataSlice& obj,
                                  const DataSlice& attr_name) {
  ASSIGN_OR_RETURN(auto attr_name_str, GetAttrNameAsStr(attr_name));
  return obj.GetAttr(attr_name_str);
}

absl::StatusOr<DataSlice> GetAttrWithDefault(const DataSlice& obj,
                                             const DataSlice& attr_name,
                                             const DataSlice& default_value) {
  ASSIGN_OR_RETURN(auto attr_name_str, GetAttrNameAsStr(attr_name));
  return obj.GetAttrWithDefault(attr_name_str, default_value);
}

absl::StatusOr<arolla::OperatorPtr> GroupByIndicesFamily::DoGetOperator(
    absl::Span<const arolla::QTypePtr> input_types,
    arolla::QTypePtr output_type) const {
  RETURN_IF_ERROR(VerifyGroupByIndicesInputs(input_types));
  if (output_type != arolla::GetQType<DataSlice>()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "the output must be a DataSlice, but got ", output_type->name()));
  }
  return std::make_unique<GroupByIndicesQExprOperator>(input_types,
                                                       /*sort=*/false);
}

absl::StatusOr<arolla::OperatorPtr> GroupByIndicesSortedFamily::DoGetOperator(
    absl::Span<const arolla::QTypePtr> input_types,
    arolla::QTypePtr output_type) const {
  RETURN_IF_ERROR(VerifyGroupByIndicesInputs(input_types));
  if (output_type != arolla::GetQType<DataSlice>()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "the output must be a DataSlice, but got ", output_type->name()));
  }
  return std::make_unique<GroupByIndicesQExprOperator>(input_types,
                                                       /*sort=*/true);
}

absl::StatusOr<DataSlice> Unique(const DataSlice& x, const DataSlice& sort) {
  if (x.GetShape().rank() == 0) {
    return x;
  }
  if (sort.GetShape().rank() != 0 || !sort.item().holds_value<bool>()) {
    return absl::FailedPreconditionError("sort must be a boolean scalar");
  }
  bool sort_bool = sort.item().value<bool>();
  if (sort_bool && x.slice().is_mixed_dtype()) {
    return absl::FailedPreconditionError(
        "sort is not supported for mixed dtype");
  }

  const auto& split_points =
      x.GetShape().edges().back().edge_values().values.span();
  arolla::DenseArrayBuilder<int64_t> split_points_builder(split_points.size());
  split_points_builder.Set(0, 0);

  auto process_values =
      [&]<class T, class Map>(
          const arolla::DenseArray<T>& values,
          Map& map) -> absl::StatusOr<internal::DataSliceImpl> {
    std::vector<arolla::view_type_t<T>> unique_values;
    unique_values.reserve(values.size());
    map.reserve(values.size());

    for (size_t split_id = 1; split_id < split_points.size(); ++split_id) {
      size_t begin = split_points[split_id - 1];
      size_t end = split_points[split_id];
      size_t unique_values_group_begin = unique_values.size();
      for (size_t i = begin; i < end; ++i) {
        if (!values.present(i)) {
          continue;
        }
        // We reuse the map to minimize amount of successful inserts.
        auto [it, inserted] = map.emplace(values.values[i], split_id);
        if (inserted || it->second != split_id) {
          unique_values.push_back(values.values[i]);
          it->second = split_id;
        }
      }
      split_points_builder.Set(split_id, unique_values.size());
      if (sort_bool) {
        if constexpr (internal::IsKodaScalarSortable<T>()) {
          std::sort(unique_values.begin() + unique_values_group_begin,
                    unique_values.end());
        } else {
          return absl::FailedPreconditionError(absl::StrCat(
              "sort is not supported for ", arolla::GetQType<T>()->name()));
        }
      }
    }
    internal::DataSliceImpl::Builder builder(unique_values.size());
    for (size_t i = 0; i < unique_values.size(); ++i) {
      if constexpr (std::is_same_v<T, internal::DataItem>) {
        builder.Insert(i, unique_values[i]);
      } else {
        builder.Insert(i, internal::DataItem::View<T>(unique_values[i]));
      }
    }
    return std::move(builder).Build();
  };

  absl::StatusOr<internal::DataSliceImpl> res_impl;
  if (x.slice().is_empty_and_unknown()) {
    res_impl = internal::DataSliceImpl::CreateEmptyAndUnknownType(0);
    for (size_t split_id = 1; split_id < split_points.size(); ++split_id) {
      split_points_builder.Set(split_id, 0);
    }
  } else if (x.slice().is_mixed_dtype()) {
    absl::flat_hash_map<internal::DataItem, size_t, internal::DataItem::Hash,
                        internal::DataItem::Eq>
        map;
    res_impl = process_values(x.slice().AsDataItemDenseArray(), map);
  } else {
    // TODO: Remove this unused builder. It prevents from a linker
    // error that is not yet explained.
    ABSL_ATTRIBUTE_UNUSED arolla::DenseArrayBuilder<arolla::expr::ExprQuote>
        unused(0);
    x.slice().VisitValues([&]<class T>(const arolla::DenseArray<T>& values) {
      absl::flat_hash_map<arolla::view_type_t<T>, size_t> map;
      res_impl = process_values(values, map);
    });
  }

  RETURN_IF_ERROR(res_impl.status());
  ASSIGN_OR_RETURN(auto new_shape,
                   x.GetShape()
                       .RemoveDims(/*from=*/x.GetShape().rank() - 1)
                       .AddDims({arolla::DenseArrayEdge::UnsafeFromSplitPoints(
                           std::move(split_points_builder).Build())}));
  return DataSlice::Create(*std::move(res_impl), std::move(new_shape),
                           x.GetSchemaImpl());
}

absl::StatusOr<DataSlice> ItemIdBits(const DataSlice& ds,
                                     const DataSlice& last) {
  if (last.GetShape().rank() != 0) {
    return absl::InvalidArgumentError("last must be an item");
  }
  if (last.dtype() != arolla::GetQType<int32_t>() &&
      last.dtype() != arolla::GetQType<int64_t>()) {
    return absl::InvalidArgumentError("last must be an integer");
  }

  const internal::DataItem& item = last.item();
  if (!item.has_value()) {
    return absl::InvalidArgumentError("last cannot be missing");
  }
  if (!schema::VerifySchemaForItemIds(ds.GetSchemaImpl())) {
    return absl::InvalidArgumentError(
        "the schema of the ds must be itemid, any, or object");
  }
  int64_t val =
      item.holds_value<int>() ? item.value<int>() : item.value<int64_t>();
  return ds.VisitImpl([&](const auto& impl) {
    return DataSlice::Create(internal::ItemIdBits()(impl, val), ds.GetShape(),
                             internal::DataItem(schema::kInt64), ds.GetDb());
  });
}

absl::StatusOr<DataSlice> ListSize(const DataSlice& lists) {
  const auto& db = lists.GetDb();
  if (db == nullptr) {
    return absl::InvalidArgumentError(
        "Not possible to get List size without a DataBag");
  }
  FlattenFallbackFinder fb_finder(*db);
  internal::DataItem schema(schema::kInt64);
  return lists.VisitImpl([&]<class T>(
                             const T& impl) -> absl::StatusOr<DataSlice> {
    ASSIGN_OR_RETURN(auto res_impl, db->GetImpl().GetListSize(
                                        impl, fb_finder.GetFlattenFallbacks()));
    if constexpr (std::is_same_v<T, internal::DataItem>) {
      return DataSlice::Create(std::move(res_impl), lists.GetShape(),
                               std::move(schema), /*db=*/nullptr);
    } else {
      return DataSlice::Create(
          internal::DataSliceImpl::Create(std::move(res_impl)),
          lists.GetShape(), std::move(schema), /*db=*/nullptr);
    }
  });
}

absl::StatusOr<DataSlice> GetNoFollowedSchema(const DataSlice& schema_ds) {
  return schema_ds.GetNoFollowedSchema();
}

absl::StatusOr<DataSlice> Follow(const DataSlice& ds) {
  ASSIGN_OR_RETURN(auto nofollowed_schema_item,
                   schema::GetNoFollowedSchemaItem(ds.GetSchemaImpl()));
  return ds.WithSchema(nofollowed_schema_item);
}

absl::StatusOr<DataSlice> Reverse(const DataSlice& obj) {
  if (obj.impl_empty_and_unknown() || obj.GetShape().rank() == 0) {
    return obj;
  }
  return DataSlice::Create(
      koladata::internal::ReverseOp{}(obj.slice(), obj.GetShape()),
      obj.GetShape(), obj.GetSchemaImpl(), obj.GetDb());
}

absl::StatusOr<DataSlice> Select(const DataSlice& ds, const DataSlice& filter,
                                 const bool expand_filter) {
  const internal::DataItem& schema = filter.GetSchemaImpl();

  if (schema != schema::kAny && schema != schema::kObject &&
      schema != schema::kMask) {
    return absl::InvalidArgumentError(
        "the schema of the filter DataSlice should only be Any, Object or "
        "Mask");
  }
  const DataSlice::JaggedShape& fltr_shape =
      expand_filter ? ds.GetShape() : filter.GetShape();
  ASSIGN_OR_RETURN(auto fltr, BroadcastToShape(filter, fltr_shape));
  return ds.VisitImpl([&](const auto& ds_impl) {
    return fltr.VisitImpl(
        [&](const auto& filter_impl) -> absl::StatusOr<DataSlice> {
          ASSIGN_OR_RETURN((auto [result_ds, result_shape]),
                           internal::SelectOp()(ds_impl, ds.GetShape(),
                                                filter_impl, fltr.GetShape()));
          return DataSlice::Create(std::move(result_ds),
                                   std::move(result_shape), ds.GetSchemaImpl(),
                                   ds.GetDb());
        });
  });
}

absl::StatusOr<DataSlice> ReverseSelect(const DataSlice& ds,
                                        const DataSlice& filter) {
  const internal::DataItem& schema = filter.GetSchemaImpl();

  if (schema != schema::kAny && schema != schema::kObject &&
      schema != schema::kMask) {
    return absl::InvalidArgumentError(
        "the schema of the filter DataSlice should only be Any, Object or "
        "Mask");
  }
  auto ds_shape = ds.GetShape();
  auto filter_shape = filter.GetShape();
  if (ds_shape.rank() != filter_shape.rank()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "the rank of the ds and filter DataSlice must be the same. Got "
        "rank(ds): ",
        ds_shape.rank(), ", rank(filter): ", filter_shape.rank()));
  }
  return ds.VisitImpl([&](const auto& ds_impl) {
    return filter.VisitImpl(
        [&](const auto& filter_impl) -> absl::StatusOr<DataSlice> {
          ASSIGN_OR_RETURN(
              auto res, internal::ReverseSelectOp()(ds_impl, ds_shape,
                                                    filter_impl, filter_shape));
          return DataSlice::Create(std::move(res), filter_shape,
                                   ds.GetSchemaImpl(), ds.GetDb());
        });
  });
}

absl::StatusOr<DataSlice> ShallowClone(const DataSlice& ds,
                                       const DataSlice& schema) {
  const auto& db = ds.GetDb();
  if (db == nullptr) {
    return absl::InvalidArgumentError("cannot clone without a DataBag");
  }
  const auto& schema_db = schema.GetDb();
  RETURN_IF_ERROR(schema.VerifyIsSchema());
  const auto& schema_impl = schema.impl<internal::DataItem>();
  FlattenFallbackFinder fb_finder(*db);
  auto fallbacks_span = fb_finder.GetFlattenFallbacks();
  return ds.VisitImpl([&](const auto& impl) -> absl::StatusOr<DataSlice> {
    auto result_slice_impl = impl;
    internal::DataItem result_schema_impl;
    auto result_db = DataBag::Empty();
    ASSIGN_OR_RETURN(auto result_db_impl, result_db->GetMutableImpl());
    internal::ShallowCloneOp clone_op(&result_db_impl.get());
    if (schema_db == nullptr || schema_db == db) {
      ASSIGN_OR_RETURN(
          std::tie(result_slice_impl, result_schema_impl),
          clone_op(impl, schema_impl, db->GetImpl(), fallbacks_span));
    } else {
      FlattenFallbackFinder schema_fb_finder(*schema_db);
      auto schema_fallbacks_span = schema_fb_finder.GetFlattenFallbacks();
      ASSIGN_OR_RETURN(
          std::tie(result_slice_impl, result_schema_impl),
          clone_op(impl, schema_impl, db->GetImpl(), fallbacks_span,
                   schema_db->GetImpl(), schema_fallbacks_span));
    }
    return DataSlice::Create(result_slice_impl, ds.GetShape(),
                             result_schema_impl, result_db);
  });
}

absl::StatusOr<arolla::OperatorPtr> SubsliceOperatorFamily::DoGetOperator(
    absl::Span<const arolla::QTypePtr> input_types,
    arolla::QTypePtr output_type) const {
  if (input_types.empty()) {
    return OperatorNotDefinedError("kde.core.subslice", input_types,
                                   "expected at least 1 argument");
  }
  if (input_types[0] != arolla::GetQType<DataSlice>()) {
    return OperatorNotDefinedError("kde.core.subslice", input_types,
                                   "'x' must be a DataSlice");
  }

  std::optional<int64_t> ellipsis_pos_for_error;
  for (size_t i = 1; i < input_types.size(); ++i) {
    auto input_type = input_types[i];
    if (auto status =
            IsSliceQTypeValid(input_type, i - 1, ellipsis_pos_for_error);
        !status.ok()) {
      return OperatorNotDefinedError(
          "kde.core.subslice", input_types,
          absl::StrFormat("slicing argument at position %d is invalid: %s",
                          i - 1, status.message()));
    }
  }
  return arolla::EnsureOutputQTypeMatches(
      std::make_shared<SubsliceOperator>(input_types), input_types,
      output_type);
}

absl::StatusOr<DataSlice> At(const DataSlice& x, const DataSlice& indices) {
  const auto& x_shape = x.GetShape();
  if (x_shape.rank() == 0) {
    return absl::InvalidArgumentError("kd.at is not supported for DataItem.");
  }
  const auto shape_for_expansion = x_shape.RemoveDims(x_shape.rank() - 1);
  const auto& indices_shape = indices.GetShape();
  if (indices_shape.rank() >= shape_for_expansion.rank()) {
    if (!shape_for_expansion.IsBroadcastableTo(indices_shape)) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "DataSlice with shape=%s cannot be expanded to shape=%s; kd.at "
          "requires shape(x)[:-1] to be broadcastable to shape(indices) when "
          "ndim(x) <= ndim(indices)",
          arolla::Repr(indices_shape), arolla::Repr(shape_for_expansion)));
    }
    return AtImpl(x, indices);
  } else {
    // Expand indices if rank(indices_shape) < rank(shape_for_expansion).
    ASSIGN_OR_RETURN(
        auto expanded_indices, BroadcastToShape(indices, shape_for_expansion),
        _ << "kd.at requires shape(indices) to be broadcastable to "
          << "shape(x)[:-1] when ndim(x) - 1 > ndim(indices)");
    return AtImpl(x, expanded_indices);
  }
}

absl::StatusOr<DataSlice> Translate(const DataSlice& keys_to,
                                    const DataSlice& keys_from,
                                    const DataSlice& values_from) {
  const auto& from_shape = keys_from.GetShape();
  if (!from_shape.IsEquivalentTo(values_from.GetShape())) {
    return absl::InvalidArgumentError(
        "keys_from and values_from must have the same shape");
  }

  const auto& to_shape = keys_to.GetShape();
  if (to_shape.rank() == 0 || from_shape.rank() == 0) {
    return absl::InvalidArgumentError(
        "keys_to, keys_from and values_from must have at least one dimension");
  }

  const auto shape_without_last_dim = to_shape.RemoveDims(to_shape.rank() - 1);
  if (!from_shape.RemoveDims(from_shape.rank() - 1)
           .IsEquivalentTo(shape_without_last_dim)) {
    return absl::InvalidArgumentError(
        "keys_from and keys_to must have the same dimensions except the last "
        "one");
  }

  if (keys_from.GetSchemaImpl() != keys_to.GetSchemaImpl()) {
    return absl::InvalidArgumentError(
        "keys_from and keys_to must have the same schema");
  }

  auto temp_db = DataBag::Empty();
  ASSIGN_OR_RETURN(
      auto lookup,
      CreateDictShaped(temp_db, shape_without_last_dim,
                       keys_from.WithDb(nullptr), values_from.WithDb(nullptr)));
  ASSIGN_OR_RETURN(auto unique_keys, lookup.GetDictKeys());
  if (!unique_keys.GetShape().IsEquivalentTo(keys_from.GetShape())) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "keys_from must be unique within each group of the last dimension: "
        "original shape %s vs shape after dedup %s. Consider using "
        "translate_group instead.",
        arolla::Repr(keys_from.GetShape()),
        arolla::Repr(unique_keys.GetShape())));
  }
  ASSIGN_OR_RETURN(auto res, lookup.GetFromDict(keys_to));
  return res.WithDb(values_from.GetDb());
}

absl::StatusOr<arolla::OperatorPtr> UuSchemaOperatorFamily::DoGetOperator(
    absl::Span<const arolla::QTypePtr> input_types,
    arolla::QTypePtr output_type) const {
  if (input_types.size() != 2) {
    return absl::InvalidArgumentError("requires exactly 2 arguments");
  }
  if (input_types[0] != arolla::GetQType<DataSlice>()) {
    return absl::InvalidArgumentError(
        "requires first argument to be DataSlice");
  }
  RETURN_IF_ERROR(VerifyNamedTuple(input_types[1]));
  return arolla::EnsureOutputQTypeMatches(
      std::make_shared<UuSchemaOperator>(input_types), input_types,
      output_type);
}

absl::StatusOr<arolla::OperatorPtr> UuidOperatorFamily::DoGetOperator(
    absl::Span<const arolla::QTypePtr> input_types,
    arolla::QTypePtr output_type) const {
  if (input_types.size() != 2) {
    return absl::InvalidArgumentError("requires exactly 2 arguments");
  }
  if (input_types[0] != arolla::GetQType<DataSlice>()) {
    return absl::InvalidArgumentError(
        "requires first argument to be DataSlice");
  }
  RETURN_IF_ERROR(VerifyNamedTuple(input_types[1]));
  return arolla::EnsureOutputQTypeMatches(
      std::make_shared<UuidOperator>(input_types), input_types, output_type);
}

absl::StatusOr<arolla::OperatorPtr> UuObjOperatorFamily::DoGetOperator(
    absl::Span<const arolla::QTypePtr> input_types,
    arolla::QTypePtr output_type) const {
  if (input_types.size() != 2) {
    return absl::InvalidArgumentError("requires exactly 2 arguments");
  }
  if (input_types[0] != arolla::GetQType<DataSlice>()) {
    return absl::InvalidArgumentError(
        "requires first argument to be DataSlice");
  }
  RETURN_IF_ERROR(VerifyNamedTuple(input_types[1]));
  return arolla::EnsureOutputQTypeMatches(
      std::make_shared<UuObjOperator>(input_types), input_types, output_type);
}

}  // namespace koladata::ops
