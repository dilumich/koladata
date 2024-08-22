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
#ifndef KOLADATA_OPERATORS_MATH_H_
#define KOLADATA_OPERATORS_MATH_H_

#include "absl/status/statusor.h"
#include "koladata/data_slice.h"

namespace koladata::ops {

// kde.math.subtract.
absl::StatusOr<DataSlice> Subtract(const DataSlice& x, const DataSlice& y);

// kde.math.multiply.
absl::StatusOr<DataSlice> Multiply(const DataSlice& x, const DataSlice& y);

// kde.math.maximum.
absl::StatusOr<DataSlice> Maximum(const DataSlice& x, const DataSlice& y);

// kde.math.minimum.
absl::StatusOr<DataSlice> Minimum(const DataSlice& x, const DataSlice& y);

// kde.math._agg_sum.
absl::StatusOr<DataSlice> AggSum(const DataSlice& x);

// kde.math._agg_max.
absl::StatusOr<DataSlice> AggMax(const DataSlice& x);

// kde.math._agg_min.
absl::StatusOr<DataSlice> AggMin(const DataSlice& x);

}  // namespace koladata::ops

#endif  // KOLADATA_OPERATORS_MATH_H_
