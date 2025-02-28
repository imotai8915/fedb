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

%module sql_router_sdk
%include "std_unique_ptr.i"
%include std_string.i
%include std_shared_ptr.i
%include stl.i
%include stdint.i
%include std_vector.i
#ifdef SWIGJAVA
%include various.i
%apply char *BYTE { char *string_buffer_var_name };
#endif

%shared_ptr(hybridse::sdk::ResultSet);
%shared_ptr(hybridse::sdk::Schema);
%shared_ptr(fedb::sdk::SQLRouter);
%shared_ptr(fedb::sdk::SQLRequestRow);
%shared_ptr(fedb::sdk::SQLRequestRowBatch);
%shared_ptr(fedb::sdk::ColumnIndicesSet);
%shared_ptr(fedb::sdk::SQLInsertRow);
%shared_ptr(fedb::sdk::SQLInsertRows);
%shared_ptr(fedb::sdk::ExplainInfo);
%shared_ptr(hybridse::sdk::ProcedureInfo);
%shared_ptr(fedb::sdk::QueryFuture);
%shared_ptr(fedb::sdk::TableReader);
%template(VectorUint32) std::vector<uint32_t>;
%template(VectorString) std::vector<std::string>;

%{
#include "sdk/sql_router.h"
#include "sdk/result_set.h"
#include "sdk/base.h"
#include "sdk/sql_request_row.h"
#include "sdk/sql_insert_row.h"
#include "sdk/table_reader.h"

using hybridse::sdk::Schema;
using hybridse::sdk::ResultSet;
using fedb::sdk::SQLRouter;
using fedb::sdk::SQLRouterOptions;
using fedb::sdk::SQLRequestRow;
using fedb::sdk::SQLRequestRowBatch;
using fedb::sdk::ColumnIndicesSet;
using fedb::sdk::SQLInsertRow;
using fedb::sdk::SQLInsertRows;
using fedb::sdk::ExplainInfo;
using hybridse::sdk::ProcedureInfo;
using fedb::sdk::QueryFuture;
using fedb::sdk::TableReader;
%}

%include "sdk/sql_router.h"
%include "sdk/base.h"
%include "sdk/result_set.h"
%include "sdk/sql_request_row.h"
%include "sdk/sql_insert_row.h"
%include "sdk/table_reader.h"
