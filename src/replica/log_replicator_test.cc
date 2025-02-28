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


#include "replica/log_replicator.h"
#include <brpc/server.h>
#include <gtest/gtest.h>
#include <sched.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include "base/glog_wapper.h"
#include "proto/tablet.pb.h"
#include "replica/replicate_node.h"
#include "storage/mem_table.h"
#include "storage/segment.h"
#include "storage/ticket.h"
#include "common/thread_pool.h"
#include "common/timer.h"

using ::baidu::common::ThreadPool;
using ::google::protobuf::Closure;
using ::google::protobuf::RpcController;
using ::fedb::storage::DataBlock;
using ::fedb::storage::MemTable;
using ::fedb::storage::Table;
using ::fedb::storage::TableIterator;
using ::fedb::storage::Ticket;

namespace fedb {
namespace replica {

const std::map<std::string, std::string> g_endpoints;

class MockTabletImpl : public ::fedb::api::TabletServer {
 public:
    MockTabletImpl(const ReplicatorRole& role, const std::string& path,
                   const std::map<std::string, std::string>& real_ep_map,
                   std::shared_ptr<MemTable> table)
        : role_(role),
          path_(path),
          real_ep_map_(real_ep_map),
          replicator_(path_, real_ep_map_, role_, table, &follower_) {}

    ~MockTabletImpl() {}
    bool Init() {
        // table_ = new Table("test", 1, 1, 8, 0, false, g_endpoints);
        // table_->Init();
        return replicator_.Init();
    }

    void Put(RpcController* controller, const ::fedb::api::PutRequest* request,
             ::fedb::api::PutResponse* response, Closure* done) {}

    void Scan(RpcController* controller,
              const ::fedb::api::ScanRequest* request,
              ::fedb::api::ScanResponse* response, Closure* done) {}

    void CreateTable(RpcController* controller,
                     const ::fedb::api::CreateTableRequest* request,
                     ::fedb::api::CreateTableResponse* response,
                     Closure* done) {}

    void DropTable(RpcController* controller,
                   const ::fedb::api::DropTableRequest* request,
                   ::fedb::api::DropTableResponse* response, Closure* done) {}

    void AppendEntries(RpcController* controller,
                       const ::fedb::api::AppendEntriesRequest* request,
                       ::fedb::api::AppendEntriesResponse* response,
                       Closure* done) {
        bool ok = replicator_.AppendEntries(request, response);
        if (ok) {
            PDLOG(INFO, "receive log entry from leader ok");
            response->set_code(0);
        } else {
            PDLOG(INFO, "receive log entry from leader error");
            response->set_code(1);
        }
        done->Run();
        replicator_.Notify();
    }

    void SetMode(bool follower) { follower_.store(follower); }

    bool GetMode() { return follower_.load(std::memory_order_relaxed); }

 private:
    ReplicatorRole role_;
    std::string path_;
    std::map<std::string, std::string> real_ep_map_;
    LogReplicator replicator_;
    std::atomic<bool> follower_;
};

bool ReceiveEntry(const ::fedb::api::LogEntry& entry) { return true; }

class LogReplicatorTest : public ::testing::Test {
 public:
    LogReplicatorTest() {}

    ~LogReplicatorTest() {}
};

inline std::string GenRand() { return std::to_string(rand() % 10000000 + 1); } // NOLINT

TEST_F(LogReplicatorTest, Init) {
    std::map<std::string, std::string> map;
    std::string folder = "/tmp/" + GenRand() + "/";
    std::map<std::string, uint32_t> mapping;
    std::atomic<bool> follower(false);
    mapping.insert(std::make_pair("idx", 0));
    std::shared_ptr<MemTable> table = std::make_shared<MemTable>(
        "test", 1, 1, 8, mapping, 0, ::fedb::api::TTLType::kAbsoluteTime);
    table->Init();
    LogReplicator replicator(folder, map, kLeaderNode, table, &follower);
    bool ok = replicator.Init();
    ASSERT_TRUE(ok);
}

TEST_F(LogReplicatorTest, BenchMark) {
    std::map<std::string, std::string> map;
    std::string folder = "/tmp/" + GenRand() + "/";
    std::map<std::string, uint32_t> mapping;
    std::atomic<bool> follower(false);
    mapping.insert(std::make_pair("idx", 0));
    std::shared_ptr<MemTable> table = std::make_shared<MemTable>(
        "test", 1, 1, 8, mapping, 0, ::fedb::api::TTLType::kAbsoluteTime);
    table->Init();
    LogReplicator replicator(folder, map, kLeaderNode, table, &follower);
    bool ok = replicator.Init();
    ::fedb::api::LogEntry entry;
    entry.set_term(1);
    entry.set_pk("test");
    entry.set_value("test");
    entry.set_ts(9527);
    ok = replicator.AppendEntry(entry);
    ASSERT_TRUE(ok);
}

TEST_F(LogReplicatorTest, LeaderAndFollowerMulti) {
    brpc::ServerOptions options;
    brpc::Server server0;
    brpc::Server server1;
    std::map<std::string, uint32_t> mapping;
    mapping.insert(std::make_pair("card", 0));
    mapping.insert(std::make_pair("merchant", 1));
    std::shared_ptr<MemTable> t7 = std::make_shared<MemTable>(
        "test", 1, 1, 8, mapping, 0, ::fedb::api::TTLType::kAbsoluteTime);
    t7->Init();
    {
        std::string follower_addr = "127.0.0.1:17527";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower =
            new MockTabletImpl(kFollowerNode, folder, g_endpoints, t7);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        if (server0.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server0.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }

    std::vector<std::string> endpoints;
    endpoints.push_back("127.0.0.1:17527");
    std::string folder = "/tmp/" + GenRand() + "/";
    std::atomic<bool> follower(false);
    LogReplicator leader(folder, g_endpoints, kLeaderNode, t7, &follower);
    bool ok = leader.Init();
    ASSERT_TRUE(ok);
    // put the first row
    {
        ::fedb::api::LogEntry entry;
        ::fedb::api::Dimension* d1 = entry.add_dimensions();
        d1->set_key("card0");
        d1->set_idx(0);
        ::fedb::api::Dimension* d2 = entry.add_dimensions();
        d2->set_key("merchant0");
        d2->set_idx(1);
        entry.set_ts(9527);
        entry.set_value("value 1");
        ok = leader.AppendEntry(entry);
        ASSERT_TRUE(ok);
    }
    // the second row
    {
        ::fedb::api::LogEntry entry;
        ::fedb::api::Dimension* d1 = entry.add_dimensions();
        d1->set_key("card1");
        d1->set_idx(0);
        ::fedb::api::Dimension* d2 = entry.add_dimensions();
        d2->set_key("merchant0");
        d2->set_idx(1);
        entry.set_ts(9526);
        entry.set_value("value 2");
        ok = leader.AppendEntry(entry);
        ASSERT_TRUE(ok);
    }
    // the third row
    {
        ::fedb::api::LogEntry entry;
        ::fedb::api::Dimension* d1 = entry.add_dimensions();
        d1->set_key("card0");
        d1->set_idx(0);
        entry.set_ts(9525);
        entry.set_value("value 3");
        ok = leader.AppendEntry(entry);
        ASSERT_TRUE(ok);
    }
    leader.Notify();
    std::map<std::string, std::string> map;
    map.insert(std::make_pair("127.0.0.1:17528", ""));
    leader.AddReplicateNode(map);
    sleep(2);

    std::shared_ptr<MemTable> t8 = std::make_shared<MemTable>(
        "test", 1, 1, 8, mapping, 0, ::fedb::api::TTLType::kAbsoluteTime);
    t8->Init();
    {
        std::string follower_addr = "127.0.0.1:17528";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower =
            new MockTabletImpl(kFollowerNode, folder, g_endpoints, t8);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        if (server1.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server1.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }
    sleep(20);
    leader.DelAllReplicateNode();
    ASSERT_EQ(3, (int64_t)t8->GetRecordCnt());
    ASSERT_EQ(5, (int64_t)t8->GetRecordIdxCnt());
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t8->NewIterator(0, "card0", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::fedb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value 1", value_str);
        ASSERT_EQ(9527, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value 3", value_str1);
        ASSERT_EQ(9525, (signed)it->GetKey());

        it->Next();
        ASSERT_FALSE(it->Valid());
    }
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t8->NewIterator(1, "merchant0", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::fedb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value 1", value_str);
        ASSERT_EQ(9527, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value 2", value_str1);
        ASSERT_EQ(9526, (signed)it->GetKey());

        it->Next();
        ASSERT_FALSE(it->Valid());
    }
}

TEST_F(LogReplicatorTest, LeaderAndFollower) {
    brpc::ServerOptions options;
    brpc::Server server0;
    brpc::Server server1;
    brpc::Server server2;
    std::map<std::string, uint32_t> mapping;
    mapping.insert(std::make_pair("idx", 0));
    std::shared_ptr<MemTable> t7 = std::make_shared<MemTable>(
        "test", 1, 1, 8, mapping, 0, ::fedb::api::TTLType::kAbsoluteTime);
    t7->Init();
    {
        std::string follower_addr = "127.0.0.1:18527";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower =
            new MockTabletImpl(kFollowerNode, folder, g_endpoints, t7);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        if (server0.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server0.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }

    std::vector<std::string> endpoints;
    endpoints.push_back("127.0.0.1:18527");
    std::string folder = "/tmp/" + GenRand() + "/";
    std::atomic<bool> follower(false);
    LogReplicator leader(folder, g_endpoints, kLeaderNode, t7, &follower);
    bool ok = leader.Init();
    ASSERT_TRUE(ok);
    ::fedb::api::LogEntry entry;
    entry.set_pk("test_pk");
    entry.set_value("value1");
    entry.set_ts(9527);
    ok = leader.AppendEntry(entry);
    entry.set_value("value2");
    entry.set_ts(9526);
    ok = leader.AppendEntry(entry);
    entry.set_value("value3");
    entry.set_ts(9525);
    ok = leader.AppendEntry(entry);
    entry.set_value("value4");
    entry.set_ts(9524);
    ok = leader.AppendEntry(entry);
    ASSERT_TRUE(ok);
    leader.Notify();
    std::map<std::string, std::string> map;
    map.insert(std::make_pair("127.0.0.1:18528", ""));
    leader.AddReplicateNode(map);
    map.clear();
    map.insert(std::make_pair("127.0.0.1:18529", ""));
    leader.AddReplicateNode(map, 2);
    sleep(2);

    std::shared_ptr<MemTable> t8 = std::make_shared<MemTable>(
        "test", 1, 1, 8, mapping, 0, ::fedb::api::TTLType::kAbsoluteTime);
    t8->Init();
    {
        std::string follower_addr = "127.0.0.1:18528";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower =
            new MockTabletImpl(kFollowerNode, folder, g_endpoints, t8);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        if (server1.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server1.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }
    std::shared_ptr<MemTable> t9 = std::make_shared<MemTable>(
        "test", 2, 1, 8, mapping, 0, ::fedb::api::TTLType::kAbsoluteTime);
    t9->Init();
    {
        std::string follower_addr = "127.0.0.1:18529";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower =
            new MockTabletImpl(kFollowerNode, folder, g_endpoints, t9);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        follower->SetMode(true);
        if (server2.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server2.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }
    sleep(20);
    int result = server1.Stop(10000);
    ASSERT_EQ(0, result);
    sleep(2);
    entry.Clear();
    entry.set_pk("test_pk");
    entry.set_value("value5");
    entry.set_ts(9523);
    ok = leader.AppendEntry(entry);
    ASSERT_TRUE(ok);
    leader.Notify();

    sleep(2);
    leader.DelAllReplicateNode();
    ASSERT_EQ(4, (signed)t8->GetRecordCnt());
    ASSERT_EQ(4, (signed)t8->GetRecordIdxCnt());
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t8->NewIterator("test_pk", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::fedb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value1", value_str);
        ASSERT_EQ(9527, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value2", value_str1);
        ASSERT_EQ(9526, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str2(value.data(), value.size());
        ASSERT_EQ("value3", value_str2);
        ASSERT_EQ(9525, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str3(value.data(), value.size());
        ASSERT_EQ("value4", value_str3);
        ASSERT_EQ(9524, (signed)it->GetKey());
    }
    ASSERT_EQ(4, (signed)t9->GetRecordCnt());
    ASSERT_EQ(4, (signed)t9->GetRecordIdxCnt());
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t9->NewIterator("test_pk", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::fedb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value1", value_str);
        ASSERT_EQ(9527, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value2", value_str1);
        ASSERT_EQ(9526, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str2(value.data(), value.size());
        ASSERT_EQ("value3", value_str2);
        ASSERT_EQ(9525, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str3(value.data(), value.size());
        ASSERT_EQ("value4", value_str3);
        ASSERT_EQ(9524, (signed)it->GetKey());
    }
}

TEST_F(LogReplicatorTest, Leader_Remove_local_follower) {
    brpc::ServerOptions options;
    brpc::Server server0;
    brpc::Server server1;
    brpc::Server server2;
    std::map<std::string, uint32_t> mapping;
    mapping.insert(std::make_pair("idx", 0));
    std::shared_ptr<MemTable> t7 = std::make_shared<MemTable>(
        "test", 1, 1, 8, mapping, 0, ::fedb::api::TTLType::kAbsoluteTime);
    t7->Init();
    {
        std::string follower_addr = "127.0.0.1:18527";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower =
            new MockTabletImpl(kFollowerNode, folder, g_endpoints, t7);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        if (server0.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server0.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }

    std::vector<std::string> endpoints;
    endpoints.push_back("127.0.0.1:18527");
    std::string folder = "/tmp/" + GenRand() + "/";
    std::atomic<bool> follower(false);
    LogReplicator leader(folder, g_endpoints, kLeaderNode, t7, &follower);
    bool ok = leader.Init();
    ASSERT_TRUE(ok);
    ::fedb::api::LogEntry entry;
    entry.set_pk("test_pk");
    entry.set_value("value1");
    entry.set_ts(9527);
    ok = leader.AppendEntry(entry);
    entry.set_value("value2");
    entry.set_ts(9526);
    ok = leader.AppendEntry(entry);
    entry.set_value("value3");
    entry.set_ts(9525);
    ok = leader.AppendEntry(entry);
    entry.set_value("value4");
    entry.set_ts(9524);
    ok = leader.AppendEntry(entry);
    ASSERT_TRUE(ok);
    leader.Notify();
    std::map<std::string, std::string> map;
    map.insert(std::make_pair("127.0.0.1:18528", ""));
    leader.AddReplicateNode(map);
    map.clear();
    map.insert(std::make_pair("127.0.0.1:18529", ""));
    leader.AddReplicateNode(map, 2);
    sleep(2);

    std::shared_ptr<MemTable> t8 = std::make_shared<MemTable>(
        "test", 1, 1, 8, mapping, 0, ::fedb::api::TTLType::kAbsoluteTime);
    t8->Init();
    {
        std::string follower_addr = "127.0.0.1:18528";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower =
            new MockTabletImpl(kFollowerNode, folder, g_endpoints, t8);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        if (server1.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server1.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }
    std::shared_ptr<MemTable> t9 = std::make_shared<MemTable>(
        "test", 2, 1, 8, mapping, 0, ::fedb::api::TTLType::kAbsoluteTime);
    t9->Init();
    {
        std::string follower_addr = "127.0.0.1:18529";
        std::string folder = "/tmp/" + GenRand() + "/";
        MockTabletImpl* follower =
            new MockTabletImpl(kFollowerNode, folder, g_endpoints, t9);
        bool ok = follower->Init();
        ASSERT_TRUE(ok);
        follower->SetMode(true);
        if (server2.AddService(follower, brpc::SERVER_OWNS_SERVICE) != 0) {
            ASSERT_TRUE(false);
        }
        if (server2.Start(follower_addr.c_str(), &options) != 0) {
            ASSERT_TRUE(false);
        }
        PDLOG(INFO, "start follower");
    }
    sleep(20);
    leader.DelReplicateNode("127.0.0.1:18528");
    int result = server1.Stop(10000);
    ASSERT_EQ(0, result);
    sleep(2);
    entry.Clear();
    entry.set_pk("test_pk");
    entry.set_value("value5");
    entry.set_ts(9523);
    ok = leader.AppendEntry(entry);
    ASSERT_TRUE(ok);
    leader.Notify();

    sleep(4);
    leader.DelAllReplicateNode();
    ASSERT_EQ(4, (signed)t8->GetRecordCnt());
    ASSERT_EQ(4, (signed)t8->GetRecordIdxCnt());
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t8->NewIterator("test_pk", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::fedb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value1", value_str);
        ASSERT_EQ(9527, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value2", value_str1);
        ASSERT_EQ(9526, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str2(value.data(), value.size());
        ASSERT_EQ("value3", value_str2);
        ASSERT_EQ(9525, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str3(value.data(), value.size());
        ASSERT_EQ("value4", value_str3);
        ASSERT_EQ(9524, (signed)it->GetKey());
    }
    ASSERT_EQ(5, (signed)t9->GetRecordCnt());
    ASSERT_EQ(5, (signed)t9->GetRecordIdxCnt());
    {
        Ticket ticket;
        // check 18527
        TableIterator* it = t9->NewIterator("test_pk", ticket);
        it->Seek(9527);
        ASSERT_TRUE(it->Valid());
        ::fedb::base::Slice value = it->GetValue();
        std::string value_str(value.data(), value.size());
        ASSERT_EQ("value1", value_str);
        ASSERT_EQ(9527, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str1(value.data(), value.size());
        ASSERT_EQ("value2", value_str1);
        ASSERT_EQ(9526, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str2(value.data(), value.size());
        ASSERT_EQ("value3", value_str2);
        ASSERT_EQ(9525, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str3(value.data(), value.size());
        ASSERT_EQ("value4", value_str3);
        ASSERT_EQ(9524, (signed)it->GetKey());

        it->Next();
        ASSERT_TRUE(it->Valid());
        value = it->GetValue();
        std::string value_str4(value.data(), value.size());
        ASSERT_EQ("value4", value_str3);
        ASSERT_EQ(9523, (signed)it->GetKey());
    }
}

}  // namespace replica
}  // namespace fedb

int main(int argc, char** argv) {
    srand(time(NULL));
    ::fedb::base::SetLogLevel(INFO);
    ::testing::InitGoogleTest(&argc, argv);
    int ok = RUN_ALL_TESTS();
    return ok;
}
