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
#include "koladata/repr_utils.h"

#include <optional>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "koladata/data_bag.h"
#include "koladata/data_slice.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/error.pb.h"
#include "koladata/internal/error_utils.h"
#include "koladata/internal/object_id.h"
#include "koladata/object_factories.h"
#include "koladata/test_utils.h"
#include "koladata/testing/status_matchers_backport.h"
#include "arolla/util/init_arolla.h"
#include "arolla/util/testing/equals_proto.h"

namespace koladata {
namespace {

using ::arolla::testing::EqualsProto;
using ::koladata::internal::Error;
using ::koladata::internal::ObjectId;
using ::koladata::testing::StatusIs;
using ::testing::MatchesRegex;

class ReprUtilTest : public ::testing::Test {
 protected:
  void SetUp() override { arolla::InitArolla(); }
};

TEST_F(ReprUtilTest, TestAssembleError) {
  DataBagPtr bag = DataBag::Empty();

  DataSlice value_1 = test::DataItem(1);
  DataSlice value_2 = test::DataItem("b");

  ASSERT_OK_AND_ASSIGN(
      DataSlice entity,
      EntityCreator::FromAttrs(bag, {"a", "b"}, {value_1, value_2}));
  schema::DType dtype = schema::GetDType<int>();

  Error error;
  internal::NoCommonSchema* no_common_schema = error.mutable_no_common_schema();
  ASSERT_OK_AND_ASSIGN(*no_common_schema->mutable_common_schema(),
                       internal::EncodeDataItem(entity.GetSchemaImpl()));
  ASSERT_OK_AND_ASSIGN(*no_common_schema->mutable_conflicting_schema(),
                       internal::EncodeDataItem(internal::DataItem(dtype)));

  absl::Status status = AssembleErrorMessage(
      internal::WithErrorPayload(absl::InvalidArgumentError("error"), error),
      {bag});
  std::optional<Error> payload = internal::GetErrorPayload(status);
  EXPECT_TRUE(payload.has_value());
  EXPECT_TRUE(payload->has_no_common_schema());
  EXPECT_THAT(
      payload->error_message(),
      AllOf(
          MatchesRegex(
              R"regex((.|\n)*cannot find a common schema for provided schemas(.|\n)*)regex"),
          MatchesRegex(
              R"regex((.|\n)*the common schema\(s\) [0-9a-f]{32}:0: SCHEMA\(a=INT32, b=TEXT\)(.|\n)*)regex"),
          MatchesRegex(
              R"regex((.|\n)*the first conflicting schema INT32: INT32(.|\n)*)regex")));
}

TEST_F(ReprUtilTest, TestAssembleErrorMissingContextData) {
  Error error;
  internal::NoCommonSchema* no_common_schema = error.mutable_no_common_schema();
  ASSERT_OK_AND_ASSIGN(
      *no_common_schema->mutable_conflicting_schema(),
      internal::EncodeDataItem(internal::DataItem(schema::GetDType<int>())));
  ASSERT_OK_AND_ASSIGN(*no_common_schema->mutable_common_schema(),
                       internal::EncodeDataItem(internal::DataItem(
                           internal::AllocateSingleObject())));
  absl::Status status = AssembleErrorMessage(
          internal::WithErrorPayload(absl::InternalError("error"), error), {});
  std::optional<Error> payload = internal::GetErrorPayload(status);
  EXPECT_TRUE(payload.has_value());
  EXPECT_TRUE(payload->has_no_common_schema());
  EXPECT_THAT(
      payload->error_message(),
      AllOf(
          MatchesRegex(R"regex((.|\n)*conflicting schema INT32(.|\n)*)regex"),
          MatchesRegex(
              R"regex((.|\n)*the common schema\(s\) \$[0-9a-f]{32}:0(.|\n)*)regex")));

  Error error2;
  ASSERT_OK_AND_ASSIGN(
      *error2.mutable_missing_object_schema()->mutable_missing_schema_item(),
      internal::EncodeDataItem(
          internal::DataItem(internal::AllocateSingleObject())));
  EXPECT_THAT(
      AssembleErrorMessage(
          internal::WithErrorPayload(absl::InternalError("error"), error2), {}),
      StatusIs(absl::StatusCode::kInvalidArgument, "missing data slice"));
}

TEST_F(ReprUtilTest, TestAssembleErrorNotHandlingOkStatus) {
  EXPECT_TRUE(
      AssembleErrorMessage(absl::OkStatus(), {.db = DataBag::Empty()}).ok());
}

}  // namespace
}  // namespace koladata
