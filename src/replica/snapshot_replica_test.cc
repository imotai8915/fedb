/*
 * Copyright 2021 4Paradigm
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <brpc/server.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <sched.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "base/file_util.h"
#include "client/tablet_client.h"
#include "base/glog_wapper.h"
#include "proto/tablet.pb.h"
#include "replica/log_replicator.h"
#include "replica/replicate_node.h"
#include "storage/segment.h"
#include "storage/table.h"
#include "storage/ticket.h"
#include "tablet/tablet_impl.h"
#include "common/thread_pool.h"
#include "common/timer.h"

using ::baidu::common::ThreadPool;
using ::google::protobuf::Closure;
using ::google::protobuf::RpcController;
using ::fedb::storage::DataBlock;
using ::fedb::storage::Table;
using ::fedb::storage::Ticket;
using ::fedb::tablet::TabletImpl;

DECLARE_string(db_root_path);
DECLARE_string(endpoint);

inline std::string GenRand() { return std::to_string(rand() % 10000000 + 1); } // NOLINT

namespace fedb {
namespace replica {

class MockClosure : public ::google::protobuf::Closure {
 public:
    MockClosure() {}
    ~MockClosure() {}
    void Run() {}
};

class SnapshotReplicaTest : public ::testing::Test {
 public:
    SnapshotReplicaTest() {}

    ~SnapshotReplicaTest() {}
};

TEST_F(SnapshotReplicaTest, AddReplicate) {
    ::fedb::tablet::TabletImpl* tablet = new ::fedb::tablet::TabletImpl();
    tablet->Init("");
    brpc::Server server;
    if (server.AddService(tablet, brpc::SERVER_OWNS_SERVICE) != 0) {
        PDLOG(WARNING, "fail to register tablet rpc service");
        exit(1);
    }
    brpc::ServerOptions options;
    std::string leader_point = "127.0.0.1:18529";
    if (server.Start(leader_point.c_str(), &options) != 0) {
        PDLOG(WARNING, "fail to start server %s", leader_point.c_str());
        exit(1);
    }

    uint32_t tid = 2;
    uint32_t pid = 123;

    ::fedb::client::TabletClient client(leader_point, "");
    client.Init();
    std::vector<std::string> endpoints;
    bool ret =
        client.CreateTable("table1", tid, pid, 100000, 0, true, endpoints,
                           ::fedb::api::TTLType::kAbsoluteTime, 16, 0,
                           ::fedb::api::CompressType::kNoCompress);
    ASSERT_TRUE(ret);

    std::string end_point = "127.0.0.1:18530";
    ret = client.AddReplica(tid, pid, end_point);
    ASSERT_TRUE(ret);
    sleep(1);

    ::fedb::api::TableStatus table_status;
    ASSERT_TRUE(client.GetTableStatus(tid, pid, table_status));
    ASSERT_EQ(::fedb::api::kTableNormal, table_status.state());

    ret = client.DelReplica(tid, pid, end_point);
    ASSERT_TRUE(ret);
}

TEST_F(SnapshotReplicaTest, LeaderAndFollower) {
    ::fedb::tablet::TabletImpl* tablet = new ::fedb::tablet::TabletImpl();
    tablet->Init("");
    brpc::Server server;
    if (server.AddService(tablet, brpc::SERVER_OWNS_SERVICE) != 0) {
        PDLOG(WARNING, "fail to register tablet rpc service");
        exit(1);
    }
    brpc::ServerOptions options;
    std::string leader_point = "127.0.0.1:18529";
    if (server.Start(leader_point.c_str(), &options) != 0) {
        PDLOG(WARNING, "fail to start server %s", leader_point.c_str());
        exit(1);
    }
    // server.RunUntilAskedToQuit();

    uint32_t tid = 1;
    uint32_t pid = 123;

    ::fedb::client::TabletClient client(leader_point, "");
    client.Init();
    std::vector<std::string> endpoints;
    bool ret =
        client.CreateTable("table1", tid, pid, 100000, 0, true, endpoints,
                           ::fedb::api::TTLType::kAbsoluteTime, 16, 0,
                           ::fedb::api::CompressType::kNoCompress);
    ASSERT_TRUE(ret);
    uint64_t cur_time = ::baidu::common::timer::get_micros() / 1000;
    ret = client.Put(tid, pid, "testkey", cur_time, "value1");
    ASSERT_TRUE(ret);

    uint32_t count = 0;
    while (count < 10) {
        count++;
        char key[100];
        snprintf(key, sizeof(key), "test%u", count);
        client.Put(tid, pid, key, cur_time, key);
    }

    FLAGS_db_root_path = "/tmp/" + ::GenRand();
    FLAGS_endpoint = "127.0.0.1:18530";
    ::fedb::tablet::TabletImpl* tablet1 = new ::fedb::tablet::TabletImpl();
    tablet1->Init("");
    brpc::Server server1;
    if (server1.AddService(tablet1, brpc::SERVER_OWNS_SERVICE) != 0) {
        PDLOG(WARNING, "fail to register tablet rpc service");
        exit(1);
    }
    std::string follower_point = "127.0.0.1:18530";
    if (server1.Start(follower_point.c_str(), &options) != 0) {
        PDLOG(WARNING, "fail to start server %s", follower_point.c_str());
        exit(1);
    }
    // server.RunUntilAskedToQuit();
    ::fedb::client::TabletClient client1(follower_point, "");
    client1.Init();
    ret = client1.CreateTable("table1", tid, pid, 14400, 0, false, endpoints,
                              ::fedb::api::TTLType::kAbsoluteTime, 16, 0,
                              ::fedb::api::CompressType::kNoCompress);
    ASSERT_TRUE(ret);
    client.AddReplica(tid, pid, follower_point);
    sleep(3);

    ::fedb::api::ScanRequest sr;
    MockClosure closure;
    sr.set_tid(tid);
    sr.set_pid(pid);
    sr.set_pk("testkey");
    sr.set_st(cur_time + 1);
    sr.set_et(cur_time - 1);
    sr.set_limit(10);
    ::fedb::api::ScanResponse srp;
    tablet1->Scan(NULL, &sr, &srp, &closure);
    ASSERT_EQ(1, (int64_t)srp.count());
    ASSERT_EQ(0, srp.code());

    ret = client.Put(tid, pid, "newkey", cur_time, "value2");
    ASSERT_TRUE(ret);
    sleep(2);
    sr.set_pk("newkey");
    tablet1->Scan(NULL, &sr, &srp, &closure);
    ASSERT_EQ(1, (int64_t)srp.count());
    ASSERT_EQ(0, srp.code());
    {
        ::fedb::api::GetTableFollowerRequest gr;
        ::fedb::api::GetTableFollowerResponse grs;
        gr.set_tid(tid);
        gr.set_pid(pid);
        tablet->GetTableFollower(NULL, &gr, &grs, &closure);
        ASSERT_EQ(0, grs.code());
        ASSERT_EQ(12, (int64_t)grs.offset());
        ASSERT_EQ(1, grs.follower_info_size());
        ASSERT_STREQ(follower_point.c_str(),
                     grs.follower_info(0).endpoint().c_str());
        ASSERT_EQ(12, (int64_t)grs.follower_info(0).offset());
    }
    ::fedb::api::DropTableRequest dr;
    dr.set_tid(tid);
    dr.set_pid(pid);
    ::fedb::api::DropTableResponse drs;
    tablet->DropTable(NULL, &dr, &drs, &closure);
    sleep(2);
}

TEST_F(SnapshotReplicaTest, LeaderAndFollowerTS) {
    ::fedb::tablet::TabletImpl* tablet = new ::fedb::tablet::TabletImpl();
    tablet->Init("");
    brpc::Server server;
    if (server.AddService(tablet, brpc::SERVER_OWNS_SERVICE) != 0) {
        PDLOG(WARNING, "fail to register tablet rpc service");
        exit(1);
    }
    brpc::ServerOptions options;
    std::string leader_point = "127.0.0.1:18529";
    if (server.Start(leader_point.c_str(), &options) != 0) {
        PDLOG(WARNING, "fail to start server %s", leader_point.c_str());
        exit(1);
    }
    uint32_t tid = 1;
    uint32_t pid = 123;
    ::fedb::client::TabletClient client(leader_point, "");
    client.Init();
    ::fedb::api::TableMeta table_meta;
    table_meta.set_name("test");
    table_meta.set_tid(tid);
    table_meta.set_pid(pid);
    table_meta.set_ttl(0);
    table_meta.set_seg_cnt(8);
    ::fedb::common::ColumnDesc* column_desc1 = table_meta.add_column_desc();
    column_desc1->set_name("card");
    column_desc1->set_type("string");
    ::fedb::common::ColumnDesc* column_desc2 = table_meta.add_column_desc();
    column_desc2->set_name("mcc");
    column_desc2->set_type("string");
    ::fedb::common::ColumnDesc* column_desc3 = table_meta.add_column_desc();
    column_desc3->set_name("amt");
    column_desc3->set_type("double");
    ::fedb::common::ColumnDesc* column_desc4 = table_meta.add_column_desc();
    column_desc4->set_name("ts1");
    column_desc4->set_type("int64");
    column_desc4->set_is_ts_col(true);
    ::fedb::common::ColumnDesc* column_desc5 = table_meta.add_column_desc();
    column_desc5->set_name("ts2");
    column_desc5->set_type("int64");
    column_desc5->set_is_ts_col(true);
    ::fedb::common::ColumnKey* column_key1 = table_meta.add_column_key();
    column_key1->set_index_name("card");
    column_key1->add_ts_name("ts1");
    column_key1->add_ts_name("ts2");
    ::fedb::common::ColumnKey* column_key2 = table_meta.add_column_key();
    column_key2->set_index_name("mcc");
    column_key2->add_ts_name("ts1");
    table_meta.set_mode(::fedb::api::TableMode::kTableLeader);
    bool ret = client.CreateTable(table_meta);
    ASSERT_TRUE(ret);
    uint64_t cur_time = ::baidu::common::timer::get_micros() / 1000;
    std::vector<std::pair<std::string, uint32_t>> dimensions;
    dimensions.push_back(std::make_pair("card0", 0));
    dimensions.push_back(std::make_pair("mcc0", 1));
    std::vector<uint64_t> ts_dimensions = {cur_time, cur_time - 100};
    ret = client.Put(tid, pid, dimensions, ts_dimensions, "value0");
    ASSERT_TRUE(ret);

    FLAGS_db_root_path = "/tmp/" + ::GenRand();
    FLAGS_endpoint = "127.0.0.1:18530";
    ::fedb::tablet::TabletImpl* tablet1 = new ::fedb::tablet::TabletImpl();
    tablet1->Init("");
    brpc::Server server1;
    if (server1.AddService(tablet1, brpc::SERVER_OWNS_SERVICE) != 0) {
        PDLOG(WARNING, "fail to register tablet rpc service");
        exit(1);
    }
    std::string follower_point = "127.0.0.1:18530";
    if (server1.Start(follower_point.c_str(), &options) != 0) {
        PDLOG(WARNING, "fail to start server %s", follower_point.c_str());
        exit(1);
    }
    ::fedb::client::TabletClient client1(follower_point, "");
    client1.Init();
    table_meta.set_mode(::fedb::api::TableMode::kTableFollower);
    ret = client1.CreateTable(table_meta);
    ASSERT_TRUE(ret);
    client.AddReplica(tid, pid, follower_point);
    sleep(3);

    ::fedb::api::ScanRequest sr;
    MockClosure closure;
    sr.set_tid(tid);
    sr.set_pid(pid);
    sr.set_pk("card0");
    sr.set_idx_name("card");
    sr.set_ts_name("ts2");
    sr.set_st(cur_time + 1);
    sr.set_et(0);
    ::fedb::api::ScanResponse srp;
    tablet1->Scan(NULL, &sr, &srp, &closure);
    ASSERT_EQ(1, (int64_t)srp.count());
    ASSERT_EQ(0, srp.code());

    ::fedb::api::DropTableRequest dr;
    dr.set_tid(tid);
    dr.set_pid(pid);
    ::fedb::api::DropTableResponse drs;
    tablet->DropTable(NULL, &dr, &drs, &closure);
    tablet1->DropTable(NULL, &dr, &drs, &closure);
    sleep(1);
}

}  // namespace replica
}  // namespace fedb

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    srand(time(NULL));
    ::fedb::base::SetLogLevel(INFO);
    ::google::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_db_root_path = "/tmp/" + ::GenRand();
    return RUN_ALL_TESTS();
}
