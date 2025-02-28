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


#pragma once

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "proto/name_server.pb.h"
#include "proto/tablet.pb.h"
#include "proto/type.pb.h"

namespace fedb {
namespace storage {

static constexpr uint32_t MAX_INDEX_NUM = 200;

enum TTLType {
    kAbsoluteTime = 1,
    kRelativeTime = 2,
    kLatestTime = 3,
    kAbsAndLat = 4,
    kAbsOrLat = 5
};

struct TTLSt {
    TTLSt() : abs_ttl(0), lat_ttl(0), ttl_type(::fedb::storage::TTLType::kAbsoluteTime) {}
    TTLSt(uint64_t abs, uint64_t lat, ::fedb::storage::TTLType type) : abs_ttl(abs), lat_ttl(lat), ttl_type(type) {}
    explicit TTLSt(const ::fedb::api::TTLDesc& ttl_desc) : abs_ttl(ttl_desc.abs_ttl() * 60 * 1000),
        lat_ttl(ttl_desc.lat_ttl()) {
        ttl_type = ConvertTTLType(ttl_desc.ttl_type());
    }

    explicit TTLSt(const ::fedb::common::TTLSt& ttl) : abs_ttl(ttl.abs_ttl() * 60 * 1000), lat_ttl(ttl.lat_ttl()) {
        ttl_type = ConvertTTLType(ttl.ttl_type());
    }

    static TTLType ConvertTTLType(::fedb::api::TTLType type) {
        switch (type) {
            case ::fedb::api::TTLType::kAbsoluteTime:
                return TTLType::kAbsoluteTime;
            case ::fedb::api::TTLType::kLatestTime:
                return TTLType::kLatestTime;
            case ::fedb::api::TTLType::kAbsAndLat:
                return TTLType::kAbsAndLat;
            case ::fedb::api::TTLType::kAbsOrLat:
                return TTLType::kAbsOrLat;
            default:
                return TTLType::kAbsoluteTime;
        }
    }

    ::fedb::api::TTLType GetTabletTTLType() const {
        switch (ttl_type) {
            case TTLType::kAbsoluteTime:
                return ::fedb::api::TTLType::kAbsoluteTime;
            case TTLType::kLatestTime:
                return ::fedb::api::TTLType::kLatestTime;
            case TTLType::kAbsAndLat:
                return ::fedb::api::TTLType::kAbsAndLat;
            case TTLType::kAbsOrLat:
                return ::fedb::api::TTLType::kAbsOrLat;
            default:
                return ::fedb::api::TTLType::kAbsoluteTime;
        }
    }

    static TTLType ConvertTTLType(::fedb::type::TTLType type) {
        switch (type) {
            case ::fedb::type::TTLType::kAbsoluteTime:
                return TTLType::kAbsoluteTime;
            case ::fedb::type::TTLType::kLatestTime:
                return TTLType::kLatestTime;
            case ::fedb::type::TTLType::kAbsAndLat:
                return TTLType::kAbsAndLat;
            case ::fedb::type::TTLType::kAbsOrLat:
                return TTLType::kAbsOrLat;
            default:
                return TTLType::kAbsoluteTime;
        }
    }

    bool NeedGc() const {
        switch (ttl_type) {
            case TTLType::kAbsoluteTime:
                return abs_ttl == 0 ? false : true;
            case TTLType::kLatestTime:
                return lat_ttl == 0 ? false : true;
            case TTLType::kAbsAndLat:
                return abs_ttl == 0 || lat_ttl == 0 ? false : true;
            case TTLType::kAbsOrLat:
                return abs_ttl == 0 && lat_ttl == 0 ? false : true;
            default:
                return true;
        }
    }

    bool IsExpired(uint64_t abs, uint32_t record_idx) const {
        switch (ttl_type) {
            case TTLType::kAbsoluteTime:
                if (abs_ttl == 0) return false;
                return abs <= abs_ttl;
            case TTLType::kLatestTime:
                if (lat_ttl == 0) return false;
                return record_idx > lat_ttl;
            case TTLType::kAbsAndLat:
                if (abs_ttl == 0 || lat_ttl == 0) return false;
                return abs <= abs_ttl && record_idx > lat_ttl;
            case TTLType::kAbsOrLat: {
                if (abs_ttl == 0) {
                    if (lat_ttl == 0) {
                        return false;
                    }
                    return record_idx > lat_ttl;
                } else if (lat_ttl == 0) {
                    return abs <= abs_ttl;
                } else {
                    return abs <= abs_ttl || record_idx > lat_ttl;
                }
            }
            default:
                return true;
        }
    }

    std::string ToString() const {
        switch (ttl_type) {
            case TTLType::kAbsoluteTime:
                return std::to_string(abs_ttl) + "min";
            case TTLType::kLatestTime:
                return std::to_string(lat_ttl);
            case TTLType::kAbsAndLat:
                return std::to_string(abs_ttl) + "min&&" + std::to_string(lat_ttl);
            case TTLType::kAbsOrLat:
                return std::to_string(abs_ttl) + "min||" + std::to_string(lat_ttl);
            default:
                return "invalid ttl_type";
        }
    }

    uint64_t abs_ttl;
    uint64_t lat_ttl;
    TTLType ttl_type;
};

struct UpdateTTLMeta {
    explicit UpdateTTLMeta(const TTLSt& new_ttl) : ttl(new_ttl), ts_idx(-1), index_name() {}
    UpdateTTLMeta(const TTLSt& new_ttl, int32_t idx) : ttl(new_ttl), ts_idx(idx), index_name() {}
    UpdateTTLMeta(const TTLSt& new_ttl, const std::string& name) : ttl(new_ttl),
        ts_idx(-1), index_name(name) {}
    TTLSt ttl;
    int32_t ts_idx;
    std::string index_name;
};

enum class IndexStatus { kReady = 0, kWaiting, kDeleting, kDeleted };

class ColumnDef {
 public:
    ColumnDef(const std::string& name, uint32_t id, ::fedb::type::DataType type, bool not_null);
    ColumnDef(const std::string& name, uint32_t id, ::fedb::type::DataType type, bool not_null, int ts_idx);
    inline uint32_t GetId() const { return id_; }
    inline const std::string& GetName() const { return name_; }
    inline ::fedb::type::DataType GetType() const { return type_; }
    inline bool NotNull() const { return not_null_; }
    void SetTsIdx(int32_t ts_idx) { ts_idx_ = ts_idx; }
    inline int32_t GetTsIdx() const { return ts_idx_; }

    static bool CheckTsType(::fedb::type::DataType type) {
        if (type == ::fedb::type::kBigInt || type == ::fedb::type::kTimestamp) {
            return true;
        }
        return false;
    }

 private:
    std::string name_;
    uint32_t id_;
    ::fedb::type::DataType type_;
    bool not_null_;
    int32_t ts_idx_;
};

class TableColumn {
 public:
    TableColumn() = default;
    std::shared_ptr<ColumnDef> GetColumn(uint32_t idx);
    std::shared_ptr<ColumnDef> GetColumn(const std::string& name);
    void AddColumn(std::shared_ptr<ColumnDef> column_def);
    const std::vector<std::shared_ptr<ColumnDef>>& GetAllColumn();
    inline uint32_t Size() { return columns_.size(); }

 private:
    std::vector<std::shared_ptr<ColumnDef>> columns_;
    std::unordered_map<std::string, std::shared_ptr<ColumnDef>> column_map_;
};

class IndexDef {
 public:
    IndexDef(const std::string& name, uint32_t id);
    IndexDef(const std::string& name, uint32_t id, IndexStatus stauts);
    IndexDef(const std::string& name, uint32_t id, const IndexStatus& stauts, ::fedb::type::IndexType type,
             const std::vector<ColumnDef>& column_idx_map);
    const std::string& GetName() const { return name_; }
    inline const std::shared_ptr<ColumnDef>& GetTsColumn() const { return ts_column_; }
    void SetTsColumn(const std::shared_ptr<ColumnDef>& ts_column) { ts_column_ = ts_column; }
    inline bool IsReady() { return status_.load(std::memory_order_acquire) == IndexStatus::kReady; }
    inline uint32_t GetId() const { return index_id_; }
    void SetStatus(IndexStatus status) { status_.store(status, std::memory_order_release); }
    IndexStatus GetStatus() const { return status_.load(std::memory_order_acquire); }
    inline ::fedb::type::IndexType GetType() { return type_; }
    inline const std::vector<ColumnDef>& GetColumns() { return columns_; }
    void SetTTL(const TTLSt& ttl);
    TTLType GetTTLType() const;
    std::shared_ptr<TTLSt> GetTTL() const;
    inline void SetInnerPos(int32_t inner_pos) { inner_pos_ = inner_pos; }
    inline uint32_t GetInnerPos() const { return inner_pos_; }

 private:
    std::string name_;
    uint32_t index_id_;
    uint32_t inner_pos_;
    std::atomic<IndexStatus> status_;
    ::fedb::type::IndexType type_;
    std::vector<ColumnDef> columns_;
    std::shared_ptr<TTLSt> ttl_st_;
    std::shared_ptr<ColumnDef> ts_column_;
};

class InnerIndexSt {
 public:
     InnerIndexSt(uint32_t id, const std::vector<std::shared_ptr<IndexDef>>& index) :
         id_(id), index_(index), ts_() {
        for (const auto& cur_index : index) {
            auto ts_col = cur_index->GetTsColumn();
            if (ts_col && ts_col->GetTsIdx() >= 0) {
                ts_.push_back(ts_col->GetTsIdx());
            }
        }
     }
     inline uint32_t GetId() const { return id_; }
     inline const std::vector<uint32_t>& GetTsIdx() const { return ts_; }
     inline const std::vector<std::shared_ptr<IndexDef>>& GetIndex() const { return index_; }
     uint32_t GetKeyEntryMaxHeight(uint32_t abs_max_height, uint32_t lat_max_height) const;
 private:
     const uint32_t id_;
     const std::vector<std::shared_ptr<IndexDef>> index_;
     std::vector<uint32_t> ts_;
};

bool ColumnDefSortFunc(const ColumnDef& cd_a, const ColumnDef& cd_b);

class TableIndex {
 public:
    TableIndex();
    int ParseFromMeta(const ::fedb::api::TableMeta& table_meta, std::map<std::string, uint8_t>* ts_mapping);
    void ReSet();
    std::shared_ptr<IndexDef> GetIndex(uint32_t idx);
    std::shared_ptr<IndexDef> GetIndex(const std::string& name);
    std::shared_ptr<IndexDef> GetIndex(const std::string& name, uint32_t ts_idx);
    std::shared_ptr<IndexDef> GetIndex(uint32_t idx, uint32_t ts_idx);
    int AddIndex(std::shared_ptr<IndexDef> index_def);
    std::vector<std::shared_ptr<IndexDef>> GetAllIndex();
    std::shared_ptr<std::vector<std::shared_ptr<InnerIndexSt>>> GetAllInnerIndex() const;
    std::shared_ptr<InnerIndexSt> GetInnerIndex(uint32_t idx) const;
    uint32_t Size() const;
    int32_t GetMaxIndexId() const;
    bool HasAutoGen();
    std::shared_ptr<IndexDef> GetPkIndex();
    const std::shared_ptr<IndexDef> GetIndexByCombineStr(const std::string& combine_str);
    bool IsColName(const std::string& name);
    bool IsUniqueColName(const std::string& name);
    int32_t GetInnerIndexPos(uint32_t column_key_pos) const;
    void SetInnerIndexPos(uint32_t column_key_pos, uint32_t inner_pos);
    void AddInnerIndex(const std::shared_ptr<InnerIndexSt>& inner_index);

 private:
    int AddMultiTsIndex(uint32_t pos, const std::shared_ptr<IndexDef>& index);
    void FillIndexVal(const ::fedb::api::TableMeta& table_meta, uint32_t ts_num);

 private:
    std::shared_ptr<std::vector<std::shared_ptr<IndexDef>>> indexs_;
    std::shared_ptr<std::vector<std::vector<std::shared_ptr<IndexDef>>>> multi_ts_indexs_;
    std::shared_ptr<std::vector<std::shared_ptr<InnerIndexSt>>> inner_indexs_;
    std::vector<std::shared_ptr<std::atomic<int32_t>>> column_key_2_inner_index_;
    std::shared_ptr<IndexDef> pk_index_;
    std::shared_ptr<std::unordered_map<std::string, std::shared_ptr<IndexDef>>> combine_col_name_map_;
    std::shared_ptr<std::vector<std::string>> col_name_vec_;
    std::shared_ptr<std::vector<std::string>> unique_col_name_vec_;
};

class PartitionSt {
 public:
    PartitionSt() = default;
    explicit PartitionSt(const ::fedb::nameserver::TablePartition& partitions);
    explicit PartitionSt(const ::fedb::common::TablePartition& partitions);

    inline const std::string& GetLeader() const { return leader_; }
    inline const std::vector<std::string>& GetFollower() const { return follower_; }
    inline uint32_t GetPid() const { return pid_; }

    bool operator==(const PartitionSt& partition_st) const;

 private:
    uint32_t pid_;
    std::string leader_;
    std::vector<std::string> follower_;
};

class TableSt {
 public:
    TableSt() : name_(), db_(), tid_(0), pid_num_(0), partitions_() {}

    explicit TableSt(const ::fedb::nameserver::TableInfo& table_info);

    explicit TableSt(const ::fedb::api::TableMeta& meta);

    inline const std::string& GetName() const { return name_; }

    inline const std::string& GetDB() const { return db_; }

    inline uint32_t GetTid() const { return tid_; }

    std::shared_ptr<std::vector<PartitionSt>> GetPartitions() const {
        return std::atomic_load_explicit(&partitions_, std::memory_order_relaxed);
    }

    PartitionSt GetPartition(uint32_t pid) const {
        auto partitions = std::atomic_load_explicit(&partitions_, std::memory_order_relaxed);
        if (pid < partitions->size()) {
            return partitions->at(pid);
        }
        return PartitionSt();
    }

    bool SetPartition(const PartitionSt& partition_st);

    inline uint32_t GetPartitionNum() const { return pid_num_; }

    inline const ::google::protobuf::RepeatedPtrField<::fedb::common::ColumnDesc>& GetColumns() const {
        return column_desc_;
    }

    inline const ::google::protobuf::RepeatedPtrField<::fedb::common::ColumnKey>& GetColumnKey() const {
        return column_key_;
    }

 private:
    std::string name_;
    std::string db_;
    uint32_t tid_;
    uint32_t pid_num_;
    ::google::protobuf::RepeatedPtrField<::fedb::common::ColumnDesc> column_desc_;
    ::google::protobuf::RepeatedPtrField<::fedb::common::ColumnKey> column_key_;
    std::shared_ptr<std::vector<PartitionSt>> partitions_;
};

}  // namespace storage
}  // namespace fedb
