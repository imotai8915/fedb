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
DECLARE_int32(binlog_single_file_max_size);
DECLARE_int32(binlog_delete_interval);
DECLARE_int32(make_snapshot_threshold_offset);
DECLARE_string(snapshot_compression);

namespace fedb {
namespace replica {

class BinlogTest : public ::testing::Test {
 public:
    BinlogTest() {}

    ~BinlogTest() {}
};

TEST_F(BinlogTest, DeleteBinlog) {
    FLAGS_binlog_single_file_max_size = 1;
    FLAGS_binlog_delete_interval = 500;
    ::fedb::tablet::TabletImpl* tablet = new ::fedb::tablet::TabletImpl();
    tablet->Init("");
    int offset = FLAGS_make_snapshot_threshold_offset;
    FLAGS_make_snapshot_threshold_offset = 0;
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

    uint64_t cur_time = ::baidu::common::timer::get_micros() / 1000;
    int count = 1000;
    while (count) {
        char key[20];
        snprintf(key, sizeof(key), "testkey_%d", count);
        ret = client.Put(tid, pid, key, cur_time, std::string(10 * 1024, 'a'));
        count--;
    }
    ret = client.MakeSnapshot(tid, pid, 0);
    std::string binlog_path = FLAGS_db_root_path + "/2_123/binlog";
    std::vector<std::string> vec;
    ASSERT_TRUE(ret);
    for (int i = 0; i < 50; i++) {
        vec.clear();
        ::fedb::base::GetFileName(binlog_path, vec);
        if (vec.size() == 1) {
            break;
        }
        sleep(2);
    }
    vec.clear();
    ::fedb::base::GetFileName(binlog_path, vec);
    ASSERT_EQ(1, (int64_t)vec.size());
    std::string file_name = binlog_path + "/00000004.log";
    ASSERT_STREQ(file_name.c_str(), vec[0].c_str());
    FLAGS_make_snapshot_threshold_offset = offset;
}

}  // namespace replica
}  // namespace fedb

inline std::string GenRand() { return std::to_string(rand() % 10000000 + 1); } // NOLINT

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    srand(time(NULL));
    ::fedb::base::SetLogLevel(DEBUG);
    ::google::ParseCommandLineFlags(&argc, &argv, true);
    int ret = 0;
    std::vector<std::string> vec{"off", "zlib", "snappy"};
    for (size_t i = 0; i < vec.size(); i++) {
        std::cout << "compress type: " << vec[i] << std::endl;
        FLAGS_db_root_path = "/tmp/" + GenRand();
        FLAGS_snapshot_compression = vec[i];
        ret += RUN_ALL_TESTS();
    }
    return ret;
    // return RUN_ALL_TESTS();
}
