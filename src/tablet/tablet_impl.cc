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


#include "tablet/tablet_impl.h"

#include <gflags/gflags.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef DISALLOW_COPY_AND_ASSIGN
#undef DISALLOW_COPY_AND_ASSIGN
#endif
#include <snappy.h>
#include <algorithm>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#include "boost/bind.hpp"
#include "boost/container/deque.hpp"
#include "config.h"  // NOLINT
#ifdef TCMALLOC_ENABLE
#include "gperftools/malloc_extension.h"
#endif
#include "base/file_util.h"
#include "base/glog_wapper.h"
#include "base/hash.h"
#include "base/status.h"
#include "base/strings.h"
#include "brpc/controller.h"
#include "butil/iobuf.h"
#include "codec/codec.h"
#include "glog/logging.h"
#include "storage/binlog.h"
#include "storage/segment.h"
#include "tablet/file_sender.h"
#include "common/timer.h"
#include "codec/sql_rpc_row_codec.h"
#include "codec/row_codec.h"

using google::protobuf::RepeatedPtrField;
using ::fedb::storage::DataBlock;
using ::fedb::storage::Table;
using ::fedb::base::ReturnCode;
using ::fedb::codec::SchemaCodec;

DECLARE_int32(gc_interval);
DECLARE_int32(gc_pool_size);
DECLARE_int32(statdb_ttl);
DECLARE_uint32(scan_max_bytes_size);
DECLARE_uint32(scan_reserve_size);
DECLARE_double(mem_release_rate);
DECLARE_string(db_root_path);
DECLARE_bool(binlog_notify_on_put);
DECLARE_int32(task_pool_size);
DECLARE_int32(io_pool_size);
DECLARE_int32(make_snapshot_time);
DECLARE_int32(make_snapshot_check_interval);
DECLARE_uint32(make_snapshot_offline_interval);
DECLARE_bool(recycle_bin_enabled);
DECLARE_uint32(recycle_ttl);
DECLARE_string(recycle_bin_root_path);
DECLARE_int32(make_snapshot_threshold_offset);
DECLARE_uint32(get_table_diskused_interval);
DECLARE_uint32(task_check_interval);
DECLARE_uint32(load_index_max_wait_time);
DECLARE_bool(use_name);
DECLARE_bool(enable_distsql);
DECLARE_string(snapshot_compression);
DECLARE_string(file_compression);

// cluster config
DECLARE_string(endpoint);
DECLARE_string(zk_cluster);
DECLARE_string(zk_root_path);
DECLARE_int32(zk_session_timeout);
DECLARE_int32(zk_keep_alive_check_interval);

DECLARE_int32(binlog_sync_to_disk_interval);
DECLARE_int32(binlog_delete_interval);
DECLARE_uint32(absolute_ttl_max);
DECLARE_uint32(latest_ttl_max);
DECLARE_uint32(max_traverse_cnt);
DECLARE_uint32(snapshot_ttl_time);
DECLARE_uint32(snapshot_ttl_check_interval);
DECLARE_uint32(put_slow_log_threshold);
DECLARE_uint32(query_slow_log_threshold);
DECLARE_int32(snapshot_pool_size);

namespace fedb {
namespace tablet {

static const std::string SERVER_CONCURRENCY_KEY = "server";  // NOLINT
static const uint32_t SEED = 0xe17a1465;

TabletImpl::TabletImpl()
    : tables_(),
      mu_(),
      gc_pool_(FLAGS_gc_pool_size),
      replicators_(),
      snapshots_(),
      zk_client_(NULL),
      keep_alive_pool_(1),
      task_pool_(FLAGS_task_pool_size),
      io_pool_(FLAGS_io_pool_size),
      snapshot_pool_(FLAGS_snapshot_pool_size),
      server_(NULL),
      mode_root_paths_(),
      mode_recycle_root_paths_(),
      follower_(false),
      catalog_(new ::fedb::catalog::TabletCatalog()),
      engine_(),
      zk_cluster_(),
      zk_path_(),
      endpoint_(),
      sp_cache_(std::shared_ptr<SpCache>(new SpCache())),
      notify_path_() {
}

TabletImpl::~TabletImpl() {
    task_pool_.Stop(true);
    keep_alive_pool_.Stop(true);
    gc_pool_.Stop(true);
    io_pool_.Stop(true);
    snapshot_pool_.Stop(true);
    delete zk_client_;
}

bool TabletImpl::Init(const std::string& real_endpoint) {
    return Init(FLAGS_zk_cluster, FLAGS_zk_root_path,
            FLAGS_endpoint, real_endpoint);
}

bool TabletImpl::Init(const std::string& zk_cluster, const std::string& zk_path,
        const std::string& endpoint, const std::string& real_endpoint) {
    ::hybridse::vm::EngineOptions options;
    options.set_cluster_optimized(FLAGS_enable_distsql);
    engine_ = std::unique_ptr<::hybridse::vm::Engine>(new ::hybridse::vm::Engine(catalog_, options));
    catalog_->SetLocalTablet(std::shared_ptr<::hybridse::vm::Tablet>(
        new ::hybridse::vm::LocalTablet(engine_.get(), sp_cache_)));
    std::set<std::string> snapshot_compression_set{"off", "zlib", "snappy"};
    if (snapshot_compression_set.find(FLAGS_snapshot_compression) == snapshot_compression_set.end()) {
        LOG(WARNING) << "wrong snapshot_compression: " << FLAGS_snapshot_compression;
        return false;
    }
    std::set<std::string> file_compression_set{"off", "zlib", "lz4"};
    if (file_compression_set.find(FLAGS_file_compression) == file_compression_set.end()) {
        LOG(WARNING) << "wrong FLAGS_file_compression: " << FLAGS_file_compression;
        return false;
    }
    zk_cluster_ = zk_cluster;
    zk_path_ = zk_path;
    endpoint_ = endpoint;
    notify_path_ = zk_path + "/table/notify";
    sp_root_path_ = zk_path + "/store_procedure/db_sp_data";
    std::lock_guard<std::mutex> lock(mu_);
    ::fedb::base::SplitString(FLAGS_db_root_path, ",", mode_root_paths_);

    ::fedb::base::SplitString(
        FLAGS_recycle_bin_root_path, ",", mode_recycle_root_paths_);
    if (!zk_cluster.empty()) {
        zk_client_ = new ZkClient(zk_cluster, real_endpoint,
                FLAGS_zk_session_timeout, endpoint, zk_path);
        bool ok = zk_client_->Init();
        if (!ok) {
            PDLOG(WARNING, "fail to init zookeeper with cluster %s",
                  zk_cluster.c_str());
            return false;
        }
    } else {
        PDLOG(INFO, "zk cluster disabled");
    }

    if (FLAGS_make_snapshot_time < 0 || FLAGS_make_snapshot_time > 23) {
        PDLOG(WARNING, "make_snapshot_time[%d] is illegal.",
              FLAGS_make_snapshot_time);
        return false;
    }

    if (!CreateMultiDir(mode_root_paths_)) {
        PDLOG(WARNING, "fail to create db root path %s",
              FLAGS_db_root_path.c_str());
        return false;
    }

    if (!CreateMultiDir(mode_recycle_root_paths_)) {
        PDLOG(WARNING, "fail to create recycle bin root path %s",
              FLAGS_recycle_bin_root_path.c_str());
        return false;
    }

    std::map<std::string, std::string> real_endpoint_map = { {endpoint, real_endpoint} };
    if (!catalog_->UpdateClient(real_endpoint_map)) {
        PDLOG(WARNING, "update client failed");
        return false;
    }

    snapshot_pool_.DelayTask(FLAGS_make_snapshot_check_interval,
                             boost::bind(&TabletImpl::SchedMakeSnapshot, this));
    task_pool_.AddTask(boost::bind(&TabletImpl::GetDiskused, this));
    if (FLAGS_recycle_ttl != 0) {
        task_pool_.DelayTask(FLAGS_recycle_ttl * 60 * 1000,
                             boost::bind(&TabletImpl::SchedDelRecycle, this));
    }
#ifdef TCMALLOC_ENABLE
    MallocExtension* tcmalloc = MallocExtension::instance();
    tcmalloc->SetMemoryReleaseRate(FLAGS_mem_release_rate);
#endif
    return true;
}

void TabletImpl::UpdateTTL(RpcController* ctrl,
                           const ::fedb::api::UpdateTTLRequest* request,
                           ::fedb::api::UpdateTTLResponse* response,
                           Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());

    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
              request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    ::fedb::api::TTLType ttl_type = request->type();
    uint64_t abs_ttl = 0;
    uint64_t lat_ttl = 0;
    if (request->has_ttl_desc()) {
        ttl_type = request->ttl_desc().ttl_type();
        abs_ttl = request->ttl_desc().abs_ttl();
        lat_ttl = request->ttl_desc().lat_ttl();
    } else if (request->has_value()) {
        if (ttl_type == ::fedb::api::TTLType::kAbsoluteTime) {
            abs_ttl = request->value();
            lat_ttl = 0;
        } else {
            abs_ttl = 0;
            lat_ttl = request->value();
        }
    }
    ::fedb::storage::TTLSt ttl_st(abs_ttl * 60 * 1000, lat_ttl, ::fedb::storage::TTLSt::ConvertTTLType(ttl_type));
    if (ttl_st.ttl_type != table->GetTTL().ttl_type) {
        response->set_code(::fedb::base::ReturnCode::kTtlTypeMismatch);
        response->set_msg("ttl type mismatch");
        PDLOG(WARNING, "ttl type mismatch. tid %u, pid %u", request->tid(), request->pid());
        return;
    }
    if (abs_ttl > FLAGS_absolute_ttl_max || lat_ttl > FLAGS_latest_ttl_max) {
        response->set_code(
            ::fedb::base::ReturnCode::kTtlIsGreaterThanConfValue);
        response->set_msg("ttl is greater than conf value. max abs_ttl is " +
                          std::to_string(FLAGS_absolute_ttl_max) +
                          ", max lat_ttl is " +
                          std::to_string(FLAGS_latest_ttl_max));
        PDLOG(WARNING,
              "ttl is greater than conf value. abs_ttl[%lu] lat_ttl[%lu] "
              "ttl_type[%s] max abs_ttl[%u] max lat_ttl[%u]",
              abs_ttl, lat_ttl, ::fedb::api::TTLType_Name(ttl_type).c_str(),
              FLAGS_absolute_ttl_max, FLAGS_latest_ttl_max);
        return;
    }
    if (request->has_ts_name() && request->ts_name().size() > 0) {
        auto iter = table->GetTSMapping().find(request->ts_name());
        if (iter == table->GetTSMapping().end()) {
            PDLOG(WARNING, "ts name %s not found in table tid %u, pid %u",
                  request->ts_name().c_str(), request->tid(), request->pid());
            response->set_code(::fedb::base::ReturnCode::kTsNameNotFound);
            response->set_msg("ts name not found");
            return;
        }
        table->SetTTL(::fedb::storage::UpdateTTLMeta(ttl_st, iter->second));
        PDLOG(INFO,
              "update table #tid %d #pid %d ttl to abs_ttl %lu lat_ttl %lu, "
              "ts_name %s",
              request->tid(), request->pid(), abs_ttl, lat_ttl,
              request->ts_name().c_str());
    } else if (!table->GetTSMapping().size()) {
        table->SetTTL(::fedb::storage::UpdateTTLMeta(ttl_st));
        PDLOG(INFO,
              "update table #tid %d #pid %d ttl to abs_ttl %lu lat_ttl %lu",
              request->tid(), request->pid(), abs_ttl, lat_ttl);
    } else {
        PDLOG(WARNING, "set ttl without ts name,  table tid %u, pid %u",
              request->tid(), request->pid());
        response->set_code(::fedb::base::ReturnCode::kTsNameNotFound);
        response->set_msg("set ttl need to specify ts column");
        return;
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

bool TabletImpl::RegisterZK() {
    if (!zk_cluster_.empty()) {
        if (FLAGS_use_name) {
            if (!zk_client_->RegisterName()) {
                return false;
            }
        }
        if (!zk_client_->Register(true)) {
            PDLOG(WARNING, "fail to register tablet with endpoint %s",
                  endpoint_.c_str());
            return false;
        }
        PDLOG(INFO, "tablet with endpoint %s register to zk cluster %s ok",
              endpoint_.c_str(), zk_cluster_.c_str());
        if (zk_client_->IsExistNode(notify_path_) != 0) {
            zk_client_->CreateNode(notify_path_, "1");
        }
        if (!zk_client_->WatchItem(notify_path_, boost::bind(&TabletImpl::RefreshTableInfo, this))) {
            LOG(WARNING) << "add notify watcher failed";
            return false;
        }
        keep_alive_pool_.DelayTask(
            FLAGS_zk_keep_alive_check_interval,
            boost::bind(&TabletImpl::CheckZkClient, this));
    }
    return true;
}

bool TabletImpl::CheckGetDone(::fedb::api::GetType type, uint64_t ts,
                              uint64_t target_ts) {
    switch (type) {
        case fedb::api::GetType::kSubKeyEq:
            if (ts == target_ts) {
                return true;
            }
            break;
        case fedb::api::GetType::kSubKeyLe:
            if (ts <= target_ts) {
                return true;
            }
            break;
        case fedb::api::GetType::kSubKeyLt:
            if (ts < target_ts) {
                return true;
            }
            break;
        case fedb::api::GetType::kSubKeyGe:
            if (ts >= target_ts) {
                return true;
            }
            break;
        case fedb::api::GetType::kSubKeyGt:
            if (ts > target_ts) {
                return true;
            }
    }
    return false;
}

int32_t TabletImpl::GetIndex(const ::fedb::api::GetRequest* request,
                             const ::fedb::api::TableMeta& meta,
                             const std::map<int32_t, std::shared_ptr<Schema>>& vers_schema,
                             CombineIterator* it, std::string* value,
                             uint64_t* ts) {
    if (it == nullptr || value == nullptr || ts == nullptr) {
        PDLOG(WARNING, "invalid args");
        return -1;
    }
    uint64_t st = request->ts();
    fedb::api::GetType st_type = request->type();
    uint64_t et = request->et();
    const fedb::api::GetType& et_type = request->et_type();
    if (st_type == ::fedb::api::kSubKeyEq &&
        et_type == ::fedb::api::kSubKeyEq && st != et) {
        return -1;
    }
    ::fedb::api::GetType real_et_type = et_type;
    ::fedb::storage::TTLType ttl_type = it->GetTTLType();
    uint64_t expire_time = it->GetExpireTime();
    if (ttl_type == ::fedb::storage::TTLType::kAbsoluteTime ||
        ttl_type == ::fedb::storage::TTLType::kAbsOrLat) {
        et = std::max(et, expire_time);
    }
    if (et < expire_time && et_type == ::fedb::api::GetType::kSubKeyGt) {
        real_et_type = ::fedb::api::GetType::kSubKeyGe;
    }
    bool enable_project = false;
    fedb::codec::RowProject row_project(vers_schema, request->projection());
    if (request->projection().size() > 0 && meta.format_version() == 1) {
        if (meta.compress_type() == api::kSnappy) {
            return -1;
        }
        bool ok = row_project.Init();
        if (!ok) {
            PDLOG(WARNING, "invalid project list");
            return -1;
        }
        enable_project = true;
    }
    if (st > 0 && st < et) {
        DEBUGLOG("invalid args for st %lu less than et %lu or expire time %lu",
                 st, et, expire_time);
        return -1;
    }
    if (it->Valid()) {
        *ts = it->GetTs();
        if (st_type == ::fedb::api::GetType::kSubKeyEq && st > 0 &&
            *ts != st) {
            return 1;
        }
        bool jump_out = false;
        if (st_type == ::fedb::api::GetType::kSubKeyGe ||
            st_type == ::fedb::api::GetType::kSubKeyGt) {
            ::fedb::base::Slice it_value = it->GetValue();
            if (enable_project) {
                int8_t* ptr = nullptr;
                uint32_t size = 0;
                fedb::base::Slice data = it->GetValue();
                const int8_t* row_ptr = reinterpret_cast<const int8_t*>(data.data());
                bool ok = row_project.Project(row_ptr, data.size(), &ptr, &size);
                if (!ok) {
                    PDLOG(WARNING, "fail to make a projection");
                    return -4;
                }
                value->assign(reinterpret_cast<char*>(ptr), size);
                delete[] ptr;
            } else {
                value->assign(it_value.data(), it_value.size());
            }
            return 0;
        }
        switch (real_et_type) {
            case ::fedb::api::GetType::kSubKeyEq:
                if (*ts != et) {
                    jump_out = true;
                }
                break;

            case ::fedb::api::GetType::kSubKeyGt:
                if (*ts <= et) {
                    jump_out = true;
                }
                break;

            case ::fedb::api::GetType::kSubKeyGe:
                if (*ts < et) {
                    jump_out = true;
                }
                break;

            default:
                PDLOG(WARNING, "invalid et type %s",
                      ::fedb::api::GetType_Name(et_type).c_str());
                return -2;
        }
        if (jump_out) {
            return 1;
        }
        if (enable_project) {
            int8_t* ptr = nullptr;
            uint32_t size = 0;
            fedb::base::Slice data = it->GetValue();
            const int8_t* row_ptr = reinterpret_cast<const int8_t*>(data.data());
            bool ok = row_project.Project(row_ptr, data.size(), &ptr, &size);
            if (!ok) {
                PDLOG(WARNING, "fail to make a projection");
                return -4;
            }
            value->assign(reinterpret_cast<char*>(ptr), size);
            delete[] ptr;
        } else {
            value->assign(it->GetValue().data(), it->GetValue().size());
        }
        return 0;
    }
    // not found
    return 1;
}

void TabletImpl::Get(RpcController* controller,
                     const ::fedb::api::GetRequest* request,
                     ::fedb::api::GetResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint64_t start_time = ::baidu::common::timer::get_micros();
    uint32_t tid = request->tid();
    uint32_t pid_num = 1;
    if (request->pid_group_size() > 0) {
        pid_num = request->pid_group_size();
    }
    std::vector<QueryIt> query_its(pid_num);
    std::shared_ptr<::fedb::storage::TTLSt> ttl;
    ::fedb::storage::TTLSt expired_value;
    for (uint32_t idx = 0; idx < pid_num; idx++) {
        uint32_t pid = 0;
        if (request->pid_group_size() > 0) {
            pid = request->pid_group(idx);
        } else {
            pid = request->pid();
        }
        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", tid, pid);
            response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            return;
        }
        if (table->GetTableStat() == ::fedb::storage::kLoading) {
            PDLOG(WARNING, "table is loading. tid %u, pid %u", tid, pid);
            response->set_code(::fedb::base::ReturnCode::kTableIsLoading);
            response->set_msg("table is loading");
            return;
        }
        uint32_t index = 0;
        int ts_index = -1;
        if (request->has_ts_name() && request->ts_name().size() > 0) {
            auto iter = table->GetTSMapping().find(request->ts_name());
            if (iter == table->GetTSMapping().end()) {
                PDLOG(WARNING, "ts name %s not found in table tid %u, pid %u",
                      request->ts_name().c_str(), tid, pid);
                response->set_code(::fedb::base::ReturnCode::kTsNameNotFound);
                response->set_msg("ts name not found");
                return;
            }
            ts_index = iter->second;
        }
        std::string index_name;
        if (request->has_idx_name() && request->idx_name().size() > 0) {
            index_name = request->idx_name();
        } else {
            index_name = table->GetPkIndex()->GetName();
        }
        std::shared_ptr<IndexDef> index_def;
        if (ts_index >= 0) {
            index_def = table->GetIndex(index_name, ts_index);
        } else {
            index_def = table->GetIndex(index_name);
        }
        if (!index_def || !index_def->IsReady()) {
            PDLOG(WARNING, "idx name %s not found in table tid %u, pid %u", index_name.c_str(), tid, pid);
            response->set_code(::fedb::base::ReturnCode::kIdxNameNotFound);
            response->set_msg("idx name not found");
            return;
        }
        index = index_def->GetId();
        if (!ttl) {
            ttl = index_def->GetTTL();
            expired_value = *ttl;
            expired_value.abs_ttl = table->GetExpireTime(expired_value);
        }
        GetIterator(table, request->key(), index, ts_index, &query_its[idx].it,
                    &query_its[idx].ticket);
        if (!query_its[idx].it) {
            response->set_code(::fedb::base::ReturnCode::kTsNameNotFound);
            response->set_msg("ts name not found");
            return;
        }
        query_its[idx].table = table;
    }
    const ::fedb::api::TableMeta& table_meta = query_its.begin()->table->GetTableMeta();
    const std::map<int32_t, std::shared_ptr<Schema>> vers_schema = query_its.begin()->table->GetAllVersionSchema();
    CombineIterator combine_it(std::move(query_its), request->ts(), request->type(), expired_value);
    combine_it.SeekToFirst();
    std::string* value = response->mutable_value();
    uint64_t ts = 0;
    int32_t code = GetIndex(request, table_meta, vers_schema, &combine_it, value, &ts);
    response->set_ts(ts);
    response->set_code(code);
    uint64_t end_time = ::baidu::common::timer::get_micros();
    if (start_time + FLAGS_query_slow_log_threshold < end_time) {
        std::string index_name;
        if (request->has_idx_name() && request->idx_name().size() > 0) {
            index_name = request->idx_name();
        }
        PDLOG(INFO,
              "slow log[get]. key %s index_name %s time %lu. tid %u, pid %u",
              request->key().c_str(), index_name.c_str(), end_time - start_time,
              request->tid(), request->pid());
    }
    switch (code) {
        case 1:
            response->set_code(::fedb::base::ReturnCode::kKeyNotFound);
            response->set_msg("key not found");
            return;
        case 0:
            return;
        case -1:
            response->set_msg("invalid args");
            response->set_code(::fedb::base::ReturnCode::kInvalidParameter);
            return;
        case -2:
            response->set_code(::fedb::base::ReturnCode::kInvalidParameter);
            response->set_msg("st/et sub key type is invalid");
            return;
        default:
            return;
    }
}

void TabletImpl::Put(RpcController* controller,
                     const ::fedb::api::PutRequest* request,
                     ::fedb::api::PutResponse* response, Closure* done) {
    if (follower_.load(std::memory_order_relaxed)) {
        response->set_code(::fedb::base::ReturnCode::kIsFollowerCluster);
        response->set_msg("is follower cluster");
        done->Run();
        return;
    }
    uint64_t start_time = ::baidu::common::timer::get_micros();
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
                request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        done->Run();
        return;
    }
    DLOG(INFO) << " request format_version " << request->format_version()
        << " request dimension size " << request->dimensions_size()
        << " request time " << request->time();
    if ((!request->has_format_version() &&
                table->GetTableMeta().format_version() == 1) ||
            (request->has_format_version() &&
             request->format_version() !=
             table->GetTableMeta().format_version())) {
        response->set_code(::fedb::base::ReturnCode::kPutBadFormat);
        response->set_msg("put bad format");
        done->Run();
        return;
    }
    if (request->time() == 0 && request->ts_dimensions_size() == 0) {
        response->set_code(
                ::fedb::base::ReturnCode::kTsMustBeGreaterThanZero);
        response->set_msg("ts must be greater than zero");
        done->Run();
        return;
    }
    if (!table->IsLeader()) {
        response->set_code(::fedb::base::ReturnCode::kTableIsFollower);
        response->set_msg("table is follower");
        done->Run();
        return;
    }
    if (table->GetTableStat() == ::fedb::storage::kLoading) {
        PDLOG(WARNING, "table is loading. tid %u, pid %u", request->tid(),
                request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsLoading);
        response->set_msg("table is loading");
        done->Run();
        return;
    }
    bool ok = false;
    if (request->dimensions_size() > 0) {
        int32_t ret_code = CheckDimessionPut(request, table->GetIdxCnt());
        if (ret_code != 0) {
            response->set_code(
                    ::fedb::base::ReturnCode::kInvalidDimensionParameter);
            response->set_msg("invalid dimension parameter");
            done->Run();
            return;
        }
        if (request->ts_dimensions_size() > 0) {
            DLOG(INFO) << "put data to tid " << request->tid() << " pid "
                << request->pid() << " with key "
                << request->dimensions(0).key() << " ts "
                << request->ts_dimensions(0).ts();
            ok = table->Put(request->dimensions(), request->ts_dimensions(),
                    request->value());
        } else {
            DLOG(INFO) << "put data to tid " << request->tid() << " pid "
                << request->pid() << " with key "
                << request->dimensions(0).key() << " ts "
                << request->time();

            ok = table->Put(request->time(), request->value(),
                    request->dimensions());
        }
    } else {
        ok = table->Put(request->pk(), request->time(),
                request->value().c_str(), request->value().size());
    }
    if (!ok) {
        response->set_code(::fedb::base::ReturnCode::kPutFailed);
        response->set_msg("put failed");
        done->Run();
        return;
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    std::shared_ptr<LogReplicator> replicator;
    do {
        replicator = GetReplicator(request->tid(), request->pid());
        if (!replicator) {
            PDLOG(
                    WARNING,
                    "fail to find table tid %u pid %u leader's log replicator",
                    request->tid(), request->pid());
            break;
        }
        ::fedb::api::LogEntry entry;
        entry.set_pk(request->pk());
        entry.set_ts(request->time());
        entry.set_value(request->value());
        entry.set_term(replicator->GetLeaderTerm());
        if (request->dimensions_size() > 0) {
            entry.mutable_dimensions()->CopyFrom(request->dimensions());
        }
        if (request->ts_dimensions_size() > 0) {
            entry.mutable_ts_dimensions()->CopyFrom(
                    request->ts_dimensions());
        }
        replicator->AppendEntry(entry);
    } while (false);
    uint64_t end_time = ::baidu::common::timer::get_micros();
    if (start_time + FLAGS_put_slow_log_threshold < end_time) {
        std::string key;
        if (request->dimensions_size() > 0) {
            for (int idx = 0; idx < request->dimensions_size(); idx++) {
                if (!key.empty()) {
                    key.append(", ");
                }
                key.append(std::to_string(request->dimensions(idx).idx()));
                key.append(":");
                key.append(request->dimensions(idx).key());
            }
        } else {
            key = request->pk();
        }
        PDLOG(INFO, "slow log[put]. key %s time %lu. tid %u, pid %u",
                key.c_str(), end_time - start_time, request->tid(),
                request->pid());
    }
    done->Run();
    if (replicator) {
        if (FLAGS_binlog_notify_on_put) {
            replicator->Notify();
        }
    }
}

int TabletImpl::CheckTableMeta(const fedb::api::TableMeta* table_meta,
                               std::string& msg) {
    msg.clear();
    if (table_meta->name().size() <= 0) {
        msg = "table name is empty";
        return -1;
    }
    if (table_meta->tid() <= 0) {
        msg = "tid is zero";
        return -1;
    }
    ::fedb::api::TTLType type = ::fedb::api::TTLType::kAbsoluteTime;
    if (table_meta->has_ttl_desc()) {
        type = table_meta->ttl_desc().ttl_type();
        if ((table_meta->ttl_desc().abs_ttl() > FLAGS_absolute_ttl_max) ||
            (table_meta->ttl_desc().lat_ttl() > FLAGS_latest_ttl_max)) {
            msg = "ttl is greater than conf value. max abs_ttl is " +
                  std::to_string(FLAGS_absolute_ttl_max) + ", max lat_ttl is " +
                  std::to_string(FLAGS_latest_ttl_max);
            return -1;
        }
    } else if (table_meta->has_ttl()) {
        uint64_t ttl = table_meta->ttl();
        type = table_meta->ttl_type();
        if ((type == ::fedb::api::TTLType::kAbsoluteTime &&
             ttl > FLAGS_absolute_ttl_max) ||
            (type == ::fedb::api::kLatestTime && ttl > FLAGS_latest_ttl_max)) {
            uint32_t max_ttl = type == ::fedb::api::TTLType::kAbsoluteTime
                                   ? FLAGS_absolute_ttl_max
                                   : FLAGS_latest_ttl_max;
            msg = "ttl is greater than conf value. max ttl is " +
                  std::to_string(max_ttl);
            return -1;
        }
    }

    std::map<std::string, std::string> column_map;
    std::set<std::string> ts_set;
    bool has_set_ts_col = false;
    if (table_meta->column_desc_size() > 0) {
        for (const auto& column_desc : table_meta->column_desc()) {
            if (column_map.find(column_desc.name()) != column_map.end()) {
                msg = "has repeated column name " + column_desc.name();
                return -1;
            }
            if (column_desc.is_ts_col()) {
                if (column_desc.add_ts_idx()) {
                    msg = "can not set add_ts_idx and is_ts_col together. column name " + column_desc.name();
                    return -1;
                }
                if (column_desc.type() != "int64" && column_desc.type() != "uint64" &&
                    column_desc.type() != "timestamp") {
                    msg = "ttl column type must be int64, uint64, timestamp but " + column_desc.type();
                    return -1;
                }
                if (column_desc.has_abs_ttl() || column_desc.has_lat_ttl()) {
                    if ((column_desc.abs_ttl() > FLAGS_absolute_ttl_max) ||
                        (column_desc.lat_ttl() > FLAGS_latest_ttl_max)) {
                        msg =
                            "ttl is greater than conf value. max abs_ttl is " +
                            std::to_string(FLAGS_absolute_ttl_max) +
                            ", max lat_ttl is " +
                            std::to_string(FLAGS_latest_ttl_max);
                        return -1;
                    }
                } else if (column_desc.has_ttl()) {
                    uint64_t ttl = column_desc.ttl();
                    if ((type == ::fedb::api::TTLType::kAbsoluteTime &&
                         ttl > FLAGS_absolute_ttl_max) ||
                        (type == ::fedb::api::kLatestTime &&
                         ttl > FLAGS_latest_ttl_max)) {
                        uint32_t max_ttl =
                            type == ::fedb::api::TTLType::kAbsoluteTime
                                ? FLAGS_absolute_ttl_max
                                : FLAGS_latest_ttl_max;
                        msg = "ttl is greater than conf value. max ttl is " +
                              std::to_string(max_ttl);
                        return -1;
                    }
                }
                has_set_ts_col = true;
                ts_set.insert(column_desc.name());
            }
            if (column_desc.add_ts_idx() &&
                ((column_desc.type() == "float") ||
                 (column_desc.type() == "double"))) {
                msg = "float or double column can not be index";
                return -1;
            }
            column_map.insert(
                std::make_pair(column_desc.name(), column_desc.type()));
        }
    }
    std::set<std::string> index_set;
    if (table_meta->column_key_size() > 0) {
        for (const auto& column_key : table_meta->column_key()) {
            if (index_set.find(column_key.index_name()) != index_set.end()) {
                msg = "has repeated index name " + column_key.index_name();
                return -1;
            }
            index_set.insert(column_key.index_name());
            bool has_col = false;
            for (const auto& column_name : column_key.col_name()) {
                has_col = true;
                auto iter = column_map.find(column_name);
                if (iter == column_map.end()) {
                    msg = "not found column name " + column_name;
                    return -1;
                }
                if ((iter->second == "float") || (iter->second == "double")) {
                    msg =
                        "float or double column can not be index" + column_name;
                    return -1;
                }
                if (ts_set.find(column_name) != ts_set.end()) {
                    msg =
                        "column name in column key can not set ts col. column "
                        "name " +
                        column_name;
                    return -1;
                }
            }
            if (!has_col) {
                auto iter = column_map.find(column_key.index_name());
                if (iter == column_map.end()) {
                    msg =
                        "index must member of columns when column key col name "
                        "is empty";
                    return -1;
                } else {
                    if ((iter->second == "float") ||
                        (iter->second == "double")) {
                        msg = "indxe name column type can not float or column";
                        return -1;
                    }
                }
            }
            std::set<std::string> ts_name_set;
            for (const auto& ts_name : column_key.ts_name()) {
                auto iter = column_map.find(ts_name);
                if (iter == column_map.end()) {
                    msg = "not found column name " + ts_name;
                    return -1;
                }
                if (has_set_ts_col && ts_set.find(ts_name) == ts_set.end()) {
                    msg = "not found ts_name " + ts_name;
                    return -1;
                }
                if (ts_name_set.find(ts_name) != ts_name_set.end()) {
                    msg = "has repeated ts_name " + ts_name;
                    return -1;
                }
                ts_name_set.insert(ts_name);
            }
            if (ts_set.size() > 1 && column_key.ts_name_size() == 0) {
                msg = "ts column num more than one, must set ts name";
                return -1;
            }
            if (column_key.has_ttl()) {
                if (column_key.ttl().abs_ttl() > FLAGS_absolute_ttl_max ||
                        column_key.ttl().lat_ttl() > FLAGS_latest_ttl_max) {
                    msg = "ttl is greater than conf value. max abs_ttl is " + std::to_string(FLAGS_absolute_ttl_max) +
                        ", max lat_ttl is " + std::to_string(FLAGS_latest_ttl_max);
                    return -1;
                }
            }
        }
    } else if (ts_set.size() > 1) {
        msg = "column_key should be set when has two or more ts columns";
        return -1;
    }
    return 0;
}

int32_t TabletImpl::ScanIndex(const ::fedb::api::ScanRequest* request, const ::fedb::api::TableMeta& meta,
                              const std::map<int32_t, std::shared_ptr<Schema>>& vers_schema,
                              CombineIterator* combine_it, butil::IOBuf* io_buf, uint32_t* count) {
    uint32_t limit = request->limit();
    uint32_t atleast = request->atleast();
    if (combine_it == NULL || io_buf == NULL || count == NULL || (atleast > limit && limit != 0)) {
        PDLOG(WARNING, "invalid args");
        return -1;
    }
    uint64_t st = request->st();
    uint64_t et = request->et();
    fedb::api::GetType et_type = request->et_type();
    fedb::api::GetType real_et_type = et_type;
    uint64_t expire_time = combine_it->GetExpireTime();
    if (et < expire_time && et_type == ::fedb::api::GetType::kSubKeyGt) {
        real_et_type = ::fedb::api::GetType::kSubKeyGe;
    }
    ::fedb::storage::TTLType ttl_type = combine_it->GetTTLType();
    if (ttl_type == ::fedb::storage::TTLType::kAbsoluteTime || ttl_type == ::fedb::storage::TTLType::kAbsOrLat) {
        et = std::max(et, expire_time);
    }

    if (st > 0 && st < et) {
        PDLOG(WARNING, "invalid args for st %lu less than et %lu or expire time %lu", st, et, expire_time);
        return -1;
    }

    bool enable_project = false;
    ::fedb::codec::RowProject row_project(vers_schema, request->projection());
    if (request->projection().size() > 0 && meta.format_version() == 1) {
        if (meta.compress_type() == api::kSnappy) {
            LOG(WARNING) << "project on compress row data do not eing supported";
            return -1;
        }
        bool ok = row_project.Init();
        if (!ok) {
            PDLOG(WARNING, "invalid project list");
            return -1;
        }
        enable_project = true;
    }
    bool remove_duplicated_record =
        request->has_enable_remove_duplicated_record() && request->enable_remove_duplicated_record();
    uint64_t last_time = 0;
    uint32_t total_block_size = 0;
    uint32_t record_count = 0;
    combine_it->SeekToFirst();
    while (combine_it->Valid()) {
        if (limit > 0 && record_count >= limit) {
            break;
        }
        if (remove_duplicated_record && record_count > 0 && last_time == combine_it->GetTs()) {
            combine_it->Next();
            continue;
        }
        uint64_t ts = combine_it->GetTs();
        if (atleast <= 0 || record_count >= atleast) {
            bool jump_out = false;
            switch (real_et_type) {
                case ::fedb::api::GetType::kSubKeyEq:
                    if (ts != et) {
                        jump_out = true;
                    }
                    break;
                case ::fedb::api::GetType::kSubKeyGt:
                    if (ts <= et) {
                        jump_out = true;
                    }
                    break;
                case ::fedb::api::GetType::kSubKeyGe:
                    if (ts < et) {
                        jump_out = true;
                    }
                    break;
                default:
                    PDLOG(WARNING, "invalid et type %s", ::fedb::api::GetType_Name(et_type).c_str());
                    return -2;
            }
            if (jump_out) break;
        }
        last_time = ts;
        if (enable_project) {
            int8_t* ptr = nullptr;
            uint32_t size = 0;
            fedb::base::Slice data = combine_it->GetValue();
            const int8_t* row_ptr = reinterpret_cast<const int8_t*>(data.data());
            bool ok = row_project.Project(row_ptr, data.size(), &ptr, &size);
            if (!ok) {
                PDLOG(WARNING, "fail to make a projection");
                return -4;
            }
            io_buf->append(reinterpret_cast<void*>(ptr), size);
            total_block_size += size;
        } else {
            fedb::base::Slice data = combine_it->GetValue();
            io_buf->append(reinterpret_cast<const void*>(data.data()), data.size());
            total_block_size += data.size();
        }
        record_count++;
        if (total_block_size > FLAGS_scan_max_bytes_size) {
            LOG(WARNING) << "reach the max byte size " << FLAGS_scan_max_bytes_size << " cur is " << total_block_size;
            return -3;
        }
        combine_it->Next();
    }
    *count = record_count;
    return 0;
}
int32_t TabletImpl::ScanIndex(const ::fedb::api::ScanRequest* request,
                              const ::fedb::api::TableMeta& meta,
                              const std::map<int32_t, std::shared_ptr<Schema>>& vers_schema,
                              CombineIterator* combine_it, std::string* pairs,
                              uint32_t* count) {
    uint32_t limit = request->limit();
    uint32_t atleast = request->atleast();
    if (combine_it == NULL || pairs == NULL || count == NULL ||
        (atleast > limit && limit != 0)) {
        PDLOG(WARNING, "invalid args");
        return -1;
    }
    uint64_t st = request->st();
    uint64_t et = request->et();
    fedb::api::GetType et_type = request->et_type();
    fedb::api::GetType real_et_type = et_type;
    uint64_t expire_time = combine_it->GetExpireTime();
    if (et < expire_time && et_type == ::fedb::api::GetType::kSubKeyGt) {
        real_et_type = ::fedb::api::GetType::kSubKeyGe;
    }
    ::fedb::storage::TTLType ttl_type = combine_it->GetTTLType();
    if (ttl_type == ::fedb::storage::TTLType::kAbsoluteTime ||
        ttl_type == ::fedb::storage::TTLType::kAbsOrLat) {
        et = std::max(et, expire_time);
    }
    if (st > 0 && st < et) {
        PDLOG(WARNING,
              "invalid args for st %lu less than et %lu or expire time %lu", st,
              et, expire_time);
        return -1;
    }

    bool enable_project = false;
    ::fedb::codec::RowProject row_project(vers_schema, request->projection());
    if (request->projection().size() > 0 && meta.format_version() == 1) {
        if (meta.compress_type() == api::kSnappy) {
            LOG(WARNING) << "project on compress row data do not eing supported";
            return -1;
        }
        bool ok = row_project.Init();
        if (!ok) {
            PDLOG(WARNING, "invalid project list");
            return -1;
        }
        enable_project = true;
    }
    bool remove_duplicated_record = request->has_enable_remove_duplicated_record() &&
                                    request->enable_remove_duplicated_record();
    uint64_t last_time = 0;
    boost::container::deque<std::pair<uint64_t, ::fedb::base::Slice>> tmp;
    uint32_t total_block_size = 0;
    combine_it->SeekToFirst();
    while (combine_it->Valid()) {
        if (limit > 0 && tmp.size() >= limit) {
            break;
        }
        if (remove_duplicated_record && tmp.size() > 0 &&
            last_time == combine_it->GetTs()) {
            combine_it->Next();
            continue;
        }
        uint64_t ts = combine_it->GetTs();
        if (atleast <= 0 || tmp.size() >= atleast) {
            bool jump_out = false;
            switch (real_et_type) {
                case ::fedb::api::GetType::kSubKeyEq:
                    if (ts != et) {
                        jump_out = true;
                    }
                    break;
                case ::fedb::api::GetType::kSubKeyGt:
                    if (ts <= et) {
                        jump_out = true;
                    }
                    break;
                case ::fedb::api::GetType::kSubKeyGe:
                    if (ts < et) {
                        jump_out = true;
                    }
                    break;
                default:
                    PDLOG(WARNING, "invalid et type %s",
                          ::fedb::api::GetType_Name(et_type).c_str());
                    return -2;
            }
            if (jump_out) break;
        }
        last_time = ts;
        if (enable_project) {
            int8_t* ptr = nullptr;
            uint32_t size = 0;
            fedb::base::Slice data = combine_it->GetValue();
            const int8_t* row_ptr = reinterpret_cast<const int8_t*>(data.data());
            bool ok = row_project.Project(row_ptr, data.size(), &ptr, &size);
            if (!ok) {
                PDLOG(WARNING, "fail to make a projection");
                return -4;
            }
            tmp.emplace_back(ts, Slice(reinterpret_cast<char*>(ptr), size, true));
            total_block_size += size;
        } else {
            fedb::base::Slice data = combine_it->GetValue();
            total_block_size += data.size();
            tmp.emplace_back(ts, data);
        }
        if (total_block_size > FLAGS_scan_max_bytes_size) {
            LOG(WARNING) << "reach the max byte size " << FLAGS_scan_max_bytes_size << " cur is " << total_block_size;
            return -3;
        }
        combine_it->Next();
    }
    int32_t ok = ::fedb::codec::EncodeRows(tmp, total_block_size, pairs);
    if (ok == -1) {
        PDLOG(WARNING, "fail to encode rows");
        return -4;
    }
    *count = tmp.size();
    return 0;
}

int32_t TabletImpl::CountIndex(uint64_t expire_time, uint64_t expire_cnt,
                               ::fedb::storage::TTLType ttl_type,
                               ::fedb::storage::TableIterator* it,
                               const ::fedb::api::CountRequest* request,
                               uint32_t* count) {
    uint64_t st = request->st();
    const fedb::api::GetType& st_type = request->st_type();
    uint64_t et = request->et();
    const fedb::api::GetType& et_type = request->et_type();
    bool remove_duplicated_record =
        request->has_enable_remove_duplicated_record() &&
        request->enable_remove_duplicated_record();
    if (it == NULL || count == NULL) {
        PDLOG(WARNING, "invalid args");
        return -1;
    }
    fedb::api::GetType real_st_type = st_type;
    fedb::api::GetType real_et_type = et_type;
    if (et < expire_time && et_type == ::fedb::api::GetType::kSubKeyGt) {
        real_et_type = ::fedb::api::GetType::kSubKeyGe;
    }
    if (ttl_type == ::fedb::storage::TTLType::kAbsoluteTime ||
        ttl_type == ::fedb::storage::TTLType::kAbsOrLat) {
        et = std::max(et, expire_time);
    }
    if (st_type == ::fedb::api::GetType::kSubKeyEq) {
        real_st_type = ::fedb::api::GetType::kSubKeyLe;
    }
    if (st_type != ::fedb::api::GetType::kSubKeyEq &&
        st_type != ::fedb::api::GetType::kSubKeyLe &&
        st_type != ::fedb::api::GetType::kSubKeyLt) {
        PDLOG(WARNING, "invalid st type %s",
              ::fedb::api::GetType_Name(st_type).c_str());
        return -2;
    }
    uint32_t cnt = 0;
    if (st > 0) {
        if (st < et) {
            return -1;
        }
        if (expire_cnt == 0) {
            Seek(it, st, real_st_type);
        } else {
            switch (ttl_type) {
                case ::fedb::storage::TTLType::kAbsoluteTime:
                    Seek(it, st, real_st_type);
                    break;
                case ::fedb::storage::TTLType::kAbsAndLat:
                    if (!SeekWithCount(it, st, real_st_type, expire_cnt,
                                       &cnt)) {
                        Seek(it, st, real_st_type);
                    }
                    break;
                default:
                    SeekWithCount(it, st, real_st_type, expire_cnt, &cnt);
                    break;
            }
        }
    } else {
        it->SeekToFirst();
    }

    uint64_t last_key = 0;
    uint32_t internal_cnt = 0;

    while (it->Valid()) {
        if (remove_duplicated_record && internal_cnt > 0 &&
            last_key == it->GetKey()) {
            cnt++;
            it->Next();
            continue;
        }
        if (ttl_type == ::fedb::storage::TTLType::kAbsoluteTime) {
            if (expire_time != 0 && it->GetKey() <= expire_time) {
                break;
            }
        } else if (ttl_type == ::fedb::storage::TTLType::kLatestTime) {
            if (expire_cnt != 0 && cnt >= expire_cnt) {
                break;
            }
        } else if (ttl_type == ::fedb::storage::TTLType::kAbsAndLat) {
            if ((expire_cnt != 0 && cnt >= expire_cnt) &&
                (expire_time != 0 && it->GetKey() <= expire_time)) {
                break;
            }
        } else {
            if ((expire_cnt != 0 && cnt >= expire_cnt) ||
                (expire_time != 0 && it->GetKey() <= expire_time)) {
                break;
            }
        }
        ++cnt;
        bool jump_out = false;
        last_key = it->GetKey();
        switch (real_et_type) {
            case ::fedb::api::GetType::kSubKeyEq:
                if (it->GetKey() != et) {
                    jump_out = true;
                }
                break;
            case ::fedb::api::GetType::kSubKeyGt:
                if (it->GetKey() <= et) {
                    jump_out = true;
                }
                break;
            case ::fedb::api::GetType::kSubKeyGe:
                if (it->GetKey() < et) {
                    jump_out = true;
                }
                break;
            default:
                PDLOG(WARNING, "invalid et type %s",
                      ::fedb::api::GetType_Name(et_type).c_str());
                return -2;
        }
        if (jump_out) break;
        last_key = it->GetKey();
        internal_cnt++;
        it->Next();
    }
    *count = internal_cnt;
    return 0;
}

void TabletImpl::Scan(RpcController* controller,
                      const ::fedb::api::ScanRequest* request,
                      ::fedb::api::ScanResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint64_t start_time = ::baidu::common::timer::get_micros();
    if (request->st() < request->et()) {
        response->set_code(::fedb::base::ReturnCode::kStLessThanEt);
        response->set_msg("starttime less than endtime");
        return;
    }
    uint32_t tid = request->tid();
    uint32_t pid_num = 1;
    if (request->pid_group_size() > 0) {
        pid_num = request->pid_group_size();
    }
    std::vector<QueryIt> query_its(pid_num);
    std::shared_ptr<::fedb::storage::TTLSt> ttl;
    ::fedb::storage::TTLSt expired_value;
    for (uint32_t idx = 0; idx < pid_num; idx++) {
        uint32_t pid = 0;
        if (request->pid_group_size() > 0) {
            pid = request->pid_group(idx);
        } else {
            pid = request->pid();
        }
        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", tid, pid);
            response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            return;
        }
        if (table->GetTableStat() == ::fedb::storage::kLoading) {
            PDLOG(WARNING, "table is loading. tid %u, pid %u", tid, pid);
            response->set_code(::fedb::base::ReturnCode::kTableIsLoading);
            response->set_msg("table is loading");
            return;
        }
        uint32_t index = 0;
        int ts_index = -1;
        if (request->has_ts_name() && !request->ts_name().empty()) {
            auto iter = table->GetTSMapping().find(request->ts_name());
            if (iter == table->GetTSMapping().end()) {
                PDLOG(WARNING, "ts name %s not found in table tid %u, pid %u",
                      request->ts_name().c_str(), tid, pid);
                response->set_code(::fedb::base::ReturnCode::kTsNameNotFound);
                response->set_msg("ts name not found");
                return;
            }
            ts_index = iter->second;
        }
        std::string index_name;
        if (request->has_idx_name() && !request->idx_name().empty()) {
            index_name = request->idx_name();
        } else {
            index_name = table->GetPkIndex()->GetName();
        }
        std::shared_ptr<IndexDef> index_def;
        if (ts_index >= 0) {
            index_def = table->GetIndex(index_name, ts_index);
        } else {
            index_def = table->GetIndex(index_name);
        }
        if (!index_def || !index_def->IsReady()) {
            PDLOG(WARNING, "idx name %s not found in table tid %u, pid %u", index_name.c_str(), tid, pid);
            response->set_code(::fedb::base::ReturnCode::kIdxNameNotFound);
            response->set_msg("idx name not found");
            return;
        }
        index = index_def->GetId();
        if (!ttl) {
            ttl = index_def->GetTTL();
            expired_value = *ttl;
            expired_value.abs_ttl = table->GetExpireTime(expired_value);
        }
        GetIterator(table, request->pk(), index, ts_index, &query_its[idx].it,
                    &query_its[idx].ticket);
        if (!query_its[idx].it) {
            response->set_code(::fedb::base::ReturnCode::kTsNameNotFound);
            response->set_msg("ts name not found");
            return;
        }
        query_its[idx].table = table;
    }
    const ::fedb::api::TableMeta& table_meta = query_its.begin()->table->GetTableMeta();
    const std::map<int32_t, std::shared_ptr<Schema>> vers_schema = query_its.begin()->table->GetAllVersionSchema();
    CombineIterator combine_it(std::move(query_its), request->st(), request->st_type(), expired_value);
    uint32_t count = 0;
    int32_t code = 0;
    if (!request->has_use_attachment() || !request->use_attachment()) {
        std::string* pairs = response->mutable_pairs();
        code = ScanIndex(request, table_meta, vers_schema, &combine_it, pairs, &count);
        response->set_code(code);
        response->set_count(count);
    } else {
        brpc::Controller* cntl = static_cast<brpc::Controller*>(controller);
        butil::IOBuf& buf = cntl->response_attachment();
        code = ScanIndex(request, table_meta, vers_schema, &combine_it, &buf, &count);
        response->set_code(code);
        response->set_count(count);
        response->set_buf_size(buf.size());
        DLOG(INFO) << " scan " << request->pk() << " with buf size "  << buf.size();
    }
    uint64_t end_time = ::baidu::common::timer::get_micros();
    if (start_time + FLAGS_query_slow_log_threshold < end_time) {
        std::string index_name;
        if (request->has_idx_name() && request->idx_name().size() > 0) {
            index_name = request->idx_name();
        }
        PDLOG(INFO, "slow log[scan]. key %s index_name %s time %lu. tid %u, pid %u", request->pk().c_str(),
              index_name.c_str(), end_time - start_time, request->tid(), request->pid());
    }
    switch (code) {
        case 0:
            return;
        case -1:
            response->set_msg("invalid args");
            response->set_code(::fedb::base::ReturnCode::kInvalidParameter);
            return;
        case -2:
            response->set_msg("st/et sub key type is invalid");
            response->set_code(::fedb::base::ReturnCode::kInvalidParameter);
            return;
        case -3:
            response->set_code(
                ::fedb::base::ReturnCode::kReacheTheScanMaxBytesSize);
            response->set_msg("reach the max scan byte size");
            return;
        case -4:
            response->set_msg("fail to encode data rows");
            response->set_code(::fedb::base::ReturnCode::kEncodeError);
            return;
        default:
            return;
    }
}

void TabletImpl::Count(RpcController* controller,
                       const ::fedb::api::CountRequest* request,
                       ::fedb::api::CountResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
              request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (table->GetTableStat() == ::fedb::storage::kLoading) {
        PDLOG(WARNING, "table is loading. tid %u, pid %u", request->tid(),
              request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsLoading);
        response->set_msg("table is loading");
        return;
    }
    uint32_t index = 0;
    int ts_index = -1;
    if (request->has_ts_name() && request->ts_name().size() > 0) {
        auto iter = table->GetTSMapping().find(request->ts_name());
        if (iter == table->GetTSMapping().end()) {
            PDLOG(WARNING, "ts name %s not found in table tid %u, pid %u",
                  request->ts_name().c_str(), request->tid(), request->pid());
            response->set_code(::fedb::base::ReturnCode::kTsNameNotFound);
            response->set_msg("ts name not found");
            return;
        }
        ts_index = iter->second;
    }
    ::fedb::storage::TTLSt ttl;
    std::shared_ptr<IndexDef> index_def;
    std::string index_name;
    if (request->has_idx_name() && !request->idx_name().empty()) {
        index_name = request->idx_name();
    } else {
        index_name = table->GetPkIndex()->GetName();
    }
    if (ts_index >= 0) {
        index_def = table->GetIndex(index_name, ts_index);
    } else {
        index_def = table->GetIndex(index_name);
    }
    if (!index_def || !index_def->IsReady()) {
        PDLOG(WARNING, "idx name %s not found in table tid %u, pid %u",
              request->idx_name().c_str(), request->tid(), request->pid());
        response->set_code(::fedb::base::ReturnCode::kIdxNameNotFound);
        response->set_msg("idx name not found");
        return;
    }
    index = index_def->GetId();
    ttl = *index_def->GetTTL();
    if (!request->filter_expired_data()) {
        MemTable* mem_table = dynamic_cast<MemTable*>(table.get());
        if (mem_table != NULL) {
            uint64_t count = 0;
            if (ts_index >= 0) {
                if (mem_table->GetCount(index, ts_index, request->key(),
                                        count) < 0) {
                    count = 0;
                }
            } else {
                if (mem_table->GetCount(index, request->key(), count) < 0) {
                    count = 0;
                }
            }
            response->set_code(::fedb::base::ReturnCode::kOk);
            response->set_msg("ok");
            response->set_count(count);
            return;
        }
    }
    ::fedb::storage::Ticket ticket;
    ::fedb::storage::TableIterator* it = NULL;
    if (ts_index >= 0) {
        it = table->NewIterator(index, ts_index, request->key(), ticket);
    } else {
        it = table->NewIterator(index, request->key(), ticket);
    }
    if (it == NULL) {
        response->set_code(::fedb::base::ReturnCode::kTsNameNotFound);
        response->set_msg("ts name not found");
        return;
    }
    uint32_t count = 0;
    int32_t code = 0;
    code = CountIndex(table->GetExpireTime(ttl),
                      ttl.lat_ttl, index_def->GetTTLType(), it, request, &count);
    delete it;
    response->set_code(code);
    response->set_count(count);
    switch (code) {
        case 0:
            return;
        case -1:
            response->set_msg("invalid args");
            response->set_code(::fedb::base::ReturnCode::kInvalidParameter);
            return;
        case -2:
            response->set_msg("st/et sub key type is invalid");
            response->set_code(::fedb::base::ReturnCode::kInvalidParameter);
            return;
        case -3:
            response->set_code(
                ::fedb::base::ReturnCode::kReacheTheScanMaxBytesSize);
            response->set_msg("reach the max scan byte size");
            return;
        case -4:
            response->set_msg("fail to encode data rows");
            response->set_code(
                ::fedb::base::ReturnCode::kFailToUpdateTtlFromTablet);
            return;
        default:
            return;
    }
}

void TabletImpl::Traverse(RpcController* controller,
                          const ::fedb::api::TraverseRequest* request,
                          ::fedb::api::TraverseResponse* response,
                          Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
                request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (table->GetTableStat() == ::fedb::storage::kLoading) {
        PDLOG(WARNING, "table is loading. tid %u, pid %u", request->tid(),
                request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsLoading);
        response->set_msg("table is loading");
        return;
    }
    uint32_t index = 0;
    int ts_index = -1;
    std::string index_name;
    if (request->has_idx_name() && !request->idx_name().empty()) {
        index_name = request->idx_name();
    } else {
        index_name = table->GetPkIndex()->GetName();
    }
    if (request->has_ts_name() && request->ts_name().size() > 0) {
        auto iter = table->GetTSMapping().find(request->ts_name());
        if (iter == table->GetTSMapping().end()) {
            PDLOG(WARNING, "ts name %s not found in table tid %u, pid %u",
                    request->ts_name().c_str(), request->tid(),
                    request->pid());
            response->set_code(::fedb::base::ReturnCode::kTsNameNotFound);
            response->set_msg("ts name not found");
            return;
        }
        ts_index = iter->second;
    }
    std::shared_ptr<IndexDef> index_def;
    if (ts_index >= 0) {
        index_def = table->GetIndex(index_name, ts_index);
    } else {
        index_def = table->GetIndex(index_name);
    }
    if (!index_def || !index_def->IsReady()) {
        PDLOG(WARNING, "idx name %s ts_index %d not found in table. tid %u, pid %u",
                index_name.c_str(), ts_index, request->tid(), request->pid());
        response->set_code(::fedb::base::ReturnCode::kIdxNameNotFound);
        response->set_msg("idx name not found");
        return;
    }
    index = index_def->GetId();
    ::fedb::storage::TableIterator* it = NULL;
    if (ts_index >= 0) {
        it = table->NewTraverseIterator(index, ts_index);
    } else {
        it = table->NewTraverseIterator(index);
    }
    if (it == NULL) {
        response->set_code(::fedb::base::ReturnCode::kTsNameNotFound);
        response->set_msg("ts name not found, when create iterator");
        return;
    }

    uint64_t last_time = 0;
    std::string last_pk;
    if (request->has_pk() && request->pk().size() > 0) {
        DEBUGLOG("tid %u, pid %u seek pk %s ts %lu", request->tid(),
                request->pid(), request->pk().c_str(), request->ts());
        it->Seek(request->pk(), request->ts());
        last_pk = request->pk();
        last_time = request->ts();
    } else {
        DEBUGLOG("tid %u, pid %u seek to first", request->tid(),
                request->pid());
        it->SeekToFirst();
    }
    std::map<std::string,
        std::vector<std::pair<uint64_t, fedb::base::Slice>>>
            value_map;
    uint32_t total_block_size = 0;
    bool remove_duplicated_record = false;
    if (request->has_enable_remove_duplicated_record()) {
        remove_duplicated_record =
            request->enable_remove_duplicated_record();
    }
    uint32_t scount = 0;
    for (; it->Valid(); it->Next()) {
        if (request->limit() > 0 && scount > request->limit() - 1) {
            DEBUGLOG("reache the limit %u ", request->limit());
            break;
        }
        DEBUGLOG("traverse pk %s ts %lu", it->GetPK().c_str(),
                it->GetKey());
        // skip duplicate record
        if (remove_duplicated_record && last_time == it->GetKey() &&
                last_pk == it->GetPK()) {
            DEBUGLOG("filter duplicate record for key %s with ts %lu",
                    last_pk.c_str(), last_time);
            continue;
        }
        last_pk = it->GetPK();
        last_time = it->GetKey();
        if (value_map.find(last_pk) == value_map.end()) {
            value_map.insert(std::make_pair(
                        last_pk,
                        std::vector<std::pair<uint64_t, fedb::base::Slice>>()));
            value_map[last_pk].reserve(request->limit());
        }
        fedb::base::Slice value = it->GetValue();
        value_map[last_pk].push_back(std::make_pair(it->GetKey(), value));
        total_block_size += last_pk.length() + value.size();
        scount++;
        if (it->GetCount() >= FLAGS_max_traverse_cnt) {
            DEBUGLOG("traverse cnt %lu max %lu, key %s ts %lu",
                    it->GetCount(), FLAGS_max_traverse_cnt,
                    last_pk.c_str(), last_time);
            break;
        }
    }
    bool is_finish = false;
    if (it->GetCount() >= FLAGS_max_traverse_cnt) {
        DEBUGLOG("traverse cnt %lu is great than max %lu, key %s ts %lu",
                it->GetCount(), FLAGS_max_traverse_cnt, last_pk.c_str(),
                last_time);
        last_pk = it->GetPK();
        last_time = it->GetKey();
        if (last_pk.empty()) {
            is_finish = true;
        }
    } else if (scount < request->limit()) {
        is_finish = true;
    }
    uint32_t total_size = scount * (8 + 4 + 4) + total_block_size;
    std::string* pairs = response->mutable_pairs();
    if (scount <= 0) {
        pairs->resize(0);
    } else {
        pairs->resize(total_size);
    }
    char* rbuffer = reinterpret_cast<char*>(&((*pairs)[0]));
    uint32_t offset = 0;
    for (const auto& kv : value_map) {
        for (const auto& pair : kv.second) {
            DEBUGLOG("encode pk %s ts %lu size %u", kv.first.c_str(),
                    pair.first, pair.second.size());
            ::fedb::codec::EncodeFull(kv.first, pair.first,
                    pair.second.data(),
                    pair.second.size(), rbuffer, offset);
            offset += (4 + 4 + 8 + kv.first.length() + pair.second.size());
        }
    }
    delete it;
    DEBUGLOG("traverse count %d. last_pk %s last_time %lu", scount,
            last_pk.c_str(), last_time);
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_count(scount);
    response->set_pk(last_pk);
    response->set_ts(last_time);
    response->set_is_finish(is_finish);
}

void TabletImpl::Delete(RpcController* controller,
                        const ::fedb::api::DeleteRequest* request,
                        fedb::api::GeneralResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (follower_.load(std::memory_order_relaxed)) {
        response->set_code(::fedb::base::ReturnCode::kIsFollowerCluster);
        response->set_msg("is follower cluster");
        return;
    }
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
                request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (!table->IsLeader()) {
        DEBUGLOG("table is follower. tid %u, pid %u", request->tid(),
                request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsFollower);
        response->set_msg("table is follower");
        return;
    }
    if (table->GetTableStat() == ::fedb::storage::kLoading) {
        PDLOG(WARNING, "table is loading. tid %u, pid %u", request->tid(),
                request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsLoading);
        response->set_msg("table is loading");
        return;
    }
    uint32_t idx = 0;
    if (request->has_idx_name() && request->idx_name().size() > 0) {
        std::shared_ptr<IndexDef> index_def =
            table->GetIndex(request->idx_name());
        if (!index_def || !index_def->IsReady()) {
            PDLOG(WARNING, "idx name %s not found in table tid %u, pid %u",
                    request->idx_name().c_str(), request->tid(),
                    request->pid());
            response->set_code(::fedb::base::ReturnCode::kIdxNameNotFound);
            response->set_msg("idx name not found");
            return;
        }
        idx = index_def->GetId();
    }
    if (table->Delete(request->key(), idx)) {
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        DEBUGLOG("delete ok. tid %u, pid %u, key %s", request->tid(),
                request->pid(), request->key().c_str());
    } else {
        response->set_code(::fedb::base::ReturnCode::kDeleteFailed);
        response->set_msg("delete failed");
        return;
    }
    std::shared_ptr<LogReplicator> replicator;
    do {
        replicator = GetReplicator(request->tid(), request->pid());
        if (!replicator) {
            PDLOG(
                    WARNING,
                    "fail to find table tid %u pid %u leader's log replicator",
                    request->tid(), request->pid());
            break;
        }
        ::fedb::api::LogEntry entry;
        entry.set_term(replicator->GetLeaderTerm());
        entry.set_method_type(::fedb::api::MethodType::kDelete);
        ::fedb::api::Dimension* dimension = entry.add_dimensions();
        dimension->set_key(request->key());
        dimension->set_idx(idx);
        replicator->AppendEntry(entry);
    } while (false);
    if (replicator && FLAGS_binlog_notify_on_put) {
        replicator->Notify();
    }
    return;
}

void TabletImpl::Query(RpcController* ctrl,
                       const fedb::api::QueryRequest* request,
                       fedb::api::QueryResponse* response, Closure* done) {
    DLOG(INFO) << "handle query request begin!";
    brpc::ClosureGuard done_guard(done);
    brpc::Controller* cntl = static_cast<brpc::Controller*>(ctrl);
    butil::IOBuf& buf = cntl->response_attachment();
    ProcessQuery(ctrl, request, response, &buf);
}

void TabletImpl::ProcessQuery(RpcController* ctrl,
                              const fedb::api::QueryRequest* request,
                              ::fedb::api::QueryResponse* response,
                              butil::IOBuf* buf) {
    ::hybridse::base::Status status;
    if (request->is_batch()) {
        ::hybridse::vm::BatchRunSession session;
        if (request->is_debug()) {
            session.EnableDebug();
        }
        {
            bool ok =
                engine_->Get(request->sql(), request->db(), session, status);
            if (!ok) {
                response->set_msg(status.msg);
                response->set_code(::fedb::base::kSQLCompileError);
                DLOG(WARNING) << "fail to compile sql " << request->sql();
                return;
            }
        }

        auto table = session.Run();
        if (!table) {
            DLOG(WARNING) << "fail to run sql " << request->sql();
            response->set_code(::fedb::base::kSQLRunError);
            response->set_msg("fail to run sql");
            return;
        }
        auto iter = table->GetIterator();
        if (!iter) {
            response->set_schema(session.GetEncodedSchema());
            response->set_byte_size(0);
            response->set_count(0);
            response->set_code(::fedb::base::kOk);
            return;
        }
        iter->SeekToFirst();
        uint32_t byte_size = 0;
        uint32_t count = 0;
        while (iter->Valid()) {
            const ::hybridse::codec::Row& row = iter->GetValue();
            if (byte_size > FLAGS_scan_max_bytes_size) {
                LOG(WARNING) << "reach the max byte size truncate result";
                response->set_schema(session.GetEncodedSchema());
                response->set_byte_size(byte_size);
                response->set_count(count);
                response->set_code(::fedb::base::kOk);
                return;
            }
            byte_size += row.size();
            iter->Next();
            buf->append(reinterpret_cast<void*>(row.buf()), row.size());
            count += 1;
        }
        response->set_schema(session.GetEncodedSchema());
        response->set_byte_size(byte_size);
        response->set_count(count);
        response->set_code(::fedb::base::kOk);
        DLOG(INFO) << "handle batch sql " << request->sql()
                   << " with record cnt " << count << " byte size "
                   << byte_size;
    } else {
        ::hybridse::vm::RequestRunSession session;
        if (request->is_debug()) {
            session.EnableDebug();
        }
        if (request->is_procedure()) {
            const std::string& db_name = request->db();
            const std::string& sp_name = request->sp_name();
            std::shared_ptr<hybridse::vm::CompileInfo> request_compile_info;
            {
                hybridse::base::Status status;
                request_compile_info = sp_cache_->GetRequestInfo(db_name, sp_name, status);
                if (!status.isOK()) {
                    response->set_code(::fedb::base::ReturnCode::kProcedureNotFound);
                    response->set_msg(status.msg);
                    PDLOG(WARNING, status.msg.c_str());
                    return;
                }
            }
            session.SetCompileInfo(request_compile_info);
            session.SetSpName(sp_name);
            RunRequestQuery(ctrl, *request, session, *response, *buf);
        } else {
            bool ok = engine_->Get(request->sql(), request->db(), session, status);
            if (!ok || session.GetCompileInfo() == nullptr) {
                response->set_msg(status.msg);
                response->set_code(::fedb::base::kSQLCompileError);
                DLOG(WARNING) << "fail to compile sql in request mode:\n"
                    << request->sql();
                return;
            }
            RunRequestQuery(ctrl, *request, session, *response, *buf);
        }
        const std::string& sql = session.GetCompileInfo()->GetSql();
        if (response->code() != ::fedb::base::kOk) {
            DLOG(WARNING) << "fail to run sql " << sql << " error msg: " << response->msg();
        } else {
            DLOG(INFO) << "handle request sql " << sql;
        }
    }
}

void TabletImpl::SubQuery(RpcController* ctrl,
                       const fedb::api::QueryRequest* request,
                       fedb::api::QueryResponse* response, Closure* done) {
    DLOG(INFO) << "handle subquery request begin!";
    brpc::ClosureGuard done_guard(done);
    brpc::Controller* cntl = static_cast<brpc::Controller*>(ctrl);
    butil::IOBuf& buf = cntl->response_attachment();
    ProcessQuery(ctrl, request, response, &buf);
}

void TabletImpl::SQLBatchRequestQuery(RpcController* ctrl, const fedb::api::SQLBatchRequestQueryRequest* request,
                                      fedb::api::SQLBatchRequestQueryResponse* response, Closure* done) {
    DLOG(INFO) << "handle query batch request begin!";
    brpc::ClosureGuard done_guard(done);
    brpc::Controller* cntl = static_cast<brpc::Controller*>(ctrl);
    butil::IOBuf& buf = cntl->response_attachment();
    return ProcessBatchRequestQuery(ctrl, request, response, buf);
}
void TabletImpl::ProcessBatchRequestQuery(
    RpcController* ctrl, const fedb::api::SQLBatchRequestQueryRequest* request,
    fedb::api::SQLBatchRequestQueryResponse* response, butil::IOBuf& buf) {
    ::hybridse::base::Status status;
    ::hybridse::vm::BatchRequestRunSession session;
    // run session
    if (request->is_debug()) {
        session.EnableDebug();
    }
    bool is_procedure = request->is_procedure();
    if (is_procedure) {
        std::shared_ptr<hybridse::vm::CompileInfo> request_compile_info;
        {
            hybridse::base::Status status;
            request_compile_info = sp_cache_->GetBatchRequestInfo(
                request->db(), request->sp_name(), status);
            if (!status.isOK()) {
                response->set_code(
                    ::fedb::base::ReturnCode::kProcedureNotFound);
                response->set_msg(status.msg);
                PDLOG(WARNING, status.msg.c_str());
                return;
            }
            session.SetCompileInfo(request_compile_info);
            session.SetSpName(request->sp_name());
        }
    } else {
        size_t common_column_num = request->common_column_indices().size();
        for (size_t i = 0; i < common_column_num; ++i) {
            auto col_idx = request->common_column_indices().Get(i);
            session.AddCommonColumnIdx(col_idx);
        }
        bool ok = engine_->Get(request->sql(), request->db(), session, status);
        if (!ok || session.GetCompileInfo() == nullptr) {
            response->set_msg(status.msg);
            response->set_code(::fedb::base::kSQLCompileError);
            DLOG(WARNING) << "fail to get sql engine: \n"
                << request->sql() << "\n" << status.str();
            return;
        }
    }

    // fill input data
    auto compile_info = session.GetCompileInfo();
    if (compile_info == nullptr) {
        response->set_msg("compile info is null, should never happen");
        response->set_code(::fedb::base::kSQLCompileError);
        return;
    }
    const auto& batch_request_info =
        compile_info->GetBatchRequestInfo();
    size_t common_column_num = batch_request_info.common_column_indices.size();
    bool has_common_and_uncommon_row =
        !request->has_task_id() && common_column_num > 0 &&
        common_column_num <
        static_cast<size_t>(session.GetRequestSchema().size());
    size_t input_row_num = request->row_sizes().size();
    if (request->common_slices() > 0 && input_row_num > 0) {
        input_row_num -= 1;
    } else if (has_common_and_uncommon_row) {
        response->set_msg("input common row is missing");
        response->set_code(::fedb::base::kSQLRunError);
        return;
    }

    auto& io_buf = static_cast<brpc::Controller*>(ctrl)->request_attachment();
    size_t buf_offset = 0;
    std::vector<::hybridse::codec::Row> input_rows(input_row_num);
    if (has_common_and_uncommon_row) {
        size_t common_size = request->row_sizes().Get(0);
        ::hybridse::codec::Row common_row;
        if (!codec::DecodeRpcRow(io_buf, buf_offset, common_size,
                                 request->common_slices(), &common_row)) {
            response->set_msg("decode input common row failed");
            response->set_code(::fedb::base::kSQLRunError);
            return;
        }
        buf_offset += common_size;
        for (size_t i = 0; i < input_row_num; ++i) {
            ::hybridse::codec::Row non_common_row;
            size_t non_common_size = request->row_sizes().Get(i + 1);
            if (!codec::DecodeRpcRow(io_buf, buf_offset, non_common_size,
                                     request->non_common_slices(), &non_common_row)) {
                response->set_msg("decode input non common row failed");
                response->set_code(::fedb::base::kSQLRunError);
                return;
            }
            buf_offset += non_common_size;
            input_rows[i] = ::hybridse::codec::Row(
                1, common_row, 1, non_common_row);
        }
    } else {
        for (size_t i = 0; i < input_row_num; ++i) {
            size_t non_common_size = request->row_sizes().Get(i);
            if (!codec::DecodeRpcRow(io_buf, buf_offset, non_common_size,
                                     request->non_common_slices(), &input_rows[i])) {
                response->set_msg("decode input non common row failed");
                response->set_code(::fedb::base::kSQLRunError);
                return;
            }
            buf_offset += non_common_size;
        }
    }
    std::vector<::hybridse::codec::Row> output_rows;
    int32_t run_ret = 0;
    if (request->has_task_id()) {
        session.Run(request->task_id(), input_rows, output_rows);
    } else {
        session.Run(input_rows, output_rows);
    }
    if (run_ret != 0) {
        response->set_msg(status.msg);
        response->set_code(::fedb::base::kSQLRunError);
        DLOG(WARNING) << "fail to run sql: " << request->sql();
        return;
    }

    // fill output data
    size_t output_col_num = session.GetSchema().size();
    auto& output_common_indices =
        batch_request_info.output_common_column_indices;
    bool has_common_and_uncomon_slice =
        !request->has_task_id() && !output_common_indices.empty() &&
        output_common_indices.size() < output_col_num;

    if (has_common_and_uncomon_slice && !output_rows.empty()) {
        const auto& first_row = output_rows[0];
        if (first_row.GetRowPtrCnt() != 2) {
            response->set_msg("illegal row ptrs: expect 2");
            response->set_code(::fedb::base::kSQLRunError);
            LOG(WARNING) << "illegal row ptrs: expect 2";
            return;
        }
        buf.append(first_row.buf(0), first_row.size(0));
        response->add_row_sizes(first_row.size(0));
        response->set_common_slices(1);
    } else {
        response->set_common_slices(0);
    }
    response->set_non_common_slices(1);
    for (auto& output_row : output_rows) {
        if (has_common_and_uncomon_slice) {
            if (output_row.GetRowPtrCnt() != 2) {
                response->set_msg("illegal row ptrs: expect 2");
                response->set_code(::fedb::base::kSQLRunError);
                LOG(WARNING) << "illegal row ptrs: expect 2";
                return;
            }
            buf.append(output_row.buf(1), output_row.size(1));
            response->add_row_sizes(output_row.size(1));
        } else {
            if (output_row.GetRowPtrCnt() != 1) {
                response->set_msg("illegal row ptrs: expect 1");
                response->set_code(::fedb::base::kSQLRunError);
                LOG(WARNING) << "illegal row ptrs: expect 1";
                return;
            }
            buf.append(output_row.buf(0), output_row.size(0));
            response->add_row_sizes(output_row.size(0));
        }
    }

    // fill response
    for (size_t idx : output_common_indices) {
        response->add_common_column_indices(idx);
    }
    response->set_schema(session.GetEncodedSchema());
    response->set_count(output_rows.size());
    response->set_code(::fedb::base::kOk);
    DLOG(INFO) << "handle batch request sql " << request->sql()
               << " with record cnt " << output_rows.size()
               << " with schema size " << session.GetSchema().size();
}
void TabletImpl::SubBatchRequestQuery(RpcController* ctrl, const fedb::api::SQLBatchRequestQueryRequest* request,
                                      fedb::api::SQLBatchRequestQueryResponse* response, Closure* done) {
    DLOG(INFO) << "handle subquery batch request begin!";
    brpc::ClosureGuard done_guard(done);
    brpc::Controller* cntl = static_cast<brpc::Controller*>(ctrl);
    butil::IOBuf& buf = cntl->response_attachment();
    return ProcessBatchRequestQuery(ctrl, request, response, buf);
}

void TabletImpl::ChangeRole(RpcController* controller,
                            const ::fedb::api::ChangeRoleRequest* request,
                            ::fedb::api::ChangeRoleResponse* response,
                            Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::shared_ptr<Table> table = GetTable(tid, pid);
    if (!table) {
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (table->GetTableStat() != ::fedb::storage::kNormal) {
        PDLOG(WARNING, "table state[%u] can not change role. tid[%u] pid[%u]",
              table->GetTableStat(), tid, pid);
        response->set_code(::fedb::base::ReturnCode::kTableStatusIsNotKnormal);
        response->set_msg("table status is not kNormal");
        return;
    }
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (!replicator) {
        response->set_code(::fedb::base::ReturnCode::kReplicatorIsNotExist);
        response->set_msg("replicator is not exist");
        return;
    }
    bool is_leader = false;
    if (request->mode() == ::fedb::api::TableMode::kTableLeader) {
        is_leader = true;
    }
    std::map<std::string, std::string> real_ep_map;
    for (int idx = 0; idx < request->replicas_size(); idx++) {
        real_ep_map.insert(std::make_pair(request->replicas(idx), ""));
    }
    if (FLAGS_use_name) {
        if (!GetRealEp(tid, pid, &real_ep_map)) {
            response->set_code(
                ::fedb::base::ReturnCode::kServerNameNotFound);
            response->set_msg("name not found in real_ep_map");
            return;
        }
    }
    if (is_leader) {
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            if (table->IsLeader()) {
                PDLOG(WARNING, "table is leader. tid[%u] pid[%u]", tid, pid);
                response->set_code(::fedb::base::ReturnCode::kTableIsLeader);
                response->set_msg("table is leader");
                return;
            }
            PDLOG(INFO, "change to leader. tid[%u] pid[%u] term[%lu]", tid, pid,
                  request->term());
            table->SetLeader(true);
            replicator->SetRole(ReplicatorRole::kLeaderNode);
            if (!zk_cluster_.empty()) {
                replicator->SetLeaderTerm(request->term());
            }
        }
        if (replicator->AddReplicateNode(real_ep_map) < 0) {
            PDLOG(WARNING, "add replicator failed. tid[%u] pid[%u]", tid, pid);
        }
        for (auto& e : request->endpoint_tid()) {
            std::map<std::string, std::string> r_real_ep_map;
            r_real_ep_map.insert(std::make_pair(e.endpoint(), ""));
            if (FLAGS_use_name) {
                if (!GetRealEp(tid, pid, &r_real_ep_map)) {
                    response->set_code(
                            ::fedb::base::ReturnCode::kServerNameNotFound);
                    response->set_msg("name not found in r_real_ep_map");
                    return;
                }
            }
            replicator->AddReplicateNode(r_real_ep_map, e.tid());
        }
    } else {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        if (!table->IsLeader()) {
            PDLOG(WARNING, "table is follower. tid[%u] pid[%u]", tid, pid);
            response->set_code(::fedb::base::ReturnCode::kOk);
            response->set_msg("table is follower");
            return;
        }
        replicator->DelAllReplicateNode();
        replicator->SetRole(ReplicatorRole::kFollowerNode);
        table->SetLeader(false);
        PDLOG(INFO, "change to follower. tid[%u] pid[%u]", tid, pid);
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::AddReplica(RpcController* controller,
                            const ::fedb::api::ReplicaRequest* request,
                            ::fedb::api::AddReplicaResponse* response,
                            Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPMultiTask(request->task_info(),
                           ::fedb::api::TaskType::kAddReplica, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    do {
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
                  request->pid());
            response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            break;
        }
        if (!table->IsLeader()) {
            PDLOG(WARNING, "table is follower. tid %u, pid %u", request->tid(),
                  request->pid());
            response->set_code(::fedb::base::ReturnCode::kTableIsFollower);
            response->set_msg("table is follower");
            break;
        }
        std::shared_ptr<LogReplicator> replicator =
            GetReplicator(request->tid(), request->pid());
        if (!replicator) {
            response->set_code(
                ::fedb::base::ReturnCode::kReplicatorIsNotExist);
            response->set_msg("replicator is not exist");
            PDLOG(WARNING, "replicator is not exist. tid %u, pid %u",
                  request->tid(), request->pid());
            break;
        }
        std::map<std::string, std::string> real_ep_map;
        real_ep_map.insert(std::make_pair(request->endpoint(), ""));
        if (FLAGS_use_name) {
            if (!GetRealEp(request->tid(), request->pid(), &real_ep_map)) {
                response->set_code(
                        ::fedb::base::ReturnCode::kServerNameNotFound);
                response->set_msg("name not found in real_ep_map");
                break;
            }
        }
        int ret = -1;
        if (request->has_remote_tid()) {
            ret = replicator->AddReplicateNode(real_ep_map,
                    request->remote_tid());
        } else {
            ret = replicator->AddReplicateNode(real_ep_map);
        }
        if (ret == 0) {
            response->set_code(::fedb::base::ReturnCode::kOk);
            response->set_msg("ok");
        } else if (ret < 0) {
            response->set_code(
                ::fedb::base::ReturnCode::kFailToAddReplicaEndpoint);
            PDLOG(WARNING, "fail to add replica endpoint. tid %u pid %u",
                  request->tid(), request->pid());
            response->set_msg("fail to add replica endpoint");
            break;
        } else {
            response->set_code(
                ::fedb::base::ReturnCode::kReplicaEndpointAlreadyExists);
            response->set_msg("replica endpoint already exists");
            PDLOG(WARNING, "replica endpoint already exists. tid %u pid %u",
                  request->tid(), request->pid());
        }
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::fedb::api::TaskStatus::kDone);
        }
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

void TabletImpl::DelReplica(RpcController* controller,
                            const ::fedb::api::ReplicaRequest* request,
                            ::fedb::api::GeneralResponse* response,
                            Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(), ::fedb::api::TaskType::kDelReplica,
                      task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    do {
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
                  request->pid());
            response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            break;
        }
        if (!table->IsLeader()) {
            PDLOG(WARNING, "table is follower. tid %u, pid %u", request->tid(),
                  request->pid());
            response->set_code(::fedb::base::ReturnCode::kTableIsFollower);
            response->set_msg("table is follower");
            break;
        }
        std::shared_ptr<LogReplicator> replicator =
            GetReplicator(request->tid(), request->pid());
        if (!replicator) {
            response->set_code(
                ::fedb::base::ReturnCode::kReplicatorIsNotExist);
            response->set_msg("replicator is not exist");
            PDLOG(WARNING, "replicator is not exist. tid %u, pid %u",
                  request->tid(), request->pid());
            break;
        }
        int ret = replicator->DelReplicateNode(request->endpoint());
        if (ret == 0) {
            response->set_code(::fedb::base::ReturnCode::kOk);
            response->set_msg("ok");
        } else if (ret < 0) {
            response->set_code(
                ::fedb::base::ReturnCode::kReplicatorRoleIsNotLeader);
            PDLOG(WARNING, "replicator role is not leader. table %u pid %u",
                  request->tid(), request->pid());
            response->set_msg("replicator role is not leader");
            break;
        } else {
            response->set_code(::fedb::base::ReturnCode::kOk);
            PDLOG(WARNING,
                  "fail to del endpoint for table %u pid %u. replica does not "
                  "exist",
                  request->tid(), request->pid());
            response->set_msg("replica does not exist");
        }
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::fedb::api::TaskStatus::kDone);
        }
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

void TabletImpl::AppendEntries(
    RpcController* controller,
    const ::fedb::api::AppendEntriesRequest* request,
    ::fedb::api::AppendEntriesResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
              request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (!follower_.load(std::memory_order_relaxed) && table->IsLeader()) {
        PDLOG(WARNING, "table is leader. tid %u, pid %u", request->tid(),
              request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsLeader);
        response->set_msg("table is leader");
        return;
    }
    if (table->GetTableStat() == ::fedb::storage::kLoading) {
        response->set_code(::fedb::base::ReturnCode::kTableIsLoading);
        response->set_msg("table is loading");
        PDLOG(WARNING, "table is loading. tid %u, pid %u", request->tid(),
              request->pid());
        return;
    }
    std::shared_ptr<LogReplicator> replicator =
        GetReplicator(request->tid(), request->pid());
    if (!replicator) {
        response->set_code(::fedb::base::ReturnCode::kReplicatorIsNotExist);
        response->set_msg("replicator is not exist");
        return;
    }
    bool ok = replicator->AppendEntries(request, response);
    if (!ok) {
        response->set_code(
            ::fedb::base::ReturnCode::kFailToAppendEntriesToReplicator);
        response->set_msg("fail to append entries to replicator");
    } else {
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
    }
}

void TabletImpl::GetTableSchema(
    RpcController* controller,
    const ::fedb::api::GetTableSchemaRequest* request,
    ::fedb::api::GetTableSchemaResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
              request->pid());
        return;
    } else {
        response->set_schema(table->GetSchema());
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
    response->set_schema(table->GetSchema());
    response->mutable_table_meta()->CopyFrom(table->GetTableMeta());
}

void TabletImpl::UpdateTableMetaForAddField(RpcController* controller,
    const ::fedb::api::UpdateTableMetaForAddFieldRequest* request,
    ::fedb::api::GeneralResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    std::map<uint32_t, std::shared_ptr<Table>> table_map;
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        auto it = tables_.find(tid);
        if (it == tables_.end()) {
            response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table doesn`t exist");
            PDLOG(WARNING, "table tid %u doesn`t exist.", tid);
            return;
        }
        table_map = it->second;
    }
    const std::string& schema = request->schema();
    for (auto pit = table_map.begin(); pit != table_map.end(); ++pit) {
        uint32_t pid = pit->first;
        std::shared_ptr<Table> table = pit->second;
        // judge if field exists
        ::fedb::api::TableMeta table_meta;
        table_meta.CopyFrom(table->GetTableMeta());
        if (request->has_column_desc()) {
            const auto& col = request->column_desc();
            if (table->CheckFieldExist(col.name())) {
                continue;
            }
            fedb::common::ColumnDesc* column_desc = table_meta.add_added_column_desc();
            column_desc->CopyFrom(col);
        } else {
            bool do_continue = false;
            const auto& cols = request->column_descs();
            for (const auto& col : cols) {
                if (table->CheckFieldExist(col.name())) {
                    do_continue = true;
                    break;
                }
            }
            if (do_continue) {
                continue;
            }
            for (const auto& col : cols) {
                fedb::common::ColumnDesc* column_desc = table_meta.add_added_column_desc();
                column_desc->CopyFrom(col);
            }
        }
        if (request->has_version_pair()) {
            fedb::common::VersionPair* pair = table_meta.add_schema_versions();
            pair->CopyFrom(request->version_pair());
            LOG(INFO) << "add version pair";
        }
        table_meta.set_schema(schema);
        table->SetTableMeta(table_meta);
        table->SetSchema(schema);
        // update TableMeta.txt
        std::string db_root_path;
        bool ok = ChooseDBRootPath(tid, pid, db_root_path);
        if (!ok) {
            response->set_code(ReturnCode::kFailToGetDbRootPath);
            response->set_msg("fail to get db root path");
            LOG(WARNING) << "fail to get table db root path for tid " << tid << " pid " << pid;
            return;
        }
        std::string db_path = db_root_path + "/" + std::to_string(tid) + "_" +
                              std::to_string(pid);
        if (!::fedb::base::IsExists(db_path)) {
            LOG(WARNING) << "table db path doesn't exist. tid " << tid << " pid " << pid;
            response->set_code(ReturnCode::kTableDbPathIsNotExist);
            response->set_msg("table db path is not exist");
            return;
        }
        UpdateTableMeta(db_path, &table_meta, true);
        if (WriteTableMeta(db_path, &table_meta) < 0) {
            LOG(WARNING) << " write table_meta failed. tid[" << tid << "] pid [" << pid << "]";
            response->set_code(ReturnCode::kWriteDataFailed);
            response->set_msg("write data failed");
            return;
        }
        LOG(INFO) << "success update table meta for table " << tid << " pid " << pid;
    }
    response->set_code(ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::GetTableStatus(
    RpcController* controller,
    const ::fedb::api::GetTableStatusRequest* request,
    ::fedb::api::GetTableStatusResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    for (auto it = tables_.begin(); it != tables_.end(); ++it) {
        if (request->has_tid() && request->tid() != it->first) {
            continue;
        }
        for (auto pit = it->second.begin(); pit != it->second.end(); ++pit) {
            if (request->has_pid() && request->pid() != pit->first) {
                continue;
            }
            std::shared_ptr<Table> table = pit->second;
            ::fedb::api::TableStatus* status =
                response->add_all_table_status();
            status->set_mode(::fedb::api::TableMode::kTableFollower);
            if (table->IsLeader()) {
                status->set_mode(::fedb::api::TableMode::kTableLeader);
            }
            status->set_tid(table->GetId());
            status->set_pid(table->GetPid());
            status->set_compress_type(table->GetCompressType());
            status->set_name(table->GetName());
            ::fedb::api::TTLDesc* ttl_desc = status->mutable_ttl_desc();
            ::fedb::storage::TTLSt ttl = table->GetTTL();
            ttl_desc->set_abs_ttl(ttl.abs_ttl / (60 * 1000));
            ttl_desc->set_lat_ttl(ttl.lat_ttl);
            ttl_desc->set_ttl_type(ttl.GetTabletTTLType());
            status->set_ttl_type(ttl.GetTabletTTLType());
            status->set_diskused(table->GetDiskused());
            if (status->ttl_type() == ::fedb::api::TTLType::kLatestTime) {
                status->set_ttl(table->GetTTL().lat_ttl);
            } else {
                status->set_ttl(table->GetTTL().abs_ttl / (60 * 1000));
            }
            if (::fedb::api::TableState_IsValid(table->GetTableStat())) {
                status->set_state(
                    ::fedb::api::TableState(table->GetTableStat()));
            }
            std::shared_ptr<LogReplicator> replicator =
                GetReplicatorUnLock(table->GetId(), table->GetPid());
            if (replicator) {
                status->set_offset(replicator->GetOffset());
            }
            status->set_record_cnt(table->GetRecordCnt());
            if (MemTable* mem_table =
                    dynamic_cast<MemTable*>(table.get())) {
                status->set_is_expire(mem_table->GetExpireStatus());
                status->set_record_byte_size(
                        mem_table->GetRecordByteSize());
                status->set_record_idx_byte_size(
                        mem_table->GetRecordIdxByteSize());
                status->set_record_pk_cnt(mem_table->GetRecordPkCnt());
                status->set_skiplist_height(mem_table->GetKeyEntryHeight());
                uint64_t record_idx_cnt = 0;
                auto indexs = table->GetAllIndex();
                for (const auto& index_def : indexs) {
                    ::fedb::api::TsIdxStatus* ts_idx_status =
                        status->add_ts_idx_status();
                    ts_idx_status->set_idx_name(index_def->GetName());
                    uint64_t* stats = NULL;
                    uint32_t size = 0;
                    bool ok = mem_table->GetRecordIdxCnt(index_def->GetId(),
                            &stats, &size);
                    if (ok) {
                        for (uint32_t i = 0; i < size; i++) {
                            ts_idx_status->add_seg_cnts(stats[i]);
                            record_idx_cnt += stats[i];
                        }
                    }
                    delete []stats;
                }
                status->set_idx_cnt(record_idx_cnt);
            }
            if (request->has_need_schema() && request->need_schema()) {
                status->set_schema(table->GetSchema());
            }
        }
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
}

void TabletImpl::SetExpire(RpcController* controller,
                           const ::fedb::api::SetExpireRequest* request,
                           ::fedb::api::GeneralResponse* response,
                           Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
              request->pid());
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    MemTable* mem_table = dynamic_cast<MemTable*>(table.get());
    if (mem_table != NULL) {
        mem_table->SetExpire(request->is_expire());
        PDLOG(INFO, "set table expire[%d]. tid[%u] pid[%u]",
                request->is_expire(), request->tid(), request->pid());
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::MakeSnapshotInternal(
    uint32_t tid, uint32_t pid, uint64_t end_offset,
    std::shared_ptr<::fedb::api::TaskInfo> task) {
    PDLOG(INFO, "MakeSnapshotInternal begin, tid[%u] pid[%u]", tid, pid);
    std::shared_ptr<Table> table;
    std::shared_ptr<Snapshot> snapshot;
    std::shared_ptr<LogReplicator> replicator;
    bool has_error = true;
    do {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        table = GetTableUnLock(tid, pid);
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid[%u] pid[%u]", tid, pid);
            break;
        }
        if (table->GetTableStat() != ::fedb::storage::kNormal) {
            PDLOG(WARNING,
                  "table state is %d, cannot make snapshot. %u, pid %u",
                  table->GetTableStat(), tid, pid);
            break;
        }
        snapshot = GetSnapshotUnLock(tid, pid);
        if (!snapshot) {
            PDLOG(WARNING, "snapshot is not exist. tid[%u] pid[%u]", tid, pid);
            break;
        }
        replicator = GetReplicatorUnLock(tid, pid);
        if (!replicator) {
            PDLOG(WARNING, "replicator is not exist. tid[%u] pid[%u]", tid,
                  pid);
            break;
        }
        has_error = false;
    } while (0);
    if (has_error) {
        if (task) {
            std::lock_guard<std::mutex> lock(mu_);
            task->set_status(::fedb::api::kFailed);
        }
        return;
    }
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        table->SetTableStat(::fedb::storage::kMakingSnapshot);
    }
    uint64_t cur_offset = replicator->GetOffset();
    uint64_t snapshot_offset = snapshot->GetOffset();
    int ret = 0;
    if (cur_offset < snapshot_offset + FLAGS_make_snapshot_threshold_offset &&
        end_offset == 0) {
        PDLOG(INFO,
              "offset can't reach the threshold. tid[%u] pid[%u] "
              "cur_offset[%lu], snapshot_offset[%lu] end_offset[%lu]",
              tid, pid, cur_offset, snapshot_offset, end_offset);
    } else {
        uint64_t offset = 0;
        ret = snapshot->MakeSnapshot(table, offset, end_offset);
        if (ret == 0) {
            std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
            if (replicator) {
                replicator->SetSnapshotLogPartIndex(offset);
            }
        }
    }
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        table->SetTableStat(::fedb::storage::kNormal);
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (task) {
            if (ret == 0) {
                task->set_status(::fedb::api::kDone);
                auto right_now =
                    std::chrono::system_clock::now().time_since_epoch();
                int64_t ts =
                    std::chrono::duration_cast<std::chrono::seconds>(
                            right_now)
                    .count();
                table->SetMakeSnapshotTime(ts);
            } else {
                task->set_status(::fedb::api::kFailed);
            }
        }
    }
    PDLOG(INFO, "MakeSnapshotInternal finish, tid[%u] pid[%u]", tid, pid);
}

void TabletImpl::MakeSnapshot(RpcController* controller,
                              const ::fedb::api::GeneralRequest* request,
                              ::fedb::api::GeneralResponse* response,
                              Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(),
                      ::fedb::api::TaskType::kMakeSnapshot, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    uint64_t offset = 0;
    if (request->has_offset() && request->offset() > 0) {
        offset = request->offset();
    }
    do {
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            std::shared_ptr<Snapshot> snapshot = GetSnapshotUnLock(tid, pid);
            if (!snapshot) {
                response->set_code(
                    ::fedb::base::ReturnCode::kSnapshotIsNotExist);
                response->set_msg("snapshot is not exist");
                PDLOG(WARNING, "snapshot is not exist. tid[%u] pid[%u]", tid,
                      pid);
                break;
            }
            std::shared_ptr<Table> table =
                GetTableUnLock(request->tid(), request->pid());
            if (!table) {
                PDLOG(WARNING, "table is not exist. tid %u, pid %u", tid, pid);
                response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
                response->set_msg("table is not exist");
                break;
            }
            if (table->GetTableStat() != ::fedb::storage::kNormal) {
                response->set_code(
                    ::fedb::base::ReturnCode::kTableStatusIsNotKnormal);
                response->set_msg("table status is not kNormal");
                PDLOG(WARNING,
                      "table state is %d, cannot make snapshot. %u, pid %u",
                      table->GetTableStat(), tid, pid);
                break;
            }
        }
        snapshot_pool_.AddTask(boost::bind(&TabletImpl::MakeSnapshotInternal,
                                           this, tid, pid, offset, task_ptr));
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

void TabletImpl::SchedMakeSnapshot() {
    int now_hour = ::fedb::base::GetNowHour();
    if (now_hour != FLAGS_make_snapshot_time) {
        snapshot_pool_.DelayTask(
            FLAGS_make_snapshot_check_interval,
            boost::bind(&TabletImpl::SchedMakeSnapshot, this));
        return;
    }
    std::vector<std::pair<uint32_t, uint32_t>> table_set;
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        auto right_now = std::chrono::system_clock::now().time_since_epoch();
        int64_t ts =
            std::chrono::duration_cast<std::chrono::seconds>(right_now).count();
        for (auto iter = tables_.begin(); iter != tables_.end(); ++iter) {
            for (auto inner = iter->second.begin(); inner != iter->second.end();
                 ++inner) {
                if (iter->first == 0 && inner->first == 0) {
                    continue;
                }
                if (ts - inner->second->GetMakeSnapshotTime() <=
                        FLAGS_make_snapshot_offline_interval &&
                        !zk_cluster_.empty()) {
                    continue;
                }
                table_set.push_back(
                        std::make_pair(iter->first, inner->first));
            }
        }
    }
    for (auto iter = table_set.begin(); iter != table_set.end(); ++iter) {
        PDLOG(INFO, "start make snapshot tid[%u] pid[%u]", iter->first,
              iter->second);
        MakeSnapshotInternal(iter->first, iter->second, 0,
                             std::shared_ptr<::fedb::api::TaskInfo>());
    }
    // delay task one hour later avoid execute  more than one time
    snapshot_pool_.DelayTask(
        FLAGS_make_snapshot_check_interval + 60 * 60 * 1000,
        boost::bind(&TabletImpl::SchedMakeSnapshot, this));
}

void TabletImpl::SendData(RpcController* controller,
                          const ::fedb::api::SendDataRequest* request,
                          ::fedb::api::GeneralResponse* response,
                          Closure* done) {
    brpc::ClosureGuard done_guard(done);
    brpc::Controller* cntl = static_cast<brpc::Controller*>(controller);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, db_root_path);
    if (!ok) {
        response->set_code(::fedb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to get db root path");
        PDLOG(WARNING, "fail to get table db root path for tid %u, pid %u", tid,
              pid);
        return;
    }
    std::string combine_key = std::to_string(tid) + "_" + std::to_string(pid) +
                              "_" + request->file_name();
    std::shared_ptr<FileReceiver> receiver;
    std::shared_ptr<Table> table;
    if (request->block_id() == 0) {
        table = GetTable(tid, pid);
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto iter = file_receiver_map_.find(combine_key);
        if (request->block_id() == 0) {
            if (table && request->dir_name() != "index") {
                PDLOG(WARNING, "table already exists. tid %u, pid %u", tid,
                      pid);
                response->set_code(
                    ::fedb::base::ReturnCode::kTableAlreadyExists);
                response->set_msg("table already exists");
                return;
            }
            if (iter == file_receiver_map_.end()) {
                std::string path = db_root_path + "/" + std::to_string(tid) +
                                   "_" + std::to_string(pid) + "/";
                std::string dir_name;
                if (request->has_dir_name() && request->dir_name().size() > 0) {
                    dir_name = request->dir_name();
                    if (dir_name != "index") {
                        path.append("snapshot/");
                    }
                    path.append(request->dir_name() + "/");
                } else if (request->file_name() != "table_meta.txt") {
                    path.append("snapshot/");
                }
                file_receiver_map_.insert(std::make_pair(
                    combine_key, std::make_shared<FileReceiver>(
                                     request->file_name(), dir_name, path)));
                iter = file_receiver_map_.find(combine_key);
            }
            if (!iter->second->Init()) {
                PDLOG(WARNING,
                      "file receiver init failed. tid %u, pid %u, file_name %s",
                      tid, pid, request->file_name().c_str());
                response->set_code(
                    ::fedb::base::ReturnCode::kFileReceiverInitFailed);
                response->set_msg("file receiver init failed");
                file_receiver_map_.erase(iter);
                return;
            }
            PDLOG(INFO, "file receiver init ok. tid %u, pid %u, file_name %s",
                  tid, pid, request->file_name().c_str());
            response->set_code(::fedb::base::ReturnCode::kOk);
            response->set_msg("ok");
        } else if (iter == file_receiver_map_.end()) {
            PDLOG(WARNING, "cannot find receiver. tid %u, pid %u, file_name %s",
                  tid, pid, request->file_name().c_str());
            response->set_code(::fedb::base::ReturnCode::kCannotFindReceiver);
            response->set_msg("cannot find receiver");
            return;
        }
        receiver = iter->second;
    }
    if (!receiver) {
        PDLOG(WARNING, "cannot find receiver. tid %u, pid %u, file_name %s",
              tid, pid, request->file_name().c_str());
        response->set_code(::fedb::base::ReturnCode::kCannotFindReceiver);
        response->set_msg("cannot find receiver");
        return;
    }
    if (receiver->GetBlockId() == request->block_id()) {
        response->set_msg("ok");
        response->set_code(::fedb::base::ReturnCode::kOk);
        return;
    }
    if (request->block_id() != receiver->GetBlockId() + 1) {
        response->set_msg("block_id mismatch");
        PDLOG(WARNING,
              "block_id mismatch. tid %u, pid %u, file_name %s, request "
              "block_id %lu cur block_id %lu",
              tid, pid, request->file_name().c_str(), request->block_id(),
              receiver->GetBlockId());
        response->set_code(::fedb::base::ReturnCode::kBlockIdMismatch);
        return;
    }
    std::string data = cntl->request_attachment().to_string();
    if (data.length() != request->block_size()) {
        PDLOG(WARNING,
              "receive data error. tid %u, pid %u, file_name %s, expected "
              "length %u real length %u",
              tid, pid, request->file_name().c_str(), request->block_size(),
              data.length());
        response->set_code(::fedb::base::ReturnCode::kReceiveDataError);
        response->set_msg("receive data error");
        return;
    }
    if (receiver->WriteData(data, request->block_id()) < 0) {
        PDLOG(WARNING,
              "receiver write data failed. tid %u, pid %u, file_name %s", tid,
              pid, request->file_name().c_str());
        response->set_code(::fedb::base::ReturnCode::kWriteDataFailed);
        response->set_msg("write data failed");
        return;
    }
    if (request->eof()) {
        receiver->SaveFile();
        std::lock_guard<std::mutex> lock(mu_);
        file_receiver_map_.erase(combine_key);
    }
    response->set_msg("ok");
    response->set_code(::fedb::base::ReturnCode::kOk);
}

void TabletImpl::SendSnapshot(RpcController* controller,
                              const ::fedb::api::SendSnapshotRequest* request,
                              ::fedb::api::GeneralResponse* response,
                              Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(),
                      ::fedb::api::TaskType::kSendSnapshot, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::string sync_snapshot_key = request->endpoint() + "_" +
                                    std::to_string(tid) + "_" +
                                    std::to_string(pid);
    do {
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            std::shared_ptr<Table> table = GetTableUnLock(tid, pid);
            if (!table) {
                PDLOG(WARNING, "table is not exist. tid %u, pid %u", tid, pid);
                response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
                response->set_msg("table is not exist");
                break;
            }
            if (!table->IsLeader()) {
                PDLOG(WARNING, "table is follower. tid %u, pid %u", tid, pid);
                response->set_code(::fedb::base::ReturnCode::kTableIsFollower);
                response->set_msg("table is follower");
                break;
            }
            if (table->GetTableStat() != ::fedb::storage::kSnapshotPaused) {
                PDLOG(WARNING,
                      "table status is not kSnapshotPaused. tid %u, pid %u",
                      tid, pid);
                response->set_code(::fedb::base::ReturnCode::
                                       kTableStatusIsNotKsnapshotpaused);
                response->set_msg("table status is not kSnapshotPaused");
                break;
            }
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (sync_snapshot_set_.find(sync_snapshot_key) !=
            sync_snapshot_set_.end()) {
            PDLOG(WARNING, "snapshot is sending. tid %u pid %u endpoint %s",
                  tid, pid, request->endpoint().c_str());
            response->set_code(::fedb::base::ReturnCode::kSnapshotIsSending);
            response->set_msg("snapshot is sending");
            break;
        }
        sync_snapshot_set_.insert(sync_snapshot_key);
        task_pool_.AddTask(boost::bind(&TabletImpl::SendSnapshotInternal, this,
                                       request->endpoint(), tid, pid,
                                       request->remote_tid(), task_ptr));
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

void TabletImpl::SendSnapshotInternal(
    const std::string& endpoint, uint32_t tid, uint32_t pid,
    uint32_t remote_tid, std::shared_ptr<::fedb::api::TaskInfo> task) {
    bool has_error = true;
    do {
        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", tid, pid);
            break;
        }
        std::string db_root_path;
        bool ok =
            ChooseDBRootPath(tid, pid, db_root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to get db root path for table tid %u, pid %u",
                  tid, pid);
            break;
        }
        std::string real_endpoint = endpoint;
        if (FLAGS_use_name) {
            auto tmp_map = std::atomic_load_explicit(&real_ep_map_,
                    std::memory_order_acquire);
            auto iter = tmp_map->find(endpoint);
            if (iter == tmp_map->end()) {
                PDLOG(WARNING, "name %s not found in real_ep_map."
                        "tid[%u] pid[%u]", endpoint.c_str(), tid, pid);
                break;
            }
            real_endpoint = iter->second;
        }
        FileSender sender(remote_tid, pid, real_endpoint);
        if (!sender.Init()) {
            PDLOG(WARNING,
                  "Init FileSender failed. tid[%u] pid[%u] endpoint[%s]", tid,
                  pid, endpoint.c_str());
            break;
        }
        // send table_meta file
        std::string full_path = db_root_path + "/" + std::to_string(tid) + "_" +
                                std::to_string(pid) + "/";
        std::string file_name = "table_meta.txt";
        if (sender.SendFile(file_name, full_path + file_name) < 0) {
            PDLOG(WARNING, "send table_meta.txt failed. tid[%u] pid[%u]", tid,
                  pid);
            break;
        }
        full_path.append("snapshot/");
        std::string manifest_file = full_path + "MANIFEST";
        std::string snapshot_file;
        {
            int fd = open(manifest_file.c_str(), O_RDONLY);
            if (fd < 0) {
                PDLOG(WARNING, "[%s] is not exist", manifest_file.c_str());
                has_error = false;
                break;
            }
            google::protobuf::io::FileInputStream fileInput(fd);
            fileInput.SetCloseOnDelete(true);
            ::fedb::api::Manifest manifest;
            if (!google::protobuf::TextFormat::Parse(&fileInput, &manifest)) {
                PDLOG(WARNING, "parse manifest failed. tid[%u] pid[%u]", tid,
                      pid);
                break;
            }
            snapshot_file = manifest.name();
        }
        // send snapshot file
        if (sender.SendFile(snapshot_file, full_path + snapshot_file) < 0) {
            PDLOG(WARNING, "send snapshot failed. tid[%u] pid[%u]", tid,
                    pid);
            break;
        }
        // send manifest file
        file_name = "MANIFEST";
        if (sender.SendFile(file_name, full_path + file_name) < 0) {
            PDLOG(WARNING, "send MANIFEST failed. tid[%u] pid[%u]", tid, pid);
            break;
        }
        has_error = false;
        PDLOG(INFO, "send snapshot success. endpoint %s tid %u pid %u",
              endpoint.c_str(), tid, pid);
    } while (0);
    std::lock_guard<std::mutex> lock(mu_);
    if (task) {
        if (has_error) {
            task->set_status(::fedb::api::kFailed);
        } else {
            task->set_status(::fedb::api::kDone);
        }
    }
    std::string sync_snapshot_key =
        endpoint + "_" + std::to_string(tid) + "_" + std::to_string(pid);
    sync_snapshot_set_.erase(sync_snapshot_key);
}

void TabletImpl::PauseSnapshot(RpcController* controller,
                               const ::fedb::api::GeneralRequest* request,
                               ::fedb::api::GeneralResponse* response,
                               Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(),
                      ::fedb::api::TaskType::kPauseSnapshot, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    do {
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            std::shared_ptr<Table> table =
                GetTableUnLock(request->tid(), request->pid());
            if (!table) {
                PDLOG(WARNING, "table is not exist. tid %u, pid %u",
                      request->tid(), request->pid());
                response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
                response->set_msg("table is not exist");
                break;
            }
            if (table->GetTableStat() == ::fedb::storage::kSnapshotPaused) {
                PDLOG(INFO,
                      "table status is kSnapshotPaused, need not pause. "
                      "tid[%u] pid[%u]",
                      request->tid(), request->pid());
            } else if (table->GetTableStat() != ::fedb::storage::kNormal) {
                PDLOG(WARNING,
                      "table status is [%u], cann't pause. tid[%u] pid[%u]",
                      table->GetTableStat(), request->tid(), request->pid());
                response->set_code(
                    ::fedb::base::ReturnCode::kTableStatusIsNotKnormal);
                response->set_msg("table status is not kNormal");
                break;
            } else {
                table->SetTableStat(::fedb::storage::kSnapshotPaused);
                PDLOG(INFO, "table status has set[%u]. tid[%u] pid[%u]",
                      table->GetTableStat(), request->tid(), request->pid());
            }
        }
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::fedb::api::TaskStatus::kDone);
        }
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

void TabletImpl::RecoverSnapshot(RpcController* controller,
                                 const ::fedb::api::GeneralRequest* request,
                                 ::fedb::api::GeneralResponse* response,
                                 Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(),
                      ::fedb::api::TaskType::kRecoverSnapshot, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    do {
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            std::shared_ptr<Table> table =
                GetTableUnLock(request->tid(), request->pid());
            if (!table) {
                PDLOG(WARNING, "table is not exist. tid %u, pid %u",
                      request->tid(), request->pid());
                response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
                response->set_msg("table is not exist");
                break;
            }
            if (table->GetTableStat() == fedb::storage::kNormal) {
                PDLOG(INFO,
                      "table status is already kNormal, need not recover. "
                      "tid[%u] pid[%u]",
                      request->tid(), request->pid());

            } else if (table->GetTableStat() !=
                       ::fedb::storage::kSnapshotPaused) {
                PDLOG(WARNING,
                      "table status is [%u], cann't recover. tid[%u] pid[%u]",
                      table->GetTableStat(), request->tid(), request->pid());
                response->set_code(::fedb::base::ReturnCode::
                                       kTableStatusIsNotKsnapshotpaused);
                response->set_msg("table status is not kSnapshotPaused");
                break;
            } else {
                table->SetTableStat(::fedb::storage::kNormal);
                PDLOG(INFO, "table status has set[%u]. tid[%u] pid[%u]",
                      table->GetTableStat(), request->tid(), request->pid());
            }
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (task_ptr) {
            task_ptr->set_status(::fedb::api::TaskStatus::kDone);
        }
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

void TabletImpl::LoadTable(RpcController* controller,
                           const ::fedb::api::LoadTableRequest* request,
                           ::fedb::api::GeneralResponse* response,
                           Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(), ::fedb::api::TaskType::kLoadTable,
                      task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    do {
        ::fedb::api::TableMeta table_meta;
        table_meta.CopyFrom(request->table_meta());
        uint32_t tid = table_meta.tid();
        uint32_t pid = table_meta.pid();
        std::string msg;
        if (CheckTableMeta(&table_meta, msg) != 0) {
            response->set_code(
                    ::fedb::base::ReturnCode::kTableMetaIsIllegal);
            response->set_msg(msg);
            PDLOG(WARNING, "CheckTableMeta failed. tid %u, pid %u", tid,
                    pid);
            break;
        }
        std::string root_path;
        bool ok =
            ChooseDBRootPath(tid, pid, root_path);
        if (!ok) {
            response->set_code(::fedb::base::ReturnCode::kFailToGetDbRootPath);
            response->set_msg("fail to get table db root path");
            PDLOG(WARNING, "table db path is not found. tid %u, pid %u", tid,
                  pid);
            break;
        }

        std::string db_path =
            root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);
        if (!::fedb::base::IsExists(db_path)) {
            PDLOG(WARNING,
                  "table db path is not exist. tid %u, pid %u, path %s", tid,
                  pid, db_path.c_str());
            response->set_code(
                ::fedb::base::ReturnCode::kTableDbPathIsNotExist);
            response->set_msg("table db path is not exist");
            break;
        }

        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (table) {
            PDLOG(WARNING, "table with tid[%u] and pid[%u] exists", tid, pid);
            response->set_code(::fedb::base::ReturnCode::kTableAlreadyExists);
            response->set_msg("table already exists");
            break;
        }

        UpdateTableMeta(db_path, &table_meta);
        if (WriteTableMeta(db_path, &table_meta) < 0) {
            PDLOG(WARNING, "write table_meta failed. tid[%lu] pid[%lu]", tid,
                  pid);
            response->set_code(::fedb::base::ReturnCode::kWriteDataFailed);
            response->set_msg("write data failed");
            break;
        }
        if (CreateTableInternal(&table_meta, msg) < 0) {
            response->set_code(
                    ::fedb::base::ReturnCode::kCreateTableFailed);
            response->set_msg(msg.c_str());
            break;
        }
        uint64_t ttl = table_meta.ttl();
        std::string name = table_meta.name();
        uint32_t seg_cnt = 8;
        if (table_meta.seg_cnt() > 0) {
            seg_cnt = table_meta.seg_cnt();
        }
        PDLOG(INFO,
                "start to recover table with id %u pid %u name %s seg_cnt %d "
                "idx_cnt %u schema_size %u ttl %llu",
                tid, pid, name.c_str(), seg_cnt, table_meta.dimensions_size(),
                table_meta.schema().size(), ttl);
        task_pool_.AddTask(boost::bind(&TabletImpl::LoadTableInternal, this,
                    tid, pid, task_ptr));
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

int TabletImpl::LoadTableInternal(
    uint32_t tid, uint32_t pid,
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr) {
    do {
        // load snapshot data
        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (!table) {
            PDLOG(WARNING, "table with tid %u and pid %u does not exist", tid,
                  pid);
            break;
        }
        std::shared_ptr<Snapshot> snapshot = GetSnapshot(tid, pid);
        if (!snapshot) {
            PDLOG(WARNING, "snapshot with tid %u and pid %u does not exist",
                  tid, pid);
            break;
        }
        std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
        if (!replicator) {
            PDLOG(WARNING, "replicator with tid %u and pid %u does not exist",
                  tid, pid);
            break;
        }
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            table->SetTableStat(::fedb::storage::kLoading);
        }
        uint64_t latest_offset = 0;
        uint64_t snapshot_offset = 0;
        std::string db_root_path;
        bool ok =
            ChooseDBRootPath(tid, pid, db_root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to find db root path for table tid %u pid %u",
                  tid, pid);
            break;
        }
        std::string binlog_path = db_root_path + "/" + std::to_string(tid) +
                                  "_" + std::to_string(pid) + "/binlog/";
        ::fedb::storage::Binlog binlog(replicator->GetLogPart(), binlog_path);
        if (snapshot->Recover(table, snapshot_offset) &&
            binlog.RecoverFromBinlog(table, snapshot_offset, latest_offset)) {
            table->SetTableStat(::fedb::storage::kNormal);
            replicator->SetOffset(latest_offset);
            replicator->SetSnapshotLogPartIndex(snapshot->GetOffset());
            replicator->StartSyncing();
            table->SchedGc();
            gc_pool_.DelayTask(
                FLAGS_gc_interval * 60 * 1000,
                boost::bind(&TabletImpl::GcTable, this, tid, pid, false));
            io_pool_.DelayTask(
                FLAGS_binlog_sync_to_disk_interval,
                boost::bind(&TabletImpl::SchedSyncDisk, this, tid, pid));
            task_pool_.DelayTask(
                FLAGS_binlog_delete_interval,
                boost::bind(&TabletImpl::SchedDelBinlog, this, tid, pid));
            PDLOG(INFO, "load table success. tid %u pid %u", tid, pid);
            if (task_ptr) {
                std::lock_guard<std::mutex> lock(mu_);
                task_ptr->set_status(::fedb::api::TaskStatus::kDone);
                return 0;
            }
        } else {
            DeleteTableInternal(tid, pid,
                                std::shared_ptr<::fedb::api::TaskInfo>());
        }
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
    return -1;
}

int32_t TabletImpl::DeleteTableInternal(
    uint32_t tid, uint32_t pid,
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr) {
    std::string root_path;
    std::string recycle_bin_root_path;
    int32_t code = -1;
    do {
        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u pid %u", tid, pid);
            break;
        }
        bool ok =
            ChooseDBRootPath(tid, pid, root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to get db root path. tid %u pid %u", tid, pid);
            break;
        }
        ok = ChooseRecycleBinRootPath(tid, pid, recycle_bin_root_path);
        if (!ok) {
            PDLOG(WARNING, "fail to get recycle bin root path. tid %u pid %u",
                  tid, pid);
            break;
        }
        std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            engine_->ClearCacheLocked(table->GetTableMeta().db());
            tables_[tid].erase(pid);
            replicators_[tid].erase(pid);
            snapshots_[tid].erase(pid);
            if (tables_[tid].empty()) {
                tables_.erase(tid);
            }
            if (replicators_[tid].empty()) {
                replicators_.erase(tid);
            }
            if (snapshots_[tid].empty()) {
                snapshots_.erase(tid);
            }
        }
        if (replicator) {
            replicator->DelAllReplicateNode();
            PDLOG(INFO, "drop replicator for tid %u, pid %u", tid, pid);
        }
        if (!table->GetDB().empty()) {
            catalog_->DeleteTable(table->GetDB(), table->GetName(), pid);
        }
        code = 0;
    } while (0);
    if (code < 0) {
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::fedb::api::TaskStatus::kFailed);
        }
        return code;
    }

    std::string source_path =
        root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);
    if (!::fedb::base::IsExists(source_path)) {
        if (task_ptr) {
            std::lock_guard<std::mutex> lock(mu_);
            task_ptr->set_status(::fedb::api::TaskStatus::kDone);
        }
        PDLOG(INFO, "drop table ok. tid[%u] pid[%u]", tid, pid);
        return 0;
    }

    if (FLAGS_recycle_bin_enabled) {
        std::string recycle_path =
            recycle_bin_root_path + "/" + std::to_string(tid) + "_" +
            std::to_string(pid) + "_" + ::fedb::base::GetNowTime();
        ::fedb::base::Rename(source_path, recycle_path);
    } else {
        ::fedb::base::RemoveDirRecursive(source_path);
    }

    if (task_ptr) {
        std::lock_guard<std::mutex> lock(mu_);
        task_ptr->set_status(::fedb::api::TaskStatus::kDone);
    }
    PDLOG(INFO, "drop table ok. tid[%u] pid[%u]", tid, pid);
    return 0;
}

void TabletImpl::CreateTable(RpcController* controller,
                             const ::fedb::api::CreateTableRequest* request,
                             ::fedb::api::CreateTableResponse* response,
                             Closure* done) {
    brpc::ClosureGuard done_guard(done);
    const ::fedb::api::TableMeta* table_meta = &request->table_meta();
    std::string msg;
    uint32_t tid = table_meta->tid();
    uint32_t pid = table_meta->pid();
    if (CheckTableMeta(table_meta, msg) != 0) {
        response->set_code(::fedb::base::ReturnCode::kTableMetaIsIllegal);
        response->set_msg(msg);
        PDLOG(WARNING,
                "check table_meta failed. tid[%u] pid[%u], err_msg[%s]", tid,
                pid, msg.c_str());
        return;
    }
    std::shared_ptr<Table> table = GetTable(tid, pid);
    std::shared_ptr<Snapshot> snapshot = GetSnapshot(tid, pid);
    if (table || snapshot) {
        if (table) {
            PDLOG(WARNING, "table with tid[%u] and pid[%u] exists", tid,
                    pid);
        }
        if (snapshot) {
            PDLOG(WARNING, "snapshot with tid[%u] and pid[%u] exists", tid,
                    pid);
        }
        response->set_code(::fedb::base::ReturnCode::kTableAlreadyExists);
        response->set_msg("table already exists");
        return;
    }
    std::string name = table_meta->name();
    PDLOG(INFO, "start creating table tid[%u] pid[%u] with mode %s", tid, pid,
          ::fedb::api::TableMode_Name(request->table_meta().mode()).c_str());
    std::string db_root_path;
    bool ok =
        ChooseDBRootPath(tid, pid, db_root_path);
    if (!ok) {
        PDLOG(WARNING, "fail to find db root path tid[%u] pid[%u]", tid, pid);
        response->set_code(::fedb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to find db root path");
        return;
    }
    std::string table_db_path =
        db_root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);

    if (WriteTableMeta(table_db_path, table_meta) < 0) {
        PDLOG(WARNING, "write table_meta failed. tid[%u] pid[%u]", tid, pid);
        response->set_code(::fedb::base::ReturnCode::kWriteDataFailed);
        response->set_msg("write data failed");
        return;
    }
    if (CreateTableInternal(table_meta, msg) < 0) {
        response->set_code(::fedb::base::ReturnCode::kCreateTableFailed);
        response->set_msg(msg.c_str());
        return;
    }
    table = GetTable(tid, pid);
    if (!table) {
        response->set_code(::fedb::base::ReturnCode::kCreateTableFailed);
        response->set_msg("table is not exist");
        PDLOG(WARNING, "table with tid %u and pid %u does not exist", tid,
                pid);
        return;
    }
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (!replicator) {
        response->set_code(::fedb::base::ReturnCode::kCreateTableFailed);
        response->set_msg("replicator is not exist");
        PDLOG(WARNING, "replicator with tid %u and pid %u does not exist",
                tid, pid);
        return;
    }
    table->SetTableStat(::fedb::storage::kNormal);
    replicator->StartSyncing();
    io_pool_.DelayTask(
            FLAGS_binlog_sync_to_disk_interval,
            boost::bind(&TabletImpl::SchedSyncDisk, this, tid, pid));
    task_pool_.DelayTask(
            FLAGS_binlog_delete_interval,
            boost::bind(&TabletImpl::SchedDelBinlog, this, tid, pid));
    PDLOG(INFO,
            "create table with id %u pid %u name %s abs_ttl %llu lat_ttl "
            "%llu type %s",
            tid, pid, name.c_str(), table_meta->ttl_desc().abs_ttl(),
            table_meta->ttl_desc().lat_ttl(),
            ::fedb::api::TTLType_Name(table_meta->ttl_desc().ttl_type())
            .c_str());
    gc_pool_.DelayTask(
            FLAGS_gc_interval * 60 * 1000,
            boost::bind(&TabletImpl::GcTable, this, tid, pid, false));
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::ExecuteGc(RpcController* controller,
                           const ::fedb::api::ExecuteGcRequest* request,
                           ::fedb::api::GeneralResponse* response,
                           Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::shared_ptr<Table> table = GetTable(tid, pid);
    if (!table) {
        DEBUGLOG("table is not exist. tid %u pid %u", tid, pid);
        response->set_code(-1);
        response->set_msg("table not found");
        return;
    }
    gc_pool_.AddTask(boost::bind(&TabletImpl::GcTable, this, tid, pid, true));
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
    PDLOG(INFO, "ExecuteGc. tid %u pid %u", tid, pid);
}

void TabletImpl::GetTableFollower(
    RpcController* controller,
    const ::fedb::api::GetTableFollowerRequest* request,
    ::fedb::api::GetTableFollowerResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::shared_ptr<Table> table = GetTable(tid, pid);
    if (!table) {
        DEBUGLOG("table is not exist. tid %u pid %u", tid, pid);
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    if (!table->IsLeader()) {
        DEBUGLOG("table is follower. tid %u, pid %u", tid, pid);
        response->set_msg("table is follower");
        response->set_code(::fedb::base::ReturnCode::kTableIsFollower);
        return;
    }
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (!replicator) {
        DEBUGLOG("replicator is not exist. tid %u pid %u", tid, pid);
        response->set_msg("replicator is not exist");
        response->set_code(::fedb::base::ReturnCode::kReplicatorIsNotExist);
        return;
    }
    response->set_offset(replicator->GetOffset());
    std::map<std::string, uint64_t> info_map;
    replicator->GetReplicateInfo(info_map);
    if (info_map.empty()) {
        response->set_msg("has no follower");
        response->set_code(::fedb::base::ReturnCode::kNoFollower);
    }
    for (const auto& kv : info_map) {
        ::fedb::api::FollowerInfo* follower_info =
            response->add_follower_info();
        follower_info->set_endpoint(kv.first);
        follower_info->set_offset(kv.second);
    }
    response->set_msg("ok");
    response->set_code(::fedb::base::ReturnCode::kOk);
}

int32_t TabletImpl::GetSnapshotOffset(uint32_t tid, uint32_t pid,
                                      std::string& msg, uint64_t& term,
                                      uint64_t& offset) {
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, db_root_path);
    if (!ok) {
        msg = "fail to get db root path";
        PDLOG(WARNING, "fail to get table db root path");
        return 138;
    }
    std::string db_path =
        db_root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);
    std::string manifest_file = db_path + "/snapshot/MANIFEST";
    int fd = open(manifest_file.c_str(), O_RDONLY);
    if (fd < 0) {
        PDLOG(WARNING, "[%s] is not exist", manifest_file.c_str());
        return 0;
    }
    google::protobuf::io::FileInputStream fileInput(fd);
    fileInput.SetCloseOnDelete(true);
    ::fedb::api::Manifest manifest;
    if (!google::protobuf::TextFormat::Parse(&fileInput, &manifest)) {
        PDLOG(WARNING, "parse manifest failed");
        return 0;
    }
    std::string snapshot_file = db_path + "/snapshot/" + manifest.name();
    if (!::fedb::base::IsExists(snapshot_file)) {
        PDLOG(WARNING, "snapshot file[%s] is not exist", snapshot_file.c_str());
        return 0;
    }
    offset = manifest.offset();
    term = manifest.term();
    return 0;
}
void TabletImpl::GetAllSnapshotOffset(
    RpcController* controller, const ::fedb::api::EmptyRequest* request,
    ::fedb::api::TableSnapshotOffsetResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::map<uint32_t, std::vector<uint32_t>> tid_pid;
    {
        std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
        for (auto table_iter = tables_.begin(); table_iter != tables_.end();
             table_iter++) {
            if (table_iter->second.empty()) {
                continue;
            }
            uint32_t tid = table_iter->first;
            std::vector<uint32_t> pids;
            auto part_iter = table_iter->second.begin();
            for (; part_iter != table_iter->second.end(); part_iter++) {
                pids.push_back(part_iter->first);
            }
            tid_pid.insert(std::make_pair(tid, pids));
        }
    }
    std::string msg;
    for (auto iter = tid_pid.begin(); iter != tid_pid.end(); iter++) {
        uint32_t tid = iter->first;
        auto table = response->add_tables();
        table->set_tid(tid);
        for (auto pid : iter->second) {
            uint64_t term = 0, offset = 0;
            int32_t code = GetSnapshotOffset(tid, pid, msg, term, offset);
            if (code != 0) {
                continue;
            }
            auto partition = table->add_parts();
            partition->set_offset(offset);
            partition->set_pid(pid);
        }
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
}

void TabletImpl::GetCatalog(RpcController* controller,
                             const ::fedb::api::GetCatalogRequest* request,
                             ::fedb::api::GetCatalogResponse* response,
                             Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (catalog_) {
        ::fedb::common::CatalogInfo* catalog_info = response->mutable_catalog();
        catalog_info->set_version(catalog_->GetVersion());
        catalog_info->set_endpoint(FLAGS_endpoint);
        response->set_code(0);
        response->set_msg("ok");
        return;
    }
    response->set_code(-1);
    response->set_msg("catalog is not exist");
    PDLOG(WARNING, "catalog is not exist");
}

void TabletImpl::GetTermPair(RpcController* controller,
                             const ::fedb::api::GetTermPairRequest* request,
                             ::fedb::api::GetTermPairResponse* response,
                             Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (zk_cluster_.empty()) {
        response->set_code(-1);
        response->set_msg("tablet is not run in cluster mode");
        PDLOG(WARNING, "tablet is not run in cluster mode");
        return;
    }
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::shared_ptr<Table> table = GetTable(tid, pid);
    if (!table) {
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_has_table(false);
        response->set_msg("table is not exist");
        std::string msg;
        uint64_t term = 0, offset = 0;
        int32_t code = GetSnapshotOffset(tid, pid, msg, term, offset);
        response->set_code(code);
        if (code == 0) {
            response->set_term(term);
            response->set_offset(offset);
        } else {
            response->set_msg(msg);
        }
        return;
    }
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (!replicator) {
        response->set_code(::fedb::base::ReturnCode::kReplicatorIsNotExist);
        response->set_msg("replicator is not exist");
        return;
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
    response->set_has_table(true);
    if (table->IsLeader()) {
        response->set_is_leader(true);
    } else {
        response->set_is_leader(false);
    }
    response->set_term(replicator->GetLeaderTerm());
    response->set_offset(replicator->GetOffset());
}

void TabletImpl::DeleteBinlog(RpcController* controller,
                              const ::fedb::api::GeneralRequest* request,
                              ::fedb::api::GeneralResponse* response,
                              Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, db_root_path);
    if (!ok) {
        response->set_code(::fedb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to get db root path");
        PDLOG(WARNING, "fail to get table db root path");
        return;
    }
    std::string db_path =
        db_root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);
    std::string binlog_path = db_path + "/binlog";
    if (::fedb::base::IsExists(binlog_path)) {
        if (FLAGS_recycle_bin_enabled) {
            std::string recycle_bin_root_path;
            ok =
                ChooseRecycleBinRootPath(tid, pid, recycle_bin_root_path);
            if (!ok) {
                response->set_code(
                    ::fedb::base::ReturnCode::kFailToGetRecycleRootPath);
                response->set_msg("fail to get recycle root path");
                PDLOG(WARNING, "fail to get table recycle root path");
                return;
            }
            std::string recycle_path =
                recycle_bin_root_path + "/" + std::to_string(tid) + "_" +
                std::to_string(pid) + "_binlog_" + ::fedb::base::GetNowTime();
            ::fedb::base::Rename(binlog_path, recycle_path);
            PDLOG(INFO, "binlog has moved form %s to %s. tid %u pid %u",
                  binlog_path.c_str(), recycle_path.c_str(), tid, pid);
        } else {
            ::fedb::base::RemoveDirRecursive(binlog_path);
            PDLOG(INFO, "binlog %s has removed. tid %u pid %u",
                  binlog_path.c_str(), tid, pid);
        }
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::CheckFile(RpcController* controller,
                           const ::fedb::api::CheckFileRequest* request,
                           ::fedb::api::GeneralResponse* response,
                           Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, db_root_path);
    if (!ok) {
        response->set_code(::fedb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to get db root path");
        PDLOG(WARNING, "fail to get table db root path");
        return;
    }
    std::string file_name = request->file();
    std::string full_path = db_root_path + "/" + std::to_string(tid) + "_" +
                            std::to_string(pid) + "/";
    if (request->has_dir_name() && request->dir_name().size() > 0) {
        if (request->dir_name() != "index") {
            full_path.append("snapshot/");
        }
        full_path.append(request->dir_name() + "/");
    } else if (file_name != "table_meta.txt") {
        full_path.append("snapshot/");
    }
    full_path += file_name;
    uint64_t size = 0;
    if (!::fedb::base::GetFileSize(full_path, size)) {
        response->set_code(-1);
        response->set_msg("get size failed");
        PDLOG(WARNING, "get size failed. file[%s]", full_path.c_str());
        return;
    }
    if (size != request->size()) {
        response->set_code(-1);
        response->set_msg("check size failed");
        PDLOG(WARNING,
              "check size failed. file[%s] cur_size[%lu] expect_size[%lu]",
              full_path.c_str(), size, request->size());
        return;
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::GetManifest(RpcController* controller,
                             const ::fedb::api::GetManifestRequest* request,
                             ::fedb::api::GetManifestResponse* response,
                             Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::string db_root_path;
    bool ok =
        ChooseDBRootPath(request->tid(), request->pid(), db_root_path);
    if (!ok) {
        response->set_code(::fedb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to get db root path");
        PDLOG(WARNING, "fail to get table db root path");
        return;
    }
    std::string db_path = db_root_path + "/" + std::to_string(request->tid()) +
                          "_" + std::to_string(request->pid());
    std::string manifest_file = db_path + "/snapshot/MANIFEST";
    ::fedb::api::Manifest manifest;
    int fd = open(manifest_file.c_str(), O_RDONLY);
    if (fd >= 0) {
        google::protobuf::io::FileInputStream fileInput(fd);
        fileInput.SetCloseOnDelete(true);
        if (!google::protobuf::TextFormat::Parse(&fileInput, &manifest)) {
            PDLOG(WARNING, "parse manifest failed");
            response->set_code(-1);
            response->set_msg("parse manifest failed");
            return;
        }
    } else {
        PDLOG(INFO, "[%s] is not exist", manifest_file.c_str());
        manifest.set_offset(0);
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
    ::fedb::api::Manifest* manifest_r = response->mutable_manifest();
    manifest_r->CopyFrom(manifest);
}

int TabletImpl::WriteTableMeta(const std::string& path,
                               const ::fedb::api::TableMeta* table_meta) {
    if (!::fedb::base::MkdirRecur(path)) {
        PDLOG(WARNING, "fail to create path %s", path.c_str());
        return -1;
    }
    std::string full_path = path + "/table_meta.txt";
    std::string table_meta_info;
    google::protobuf::TextFormat::PrintToString(*table_meta, &table_meta_info);
    FILE* fd_write = fopen(full_path.c_str(), "w");
    if (fd_write == NULL) {
        PDLOG(WARNING, "fail to open file %s. err[%d: %s]", full_path.c_str(),
              errno, strerror(errno));
        return -1;
    }
    if (fputs(table_meta_info.c_str(), fd_write) == EOF) {
        PDLOG(WARNING, "write error. path[%s], err[%d: %s]", full_path.c_str(),
              errno, strerror(errno));
        fclose(fd_write);
        return -1;
    }
    fclose(fd_write);
    return 0;
}

int TabletImpl::UpdateTableMeta(const std::string& path,
                                ::fedb::api::TableMeta* table_meta,
                                bool for_add_column) {
    std::string full_path = path + "/table_meta.txt";
    int fd = open(full_path.c_str(), O_RDONLY);
    ::fedb::api::TableMeta old_meta;
    if (fd < 0) {
        PDLOG(WARNING, "[%s] is not exist", "table_meta.txt");
        return 1;
    } else {
        google::protobuf::io::FileInputStream fileInput(fd);
        fileInput.SetCloseOnDelete(true);
        if (!google::protobuf::TextFormat::Parse(&fileInput, &old_meta)) {
            PDLOG(WARNING, "parse table_meta failed");
            return -1;
        }
    }
    // use replicas in LoadRequest
    if (!for_add_column) {
        old_meta.clear_replicas();
        old_meta.MergeFrom(*table_meta);
        table_meta->CopyFrom(old_meta);
    }
    std::string new_name = full_path + "." + ::fedb::base::GetNowTime();
    rename(full_path.c_str(), new_name.c_str());
    return 0;
}

int TabletImpl::UpdateTableMeta(const std::string& path,
                                ::fedb::api::TableMeta* table_meta) {
    return UpdateTableMeta(path, table_meta, false);
}

int TabletImpl::CreateTableInternal(const ::fedb::api::TableMeta* table_meta,
                                    std::string& msg) {
    uint32_t tid = table_meta->tid();
    uint32_t pid = table_meta->pid();
    std::map<std::string, std::string> real_ep_map;
    for (int32_t i = 0; i < table_meta->replicas_size(); i++) {
        real_ep_map.insert(std::make_pair(table_meta->replicas(i), ""));
    }
    if (FLAGS_use_name) {
        if (!GetRealEp(tid, pid, &real_ep_map)) {
            msg.assign("name not found in real_ep_map");
            return -1;
        }
    }
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    std::shared_ptr<Table> table = GetTableUnLock(tid, pid);
    if (table) {
        PDLOG(WARNING, "table with tid[%u] and pid[%u] exists", tid, pid);
        msg.assign("table exists");
        return -1;
    }
    Table* table_ptr = new MemTable(*table_meta);
    table.reset(table_ptr);
    if (!table->Init()) {
        PDLOG(WARNING, "fail to init table. tid %u, pid %u", table_meta->tid(),
              table_meta->pid());
        msg.assign("fail to init table");
        return -1;
    }
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, db_root_path);
    if (!ok) {
        PDLOG(WARNING, "fail to get table db root path");
        msg.assign("fail to get table db root path");
        return -1;
    }
    std::string table_db_path = db_root_path + "/" +
                                std::to_string(table_meta->tid()) + "_" +
                                std::to_string(table_meta->pid());
    std::shared_ptr<LogReplicator> replicator;
    if (table->IsLeader()) {
        replicator = std::make_shared<LogReplicator>(
            table_db_path, real_ep_map, ReplicatorRole::kLeaderNode, table,
            &follower_);
    } else {
        replicator = std::make_shared<LogReplicator>(
            table_db_path, std::map<std::string, std::string>(),
            ReplicatorRole::kFollowerNode, table, &follower_);
    }
    if (!replicator) {
        PDLOG(WARNING, "fail to create replicator for table tid %u, pid %u",
              table_meta->tid(), table_meta->pid());
        msg.assign("fail create replicator for table");
        return -1;
    }
    ok = replicator->Init();
    if (!ok) {
        PDLOG(WARNING, "fail to init replicator for table tid %u, pid %u",
              table_meta->tid(), table_meta->pid());
        // clean memory
        msg.assign("fail init replicator for table");
        return -1;
    }
    if (!zk_cluster_.empty() &&
        table_meta->mode() == ::fedb::api::TableMode::kTableLeader) {
        replicator->SetLeaderTerm(table_meta->term());
    }
    ::fedb::storage::Snapshot* snapshot_ptr =
        new ::fedb::storage::MemTableSnapshot(
            table_meta->tid(), table_meta->pid(), replicator->GetLogPart(),
            db_root_path);

    if (!snapshot_ptr->Init()) {
        PDLOG(WARNING, "fail to init snapshot for tid %u, pid %u",
              table_meta->tid(), table_meta->pid());
        msg.assign("fail to init snapshot");
        return -1;
    }
    std::shared_ptr<Snapshot> snapshot(snapshot_ptr);
    tables_[table_meta->tid()].insert(std::make_pair(table_meta->pid(), table));
    snapshots_[table_meta->tid()].insert(
        std::make_pair(table_meta->pid(), snapshot));
    replicators_[table_meta->tid()].insert(
        std::make_pair(table_meta->pid(), replicator));
    if (!table_meta->db().empty()) {
        bool ok = catalog_->AddTable(*table_meta, table);
        engine_->ClearCacheLocked(table_meta->db());
        if (ok) {
            LOG(INFO) << "add table " << table_meta->name()
                << " to catalog with db " << table_meta->db();
        } else {
            LOG(WARNING) << "fail to add table " << table_meta->name()
                << " to catalog with db " << table_meta->db();
        }
    }
    return 0;
}

void TabletImpl::DropTable(RpcController* controller,
                           const ::fedb::api::DropTableRequest* request,
                           ::fedb::api::DropTableResponse* response,
                           Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(), ::fedb::api::TaskType::kDropTable,
                      task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    PDLOG(INFO, "drop table. tid[%u] pid[%u]", tid, pid);
    do {
        std::shared_ptr<Table> table = GetTable(tid, pid);
        if (!table) {
            response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            PDLOG(WARNING, "table is not exist. tid[%u] pid[%u]", tid, pid);
            break;
        } else {
            if (table->GetTableStat() ==
                    ::fedb::storage::kMakingSnapshot) {
                PDLOG(
                        WARNING,
                        "making snapshot task is running now. tid[%u] pid[%u]",
                        tid, pid);
                response->set_code(::fedb::base::ReturnCode::
                        kTableStatusIsKmakingsnapshot);
                response->set_msg("table status is kMakingSnapshot");
                break;
            }
        }
        task_pool_.AddTask(boost::bind(&TabletImpl::DeleteTableInternal,
                    this, tid, pid, task_ptr));
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

void TabletImpl::GetTaskStatus(RpcController* controller,
                               const ::fedb::api::TaskStatusRequest* request,
                               ::fedb::api::TaskStatusResponse* response,
                               Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& kv : task_map_) {
        for (const auto& task_info : kv.second) {
            ::fedb::api::TaskInfo* task = response->add_task();
            task->CopyFrom(*task_info);
        }
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::DeleteOPTask(RpcController* controller,
                              const ::fedb::api::DeleteTaskRequest* request,
                              ::fedb::api::GeneralResponse* response,
                              Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::lock_guard<std::mutex> lock(mu_);
    for (int idx = 0; idx < request->op_id_size(); idx++) {
        auto iter = task_map_.find(request->op_id(idx));
        if (iter == task_map_.end()) {
            continue;
        }
        if (!iter->second.empty()) {
            PDLOG(INFO, "delete op task. op_id[%lu] op_type[%s] task_num[%u]",
                  request->op_id(idx),
                  ::fedb::api::OPType_Name(iter->second.front()->op_type())
                      .c_str(),
                  iter->second.size());
            iter->second.clear();
        }
        task_map_.erase(iter);
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::ConnectZK(RpcController* controller,
                           const ::fedb::api::ConnectZKRequest* request,
                           ::fedb::api::GeneralResponse* response,
                           Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (zk_client_->Reconnect() && zk_client_->Register()) {
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        PDLOG(INFO, "connect zk ok");
        return;
    }
    response->set_code(-1);
    response->set_msg("connect failed");
}

void TabletImpl::DisConnectZK(RpcController* controller,
                              const ::fedb::api::DisConnectZKRequest* request,
                              ::fedb::api::GeneralResponse* response,
                              Closure* done) {
    brpc::ClosureGuard done_guard(done);
    zk_client_->CloseZK();
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
    PDLOG(INFO, "disconnect zk ok");
    return;
}

void TabletImpl::SetConcurrency(
    RpcController* ctrl, const ::fedb::api::SetConcurrencyRequest* request,
    ::fedb::api::SetConcurrencyResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    if (server_ == NULL) {
        response->set_code(-1);
        response->set_msg("server is NULL");
        return;
    }

    if (request->max_concurrency() < 0) {
        response->set_code(::fedb::base::ReturnCode::kInvalidConcurrency);
        response->set_msg("invalid concurrency " + request->max_concurrency());
        return;
    }

    if (SERVER_CONCURRENCY_KEY.compare(request->key()) == 0) {
        PDLOG(INFO, "update server max concurrency to %d",
              request->max_concurrency());
        server_->ResetMaxConcurrency(request->max_concurrency());
    } else {
        PDLOG(INFO, "update server api %s max concurrency to %d",
              request->key().c_str(), request->max_concurrency());
        server_->MaxConcurrencyOf(this, request->key()) =
            request->max_concurrency();
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::SetTaskStatus(
    std::shared_ptr<::fedb::api::TaskInfo>& task_ptr,
    ::fedb::api::TaskStatus status) {
    if (!task_ptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    task_ptr->set_status(status);
}

int TabletImpl::GetTaskStatus(std::shared_ptr<::fedb::api::TaskInfo>& task_ptr,
                              ::fedb::api::TaskStatus* status) {
    if (!task_ptr) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(mu_);
    *status = task_ptr->status();
    return 0;
}

int TabletImpl::AddOPTask(const ::fedb::api::TaskInfo& task_info,
                          ::fedb::api::TaskType task_type,
                          std::shared_ptr<::fedb::api::TaskInfo>& task_ptr) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FindTask(task_info.op_id(), task_info.task_type())) {
        PDLOG(WARNING, "task is running. op_id[%lu] op_type[%s] task_type[%s]",
              task_info.op_id(),
              ::fedb::api::OPType_Name(task_info.op_type()).c_str(),
              ::fedb::api::TaskType_Name(task_info.task_type()).c_str());
        return -1;
    }
    task_ptr.reset(task_info.New());
    task_ptr->CopyFrom(task_info);
    task_ptr->set_status(::fedb::api::TaskStatus::kDoing);
    auto iter = task_map_.find(task_info.op_id());
    if (iter == task_map_.end()) {
        task_map_.insert(std::make_pair(
            task_info.op_id(),
            std::list<std::shared_ptr<::fedb::api::TaskInfo>>()));
    }
    task_map_[task_info.op_id()].push_back(task_ptr);
    if (task_info.task_type() != task_type) {
        PDLOG(WARNING, "task type is not match. type is[%s]",
              ::fedb::api::TaskType_Name(task_info.task_type()).c_str());
        task_ptr->set_status(::fedb::api::TaskStatus::kFailed);
        return -1;
    }
    PDLOG(INFO, "add task map success, op_id[%lu] op_type[%s] task_type[%s]",
          task_info.op_id(),
          ::fedb::api::OPType_Name(task_info.op_type()).c_str(),
          ::fedb::api::TaskType_Name(task_info.task_type()).c_str());
    return 0;
}

std::shared_ptr<::fedb::api::TaskInfo> TabletImpl::FindTask(
    uint64_t op_id, ::fedb::api::TaskType task_type) {
    auto iter = task_map_.find(op_id);
    if (iter == task_map_.end()) {
        return std::shared_ptr<::fedb::api::TaskInfo>();
    }
    for (auto& task : iter->second) {
        if (task->op_id() == op_id && task->task_type() == task_type) {
            return task;
        }
    }
    return std::shared_ptr<::fedb::api::TaskInfo>();
}

int TabletImpl::AddOPMultiTask(
    const ::fedb::api::TaskInfo& task_info, ::fedb::api::TaskType task_type,
    std::shared_ptr<::fedb::api::TaskInfo>& task_ptr) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FindMultiTask(task_info)) {
        PDLOG(WARNING, "task is running. op_id[%lu] op_type[%s] task_type[%s]",
              task_info.op_id(),
              ::fedb::api::OPType_Name(task_info.op_type()).c_str(),
              ::fedb::api::TaskType_Name(task_info.task_type()).c_str());
        return -1;
    }
    task_ptr.reset(task_info.New());
    task_ptr->CopyFrom(task_info);
    task_ptr->set_status(::fedb::api::TaskStatus::kDoing);
    auto iter = task_map_.find(task_info.op_id());
    if (iter == task_map_.end()) {
        task_map_.insert(std::make_pair(
            task_info.op_id(),
            std::list<std::shared_ptr<::fedb::api::TaskInfo>>()));
    }
    task_map_[task_info.op_id()].push_back(task_ptr);
    if (task_info.task_type() != task_type) {
        PDLOG(WARNING, "task type is not match. type is[%s]",
              ::fedb::api::TaskType_Name(task_info.task_type()).c_str());
        task_ptr->set_status(::fedb::api::TaskStatus::kFailed);
        return -1;
    }
    return 0;
}

std::shared_ptr<::fedb::api::TaskInfo> TabletImpl::FindMultiTask(
    const ::fedb::api::TaskInfo& task_info) {
    auto iter = task_map_.find(task_info.op_id());
    if (iter == task_map_.end()) {
        return std::shared_ptr<::fedb::api::TaskInfo>();
    }
    for (auto& task : iter->second) {
        if (task->op_id() == task_info.op_id() &&
            task->task_type() == task_info.task_type() &&
            task->task_id() == task_info.task_id()) {
            return task;
        }
    }
    return std::shared_ptr<::fedb::api::TaskInfo>();
}

void TabletImpl::GcTable(uint32_t tid, uint32_t pid, bool execute_once) {
    std::shared_ptr<Table> table = GetTable(tid, pid);
    if (table) {
        int32_t gc_interval = FLAGS_gc_interval;
        table->SchedGc();
        if (!execute_once) {
            gc_pool_.DelayTask(
                gc_interval * 60 * 1000,
                boost::bind(&TabletImpl::GcTable, this, tid, pid, false));
        }
        return;
    }
}

std::shared_ptr<Snapshot> TabletImpl::GetSnapshot(uint32_t tid, uint32_t pid) {
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    return GetSnapshotUnLock(tid, pid);
}

std::shared_ptr<Snapshot> TabletImpl::GetSnapshotUnLock(uint32_t tid,
                                                        uint32_t pid) {
    Snapshots::iterator it = snapshots_.find(tid);
    if (it != snapshots_.end()) {
        auto tit = it->second.find(pid);
        if (tit != it->second.end()) {
            return tit->second;
        }
    }
    return std::shared_ptr<Snapshot>();
}

std::shared_ptr<LogReplicator> TabletImpl::GetReplicatorUnLock(uint32_t tid,
                                                               uint32_t pid) {
    Replicators::iterator it = replicators_.find(tid);
    if (it != replicators_.end()) {
        auto tit = it->second.find(pid);
        if (tit != it->second.end()) {
            return tit->second;
        }
    }
    return std::shared_ptr<LogReplicator>();
}

std::shared_ptr<LogReplicator> TabletImpl::GetReplicator(uint32_t tid,
                                                         uint32_t pid) {
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    return GetReplicatorUnLock(tid, pid);
}

std::shared_ptr<Table> TabletImpl::GetTable(uint32_t tid, uint32_t pid) {
    std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
    return GetTableUnLock(tid, pid);
}

std::shared_ptr<Table> TabletImpl::GetTableUnLock(uint32_t tid, uint32_t pid) {
    Tables::iterator it = tables_.find(tid);
    if (it != tables_.end()) {
        auto tit = it->second.find(pid);
        if (tit != it->second.end()) {
            return tit->second;
        }
    }
    return std::shared_ptr<Table>();
}

void TabletImpl::ShowMemPool(RpcController* controller,
                             const ::fedb::api::HttpRequest* request,
                             ::fedb::api::HttpResponse* response,
                             Closure* done) {
    brpc::ClosureGuard done_guard(done);
#ifdef TCMALLOC_ENABLE
    brpc::Controller* cntl = static_cast<brpc::Controller*>(controller);
    MallocExtension* tcmalloc = MallocExtension::instance();
    std::string stat;
    stat.resize(1024);
    char* buffer = reinterpret_cast<char*>(&(stat[0]));
    tcmalloc->GetStats(buffer, 1024);
    cntl->response_attachment().append(
        "<html><head><title>Mem Stat</title></head><body><pre>");
    cntl->response_attachment().append(stat);
    cntl->response_attachment().append("</pre></body></html>");
#endif
}

void TabletImpl::CheckZkClient() {
    if (!zk_client_->IsConnected()) {
        PDLOG(WARNING, "reconnect zk");
        if (zk_client_->Reconnect() && zk_client_->Register()) {
            PDLOG(INFO, "reconnect zk ok");
        }
    } else if (!zk_client_->IsRegisted()) {
        PDLOG(WARNING, "registe zk");
        if (zk_client_->Register()) {
            PDLOG(INFO, "registe zk ok");
        }
    }
    keep_alive_pool_.DelayTask(FLAGS_zk_keep_alive_check_interval,
                               boost::bind(&TabletImpl::CheckZkClient, this));
}

void TabletImpl::RefreshTableInfo() {
    std::string db_table_notify_path = zk_path_ + "/table/notify";
    std::string value;
    if (!zk_client_->GetNodeValue(db_table_notify_path, value)) {
        LOG(WARNING) << "fail to get node value. node is " << db_table_notify_path;
        return;
    }
    uint64_t version = 0;
    try {
        version = std::stoull(value);
    } catch (const std::exception& e) {
        LOG(WARNING) << "value is not integer";
    }
    std::string db_table_data_path = zk_path_ + "/table/db_table_data";
    std::vector<std::string> table_datas;
    if (zk_client_->IsExistNode(db_table_data_path) == 0) {
        bool ok = zk_client_->GetChildren(db_table_data_path, table_datas);
        if (!ok) {
            LOG(WARNING) << "fail to get table list with path "
                         << db_table_data_path;
            return;
        }
    } else {
        LOG(INFO) << "no tables in db";
    }
    std::vector<::fedb::nameserver::TableInfo> table_info_vec;
    for (const auto& node : table_datas) {
        std::string value;
        if (!zk_client_->GetNodeValue(db_table_data_path + "/" + node, value)) {
            LOG(WARNING) << "fail to get table data. node: " << node;
            continue;
        }
        ::fedb::nameserver::TableInfo table_info;
        if (!table_info.ParseFromString(value)) {
            LOG(WARNING) << "fail to parse table proto. node: " << node << " value: "<< value;
            continue;
        }
        table_info_vec.push_back(std::move(table_info));
    }
    // procedure part
    std::vector<std::string> sp_datas;
    if (zk_client_->IsExistNode(sp_root_path_) == 0) {
        bool ok = zk_client_->GetChildren(sp_root_path_, sp_datas);
        if (!ok) {
            LOG(WARNING) << "fail to get procedure list with path " << sp_root_path_;
            return;
        }
    } else {
        DLOG(INFO) << "no procedures in db";
    }
    fedb::catalog::Procedures db_sp_map;
    for (const auto& node : sp_datas) {
        if (node.empty()) continue;
        std::string value;
        bool ok = zk_client_->GetNodeValue(
                sp_root_path_ + "/" + node, value);
        if (!ok) {
            LOG(WARNING) << "fail to get procedure data. node: " << node;
            continue;
        }
        std::string uncompressed;
        ::snappy::Uncompress(value.c_str(), value.length(), &uncompressed);
        ::fedb::api::ProcedureInfo sp_info_pb;
        ok = sp_info_pb.ParseFromString(uncompressed);
        if (!ok) {
            LOG(WARNING) << "fail to parse procedure proto. node: " << node << " value: "<< value;
            continue;
        }
        // conver to ProcedureInfoImpl
        auto sp_info = fedb::catalog::SchemaAdapter::ConvertProcedureInfo(sp_info_pb);
        if (!sp_info) {
            LOG(WARNING) << "convert procedure info failed, sp_name: "
                << sp_info_pb.sp_name() << " db: " << sp_info_pb.db_name();
            continue;
        }
        auto it = db_sp_map.find(sp_info->GetDbName());
        if (it == db_sp_map.end()) {
            std::map<std::string,
                std::shared_ptr<hybridse::sdk::ProcedureInfo>>
                    sp_in_db = {{sp_info->GetSpName(), sp_info}};
            db_sp_map.insert(std::make_pair(sp_info->GetDbName(), sp_in_db));
        } else {
            it->second.insert(std::make_pair(sp_info->GetSpName(), sp_info));
        }
    }
    auto old_db_sp_map = catalog_->GetProcedures();
    catalog_->Refresh(table_info_vec, version, db_sp_map);
    // skip exist procedure, don`t need recompile
    for (const auto& db_sp_map_kv : db_sp_map) {
        const auto& db = db_sp_map_kv.first;
        auto old_db_sp_map_it = old_db_sp_map.find(db);
        if (old_db_sp_map_it != old_db_sp_map.end()) {
            auto old_sp_map = old_db_sp_map_it->second;
            for (const auto& sp_map_kv : db_sp_map_kv.second) {
                const auto& sp_name = sp_map_kv.first;
                auto old_sp_map_it = old_sp_map.find(sp_name);
                if (old_sp_map_it != old_sp_map.end()) {
                    continue;
                } else {
                    CreateProcedure(sp_map_kv.second);
                }
            }
        } else {
            for (const auto& sp_map_kv : db_sp_map_kv.second) {
                CreateProcedure(sp_map_kv.second);
            }
        }
    }
}

int TabletImpl::CheckDimessionPut(const ::fedb::api::PutRequest* request,
                                  uint32_t idx_cnt) {
    for (int32_t i = 0; i < request->dimensions_size(); i++) {
        if (idx_cnt <= request->dimensions(i).idx()) {
            PDLOG(WARNING,
                  "invalid put request dimensions, request idx %u is greater "
                  "than table idx cnt %u",
                  request->dimensions(i).idx(), idx_cnt);
            return -1;
        }
        if (request->dimensions(i).key().length() <= 0) {
            PDLOG(WARNING,
                  "invalid put request dimension key is empty with idx %u",
                  request->dimensions(i).idx());
            return 1;
        }
    }
    return 0;
}

void TabletImpl::SchedSyncDisk(uint32_t tid, uint32_t pid) {
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (replicator) {
        replicator->SyncToDisk();
        io_pool_.DelayTask(
            FLAGS_binlog_sync_to_disk_interval,
            boost::bind(&TabletImpl::SchedSyncDisk, this, tid, pid));
    }
}

void TabletImpl::SchedDelBinlog(uint32_t tid, uint32_t pid) {
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (replicator) {
        replicator->DeleteBinlog();
        task_pool_.DelayTask(
            FLAGS_binlog_delete_interval,
            boost::bind(&TabletImpl::SchedDelBinlog, this, tid, pid));
    }
}

bool TabletImpl::ChooseDBRootPath(uint32_t tid, uint32_t pid, std::string& path) {
    std::vector<std::string>& paths = mode_root_paths_;
    if (paths.size() < 1) {
        return false;
    }

    if (paths.size() == 1) {
        path.assign(paths[0]);
        return path.size();
    }

    std::string key = std::to_string(tid) + std::to_string(pid);
    uint32_t index =
        ::fedb::base::hash(key.c_str(), key.size(), SEED) % paths.size();
    path.assign(paths[index]);
    return path.size();
}

bool TabletImpl::ChooseRecycleBinRootPath(uint32_t tid, uint32_t pid, std::string& path) {
    std::vector<std::string>& paths = mode_recycle_root_paths_;
    if (paths.size() < 1) return false;

    if (paths.size() == 1) {
        path.assign(paths[0]);
        return true;
    }
    std::string key = std::to_string(tid) + std::to_string(pid);
    uint32_t index =
        ::fedb::base::hash(key.c_str(), key.size(), SEED) % paths.size();
    path.assign(paths[index]);
    return true;
}

void TabletImpl::DelRecycle(const std::string& path) {
    std::vector<std::string> file_vec;
    ::fedb::base::GetChildFileName(path, file_vec);
    for (auto file_path : file_vec) {
        std::string file_name = ::fedb::base::ParseFileNameFromPath(file_path);
        std::vector<std::string> parts;
        int64_t recycle_time;
        int64_t now_time = ::baidu::common::timer::get_micros() / 1000000;
        ::fedb::base::SplitString(file_name, "_", parts);
        if (parts.size() == 3) {
            recycle_time =
                ::fedb::base::ParseTimeToSecond(parts[2], "%Y%m%d%H%M%S");
        } else {
            recycle_time =
                ::fedb::base::ParseTimeToSecond(parts[3], "%Y%m%d%H%M%S");
        }
        if (FLAGS_recycle_ttl != 0 &&
            (now_time - recycle_time) > FLAGS_recycle_ttl * 60) {
            PDLOG(INFO, "delete recycle dir %s", file_path.c_str());
            ::fedb::base::RemoveDirRecursive(file_path);
        }
    }
}

void TabletImpl::SchedDelRecycle() {
    for (auto path : mode_recycle_root_paths_) {
        DelRecycle(path);
    }
    task_pool_.DelayTask(FLAGS_recycle_ttl * 60 * 1000,
                         boost::bind(&TabletImpl::SchedDelRecycle, this));
}

bool TabletImpl::CreateMultiDir(const std::vector<std::string>& dirs) {
    std::vector<std::string>::const_iterator it = dirs.begin();
    for (; it != dirs.end(); ++it) {
        std::string path = *it;
        bool ok = ::fedb::base::MkdirRecur(path);
        if (!ok) {
            PDLOG(WARNING, "fail to create dir %s", path.c_str());
            return false;
        }
    }
    return true;
}

bool TabletImpl::ChooseTableRootPath(uint32_t tid, uint32_t pid, std::string& path) {
    std::string root_path;
    bool ok = ChooseDBRootPath(tid, pid, root_path);
    if (!ok) {
        PDLOG(WARNING, "table db path doesn't found. tid %u, pid %u", tid, pid);
        return false;
    }
    path = root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);
    if (!::fedb::base::IsExists(path)) {
        PDLOG(WARNING, "table db path doesn`t exist. tid %u, pid %u", tid, pid);
        return false;
    }
    return true;
}

bool TabletImpl::GetTableRootSize(uint32_t tid, uint32_t pid, uint64_t& size) {
    std::string table_path;
    if (!ChooseTableRootPath(tid, pid, table_path)) {
        return false;
    }
    if (!::fedb::base::GetDirSizeRecur(table_path, size)) {
        PDLOG(WARNING, "get table root size failed. tid %u, pid %u", tid, pid);
        return false;
    }
    return true;
}

void TabletImpl::GetDiskused() {
    std::vector<std::shared_ptr<Table>> tables;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = tables_.begin(); it != tables_.end(); ++it) {
            for (auto pit = it->second.begin(); pit != it->second.end();
                 ++pit) {
                tables.push_back(pit->second);
            }
        }
    }
    for (const auto& table : tables) {
        uint64_t size = 0;
        if (!GetTableRootSize(table->GetId(), table->GetPid(), size)) {
            PDLOG(WARNING, "get table root size failed. tid[%u] pid[%u]",
                  table->GetId(), table->GetPid());
        } else {
            table->SetDiskused(size);
        }
    }
    task_pool_.DelayTask(FLAGS_get_table_diskused_interval,
                         boost::bind(&TabletImpl::GetDiskused, this));
}

void TabletImpl::SetMode(RpcController* controller,
                         const ::fedb::api::SetModeRequest* request,
                         ::fedb::api::GeneralResponse* response,
                         Closure* done) {
    brpc::ClosureGuard done_guard(done);
    follower_.store(request->follower(), std::memory_order_relaxed);
    std::string mode = request->follower() == true ? "follower" : "normal";
    PDLOG(INFO, "set tablet mode %s", mode.c_str());
    response->set_code(::fedb::base::ReturnCode::kOk);
}

void TabletImpl::DeleteIndex(RpcController* controller,
                             const ::fedb::api::DeleteIndexRequest* request,
                             ::fedb::api::GeneralResponse* response,
                             Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::shared_ptr<Table> table = GetTable(tid, pid);
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", tid, pid);
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    std::string root_path;
    if (!ChooseDBRootPath(tid, pid, root_path)) {
        response->set_code(::fedb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to get table db root path");
        PDLOG(WARNING, "table db path is not found. tid %u, pid %u", tid, pid);
        return;
    }
    MemTable* mem_table = dynamic_cast<MemTable*>(table.get());
    if (!mem_table->DeleteIndex(request->idx_name())) {
        response->set_code(::fedb::base::ReturnCode::kDeleteIndexFailed);
        response->set_msg("delete index failed");
        PDLOG(WARNING, "delete index %s failed. tid %u pid %u",
              request->idx_name().c_str(), tid, pid);
        return;
    }
    std::string db_path =
        root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);
    WriteTableMeta(db_path, &table->GetTableMeta());
    PDLOG(INFO, "delete index %s success. tid %u pid %u",
          request->idx_name().c_str(), tid, pid);
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::SendIndexData(
    RpcController* controller,
    const ::fedb::api::SendIndexDataRequest* request,
    ::fedb::api::GeneralResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(),
                      ::fedb::api::TaskType::kSendIndexData, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    do {
        std::shared_ptr<Table> table = GetTable(request->tid(), request->pid());
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", request->tid(),
                  request->pid());
            response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            break;
        }
        MemTable* mem_table = dynamic_cast<MemTable*>(table.get());
        if (mem_table == NULL) {
            PDLOG(WARNING, "table is not memtable. tid %u, pid %u",
                  request->tid(), request->pid());
            response->set_code(::fedb::base::ReturnCode::kTableTypeMismatch);
            response->set_msg("table is not memtable");
            break;
        }
        std::map<uint32_t, std::string> pid_endpoint_map;
        for (int idx = 0; idx < request->pairs_size(); idx++) {
            pid_endpoint_map.insert(std::make_pair(
                request->pairs(idx).pid(), request->pairs(idx).endpoint()));
        }
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        if (pid_endpoint_map.empty()) {
            PDLOG(INFO, "pid endpoint map is empty. tid %u, pid %u",
                  request->tid(), request->pid());
            SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kDone);
        } else {
            task_pool_.AddTask(boost::bind(&TabletImpl::SendIndexDataInternal,
                                           this, table, pid_endpoint_map,
                                           task_ptr));
        }
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

void TabletImpl::SendIndexDataInternal(
    std::shared_ptr<::fedb::storage::Table> table,
    const std::map<uint32_t, std::string>& pid_endpoint_map,
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr) {
    uint32_t tid = table->GetId();
    uint32_t pid = table->GetPid();
    std::string db_root_path;
    if (!ChooseDBRootPath(tid, pid, db_root_path)) {
        PDLOG(WARNING, "fail to find db root path for table tid %u pid %u", tid,
              pid);
        SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
        return;
    }
    std::string index_path = db_root_path + "/" + std::to_string(tid) + "_" +
                             std::to_string(pid) + "/index/";
    for (const auto& kv : pid_endpoint_map) {
        if (kv.first == pid) {
            continue;
        }
        std::string index_file_name = std::to_string(pid) + "_" +
                                      std::to_string(kv.first) + "_index.data";
        std::string src_file = index_path + index_file_name;
        if (!::fedb::base::IsExists(src_file)) {
            PDLOG(WARNING, "file %s is not exist. tid[%u] pid[%u]",
                  src_file.c_str(), tid, pid);
            continue;
        }
        if (kv.second == endpoint_) {
            std::shared_ptr<Table> des_table = GetTable(tid, kv.first);
            if (!table) {
                PDLOG(WARNING, "table is not exist. tid[%u] pid[%u]", tid,
                      kv.first);
                SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
                return;
            }
            std::string des_db_root_path;
            if (!ChooseDBRootPath(tid, kv.first, des_db_root_path)) {
                PDLOG(WARNING,
                      "fail to find db root path for table tid %u pid %u", tid,
                      kv.first);
                SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
                return;
            }
            std::string des_index_path = des_db_root_path + "/" +
                                         std::to_string(tid) + "_" +
                                         std::to_string(kv.first) + "/index/";
            if (!::fedb::base::IsExists(des_index_path) &&
                !::fedb::base::MkdirRecur(des_index_path)) {
                PDLOG(WARNING, "mkdir failed. tid[%u] pid[%u] path[%s]", tid,
                      pid, des_index_path.c_str());
                SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
                return;
            }
            if (db_root_path == des_db_root_path) {
                if (!::fedb::base::Rename(src_file,
                                           des_index_path + index_file_name)) {
                    PDLOG(WARNING,
                          "rename dir failed. tid[%u] pid[%u] file[%s]", tid,
                          pid, index_file_name.c_str());
                    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
                    return;
                }
                PDLOG(INFO, "rename file %s success. tid[%u] pid[%u]",
                      index_file_name.c_str(), tid, pid);
            } else {
                if (!::fedb::base::CopyFile(
                        src_file, des_index_path + index_file_name)) {
                    PDLOG(WARNING, "copy failed. tid[%u] pid[%u] file[%s]", tid,
                          pid, index_file_name.c_str());
                    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
                    return;
                }
                PDLOG(INFO, "copy file %s success. tid[%u] pid[%u]",
                      index_file_name.c_str(), tid, pid);
            }
        } else {
            std::string real_endpoint = kv.second;
            if (FLAGS_use_name) {
                auto tmp_map = std::atomic_load_explicit(&real_ep_map_,
                        std::memory_order_acquire);
                auto iter = tmp_map->find(kv.second);
                if (iter == tmp_map->end()) {
                    PDLOG(WARNING, "name %s not found in real_ep_map."
                            "tid[%u] pid[%u]", kv.second.c_str(), tid, pid);
                    break;
                }
                real_endpoint = iter->second;
            }
            FileSender sender(tid, kv.first, real_endpoint);
            if (!sender.Init()) {
                PDLOG(WARNING,
                      "Init FileSender failed. tid[%u] pid[%u] des_pid[%u] "
                      "endpoint[%s]",
                      tid, pid, kv.first, kv.second.c_str());
                SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
                return;
            }
            if (sender.SendFile(index_file_name, std::string("index"),
                                index_path + index_file_name) < 0) {
                PDLOG(WARNING,
                      "send file %s failed. tid[%u] pid[%u] des_pid[%u]",
                      index_file_name.c_str(), tid, pid, kv.first);
                SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
                return;
            }
            PDLOG(INFO,
                  "send file %s to endpoint %s success. tid[%u] pid[%u] "
                  "des_pid[%u]",
                  index_file_name.c_str(), kv.second.c_str(), tid, pid,
                  kv.first);
        }
    }
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kDone);
}

void TabletImpl::DumpIndexData(
    RpcController* controller,
    const ::fedb::api::DumpIndexDataRequest* request,
    ::fedb::api::GeneralResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(),
                      ::fedb::api::TaskType::kDumpIndexData, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    do {
        std::shared_ptr<Table> table;
        std::shared_ptr<Snapshot> snapshot;
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            table = GetTableUnLock(tid, pid);
            if (!table) {
                PDLOG(WARNING, "table is not exist. tid[%u] pid[%u]", tid, pid);
                response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
                response->set_msg("table is not exist");
                break;
            }
            if (table->GetTableStat() != ::fedb::storage::kNormal) {
                PDLOG(WARNING,
                      "table state is %d, cannot dump index data. %u, pid %u",
                      table->GetTableStat(), tid, pid);
                response->set_code(
                    ::fedb::base::ReturnCode::kTableStatusIsNotKnormal);
                response->set_msg("table status is not kNormal");
                break;
            }
            snapshot = GetSnapshotUnLock(tid, pid);
            if (!snapshot) {
                PDLOG(WARNING, "snapshot is not exist. tid[%u] pid[%u]", tid,
                      pid);
                response->set_code(
                    ::fedb::base::ReturnCode::kSnapshotIsNotExist);
                response->set_msg("table snapshot is not exist");
                break;
            }
        }
        std::shared_ptr<::fedb::storage::MemTableSnapshot> memtable_snapshot =
            std::static_pointer_cast<::fedb::storage::MemTableSnapshot>(
                snapshot);
        task_pool_.AddTask(
            boost::bind(&TabletImpl::DumpIndexDataInternal, this, table,
                        memtable_snapshot, request->partition_num(),
                        request->column_key(), request->idx(), task_ptr));
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        PDLOG(INFO, "dump index tid[%u] pid[%u]", tid, pid);
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

void TabletImpl::DumpIndexDataInternal(
    std::shared_ptr<::fedb::storage::Table> table,
    std::shared_ptr<::fedb::storage::MemTableSnapshot> memtable_snapshot,
    uint32_t partition_num, ::fedb::common::ColumnKey& column_key,
    uint32_t idx, std::shared_ptr<::fedb::api::TaskInfo> task) {
    uint32_t tid = table->GetId();
    uint32_t pid = table->GetPid();
    std::string db_root_path;
    if (!ChooseDBRootPath(tid, pid, db_root_path)) {
        PDLOG(WARNING, "fail to find db root path for table tid %u pid %u", tid,
              pid);
        SetTaskStatus(task, ::fedb::api::kFailed);
        return;
    }
    std::string index_path = db_root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid) + "/index/";
    if (!::fedb::base::MkdirRecur(index_path)) {
        LOG(WARNING) << "fail to create path " << index_path << ". tid " << tid << " pid " << pid;
        SetTaskStatus(task, ::fedb::api::kFailed);
        return;
    }
    std::vector<::fedb::log::WriteHandle*> whs;
    for (uint32_t i = 0; i < partition_num; i++) {
        std::string index_file_name = std::to_string(pid) + "_" + std::to_string(i) + "_index.data";
        std::string index_data_path = index_path + index_file_name;
        FILE* fd = fopen(index_data_path.c_str(), "wb+");
        if (fd == NULL) {
            LOG(WARNING) << "fail to create file " << index_data_path << ". tid " << tid << " pid " << pid;
            SetTaskStatus(task, ::fedb::api::kFailed);
            for (auto& wh : whs) {
                delete wh;
                wh = NULL;
            }
            return;
        }
        ::fedb::log::WriteHandle* wh = new ::fedb::log::WriteHandle("off", index_file_name, fd);
        whs.push_back(wh);
    }
    if (memtable_snapshot->DumpIndexData(table, column_key, idx, whs)) {
        PDLOG(INFO, "dump index on table tid[%u] pid[%u] succeed", tid, pid);
        SetTaskStatus(task, ::fedb::api::kDone);
    } else {
        PDLOG(WARNING, "fail to dump index on table tid[%u] pid[%u]", tid, pid);
        SetTaskStatus(task, ::fedb::api::kFailed);
    }
    for (auto& wh : whs) {
        wh->EndLog();
        delete wh;
        wh = NULL;
    }
}

void TabletImpl::LoadIndexData(
    RpcController* controller,
    const ::fedb::api::LoadIndexDataRequest* request,
    ::fedb::api::GeneralResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(),
                      ::fedb::api::TaskType::kLoadIndexData, task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    do {
        uint32_t tid = request->tid();
        uint32_t pid = request->pid();
        auto table = GetTable(tid, pid);
        if (!table) {
            PDLOG(WARNING, "table is not exist. tid %u, pid %u", tid, pid);
            response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
            response->set_msg("table is not exist");
            break;
        }
        if (table->GetTableStat() != ::fedb::storage::kNormal) {
            PDLOG(WARNING,
                  "table state is %d, cannot load index data. tid %u, pid %u",
                  table->GetTableStat(), tid, pid);
            response->set_code(
                ::fedb::base::ReturnCode::kTableStatusIsNotKnormal);
            response->set_msg("table status is not kNormal");
            break;
        }
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        if (request->partition_num() <= 1) {
            PDLOG(INFO, "partition num is %d need not load. tid %u, pid %u",
                  request->partition_num(), tid, pid);
            SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kDone);
        } else {
            uint64_t cur_time = ::baidu::common::timer::get_micros() / 1000;
            task_pool_.AddTask(
                boost::bind(&TabletImpl::LoadIndexDataInternal, this, tid, pid,
                            0, request->partition_num(), cur_time, task_ptr));
        }
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

void TabletImpl::LoadIndexDataInternal(
    uint32_t tid, uint32_t pid, uint32_t cur_pid, uint32_t partition_num,
    uint64_t last_time, std::shared_ptr<::fedb::api::TaskInfo> task) {
    uint64_t cur_time = ::baidu::common::timer::get_micros() / 1000;
    if (cur_pid == pid) {
        task_pool_.AddTask(boost::bind(&TabletImpl::LoadIndexDataInternal, this,
                                       tid, pid, cur_pid + 1, partition_num,
                                       cur_time, task));
        return;
    }
    ::fedb::api::TaskStatus status = ::fedb::api::TaskStatus::kFailed;
    if (GetTaskStatus(task, &status) < 0 ||
        status != ::fedb::api::TaskStatus::kDoing) {
        PDLOG(INFO, "terminate load index. tid %u pid %u", tid, pid);
        return;
    }
    auto table = GetTable(tid, pid);
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u pid %u", tid, pid);
        SetTaskStatus(task, ::fedb::api::TaskStatus::kFailed);
        return;
    }
    auto replicator = GetReplicator(tid, pid);
    if (!replicator) {
        PDLOG(WARNING, "replicator is not exist. tid %u pid %u", tid, pid);
        SetTaskStatus(task, ::fedb::api::TaskStatus::kFailed);
        return;
    }
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, db_root_path);
    if (!ok) {
        PDLOG(WARNING, "fail to find db root path for table tid %u pid %u", tid,
              pid);
        SetTaskStatus(task, ::fedb::api::TaskStatus::kFailed);
        return;
    }
    std::string index_path = db_root_path + "/" + std::to_string(tid) + "_" +
                             std::to_string(pid) + "/index/";
    std::string index_file_path = index_path + std::to_string(cur_pid) + "_" +
                                  std::to_string(pid) + "_index.data";
    if (!::fedb::base::IsExists(index_file_path)) {
        if (last_time + FLAGS_load_index_max_wait_time < cur_time) {
            PDLOG(WARNING, "wait time too long. tid %u pid %u file %s", tid,
                  pid, index_file_path.c_str());
            SetTaskStatus(task, ::fedb::api::TaskStatus::kFailed);
            return;
        }
        task_pool_.DelayTask(
            FLAGS_task_check_interval,
            boost::bind(&TabletImpl::LoadIndexDataInternal, this, tid, pid,
                        cur_pid, partition_num, last_time, task));
        return;
    }
    FILE* fd = fopen(index_file_path.c_str(), "rb");
    if (fd == NULL) {
        PDLOG(WARNING, "fail to open index file %s. tid %u, pid %u",
              index_file_path.c_str(), tid, pid);
        SetTaskStatus(task, ::fedb::api::TaskStatus::kFailed);
        return;
    }
    ::fedb::log::SequentialFile* seq_file = ::fedb::log::NewSeqFile(index_file_path, fd);
    ::fedb::log::Reader reader(seq_file, NULL, false, 0, false);
    std::string buffer;
    uint64_t succ_cnt = 0;
    uint64_t failed_cnt = 0;
    while (true) {
        buffer.clear();
        ::fedb::base::Slice record;
        ::fedb::base::Status status = reader.ReadRecord(&record, &buffer);
        if (status.IsWaitRecord() || status.IsEof()) {
            PDLOG(INFO,
                  "read path %s for table tid %u pid %u completed. succ_cnt "
                  "%lu, failed_cnt %lu",
                  index_file_path.c_str(), tid, pid, succ_cnt, failed_cnt);
            break;
        }
        if (!status.ok()) {
            PDLOG(WARNING,
                  "fail to read record for tid %u, pid %u with error %s", tid,
                  pid, status.ToString().c_str());
            failed_cnt++;
            continue;
        }
        ::fedb::api::LogEntry entry;
        entry.ParseFromString(std::string(record.data(), record.size()));
        if (entry.has_method_type() &&
            entry.method_type() == ::fedb::api::MethodType::kDelete) {
            table->Delete(entry.dimensions(0).key(), entry.dimensions(0).idx());
        } else {
            table->Put(entry);
        }
        replicator->AppendEntry(entry);
        succ_cnt++;
    }
    delete seq_file;
    if (cur_pid == partition_num - 1 || (cur_pid + 1 == pid && pid == partition_num - 1)) {
        if (FLAGS_recycle_bin_enabled) {
            std::string recycle_bin_root_path;
            ok = ChooseRecycleBinRootPath(tid, pid, recycle_bin_root_path);
            if (!ok) {
                LOG(WARNING) << "fail to get recycle bin root path. tid " << tid << " pid " << pid;
                fedb::base::RemoveDirRecursive(index_path);
            } else {
                std::string recycle_path = recycle_bin_root_path + "/" + std::to_string(tid) + "_" +
                                           std::to_string(pid);
                if (!fedb::base::IsExists(recycle_path)) {
                    fedb::base::Mkdir(recycle_path);
                }
                std::string dst = recycle_path + "/index" + fedb::base::GetNowTime();
                fedb::base::Rename(index_path, dst);
            }
        } else {
            fedb::base::RemoveDirRecursive(index_path);
        }
        LOG(INFO) << "load index surccess. tid " << tid << " pid " << pid;
        SetTaskStatus(task, ::fedb::api::TaskStatus::kDone);
        return;
    }
    cur_time = ::baidu::common::timer::get_micros() / 1000;
    task_pool_.AddTask(boost::bind(&TabletImpl::LoadIndexDataInternal, this,
                                   tid, pid, cur_pid + 1, partition_num,
                                   cur_time, task));
}

void TabletImpl::ExtractIndexData(
    RpcController* controller,
    const ::fedb::api::ExtractIndexDataRequest* request,
    ::fedb::api::GeneralResponse* response, Closure* done) {
    brpc::ClosureGuard done_guard(done);
    std::shared_ptr<::fedb::api::TaskInfo> task_ptr;
    if (request->has_task_info() && request->task_info().IsInitialized()) {
        if (AddOPTask(request->task_info(),
                      ::fedb::api::TaskType::kExtractIndexData,
                      task_ptr) < 0) {
            response->set_code(-1);
            response->set_msg("add task failed");
            return;
        }
    }
    do {
        uint32_t tid = request->tid();
        uint32_t pid = request->pid();
        std::shared_ptr<Table> table;
        std::shared_ptr<Snapshot> snapshot;
        {
            std::lock_guard<SpinMutex> spin_lock(spin_mutex_);
            table = GetTableUnLock(tid, pid);
            if (!table) {
                PDLOG(WARNING, "table is not exist. tid %u pid %u", tid, pid);
                response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
                response->set_msg("table is not exist");
                break;
            }
            if (table->GetTableStat() != ::fedb::storage::kNormal) {
                PDLOG(WARNING,
                      "table state is %d, cannot extract index data. tid %u, "
                      "pid %u",
                      table->GetTableStat(), tid, pid);
                response->set_code(
                    ::fedb::base::ReturnCode::kTableStatusIsNotKnormal);
                response->set_msg("table status is not kNormal");
                break;
            }
            snapshot = GetSnapshotUnLock(tid, pid);
            if (!snapshot) {
                PDLOG(WARNING, "snapshot is not exist. tid %u pid %u", tid,
                      pid);
                response->set_code(
                    ::fedb::base::ReturnCode::kSnapshotIsNotExist);
                response->set_msg("table snapshot is not exist");
                break;
            }
        }
        std::shared_ptr<::fedb::storage::MemTableSnapshot> memtable_snapshot =
            std::static_pointer_cast<::fedb::storage::MemTableSnapshot>(
                snapshot);
        task_pool_.AddTask(boost::bind(&TabletImpl::ExtractIndexDataInternal,
                                       this, table, memtable_snapshot,
                                       request->column_key(), request->idx(),
                                       request->partition_num(), task_ptr));
        response->set_code(::fedb::base::ReturnCode::kOk);
        response->set_msg("ok");
        return;
    } while (0);
    SetTaskStatus(task_ptr, ::fedb::api::TaskStatus::kFailed);
}

void TabletImpl::ExtractIndexDataInternal(
    std::shared_ptr<::fedb::storage::Table> table,
    std::shared_ptr<::fedb::storage::MemTableSnapshot> memtable_snapshot,
    ::fedb::common::ColumnKey& column_key, uint32_t idx,
    uint32_t partition_num, std::shared_ptr<::fedb::api::TaskInfo> task) {
    uint64_t offset = 0;
    uint32_t tid = table->GetId();
    uint32_t pid = table->GetPid();
    if (memtable_snapshot->ExtractIndexData(table, column_key, idx,
                                            partition_num, offset) < 0) {
        PDLOG(WARNING, "fail to extract index. tid %u pid %u", tid, pid);
        SetTaskStatus(task, ::fedb::api::TaskStatus::kFailed);
        return;
    }
    PDLOG(INFO, "extract index success. tid %u pid %u", tid, pid);
    std::shared_ptr<LogReplicator> replicator = GetReplicator(tid, pid);
    if (replicator) {
        replicator->SetSnapshotLogPartIndex(offset);
    }
    SetTaskStatus(task, ::fedb::api::TaskStatus::kDone);
}

void TabletImpl::AddIndex(RpcController* controller,
                          const ::fedb::api::AddIndexRequest* request,
                          ::fedb::api::GeneralResponse* response,
                          Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint32_t tid = request->tid();
    uint32_t pid = request->pid();
    std::shared_ptr<Table> table = GetTable(tid, pid);
    if (!table) {
        PDLOG(WARNING, "table is not exist. tid %u, pid %u", tid, pid);
        response->set_code(::fedb::base::ReturnCode::kTableIsNotExist);
        response->set_msg("table is not exist");
        return;
    }
    MemTable* mem_table = dynamic_cast<MemTable*>(table.get());
    if (mem_table == NULL) {
        PDLOG(WARNING, "table is not memtable. tid %u, pid %u", tid, pid);
        response->set_code(::fedb::base::ReturnCode::kTableTypeMismatch);
        response->set_msg("table is not memtable");
        return;
    }
    if (!mem_table->AddIndex(request->column_key())) {
        PDLOG(WARNING, "add index %s failed. tid %u, pid %u",
              request->column_key().index_name().c_str(), tid, pid);
        response->set_code(::fedb::base::ReturnCode::kAddIndexFailed);
        response->set_msg("add index failed");
        return;
    }
    std::string db_root_path;
    bool ok = ChooseDBRootPath(tid, pid, db_root_path);
    if (!ok) {
        response->set_code(::fedb::base::ReturnCode::kFailToGetDbRootPath);
        response->set_msg("fail to get db root path");
        PDLOG(WARNING, "fail to get table db root path for tid %u, pid %u", tid, pid);
        return;
    }
    std::string db_path = db_root_path + "/" + std::to_string(tid) + "_" + std::to_string(pid);
    if (!::fedb::base::IsExists(db_path)) {
        PDLOG(WARNING, "table db path doesn't exist. tid %u, pid %u", tid, pid);
        response->set_code(::fedb::base::ReturnCode::kTableDbPathIsNotExist);
        response->set_msg("table db path is not exist");
        return;
    }
    if (WriteTableMeta(db_path, &(table->GetTableMeta())) < 0) {
        PDLOG(WARNING, "write table_meta failed. tid[%u] pid[%u]", tid, pid);
        response->set_code(::fedb::base::ReturnCode::kWriteDataFailed);
        response->set_msg("write data failed");
        return;
    }
    PDLOG(INFO, "add index %s ok. tid %u pid %u",
          request->column_key().index_name().c_str(), request->tid(),
          request->pid());
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::CancelOP(RpcController* controller,
                          const fedb::api::CancelOPRequest* request,
                          fedb::api::GeneralResponse* response,
                          Closure* done) {
    brpc::ClosureGuard done_guard(done);
    uint64_t op_id = request->op_id();
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto iter = task_map_.find(op_id);
        if (iter != task_map_.end()) {
            for (auto& task : iter->second) {
                if (task->status() == ::fedb::api::TaskStatus::kInited ||
                    task->status() == ::fedb::api::TaskStatus::kDoing) {
                    task->set_status(::fedb::api::TaskStatus::kCanceled);
                    PDLOG(
                        INFO, "cancel op [%lu] task_type[%s] ", op_id,
                        ::fedb::api::TaskType_Name(task->task_type()).c_str());
                }
            }
        }
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

void TabletImpl::UpdateRealEndpointMap(RpcController* controller,
        const fedb::api::UpdateRealEndpointMapRequest* request,
        fedb::api::GeneralResponse* response, Closure* done) {
    DLOG(INFO) << "UpdateRealEndpointMap";
    brpc::ClosureGuard done_guard(done);
    if (FLAGS_zk_cluster.empty()) {
        response->set_code(-1);
        response->set_msg("tablet is not run in cluster mode");
        PDLOG(WARNING, "tablet is not run in cluster mode");
        return;
    }
    decltype(real_ep_map_) tmp_real_ep_map =
        std::make_shared<std::map<std::string, std::string>>();
    for (int i = 0; i < request->real_endpoint_map_size(); i++) {
        auto& pair = request->real_endpoint_map(i);
        tmp_real_ep_map->insert(
                std::make_pair(pair.name(), pair.real_endpoint()));
    }
    std::atomic_store_explicit(&real_ep_map_, tmp_real_ep_map,
            std::memory_order_release);
    DLOG(INFO) << "real_ep_map size is " << tmp_real_ep_map->size();
    catalog_->UpdateClient(*tmp_real_ep_map);
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
}

bool TabletImpl::GetRealEp(uint64_t tid, uint64_t pid,
        std::map<std::string, std::string>* real_ep_map) {
    if (real_ep_map == nullptr) {
        return false;
    }
    auto tmp_map = std::atomic_load_explicit(&real_ep_map_,
            std::memory_order_acquire);
    for (auto rit = real_ep_map->begin();
            rit != real_ep_map->end(); ++rit) {
        auto iter = tmp_map->find(rit->first);
        if (iter == tmp_map->end()) {
            PDLOG(WARNING, "name %s not found in real_ep_map."
                    "tid[%u] pid[%u]", rit->first.c_str(), tid, pid);
            return false;
        }
        rit->second = iter->second;
    }
    return true;
}

void TabletImpl::CreateProcedure(RpcController* controller,
        const fedb::api::CreateProcedureRequest* request,
        fedb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    auto& sp_info = request->sp_info();
    const std::string& db_name = sp_info.db_name();
    const std::string& sp_name = sp_info.sp_name();
    const std::string& sql = sp_info.sql();
    if (sp_cache_->ProcedureExist(db_name, sp_name)) {
        response->set_code(::fedb::base::ReturnCode::kProcedureAlreadyExists);
        response->set_msg("store procedure already exists");
        PDLOG(WARNING, "store procedure[%s] already exists in db[%s]", sp_name.c_str(), db_name.c_str());
        return;
    }
    ::hybridse::base::Status status;

    // build for single request
    ::hybridse::vm::RequestRunSession session;
    bool ok = engine_->Get(sql, db_name, session, status);
    if (!ok || session.GetCompileInfo() == nullptr) {
        response->set_msg(status.str());
        response->set_code(::fedb::base::kSQLCompileError);
        LOG(WARNING) << "fail to compile sql " << sql;
        return;
    }

    // build for batch request
    ::hybridse::vm::BatchRequestRunSession batch_session;
    for (auto i = 0; i < sp_info.input_schema_size(); ++i) {
        bool is_constant = sp_info.input_schema().Get(i).is_constant();
        if (is_constant) {
            batch_session.AddCommonColumnIdx(i);
        }
    }
    ok = engine_->Get(sql, db_name, batch_session, status);
    if (!ok || batch_session.GetCompileInfo() == nullptr) {
        response->set_msg(status.str());
        response->set_code(::fedb::base::kSQLCompileError);
        LOG(WARNING) << "fail to compile batch request for sql " << sql;
        return;
    }

    auto sp_info_impl = fedb::catalog::SchemaAdapter::ConvertProcedureInfo(sp_info);
    if (!sp_info_impl) {
        response->set_msg(status.str());
        response->set_code(::fedb::base::kCreateProcedureFailedOnTablet);
        LOG(WARNING) << "convert procedure info failed, sp_name: " << sp_name << " db: " << db_name;
        return;
    }
    ok = catalog_->AddProcedure(db_name, sp_name, sp_info_impl);
    if (ok) {
        LOG(INFO) << "add procedure " << sp_name
            << " to catalog with db " << db_name;
    } else {
        LOG(WARNING) << "fail to add procedure " << sp_name
            << " to catalog with db " << db_name;
    }

    sp_cache_->InsertSQLProcedureCacheEntry(db_name, sp_name, sp_info_impl, session.GetCompileInfo(),
                                            batch_session.GetCompileInfo());

    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
    LOG(INFO) << "create procedure success! sp_name: " << sp_name << ", db: " << db_name << ", sql: " << sql;
}

void TabletImpl::DropProcedure(RpcController* controller,
        const ::fedb::api::DropProcedureRequest* request,
        ::fedb::api::GeneralResponse* response,
        Closure* done) {
    brpc::ClosureGuard done_guard(done);
    const std::string& db_name = request->db_name();
    const std::string& sp_name = request->sp_name();
    sp_cache_->DropSQLProcedureCacheEntry(db_name, sp_name);
    if (!catalog_->DropProcedure(db_name, sp_name)) {
        LOG(WARNING) << "drop procedure" << db_name << "." << sp_name << " in catalog failed";
    }
    response->set_code(::fedb::base::ReturnCode::kOk);
    response->set_msg("ok");
    PDLOG(INFO, "drop procedure success. db_name[%s] sp_name[%s]",
        db_name.c_str(), sp_name.c_str());
}

void TabletImpl::RunRequestQuery(RpcController* ctrl,
                                 const fedb::api::QueryRequest& request,
                                 ::hybridse::vm::RequestRunSession& session,
                                 fedb::api::QueryResponse& response,
                                 butil::IOBuf& buf) {
    if (request.is_debug()) {
        session.EnableDebug();
    }
    ::hybridse::codec::Row row;
    auto& request_buf = static_cast<brpc::Controller*>(ctrl)->request_attachment();
    size_t input_slices = request.row_slices();
    if (!codec::DecodeRpcRow(request_buf, 0, request.row_size(), input_slices, &row)) {
        response.set_code(::fedb::base::kSQLRunError);
        response.set_msg("fail to decode input row");
        return;
    }
    ::hybridse::codec::Row output;
    int32_t ret = 0;
    if (request.has_task_id()) {
        ret = session.Run(request.task_id(), row, &output);
    } else {
        ret = session.Run(row, &output);
    }
    if (ret != 0) {
        response.set_code(::fedb::base::kSQLRunError);
        response.set_msg("fail to run sql");
        return;
    } else if (row.GetRowPtrCnt() != 1) {
        response.set_code(::fedb::base::kSQLRunError);
        response.set_msg("do not support multiple output row slices");
        return;
    }
    size_t buf_total_size;
    if (!codec::EncodeRpcRow(output, &buf, &buf_total_size)) {
        response.set_code(::fedb::base::kSQLRunError);
        response.set_msg("fail to encode sql output row");
        return;
    }
    if (!request.has_task_id()) {
        response.set_schema(session.GetEncodedSchema());
    }
    response.set_byte_size(buf_total_size);
    response.set_count(1);
    response.set_row_slices(1);
    response.set_code(::fedb::base::kOk);
}

void TabletImpl::CreateProcedure(const std::shared_ptr<hybridse::sdk::ProcedureInfo> sp_info) {
    const std::string& db_name = sp_info->GetDbName();
    const std::string& sp_name = sp_info->GetSpName();
    const std::string& sql = sp_info->GetSql();
    ::hybridse::base::Status status;
    // build for single request
    ::hybridse::vm::RequestRunSession session;
    bool ok = engine_->Get(sql, db_name, session, status);
    if (!ok || session.GetCompileInfo() == nullptr) {
        LOG(WARNING) << "fail to compile sql " << sql;
        return;
    }
    // build for batch request
    ::hybridse::vm::BatchRequestRunSession batch_session;
    for (auto i = 0; i < sp_info->GetInputSchema().GetColumnCnt(); ++i) {
        bool is_constant = sp_info->GetInputSchema().IsConstant(i);
        if (is_constant) {
            batch_session.AddCommonColumnIdx(i);
        }
    }
    ok = engine_->Get(sql, db_name, batch_session, status);
    if (!ok || batch_session.GetCompileInfo() == nullptr) {
        LOG(WARNING) << "fail to compile batch request for sql " << sql;
        return;
    }
    sp_cache_->InsertSQLProcedureCacheEntry(db_name, sp_name, sp_info, session.GetCompileInfo(),
                                            batch_session.GetCompileInfo());
    LOG(INFO) << "refresh procedure success! sp_name: " << sp_name << ", db: " << db_name << ", sql: " << sql;
}

}  // namespace tablet
}  // namespace fedb
