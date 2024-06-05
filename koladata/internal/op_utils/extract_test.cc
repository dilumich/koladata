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
#include "koladata/internal/op_utils/extract.h"

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/types/span.h"
#include "koladata/internal/data_bag.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/object_id.h"
#include "koladata/internal/schema_utils.h"
#include "koladata/internal/testing/matchers.h"
#include "koladata/testing/status_matchers_backport.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/dense_array/edge.h"
#include "arolla/memory/optional_value.h"

namespace koladata::internal {
namespace {

using ::koladata::testing::StatusIs;

using ::arolla::CreateDenseArray;
using ::koladata::internal::testing::DataBagEqual;

using TriplesT = std::vector<
    std::pair<DataItem, std::vector<std::pair<std::string_view, DataItem>>>>;

DataItem AllocateSchema() {
  return DataItem(internal::AllocateExplicitSchema());
}

template <typename T>
DataSliceImpl CreateSlice(absl::Span<const arolla::OptionalValue<T>> values) {
  return DataSliceImpl::Create(CreateDenseArray<T>(values));
}

void SetSchemaTriples(DataBagImpl& db, const TriplesT& schema_triples) {
  for (auto [schema, attrs] : schema_triples) {
    for (auto [attr_name, attr_schema] : attrs) {
      EXPECT_OK(db.SetSchemaAttr(schema, attr_name, attr_schema));
    }
  }
}

void SetDataTriples(DataBagImpl& db, const TriplesT& data_triples) {
  for (auto [item, attrs] : data_triples) {
    for (auto [attr_name, attr_data] : attrs) {
      EXPECT_OK(db.SetAttr(item, attr_name, attr_data));
    }
  }
}

TriplesT GenNoiseDataTriples() {
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(5);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto a3 = obj_ids[3];
  auto a4 = obj_ids[4];
  TriplesT data = {{a0, {{"x", DataItem(1)}, {"next", a1}}},
                   {a1, {{"y", DataItem(3)}, {"prev", a0}, {"next", a2}}},
                   {a3, {{"x", DataItem(1)}, {"y", DataItem(2)}, {"next", a4}}},
                   {a4, {{"prev", a3}}}};
  return data;
}

TriplesT GenNoiseSchemaTriples() {
  auto schema0 = AllocateSchema();
  auto schema1 = AllocateSchema();
  auto int_dtype = DataItem(schema::kInt32);
  TriplesT schema_triples = {
      {schema0, {{"self", schema0}, {"next", schema1}, {"x", int_dtype}}},
      {schema1, {{"prev", schema0}, {"y", int_dtype}}}};
  return schema_triples;
}

enum ExtractTestParam {kMainDb, kFallbackDb};

class ExtractTest : public ::testing::TestWithParam<ExtractTestParam> {
 public:
  DataBagImplPtr GetMainDb(DataBagImplPtr db) {
    switch (GetParam()) {
      case kMainDb:
        return db;
      case kFallbackDb:
        return DataBagImpl::CreateEmptyDatabag();
    }
    DCHECK(false);
  }
  DataBagImplPtr GetFallbackDb(DataBagImplPtr db) {
    switch (GetParam()) {
      case kMainDb:
        return DataBagImpl::CreateEmptyDatabag();
      case kFallbackDb:
        return db;
    }
    DCHECK(false);
  }
};

INSTANTIATE_TEST_SUITE_P(MainOrFallback, ExtractTest,
                         ::testing::Values(kMainDb, kFallbackDb));

TEST_P(ExtractTest, DataSliceEntity) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(3);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto int_dtype = DataItem(schema::kInt32);
  auto schema = AllocateSchema();

  TriplesT schema_triples = {{schema, {{"x", int_dtype}, {"y", int_dtype}}}};
  TriplesT data_triples = {{a0, {{"x", DataItem(1)}, {"y", DataItem(4)}}},
                           {a1, {{"x", DataItem(2)}, {"y", DataItem(5)}}},
                           {a2, {{"x", DataItem(3)}, {"y", DataItem(6)}}}};

  SetSchemaTriples(*db, schema_triples);
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, schema_triples);
  SetDataTriples(*expected_db, data_triples);

  ASSERT_OK_AND_ASSIGN(
      auto result_db,
      ExtractOp()(obj_ids, schema, *GetMainDb(db), {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceObjectIds) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(3);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto schema = AllocateSchema();

  TriplesT schema_triples = {{schema, {{"next", schema}, {"prev", schema}}}};
  TriplesT data_triples = {{a0, {{"prev", a2}, {"next", a1}}},
                           {a1, {{"prev", a0}, {"next", a2}}},
                           {a2, {{"prev", a1}, {"next", a0}}}};
  SetSchemaTriples(*db, schema_triples);
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, schema_triples);
  SetDataTriples(*expected_db, data_triples);

  ASSERT_OK_AND_ASSIGN(
      auto result_db,
      ExtractOp()(obj_ids, schema, *GetMainDb(db), {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceObjectSchema) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(3);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto schema0 = AllocateSchema();
  auto schema1 = AllocateSchema();
  auto schema2 = AllocateSchema();
  auto obj_dtype = DataItem(schema::kObject);

  TriplesT schema_triples = {
      {schema0, {{"next", obj_dtype}}},
      {schema1, {{"prev", obj_dtype}, {"next", obj_dtype}}},
      {schema2, {{"prev", obj_dtype}}}};
  TriplesT data_triples = {
      {a0, {{schema::kSchemaAttr, schema0}, {"next", a1}}},
      {a1, {{schema::kSchemaAttr, schema1}, {"prev", a0}, {"next", a2}}},
      {a2, {{schema::kSchemaAttr, schema2}, {"prev", a1}}}};
  SetSchemaTriples(*db, schema_triples);
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, schema_triples);
  SetDataTriples(*expected_db, data_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db,
                       ExtractOp()(DataItem(a0), obj_dtype, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceListsPrimitives) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto lists = DataSliceImpl::ObjectsFromAllocation(AllocateLists(3), 3);
  auto values =
      DataSliceImpl::Create(CreateDenseArray<int32_t>({1, 2, 3, 4, 5, 6, 7}));
  ASSERT_OK_AND_ASSIGN(auto edge, arolla::DenseArrayEdge::FromSplitPoints(
                                      CreateDenseArray<int64_t>({0, 3, 5, 7})));
  ASSERT_OK(db->ExtendLists(lists, values, edge));
  auto list_schema = AllocateSchema();
  TriplesT schema_triples = {
      {list_schema,
       {{schema::kListItemsSchemaAttr, DataItem(schema::kInt32)}}}};
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK(expected_db->ExtendLists(lists, values, edge));
  SetSchemaTriples(*expected_db, schema_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db,
                       ExtractOp()(lists, list_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceListsObjectIds) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(7);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto a3 = obj_ids[3];
  auto a4 = obj_ids[4];
  auto a5 = obj_ids[5];
  auto a6 = obj_ids[6];
  auto lists = DataSliceImpl::ObjectsFromAllocation(AllocateLists(3), 3);
  auto values = DataSliceImpl::Create(
      CreateDenseArray<DataItem>({a0, a1, a2, a3, a4, a5, a6}));
  ASSERT_OK_AND_ASSIGN(auto edge, arolla::DenseArrayEdge::FromSplitPoints(
                                      CreateDenseArray<int64_t>({0, 3, 5, 7})));
  ASSERT_OK(db->ExtendLists(lists, values, edge));
  TriplesT data_triples = {{a0, {{"x", DataItem(0)}, {"y", DataItem(0)}}},
                           {a1, {{"x", DataItem(0)}, {"y", DataItem(1)}}},
                           {a2, {{"x", DataItem(0)}, {"y", DataItem(2)}}},
                           {a3, {{"x", DataItem(1)}, {"y", DataItem(0)}}},
                           {a4, {{"x", DataItem(1)}, {"y", DataItem(1)}}},
                           {a5, {{"x", DataItem(2)}, {"y", DataItem(0)}}},
                           {a6, {{"x", DataItem(2)}, {"y", DataItem(1)}}}};
  auto list_schema = AllocateSchema();
  auto point_schema = AllocateSchema();
  TriplesT schema_triples = {
      {point_schema,
       {{"x", DataItem(schema::kInt32)}, {"y", DataItem(schema::kInt32)}}},
      {list_schema, {{schema::kListItemsSchemaAttr, point_schema}}}};
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK(expected_db->ExtendLists(lists, values, edge));
  SetDataTriples(*expected_db, data_triples);
  SetSchemaTriples(*expected_db, schema_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db,
                       ExtractOp()(lists, list_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceListsObjectIdsObjectSchema) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(7);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto a3 = obj_ids[3];
  auto a4 = obj_ids[4];
  auto a5 = obj_ids[5];
  auto a6 = obj_ids[6];
  auto lists = DataSliceImpl::ObjectsFromAllocation(AllocateLists(3), 3);
  auto values = DataSliceImpl::Create(
      CreateDenseArray<DataItem>({a0, a1, a2, a3, a4, a5, a6}));
  ASSERT_OK_AND_ASSIGN(auto edge, arolla::DenseArrayEdge::FromSplitPoints(
                                      CreateDenseArray<int64_t>({0, 3, 5, 7})));
  ASSERT_OK(db->ExtendLists(lists, values, edge));
  auto list_schema = AllocateSchema();
  auto point_schema = AllocateSchema();
  std::pair<std::string_view, DataItem> object_schema_attr = {
      schema::kSchemaAttr, point_schema};
  TriplesT data_triples = {
      {a0, {object_schema_attr, {"x", DataItem(0)}, {"y", DataItem(0)}}},
      {a1, {object_schema_attr, {"x", DataItem(0)}, {"y", DataItem(1)}}},
      {a2, {object_schema_attr, {"x", DataItem(0)}, {"y", DataItem(2)}}},
      {a3, {object_schema_attr, {"x", DataItem(1)}, {"y", DataItem(0)}}},
      {a4, {object_schema_attr, {"x", DataItem(1)}, {"y", DataItem(1)}}},
      {a5, {object_schema_attr, {"x", DataItem(2)}, {"y", DataItem(0)}}},
      {a6, {object_schema_attr, {"x", DataItem(2)}, {"y", DataItem(1)}}}};
  TriplesT schema_triples = {
      {point_schema,
       {{"x", DataItem(schema::kInt32)}, {"y", DataItem(schema::kInt32)}}},
      {list_schema,
       {{schema::kListItemsSchemaAttr, DataItem(schema::kObject)}}}};
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK(expected_db->ExtendLists(lists, values, edge));
  SetDataTriples(*expected_db, data_triples);
  SetSchemaTriples(*expected_db, schema_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db,
                       ExtractOp()(lists, list_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceDictsPrimitives) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto dicts = DataSliceImpl::ObjectsFromAllocation(AllocateDicts(3), 3);
  auto dicts_expanded = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {dicts[0], dicts[0], dicts[0], dicts[1], dicts[1], dicts[2], dicts[2]}));
  auto keys =
      DataSliceImpl::Create(CreateDenseArray<int64_t>({1, 2, 3, 1, 5, 3, 7}));
  auto values =
      DataSliceImpl::Create(CreateDenseArray<float>({1, 2, 3, 4, 5, 6, 7}));
  ASSERT_OK(db->SetInDict(dicts_expanded, keys, values));
  auto dict_schema = AllocateSchema();
  TriplesT schema_triples = {
      {dict_schema,
       {{schema::kDictKeysSchemaAttr, DataItem(schema::kInt32)},
        {schema::kDictValuesSchemaAttr, DataItem(schema::kFloat32)}}}};
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK(expected_db->SetInDict(dicts_expanded, keys, values));
  SetSchemaTriples(*expected_db, schema_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db,
                       ExtractOp()(dicts, dict_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceDictsObjectIds) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto dicts = DataSliceImpl::ObjectsFromAllocation(AllocateDicts(3), 3);
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(7);
  auto k0 = obj_ids[0];
  auto k1 = obj_ids[1];
  auto k2 = obj_ids[2];
  auto k3 = obj_ids[3];
  auto v0 = obj_ids[4];
  auto v1 = obj_ids[5];
  auto v2 = obj_ids[6];

  auto dicts_expanded = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {dicts[0], dicts[0], dicts[0], dicts[1], dicts[1], dicts[2], dicts[2]}));
  auto keys = DataSliceImpl::Create(
      CreateDenseArray<DataItem>({k0, k1, k2, k0, k3, k0, k2}));
  auto values = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {v0, v0, v0, v1, v2, DataItem(), DataItem()}));
  ASSERT_OK(db->SetInDict(dicts_expanded, keys, values));
  auto dict_schema = AllocateSchema();
  auto key_schema = AllocateSchema();
  auto value_schema = AllocateSchema();
  TriplesT data_triples = {
      {k0, {{"x", DataItem(0)}, {"y", DataItem(0)}}},
      {k1, {{"x", DataItem(0)}, {"y", DataItem(1)}}},
      {k2, {{"x", DataItem(0)}, {"y", DataItem(2)}}},
      {k3, {{"x", DataItem(1)}, {"y", DataItem(0)}}},
      {v0, {{"val", DataItem(1.5)}}},
      {v1, {{"val", DataItem(2.0)}}},
      {v2, {{"val", DataItem(2.5)}}}};
  TriplesT schema_triples = {
      {key_schema,
       {{"x", DataItem(schema::kInt32)}, {"y", DataItem(schema::kInt32)}}},
      {value_schema, {{"val", DataItem(schema::kFloat32)}}},
      {dict_schema,
       {{schema::kDictKeysSchemaAttr, key_schema},
        {schema::kDictValuesSchemaAttr, value_schema}}}};
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK(expected_db->SetInDict(dicts_expanded, keys, values));
  SetDataTriples(*expected_db, data_triples);
  SetSchemaTriples(*expected_db, schema_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db,
                       ExtractOp()(dicts, dict_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceDictsObjectIdsObjectSchema) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto dicts = DataSliceImpl::ObjectsFromAllocation(AllocateDicts(3), 3);
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(7);
  auto k0 = obj_ids[0];
  auto k1 = obj_ids[1];
  auto k2 = obj_ids[2];
  auto k3 = obj_ids[3];
  auto v0 = obj_ids[4];
  auto v1 = obj_ids[5];
  auto v2 = obj_ids[6];

  auto dicts_expanded = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {dicts[0], dicts[0], dicts[0], dicts[1], dicts[1], dicts[2], dicts[2]}));
  auto keys = DataSliceImpl::Create(
      CreateDenseArray<DataItem>({k0, k1, k2, k0, k3, k0, k2}));
  auto values = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {v0, v0, v0, v1, v2, DataItem(), DataItem()}));
  ASSERT_OK(db->SetInDict(dicts_expanded, keys, values));
  auto key_schema = AllocateSchema();
  auto value_schema = AllocateSchema();
  auto dict_schema = AllocateSchema();
  std::pair<std::string_view, DataItem> key_schema_attr = {
      schema::kSchemaAttr, key_schema};
  std::pair<std::string_view, DataItem> value_schema_attr = {
      schema::kSchemaAttr, value_schema};
  TriplesT data_triples = {
      {k0, {key_schema_attr, {"x", DataItem(0)}, {"y", DataItem(0)}}},
      {k1, {key_schema_attr, {"x", DataItem(0)}, {"y", DataItem(1)}}},
      {k2, {key_schema_attr, {"x", DataItem(0)}, {"y", DataItem(2)}}},
      {k3, {key_schema_attr, {"x", DataItem(1)}, {"y", DataItem(0)}}},
      {v0, {value_schema_attr, {"val", DataItem(1.5)}}},
      {v1, {value_schema_attr, {"val", DataItem(2.0)}}},
      {v2, {value_schema_attr, {"val", DataItem(2.5)}}}};
  TriplesT schema_triples = {
      {key_schema,
       {{"x", DataItem(schema::kInt32)}, {"y", DataItem(schema::kInt32)}}},
      {value_schema, {{"val", DataItem(schema::kFloat32)}}},
      {dict_schema,
       {{schema::kDictKeysSchemaAttr, DataItem(schema::kObject)},
        {schema::kDictValuesSchemaAttr, DataItem(schema::kObject)}}}};
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK(expected_db->SetInDict(dicts_expanded, keys, values));
  SetDataTriples(*expected_db, data_triples);
  SetSchemaTriples(*expected_db, schema_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db,
                       ExtractOp()(dicts, dict_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceDicts_LoopSchema) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto dicts = DataSliceImpl::ObjectsFromAllocation(AllocateDicts(3), 3);
  auto objs = DataSliceImpl::AllocateEmptyObjects(3);
  auto k0 = objs[0];
  auto k1 = objs[1];
  auto k2 = objs[2];
  auto dicts_expanded = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {dicts[0], dicts[0], dicts[0], dicts[1], dicts[1], dicts[2], dicts[2]}));
  auto keys = DataSliceImpl::Create(
      CreateDenseArray<DataItem>({k0, k1, k2, k1, k2, k0, k2}));
  auto values = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {dicts[0], dicts[1], dicts[2], dicts[1], dicts[2], dicts[0], dicts[2]}));
  ASSERT_OK(db->SetInDict(dicts_expanded, keys, values));
  TriplesT data_triples = {
    {k0, {{"x", DataItem(0)}}},
    {k1, {{"x", DataItem(1)}}},
    {k2, {{"x", DataItem(2)}}},
  };
  auto key_schema = AllocateSchema();
  auto dict_schema = AllocateSchema();
  TriplesT schema_triples = {
      {key_schema, {{"x", DataItem(schema::kInt64)}}},
      {dict_schema,
       {{schema::kDictKeysSchemaAttr, key_schema},
        {schema::kDictValuesSchemaAttr, dict_schema}}}};
  SetSchemaTriples(*db, schema_triples);
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK(expected_db->SetInDict(dicts_expanded, keys, values));
  SetSchemaTriples(*expected_db, schema_triples);
  SetDataTriples(*expected_db, data_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db,
                       ExtractOp()(dicts[0], dict_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceDicts_LoopSchema_NoData) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto key_schema = AllocateSchema();
  auto dict_schema = AllocateSchema();
  TriplesT schema_triples = {
      {key_schema, {{"x", DataItem(schema::kInt64)}}},
      {dict_schema,
       {{schema::kDictKeysSchemaAttr, key_schema},
        {schema::kDictValuesSchemaAttr, dict_schema}}}};
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, schema_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db,
                       ExtractOp()(DataItem(), dict_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceLists_LoopSchema) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto lists = DataSliceImpl::ObjectsFromAllocation(AllocateLists(3), 3);
  auto values = DataSliceImpl::Create(
      CreateDenseArray<DataItem>({lists[1], lists[2], lists[0]}));
  ASSERT_OK(db->AppendToList(lists, values));
  auto list_schema = AllocateSchema();
  TriplesT schema_triples = {
      {list_schema, {{schema::kListItemsSchemaAttr, list_schema}}}};
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK(expected_db->AppendToList(lists, values));
  SetSchemaTriples(*expected_db, schema_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db,
                       ExtractOp()(lists[0], list_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, DataSliceDicts_InvalidSchema_MissingKeys) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto dict_schema = AllocateSchema();
  TriplesT schema_triples = {
      {dict_schema,
       {{schema::kDictValuesSchemaAttr, DataItem(schema::kInt32)}}}};
  SetSchemaTriples(*db, schema_triples);

  EXPECT_THAT(ExtractOp()(DataItem(), dict_schema, *GetMainDb(db),
                          {GetFallbackDb(db).get()}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       ::testing::AllOf(
                           ::testing::HasSubstr("dict schema"),
                           ::testing::HasSubstr("has unexpected attributes"))));
}

TEST_P(ExtractTest, DataSliceDicts_InvalidSchema_MissingValues) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto dict_schema = AllocateSchema();
  TriplesT schema_triples = {
      {dict_schema, {{schema::kDictKeysSchemaAttr, DataItem(schema::kInt32)}}}};
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  EXPECT_THAT(ExtractOp()(DataItem(), dict_schema, *GetMainDb(db),
                          {GetFallbackDb(db).get()}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       ::testing::AllOf(
                           ::testing::HasSubstr("dict schema"),
                           ::testing::HasSubstr("has unexpected attributes"))));
}

TEST_P(ExtractTest, DataSliceLists_InvalidSchema) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto list_schema = AllocateSchema();
  TriplesT schema_triples = {
      {list_schema,
       {{schema::kListItemsSchemaAttr, DataItem(schema::kInt32)},
        {"y", DataItem(schema::kInt32)}}}};
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  EXPECT_THAT(ExtractOp()(DataItem(), list_schema, *GetMainDb(db),
                          {GetFallbackDb(db).get()}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       ::testing::AllOf(
                           ::testing::HasSubstr("list schema"),
                           ::testing::HasSubstr("has unexpected attributes"))));
}

TEST_P(ExtractTest, DataSliceDicts_InvalidSchema_UnexpectedAttr) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto dict_schema = AllocateSchema();
  TriplesT schema_triples = {
      {dict_schema,
       {{schema::kDictKeysSchemaAttr, DataItem(schema::kInt32)},
        {"x", DataItem(schema::kInt32)}}}};
  SetSchemaTriples(*db, schema_triples);

  EXPECT_THAT(ExtractOp()(DataItem(), dict_schema, *GetMainDb(db),
                          {GetFallbackDb(db).get()}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       ::testing::AllOf(
                           ::testing::HasSubstr("dict schema"),
                           ::testing::HasSubstr("has unexpected attributes"))));

  TriplesT schema_add_triples = {
      {dict_schema,
       {{schema::kDictValuesSchemaAttr, DataItem(schema::kInt32)}}}};
  SetSchemaTriples(*db, schema_add_triples);

  EXPECT_THAT(ExtractOp()(DataItem(), dict_schema, *GetMainDb(db),
                          {GetFallbackDb(db).get()}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       ::testing::AllOf(
                           ::testing::HasSubstr("dict schema"),
                           ::testing::HasSubstr("has unexpected attributes"))));
}

TEST_P(ExtractTest, ExtractSchemaForEmptySlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(1);
  auto a0 = obj_ids[0];
  auto schema1 = AllocateSchema();
  auto schema2 = AllocateSchema();

  TriplesT schema_triples = {{schema1, {{"next", schema2}}},
                             {schema2, {{"prev", schema1}}}};
  TriplesT data_triples = {};
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, schema_triples);

  ASSERT_OK_AND_ASSIGN(
      auto result_db,
      ExtractOp()(obj_ids, schema1, *GetMainDb(db), {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, RecursiveSchema) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(4);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto a3 = obj_ids[3];
  auto schema = AllocateSchema();

  TriplesT schema_triples = {{schema, {{"next", schema}}}};
  TriplesT data_triples = {
      {a0, {{"next", a1}}}, {a1, {{"next", a2}}}, {a2, {{"next", a3}}}};
  SetSchemaTriples(*db, schema_triples);
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, schema_triples);
  SetDataTriples(*expected_db, data_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db, ExtractOp()(a0, schema, *GetMainDb(db),
                                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, MixedObjectsSlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(3);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto hidden_obj_ids = DataSliceImpl::AllocateEmptyObjects(2);
  auto a3 = hidden_obj_ids[0];
  auto a4 = hidden_obj_ids[1];
  auto schema = AllocateSchema();

  TriplesT schema_triples = {{schema, {{"next", DataItem(schema::kObject)}}}};
  TriplesT data_triples = {
      {a0, {{"next", a3}}},
      {a1, {{"next", DataItem(3)}}},
      {a2, {{"next", a4}}},
      {a3, {{schema::kSchemaAttr, schema}, {"next", DataItem(5)}}},
      {a4, {{schema::kSchemaAttr, schema}}},
  };
  SetSchemaTriples(*db, schema_triples);
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, schema_triples);
  SetDataTriples(*expected_db, data_triples);

  ASSERT_OK_AND_ASSIGN(
      auto result_db,
      ExtractOp()(obj_ids, schema, *GetMainDb(db), {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, PartialSchema) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(4);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto a3 = obj_ids[3];
  auto int_dtype = DataItem(schema::kInt32);
  auto schema = AllocateSchema();

  TriplesT schema_triples = {
      {schema, {{"next", schema}, {"x", int_dtype}, {"y", int_dtype}}}};
  TriplesT data_triples = {{a1, {{"next", a2}, {"x", DataItem(1)}}},
                           {a2, {{"next", a3}, {"y", DataItem(5)}}},
                           {a3, {{"x", DataItem(3)}, {"y", DataItem(6)}}}};
  TriplesT unreachable_data_triples = {
      {a0, {{"next", a1}, {"x", DataItem(7)}, {"z", DataItem(4)}}},
      {a1, {{"prev", a0}, {"z", DataItem(5)}}},
      {a3, {{"self", a3}}}};
  TriplesT unreachable_schema_triples = {};

  SetSchemaTriples(*db, schema_triples);
  SetDataTriples(*db, unreachable_data_triples);
  SetDataTriples(*db, data_triples);

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, schema_triples);
  SetDataTriples(*expected_db, data_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db, ExtractOp()(a1, schema, *GetMainDb(db),
                                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, PartialSchemaWithDifferentDataBag) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto schema_db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(4);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto a3 = obj_ids[3];
  auto int_dtype = DataItem(schema::kInt32);
  auto schema = AllocateSchema();
  auto unreachable_schema = AllocateSchema();

  TriplesT schema_triples = {
      {schema, {{"next", schema}, {"x", int_dtype}, {"y", int_dtype}}}};
  TriplesT unreachable_schema_triples = {
      {unreachable_schema, {{"next", unreachable_schema}}}};
  TriplesT noise_schema_triples = {
      {schema, {{"next", int_dtype}, {"z", schema}, {"y", int_dtype}}}};
  TriplesT data_triples = {{a1, {{"next", a2}, {"x", DataItem(1)}}},
                           {a2, {{"next", a3}, {"y", DataItem(5)}}},
                           {a3, {{"x", DataItem(3)}, {"y", DataItem(6)}}}};
  TriplesT unreachable_data_triples = {
      {a0, {{"next", a1}, {"x", DataItem(7)}, {"z", DataItem(4)}}},
      {a1, {{"prev", a0}, {"z", DataItem(5)}}},
      {a3, {{"self", a3}}}};

  SetSchemaTriples(*schema_db, schema_triples);
  SetSchemaTriples(*schema_db, unreachable_schema_triples);
  SetSchemaTriples(*db, unreachable_schema_triples);
  SetSchemaTriples(*db, noise_schema_triples);
  SetDataTriples(*db, unreachable_data_triples);
  SetDataTriples(*db, data_triples);

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, schema_triples);
  SetDataTriples(*expected_db, data_triples);

  ASSERT_OK_AND_ASSIGN(
      auto result_db,
      ExtractOp()(a1, schema, *GetMainDb(db), {GetFallbackDb(db).get()},
                  *GetMainDb(schema_db), {GetFallbackDb(schema_db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, MergeSchemaFromTwoDatabags) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto schema_db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(4);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto int_dtype = DataItem(schema::kInt32);
  auto object_dtype = DataItem(schema::kObject);
  auto schema = AllocateSchema();
  auto unreachable_schema = AllocateSchema();

  TriplesT data_triples = {
    {a0, {{"next", a1}, {"x", DataItem(1)}}},
    {a1, {{schema::kSchemaAttr, DataItem(schema)}, {"y", DataItem(4)}}},
  };
  TriplesT unreachable_data_triples = {
      {a0, {{"y", DataItem(2)}}},  // TODO: should be extracted
      {a1, {{"x", DataItem(3)}}},
  };
  TriplesT data_db_schema_triples = {
      {schema, {{"next", object_dtype}, {"y", int_dtype}}}};
  TriplesT schema_db_schema_triples = {
      {schema, {{"next", object_dtype}, {"x", int_dtype}}}};

  SetDataTriples(*db, data_triples);
  SetDataTriples(*db, unreachable_data_triples);
  SetSchemaTriples(*db, data_db_schema_triples);
  SetSchemaTriples(*schema_db, schema_db_schema_triples);

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, data_db_schema_triples);
  SetSchemaTriples(*expected_db, schema_db_schema_triples);
  SetDataTriples(*expected_db, data_triples);

  ASSERT_OK_AND_ASSIGN(
      auto result_db,
      ExtractOp()(a0, schema, *GetMainDb(db), {GetFallbackDb(db).get()},
                  *GetMainDb(schema_db), {GetFallbackDb(schema_db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, ConflictingSchemasInTwoDatabags) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto schema_db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(4);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto text_dtype = DataItem(schema::kText);
  auto int_dtype = DataItem(schema::kInt32);
  auto object_dtype = DataItem(schema::kObject);
  auto schema = AllocateSchema();
  auto unreachable_schema = AllocateSchema();

  TriplesT data_triples = {
    {a0, {{"next", a1}}},
    {a1, {{"__schema__", DataItem(schema)}}},
  };
  TriplesT schema_triples = {
      {schema, {{"next", object_dtype}, {"x", text_dtype}}}};
  TriplesT schema_db_triples = {
      {schema, {{"next", object_dtype}, {"x", int_dtype}}}};

  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*schema_db, schema_db_triples);

  EXPECT_THAT(
      ExtractOp()(a0, schema, *GetMainDb(db), {GetFallbackDb(db).get()},
                  *GetMainDb(schema_db), {GetFallbackDb(schema_db).get()}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               ::testing::AllOf(
                   ::testing::HasSubstr("conflicting values for schema"),
                   ::testing::HasSubstr("x: INT32 != TEXT"))));
}

TEST_P(ExtractTest, NoFollowEntitySchema) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(4);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto a3 = obj_ids[3];
  auto int_dtype = DataItem(schema::kInt32);
  auto schema1 = AllocateSchema();
  auto schema2 = AllocateSchema();
  ASSERT_OK_AND_ASSIGN(auto nofollow_schema2,
                       schema::NoFollowSchemaItem(schema2));

  TriplesT schema_triples = {
      {schema1,
       {{"nofollow", nofollow_schema2}, {"x", int_dtype}, {"y", int_dtype}}},
  };
  TriplesT data_triples = {{a1, {{"nofollow", a2}, {"x", DataItem(1)}}}};
  TriplesT unreachable_data_triples = {
      {a0, {{"nofollow", a1}, {"x", DataItem(7)}, {"z", DataItem(4)}}},
      {a1, {{"prev", a0}, {"z", DataItem(5)}}},
      {a2, {{"nofollow", a3}, {"y", DataItem(5)}}},
      {a3, {{"self", a3}, {"x", DataItem(3)}, {"y", DataItem(6)}}}};
  TriplesT unreachable_schema_triples = {
      {nofollow_schema2,
       {{"nofollow", schema1}, {"x", int_dtype}, {"y", int_dtype}}}};

  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, unreachable_schema_triples);
  SetDataTriples(*db, unreachable_data_triples);
  SetDataTriples(*db, data_triples);

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, schema_triples);
  SetDataTriples(*expected_db, data_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db, ExtractOp()(a1, schema1, *GetMainDb(db),
                                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, NoFollowObjectSchema) {
    auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(3);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto schema0 = AllocateSchema();
  auto schema1 = AllocateSchema();
  auto schema2 = AllocateSchema();
  ASSERT_OK_AND_ASSIGN(auto nofollow_schema1,
                       schema::NoFollowSchemaItem(schema1));
  auto obj_dtype = DataItem(schema::kObject);

  TriplesT schema_triples = {
      {schema0, {{"nofollow", obj_dtype}}}};
  TriplesT unreachable_schema_triples = {
      {nofollow_schema1, {{"prev", obj_dtype}, {"next", obj_dtype}}},
      {schema2, {{"prev", obj_dtype}}}};
  TriplesT data_triples = {
      {a0, {{schema::kSchemaAttr, schema0}, {"nofollow", a1}}},
      {a1, {{schema::kSchemaAttr, nofollow_schema1}}}
  };
  TriplesT unreachable_data_triples = {
      {a1,
       {{schema::kSchemaAttr, nofollow_schema1}, {"prev", a0}, {"next", a2}}},
      {a2, {{schema::kSchemaAttr, schema2}, {"prev", a1}}}};
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, unreachable_schema_triples);
  SetDataTriples(*db, data_triples);
  SetDataTriples(*db, unreachable_data_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetSchemaTriples(*expected_db, schema_triples);
  SetDataTriples(*expected_db, data_triples);

  ASSERT_OK_AND_ASSIGN(auto result_db,
                       ExtractOp()(DataItem(a0), obj_dtype, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(ExtractTest, ObjectSchemaMissing) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(3);
  auto schema = DataItem(schema::kObject);

  EXPECT_THAT(
      ExtractOp()(obj_ids, schema, *GetMainDb(db), {GetFallbackDb(db).get()}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               ::testing::AllOf(
                   ::testing::HasSubstr("object"),
                   ::testing::HasSubstr("is expected to have a schema ObjectId "
                                        "in __schema__ attribute"))));
}

TEST_P(ExtractTest, InvalidSchemaType) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(3);
  auto schema = DataItem(1);

  EXPECT_THAT(
      ExtractOp()(obj_ids, schema, *GetMainDb(db), {GetFallbackDb(db).get()}),
      StatusIs(absl::StatusCode::kInternal, "unsupported schema type"));
}

TEST_P(ExtractTest, AnySchemaType) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(3);
  auto schema = DataItem(schema::kAny);

  EXPECT_THAT(
      ExtractOp()(obj_ids, schema, *GetMainDb(db), {GetFallbackDb(db).get()}),
      StatusIs(absl::StatusCode::kInternal,
               "clone/extract not supported for kAny schema"));
}

TEST_P(ExtractTest, AnySchemaTypeInside) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(2);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto schema1 = AllocateSchema();

  TriplesT schema_triples = {{schema1, {{"next", DataItem(schema::kAny)}}}};
  TriplesT data_triples = {{a0, {{"next", a1}}}};
  SetSchemaTriples(*db, schema_triples);

  EXPECT_THAT(
      ExtractOp()(a0, schema1, *GetMainDb(db), {GetFallbackDb(db).get()}),
      StatusIs(absl::StatusCode::kInternal,
               "clone/extract not supported for kAny schema"));
}

}  // namespace
}  // namespace koladata::internal
