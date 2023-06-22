// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

#include "yb/integration-tests/cdcsdk_ysql_test_base.h"

namespace yb {
namespace cdc {

TEST_F(CDCSDKYsqlTest, YB_DISABLE_TEST_IN_TSAN(TestCDCSDKConsistentStreamWithManyTransactions)) {
  FLAGS_cdc_max_stream_intent_records = 40;

  ASSERT_OK(SetUpWithParams(3, 1, false));
  auto table = ASSERT_RESULT(CreateTable(&test_cluster_, kNamespaceName, kTableName));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);
  CDCStreamId stream_id = ASSERT_RESULT(CreateDBStream());
  auto set_resp = ASSERT_RESULT(SetCDCCheckpoint(stream_id, tablets, OpId::Min()));
  ASSERT_FALSE(set_resp.has_error());

  int num_batches = 75;
  int inserts_per_batch = 100;

  std::thread t1(
      [&]() -> void { PerformSingleAndMultiShardInserts(num_batches, inserts_per_batch, 20); });
  std::thread t2([&]() -> void {
    PerformSingleAndMultiShardInserts(
        num_batches, inserts_per_batch, 50, num_batches * inserts_per_batch);
  });

  t1.join();
  t2.join();

  ASSERT_OK(test_client()->FlushTables({table.table_id()}, false, 1000, false));

  // The count array stores counts of DDL, INSERT, UPDATE, DELETE, READ, TRUNCATE, BEGIN, COMMIT in
  // that order.
  const int expected_count[] = {
      1,
      2 * num_batches * inserts_per_batch,
      0,
      0,
      0,
      0,
      2 * num_batches + num_batches * inserts_per_batch,
      2 * num_batches + num_batches * inserts_per_batch,
  };
  int count[] = {0, 0, 0, 0, 0, 0, 0, 0};

  auto get_changes_resp = GetAllPendingChangesFromCdc(stream_id, tablets);
  for (auto record : get_changes_resp.records) {
    UpdateRecordCount(record, count);
  }

  CheckRecordsConsistency(get_changes_resp.records);
  LOG(INFO) << "Got " << get_changes_resp.records.size() << " records.";
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ(expected_count[i], count[i]);
  }
  ASSERT_EQ(30301, get_changes_resp.records.size());
}

TEST_F(CDCSDKYsqlTest, YB_DISABLE_TEST_IN_TSAN(TestCDCSDKConsistentStreamWithForeignKeys)) {
  FLAGS_cdc_max_stream_intent_records = 30;
  ASSERT_OK(SetUpWithParams(3, 1, false));

  auto conn = ASSERT_RESULT(test_cluster_.ConnectToDB(kNamespaceName));
  ASSERT_OK(
      conn.Execute("CREATE TABLE test1(id int primary key, value_1 int) SPLIT INTO 1 TABLETS"));
  ASSERT_OK(
      conn.Execute("CREATE TABLE test2(id int primary key, value_2 int, test1_id int, CONSTRAINT "
                   "fkey FOREIGN KEY(test1_id) REFERENCES test1(id)) SPLIT INTO 1 TABLETS"));

  auto table1 = ASSERT_RESULT(GetTable(&test_cluster_, kNamespaceName, "test1"));
  auto table2 = ASSERT_RESULT(GetTable(&test_cluster_, kNamespaceName, "test2"));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table2, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);

  CDCStreamId stream_id = ASSERT_RESULT(CreateDBStream());
  auto set_resp = ASSERT_RESULT(SetCDCCheckpoint(stream_id, tablets, OpId::Min()));
  ASSERT_FALSE(set_resp.has_error());

  ASSERT_OK(conn.Execute("INSERT INTO test1 VALUES (1, 1)"));
  ASSERT_OK(conn.Execute("INSERT INTO test1 VALUES (2, 2)"));

  int queries_per_batch = 60;
  int num_batches = 60;
  std::thread t1([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches, queries_per_batch, "INSERT INTO test2 VALUES ($0, 1, 1)", 20);
  });
  std::thread t2([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches, queries_per_batch, "INSERT INTO test2 VALUES ($0, 1, 1)", 50,
        num_batches * queries_per_batch);
  });

  t1.join();
  t2.join();

  std::thread t3([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches, queries_per_batch, "UPDATE test2 SET test1_id=2 WHERE id = $0", 30);
  });
  std::thread t4([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches, queries_per_batch, "UPDATE test2 SET test1_id=2 WHERE id = $0", 50,
        num_batches * queries_per_batch);
  });

  t3.join();
  t4.join();

  ASSERT_OK(test_client()->FlushTables({table1.table_id()}, false, 1000, false));
  ASSERT_OK(test_client()->FlushTables({table2.table_id()}, false, 1000, false));

  // The count array stores counts of DDL, INSERT, UPDATE, DELETE, READ, TRUNCATE, BEGIN, COMMIT in
  // that order.
  const int expected_count[] = {
      1, queries_per_batch * num_batches * 2,       queries_per_batch * num_batches * 2,       0, 0,
      0, num_batches * (4 + 2 * queries_per_batch), num_batches * (4 + 2 * queries_per_batch),
  };
  int count[] = {0, 0, 0, 0, 0, 0, 0, 0};

  auto get_changes_resp = GetAllPendingChangesFromCdc(stream_id, tablets);
  for (auto record : get_changes_resp.records) {
    UpdateRecordCount(record, count);
  }

  CheckRecordsConsistency(get_changes_resp.records);

  LOG(INFO) << "Got " << get_changes_resp.records.size() << " records.";
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ(expected_count[i], count[i]);
  }
  ASSERT_EQ(29281, get_changes_resp.records.size());
}

TEST_F(CDCSDKYsqlTest, YB_DISABLE_TEST_IN_TSAN(TestCDCSDKConsistentStreamWithAbortedTransactions)) {
  FLAGS_cdc_max_stream_intent_records = 30;
  FLAGS_cdc_enable_consistent_records = false;
  ASSERT_OK(SetUpWithParams(3, 1, false));
  auto table = ASSERT_RESULT(CreateTable(&test_cluster_, kNamespaceName, kTableName));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);

  CDCStreamId stream_id = ASSERT_RESULT(CreateDBStream());
  auto set_resp = ASSERT_RESULT(SetCDCCheckpoint(stream_id, tablets, OpId::Min()));
  ASSERT_FALSE(set_resp.has_error());

  // COMMIT
  ASSERT_OK(WriteRowsHelper(1, 10, &test_cluster_, true));

  // ABORT
  ASSERT_OK(WriteRowsHelper(10, 20, &test_cluster_, false));

  // ABORT
  ASSERT_OK(WriteRowsHelper(20, 30, &test_cluster_, false));

  // COMMIT
  ASSERT_OK(WriteRowsHelper(30, 40, &test_cluster_, true));

  // ROLLBACK
  auto conn = ASSERT_RESULT(test_cluster_.ConnectToDB(kNamespaceName));
  ASSERT_OK(conn.Execute("BEGIN"));
  for (int i = 0; i < 10; i++) {
    ASSERT_OK(conn.ExecuteFormat("INSERT INTO test_table VALUES ($0, 1)", i + 40));
  }
  ASSERT_OK(conn.Execute("ROLLBACK"));

  // END
  ASSERT_OK(conn.Execute("BEGIN"));
  for (int i = 0; i < 10; i++) {
    ASSERT_OK(conn.ExecuteFormat("INSERT INTO test_table VALUES ($0, 1)", i + 50));
  }
  ASSERT_OK(conn.Execute("END"));

  // The count array stores counts of DDL, INSERT, UPDATE, DELETE, READ, TRUNCATE, BEGIN, COMMIT in
  // that order.
  const int expected_count[] = {
      1, 29, 0, 0, 0, 0, 3, 3,
  };
  int count[] = {0, 0, 0, 0, 0, 0, 0, 0};

  auto get_changes_resp = GetAllPendingChangesFromCdc(stream_id, tablets);
  for (auto record : get_changes_resp.records) {
    UpdateRecordCount(record, count);
  }

  CheckRecordsConsistency(get_changes_resp.records);

  LOG(INFO) << "Got " << get_changes_resp.records.size() << " records.";
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ(expected_count[i], count[i]);
  }
  ASSERT_EQ(36, get_changes_resp.records.size());
}

TEST_F(CDCSDKYsqlTest, YB_DISABLE_TEST_IN_TSAN(TestCDCSDKConsistentStreamWithTserverRestart)) {
  FLAGS_cdc_max_stream_intent_records = 100;

  ASSERT_OK(SetUpWithParams(3, 1, false));
  auto table = ASSERT_RESULT(CreateTable(&test_cluster_, kNamespaceName, kTableName));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);
  CDCStreamId stream_id = ASSERT_RESULT(CreateDBStream());
  auto set_resp = ASSERT_RESULT(SetCDCCheckpoint(stream_id, tablets, OpId::Min()));
  ASSERT_FALSE(set_resp.has_error());

  int num_batches = 75;
  int inserts_per_batch = 100;

  std::thread t1(
      [&]() -> void { PerformSingleAndMultiShardInserts(num_batches, inserts_per_batch, 20); });
  std::thread t2([&]() -> void {
    PerformSingleAndMultiShardInserts(
        num_batches, inserts_per_batch, 50, num_batches * inserts_per_batch);
  });

  t1.join();
  t2.join();

  auto conn = ASSERT_RESULT(test_cluster_.ConnectToDB(kNamespaceName));
  ASSERT_OK(conn.Execute("BEGIN"));
  for (int i = 0; i < 150; i++) {
    ASSERT_OK(conn.ExecuteFormat(
        "INSERT INTO test_table VALUES ($0, 1)", (2 * num_batches * inserts_per_batch) + i));
  }
  ASSERT_OK(conn.Execute("COMMIT"));

  std::thread t3([&]() -> void {
    PerformSingleAndMultiShardInserts(
        num_batches, inserts_per_batch, 20, (2 * num_batches * inserts_per_batch) + 150);
  });
  std::thread t4([&]() -> void {
    PerformSingleAndMultiShardInserts(
        num_batches, inserts_per_batch, 50, (3 * num_batches * inserts_per_batch) + 150);
  });

  t3.join();
  t4.join();

  ASSERT_OK(test_client()->FlushTables({table.table_id()}, false, 1000, false));

  // The count array stores counts of DDL, INSERT, UPDATE, DELETE, READ, TRUNCATE, BEGIN, COMMIT in
  // that order.
  const int expected_count[] = {
      2,
      4 * num_batches * inserts_per_batch + 150,
      0,
      0,
      0,
      0,
      4 * num_batches + 1 + 2 * num_batches * inserts_per_batch,
      4 * num_batches + 1 + 2 * num_batches * inserts_per_batch,
  };
  int count[] = {0, 0, 0, 0, 0, 0, 0, 0};

  auto get_changes_resp = ASSERT_RESULT(GetChangesFromCDC(stream_id, tablets));
  vector<CDCSDKProtoRecordPB> all_records;
  for (int32_t i = 0; i < get_changes_resp.cdc_sdk_proto_records_size(); i++) {
    auto record = get_changes_resp.cdc_sdk_proto_records(i);
    all_records.push_back(record);
    UpdateRecordCount(record, count);
  }

  // Restart all tservers.
  for (size_t i = 0; i < test_cluster()->num_tablet_servers(); ++i) {
    test_cluster()->mini_tablet_server(i)->Shutdown();
    ASSERT_OK(test_cluster()->mini_tablet_server(i)->Start());
  }
  SleepFor(MonoDelta::FromSeconds(60));

  auto all_pending_changes = GetAllPendingChangesFromCdc(
      stream_id,
      tablets,
      &get_changes_resp.cdc_sdk_checkpoint(),
      0,
      get_changes_resp.safe_hybrid_time());
  for (size_t i = 0; i < all_pending_changes.records.size(); i++) {
    auto record = all_pending_changes.records[i];
    all_records.push_back(record);
    UpdateRecordCount(record, count);
  }

  CheckRecordsConsistency(all_records);
  LOG(INFO) << "Got " << all_records.size() << " records.";
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ(expected_count[i], count[i]);
  }
  ASSERT_EQ(60754, all_records.size());
}

TEST_F(CDCSDKYsqlTest, YB_DISABLE_TEST_IN_TSAN(TestCDCSDKConsistentStreamWithDDLStatements)) {
  ASSERT_OK(SetUpWithParams(3, 1, false));
  auto table = ASSERT_RESULT(CreateTable(&test_cluster_, kNamespaceName, kTableName));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);
  CDCStreamId stream_id = ASSERT_RESULT(CreateDBStream());
  auto set_resp = ASSERT_RESULT(SetCDCCheckpoint(stream_id, tablets, OpId::Min()));
  ASSERT_FALSE(set_resp.has_error());

  int num_batches = 75;
  int inserts_per_batch = 100;

  std::thread t1(
      [&]() -> void { PerformSingleAndMultiShardInserts(num_batches, inserts_per_batch, 20); });
  std::thread t2([&]() -> void {
    PerformSingleAndMultiShardInserts(
        num_batches, inserts_per_batch, 50, num_batches * inserts_per_batch);
  });

  t1.join();
  t2.join();

  auto conn = ASSERT_RESULT(test_cluster_.ConnectToDB(kNamespaceName));
  ASSERT_OK(conn.Execute("ALTER TABLE test_table ADD value_2 int;"));
  ASSERT_OK(conn.Execute("ALTER TABLE test_table DROP value_1;"));

  std::thread t3([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches,
        inserts_per_batch,
        "INSERT INTO test_table VALUES ($0, 1)",
        20,
        (2 * num_batches * inserts_per_batch));
  });
  std::thread t4([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches,
        inserts_per_batch,
        "INSERT INTO test_table VALUES ($0, 1)",
        50,
        (3 * num_batches * inserts_per_batch));
  });

  t3.join();
  t4.join();

  ASSERT_OK(test_client()->FlushTables({table.table_id()}, false, 1000, false));

  // The count array stores counts of DDL, INSERT, UPDATE, DELETE, READ, TRUNCATE, BEGIN, COMMIT in
  // that order.
  const int expected_count[] = {
      3,
      4 * num_batches * inserts_per_batch,
      0,
      0,
      0,
      0,
      4 * num_batches + 2 * num_batches * inserts_per_batch,
      4 * num_batches + 2 * num_batches * inserts_per_batch,
  };
  int count[] = {0, 0, 0, 0, 0, 0, 0, 0};

  auto get_changes_resp = GetAllPendingChangesFromCdc(stream_id, tablets);
  for (size_t i = 0; i < get_changes_resp.records.size(); i++) {
    auto record = get_changes_resp.records[i];
    UpdateRecordCount(record, count);
  }

  CheckRecordsConsistency(get_changes_resp.records);
  LOG(INFO) << "Got " << get_changes_resp.records.size() << " records.";
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ(expected_count[i], count[i]);
  }
  ASSERT_EQ(60603, get_changes_resp.records.size());
}

TEST_F(CDCSDKYsqlTest, YB_DISABLE_TEST_IN_TSAN(TestCDCSDKConsistentStreamWithLeadershipChange)) {
  FLAGS_enable_load_balancing = false;

  ASSERT_OK(SetUpWithParams(3, 1, false));
  auto table = ASSERT_RESULT(CreateTable(&test_cluster_, kNamespaceName, kTableName));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);
  CDCStreamId stream_id = ASSERT_RESULT(CreateDBStream());
  auto set_resp = ASSERT_RESULT(SetCDCCheckpoint(stream_id, tablets, OpId::Min()));
  ASSERT_FALSE(set_resp.has_error());

  int num_batches = 75;
  int inserts_per_batch = 100;

  std::thread t1(
      [&]() -> void { PerformSingleAndMultiShardInserts(num_batches, inserts_per_batch, 20); });
  std::thread t2([&]() -> void {
    PerformSingleAndMultiShardInserts(
        num_batches, inserts_per_batch, 50, num_batches * inserts_per_batch);
  });

  t1.join();
  t2.join();

  auto conn = ASSERT_RESULT(test_cluster_.ConnectToDB(kNamespaceName));
  ASSERT_OK(conn.Execute("ALTER TABLE test_table ADD value_2 int;"));
  ASSERT_OK(conn.Execute("ALTER TABLE test_table DROP value_1;"));

  std::thread t3([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches,
        inserts_per_batch,
        "INSERT INTO test_table VALUES ($0, 1)",
        20,
        (2 * num_batches * inserts_per_batch));
  });
  std::thread t4([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches,
        inserts_per_batch,
        "INSERT INTO test_table VALUES ($0, 1)",
        50,
        (3 * num_batches * inserts_per_batch));
  });

  t3.join();
  t4.join();

  ASSERT_OK(test_client()->FlushTables({table.table_id()}, false, 1000, false));

  // The count array stores counts of DDL, INSERT, UPDATE, DELETE, READ, TRUNCATE, BEGIN, COMMIT in
  // that order.
  const int expected_count[] = {
      4,
      4 * num_batches * inserts_per_batch,
      0,
      0,
      0,
      0,
      4 * num_batches + 2 * num_batches * inserts_per_batch,
      4 * num_batches + 2 * num_batches * inserts_per_batch,
  };
  int count[] = {0, 0, 0, 0, 0, 0, 0, 0};

  size_t first_leader_index = 0;
  size_t first_follower_index = 0;
  GetTabletLeaderAndAnyFollowerIndex(tablets, &first_leader_index, &first_follower_index);

  auto get_changes_resp = ASSERT_RESULT(GetChangesFromCDC(stream_id, tablets));
  vector<CDCSDKProtoRecordPB> all_records;
  for (int32_t i = 0; i < get_changes_resp.cdc_sdk_proto_records_size(); i++) {
    auto record = get_changes_resp.cdc_sdk_proto_records(i);
    all_records.push_back(record);
    UpdateRecordCount(record, count);
  }

  // Leadership Change.
  ASSERT_OK(ChangeLeaderOfTablet(first_follower_index, tablets[0].tablet_id()));

  auto all_pending_changes = GetAllPendingChangesFromCdc(
      stream_id,
      tablets,
      &get_changes_resp.cdc_sdk_checkpoint(),
      0,
      get_changes_resp.safe_hybrid_time());
  for (size_t i = 0; i < all_pending_changes.records.size(); i++) {
    auto record = all_pending_changes.records[i];
    all_records.push_back(record);
    UpdateRecordCount(record, count);
  }

  CheckRecordsConsistency(all_records);
  LOG(INFO) << "Got " << all_records.size() << " records.";
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ(expected_count[i], count[i]);
  }
  ASSERT_EQ(60604, all_records.size());
}

TEST_F(CDCSDKYsqlTest, YB_DISABLE_TEST_IN_TSAN(TestCDCSDKConsistentStreamWithColocation)) {
  ASSERT_OK(SetUpWithParams(3, 1, false));

  auto conn = ASSERT_RESULT(test_cluster_.ConnectToDB(kNamespaceName));
  ASSERT_OK(conn.ExecuteFormat("CREATE TABLEGROUP tg1"));
  ASSERT_OK(
      conn.ExecuteFormat("CREATE TABLE test1(id int primary key, value_1 int) TABLEGROUP tg1;"));
  ASSERT_OK(
      conn.ExecuteFormat("CREATE TABLE test2(id int primary key, value_1 int) TABLEGROUP tg1;"));

  auto table1 = ASSERT_RESULT(GetTable(&test_cluster_, kNamespaceName, "test1"));
  auto table2 = ASSERT_RESULT(GetTable(&test_cluster_, kNamespaceName, "test2"));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table1, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);

  CDCStreamId stream_id = ASSERT_RESULT(CreateDBStream());
  auto set_resp = ASSERT_RESULT(SetCDCCheckpoint(stream_id, tablets, OpId::Min()));
  ASSERT_FALSE(set_resp.has_error());

  int num_batches = 50;
  int inserts_per_batch = 50;

  std::thread t1([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches, inserts_per_batch, "INSERT INTO test1 VALUES ($0, 1)", 20);
  });
  std::thread t2([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches,
        inserts_per_batch,
        "INSERT INTO test1 VALUES ($0, 1)",
        50,
        num_batches * inserts_per_batch);
  });
  std::thread t3([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches, inserts_per_batch, "INSERT INTO test2 VALUES ($0, 1)", 20);
  });
  std::thread t4([&]() -> void {
    PerformSingleAndMultiShardQueries(
        num_batches,
        inserts_per_batch,
        "INSERT INTO test2 VALUES ($0, 1)",
        50,
        num_batches * inserts_per_batch);
  });

  t1.join();
  t2.join();
  t3.join();
  t4.join();

  ASSERT_OK(test_client()->FlushTables({table1.table_id()}, false, 1000, false));
  ASSERT_OK(test_client()->FlushTables({table2.table_id()}, false, 1000, false));

  // The count array stores counts of DDL, INSERT, UPDATE, DELETE, READ, TRUNCATE, BEGIN, COMMIT in
  // that order.
  const int expected_count[] = {
      2,
      4 * num_batches * inserts_per_batch,
      0,
      0,
      0,
      0,
      8 * num_batches + 4 * num_batches * inserts_per_batch,
      8 * num_batches + 4 * num_batches * inserts_per_batch,
  };
  int count[] = {0, 0, 0, 0, 0, 0, 0, 0};

  auto get_changes_resp = GetAllPendingChangesFromCdc(stream_id, tablets);
  for (size_t i = 0; i < get_changes_resp.records.size(); i++) {
    auto record = get_changes_resp.records[i];
    UpdateRecordCount(record, count);
  }

  CheckRecordsConsistency(get_changes_resp.records);
  LOG(INFO) << "Got " << get_changes_resp.records.size() << " records.";
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ(expected_count[i], count[i]);
  }
  ASSERT_EQ(30802, get_changes_resp.records.size());
}

TEST_F(CDCSDKYsqlTest, YB_DISABLE_TEST_IN_TSAN(TestCDCSDKConsistentStreamWithTabletSplit)) {
  FLAGS_update_min_cdc_indices_interval_secs = 1;
  FLAGS_cdc_state_checkpoint_update_interval_ms = 0;
  FLAGS_aborted_intent_cleanup_ms = 1000;
  FLAGS_cdc_parent_tablet_deletion_task_retry_secs = 1;

  ASSERT_OK(SetUpWithParams(3, 1, false));
  auto table = ASSERT_RESULT(CreateTable(&test_cluster_, kNamespaceName, kTableName));
  TableId table_id = ASSERT_RESULT(GetTableId(&test_cluster_, kNamespaceName, kTableName));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);
  CDCStreamId stream_id = ASSERT_RESULT(CreateDBStream(IMPLICIT));
  auto set_resp = ASSERT_RESULT(SetCDCCheckpoint(stream_id, tablets, OpId::Min()));
  ASSERT_FALSE(set_resp.has_error());

  int num_batches = 75;
  int inserts_per_batch = 100;

  std::thread t1(
      [&]() -> void { PerformSingleAndMultiShardInserts(num_batches, inserts_per_batch, 20); });
  std::thread t2([&]() -> void {
    PerformSingleAndMultiShardInserts(
        num_batches, inserts_per_batch, 50, num_batches * inserts_per_batch);
  });

  t1.join();
  t2.join();

  ASSERT_OK(test_client()->FlushTables({table.table_id()}, false, 1000, true));
  ASSERT_OK(test_cluster_.mini_cluster_->CompactTablets());
  WaitUntilSplitIsSuccesful(tablets.Get(0).tablet_id(), table);

  std::thread t3([&]() -> void {
    PerformSingleAndMultiShardInserts(
        num_batches, inserts_per_batch, 20, (2 * num_batches * inserts_per_batch));
  });
  std::thread t4([&]() -> void {
    PerformSingleAndMultiShardInserts(
        num_batches, inserts_per_batch, 50, (3 * num_batches * inserts_per_batch));
  });

  t3.join();
  t4.join();

  ASSERT_OK(test_client()->FlushTables({table.table_id()}, false, 1000, true));
  ASSERT_OK(test_cluster_.mini_cluster_->CompactTablets());

  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets_after_first_split;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets_after_first_split, nullptr));
  ASSERT_EQ(tablets_after_first_split.size(), 2);

  // The count array stores counts of DDL, INSERT, UPDATE, DELETE, READ, TRUNCATE, BEGIN, COMMIT in
  // that order.
  const int expected_count_1[] = {
      1,
      2 * num_batches * inserts_per_batch,
      0,
      0,
      0,
      0,
      2 * num_batches + num_batches * inserts_per_batch,
      2 * num_batches + num_batches * inserts_per_batch,
  };
  const int expected_count_2[] = {3, 4 * num_batches * inserts_per_batch, 0, 0, 0, 0};
  int count[] = {0, 0, 0, 0, 0, 0, 0, 0};

  auto parent_get_changes = GetAllPendingChangesFromCdc(stream_id, tablets);
  for (size_t i = 0; i < parent_get_changes.records.size(); i++) {
    auto record = parent_get_changes.records[i];
    UpdateRecordCount(record, count);
  }

  CheckRecordsConsistency(parent_get_changes.records);
  for (int i = 0; i < 8; i++) {
    ASSERT_EQ(expected_count_1[i], count[i]);
  }

  // Wait until the 'cdc_parent_tablet_deletion_task_' has run.
  SleepFor(MonoDelta::FromSeconds(2));

  auto get_tablets_resp =
      ASSERT_RESULT(GetTabletListToPollForCDC(stream_id, table_id, tablets[0].tablet_id()));
  for (const auto& tablet_checkpoint_pair : get_tablets_resp.tablet_checkpoint_pairs()) {
    auto new_tablet = tablet_checkpoint_pair.tablet_locations();
    auto new_checkpoint = tablet_checkpoint_pair.cdc_sdk_checkpoint();

    google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
    auto tablet_ptr = tablets.Add();
    tablet_ptr->CopyFrom(new_tablet);

    auto child_get_changes = GetAllPendingChangesFromCdc(stream_id, tablets, &new_checkpoint);
    vector<CDCSDKProtoRecordPB> child_plus_parent = parent_get_changes.records;
    for (size_t i = 0; i < child_get_changes.records.size(); i++) {
      auto record = child_get_changes.records[i];
      child_plus_parent.push_back(record);
      UpdateRecordCount(record, count);
    }
    CheckRecordsConsistency(child_plus_parent);
  }

  for (int i = 0; i < 6; i++) {
    ASSERT_EQ(expected_count_2[i], count[i]);
  }
}

TEST_F(CDCSDKYsqlTest, YB_DISABLE_TEST_IN_TSAN(TestCDCSDKHistoricalMaxOpId)) {
  ASSERT_OK(SetUpWithParams(3, 1, false));
  auto table = ASSERT_RESULT(CreateTable(&test_cluster_, kNamespaceName, kTableName));
  TableId table_id = ASSERT_RESULT(GetTableId(&test_cluster_, kNamespaceName, kTableName));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);

  // Should be -1.-1 in the beginning.
  ASSERT_EQ(GetHistoricalMaxOpId(tablets), OpId::Invalid());

  // Aborted transactions shouldn't change max_op_id.
  ASSERT_OK(WriteRowsHelper(0, 100, &test_cluster_, false));
  SleepFor(MonoDelta::FromSeconds(5));
  ASSERT_EQ(GetHistoricalMaxOpId(tablets), OpId::Invalid());

  // Committed transactions should change max_op_id.
  ASSERT_OK(WriteRowsHelper(100, 200, &test_cluster_, true));
  OpId historical_max_op_id;
  ASSERT_OK(WaitFor(
      [&]() {
        historical_max_op_id = GetHistoricalMaxOpId(tablets);
        return historical_max_op_id > OpId::Invalid();
      },
      MonoDelta::FromSeconds(5),
      "historical_max_op_id should change"));

  // Aborted transactions shouldn't change max_op_id.
  ASSERT_OK(WriteRowsHelper(200, 300, &test_cluster_, false));
  SleepFor(MonoDelta::FromSeconds(5));
  OpId new_historical_max_op_id = GetHistoricalMaxOpId(tablets);
  ASSERT_EQ(new_historical_max_op_id, historical_max_op_id);

  // Committed transactions should change max_op_id.
  ASSERT_OK(WriteRowsHelper(300, 400, &test_cluster_, true));
  ASSERT_OK(WaitFor(
      [&]() {
        new_historical_max_op_id = GetHistoricalMaxOpId(tablets);
        return new_historical_max_op_id > historical_max_op_id;
      },
      MonoDelta::FromSeconds(5),
      "historical_max_op_id should change"));
}

TEST_F(CDCSDKYsqlTest, YB_DISABLE_TEST_IN_TSAN(TestCDCSDKHistoricalMaxOpIdWithTserverRestart)) {
  ASSERT_OK(SetUpWithParams(3, 1, false));
  auto table = ASSERT_RESULT(CreateTable(&test_cluster_, kNamespaceName, kTableName));
  TableId table_id = ASSERT_RESULT(GetTableId(&test_cluster_, kNamespaceName, kTableName));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);

  // Should be -1.-1 in the beginning.
  ASSERT_EQ(GetHistoricalMaxOpId(tablets), OpId::Invalid());

  // Committed transactions should change max_op_id.
  ASSERT_OK(WriteRowsHelper(0, 100, &test_cluster_, true));
  OpId historical_max_op_id;
  ASSERT_OK(WaitFor(
      [&]() {
        historical_max_op_id = GetHistoricalMaxOpId(tablets);
        return historical_max_op_id > OpId::Invalid();
      },
      MonoDelta::FromSeconds(5),
      "historical_max_op_id should change"));

  // Restart all tservers.
  for (size_t i = 0; i < test_cluster()->num_tablet_servers(); ++i) {
    test_cluster()->mini_tablet_server(i)->Shutdown();
    ASSERT_OK(test_cluster()->mini_tablet_server(i)->Start());
  }

  // Should be same as before restart.
  ASSERT_OK(WaitFor(
      [&]() { return GetHistoricalMaxOpId(tablets) == historical_max_op_id; },
      MonoDelta::FromSeconds(30),
      "historical_max_op_id should be same as before restart"));
}

TEST_F(
    CDCSDKYsqlTest,
    YB_DISABLE_TEST_IN_TSAN(TestCDCSDKHistoricalMaxOpIdTserverRestartWithFlushTables)) {
  ASSERT_OK(SetUpWithParams(3, 1, false));
  auto table = ASSERT_RESULT(CreateTable(&test_cluster_, kNamespaceName, kTableName));
  TableId table_id = ASSERT_RESULT(GetTableId(&test_cluster_, kNamespaceName, kTableName));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);

  // Flushed transactions are replayed only if there is a cdc stream.
  CDCStreamId stream_id = ASSERT_RESULT(CreateDBStream());
  auto set_resp = ASSERT_RESULT(SetCDCCheckpoint(stream_id, tablets, OpId::Min()));
  ASSERT_FALSE(set_resp.has_error());

  // Should be -1.-1 in the beginning.
  ASSERT_EQ(GetHistoricalMaxOpId(tablets), OpId::Invalid());

  // Committed transactions should change max_op_id.
  ASSERT_OK(WriteRowsHelper(0, 100, &test_cluster_, true));
  ASSERT_OK(test_client()->FlushTables({table.table_id()}, false, 1000, true));

  OpId historical_max_op_id;
  ASSERT_OK(WaitFor(
      [&]() {
        historical_max_op_id = GetHistoricalMaxOpId(tablets);
        return historical_max_op_id > OpId::Invalid();
      },
      MonoDelta::FromSeconds(5),
      "historical_max_op_id should change"));

  // Restart all tservers.
  for (size_t i = 0; i < test_cluster()->num_tablet_servers(); ++i) {
    test_cluster()->mini_tablet_server(i)->Shutdown();
    ASSERT_OK(test_cluster()->mini_tablet_server(i)->Start());
  }

  // Should be same as before restart.
  ASSERT_OK(WaitFor(
      [&]() { return GetHistoricalMaxOpId(tablets) == historical_max_op_id; },
      MonoDelta::FromSeconds(30),
      "historical_max_op_id should be same as before restart"));
}

TEST_F(CDCSDKYsqlTest, YB_DISABLE_TEST_IN_TSAN(TestCDCSDKHistoricalMaxOpIdWithTabletSplit)) {
  ASSERT_OK(SetUpWithParams(3, 1, false));
  auto table = ASSERT_RESULT(CreateTable(&test_cluster_, kNamespaceName, kTableName));
  TableId table_id = ASSERT_RESULT(GetTableId(&test_cluster_, kNamespaceName, kTableName));
  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets, nullptr));
  ASSERT_EQ(tablets.size(), 1);

  // Should be -1.-1 in the beginning.
  ASSERT_EQ(GetHistoricalMaxOpId(tablets), OpId::Invalid());

  // Committed transactions should change max_op_id.
  ASSERT_OK(WriteRowsHelper(0, 100, &test_cluster_, true));
  OpId historical_max_op_id;
  ASSERT_OK(WaitFor(
      [&]() {
        historical_max_op_id = GetHistoricalMaxOpId(tablets);
        return historical_max_op_id > OpId::Invalid();
      },
      MonoDelta::FromSeconds(5),
      "historical_max_op_id should change"));

  ASSERT_OK(test_client()->FlushTables({table.table_id()}, false, 1000, true));
  ASSERT_OK(test_cluster_.mini_cluster_->CompactTablets());
  WaitUntilSplitIsSuccesful(tablets.Get(0).tablet_id(), table);

  google::protobuf::RepeatedPtrField<master::TabletLocationsPB> tablets_after_first_split;
  ASSERT_OK(test_client()->GetTablets(table, 0, &tablets_after_first_split, nullptr));
  ASSERT_EQ(tablets_after_first_split.size(), 2);

  // Should be same as before split.
  OpId new_historical_max_op_id = GetHistoricalMaxOpId(tablets_after_first_split);
  ASSERT_EQ(new_historical_max_op_id, OpId::Invalid());
  new_historical_max_op_id = GetHistoricalMaxOpId(tablets_after_first_split, 1);
  ASSERT_EQ(new_historical_max_op_id, OpId::Invalid());

  ASSERT_OK(WriteRowsHelper(1000, 2000, &test_cluster_, true));
  ASSERT_OK(WaitFor(
      [&]() {
        return (GetHistoricalMaxOpId(tablets_after_first_split) > historical_max_op_id) &&
               (GetHistoricalMaxOpId(tablets_after_first_split, 1) > historical_max_op_id);
      },
      MonoDelta::FromSeconds(5),
      "historical_max_op_id should change"));
}

}  // namespace cdc
}  // namespace yb