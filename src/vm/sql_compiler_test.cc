/*
 * sql_compiler_test.cc
 * Copyright (C) 4paradigm.com 2019 wangtaize <wangtaize@4paradigm.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "vm/sql_compiler.h"
#include <memory>
#include <utility>
#include "boost/algorithm/string.hpp"
#include "case/sql_case.h"
#include "gtest/gtest.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "parser/parser.h"
#include "plan/planner.h"
#include "tablet/tablet_catalog.h"
#include "vm/simple_catalog.h"
#include "vm/test_base.h"

using namespace llvm;       // NOLINT
using namespace llvm::orc;  // NOLINT

ExitOnError ExitOnErr;

namespace fesql {
namespace vm {

using fesql::sqlcase::SQLCase;
std::vector<SQLCase> InitCases(std::string yaml_path);
void InitCases(std::string yaml_path, std::vector<SQLCase>& cases);  // NOLINT

void InitCases(std::string yaml_path, std::vector<SQLCase>& cases) {  // NOLINT
    if (!SQLCase::CreateSQLCasesFromYaml(
            fesql::sqlcase::FindFesqlDirPath(), yaml_path, cases,
            std::vector<std::string>({"physical-plan-unsupport",
                                      "plan-unsupport", "parser-unsupport"}))) {
        FAIL();
    }
}
std::vector<SQLCase> InitCases(std::string yaml_path) {
    std::vector<SQLCase> cases;
    InitCases(yaml_path, cases);
    return cases;
}
class SQLCompilerTest : public ::testing::TestWithParam<SQLCase> {};
INSTANTIATE_TEST_CASE_P(
    SqlSimpleQueryParse, SQLCompilerTest,
    testing::ValuesIn(InitCases("cases/plan/simple_query.yaml")));
INSTANTIATE_TEST_CASE_P(
    SqlWindowQueryParse, SQLCompilerTest,
    testing::ValuesIn(InitCases("cases/plan/window_query.yaml")));

INSTANTIATE_TEST_CASE_P(
    SqlWherePlan, SQLCompilerTest,
    testing::ValuesIn(InitCases("cases/plan/where_query.yaml")));

INSTANTIATE_TEST_CASE_P(
    SqlGroupPlan, SQLCompilerTest,
    testing::ValuesIn(InitCases("cases/plan/group_query.yaml")));

INSTANTIATE_TEST_CASE_P(
    SqlJoinPlan, SQLCompilerTest,
    testing::ValuesIn(InitCases("cases/plan/join_query.yaml")));

void CompilerCheck(std::shared_ptr<Catalog> catalog, const std::string sql,
                   EngineMode engine_mode,
                   const bool enable_batch_window_paralled) {
    SQLCompiler sql_compiler(catalog, false, true, false);
    SQLContext sql_context;
    sql_context.sql = sql;
    sql_context.db = "db";
    sql_context.engine_mode = engine_mode;
    sql_context.is_performance_sensitive = false;
    sql_context.enable_batch_window_parallelization =
        enable_batch_window_paralled;
    base::Status compile_status;
    bool ok = sql_compiler.Compile(sql_context, compile_status);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(nullptr != sql_context.physical_plan);
    std::ostringstream oss;
    sql_context.physical_plan->Print(oss, "");
    std::cout << "physical plan:\n" << sql << "\n" << oss.str() << std::endl;

    std::ostringstream oss_schema;
    PrintSchema(oss_schema, sql_context.schema);
    std::cout << "schema:\n" << oss_schema.str();
}
void CompilerCheck(std::shared_ptr<Catalog> catalog, const std::string sql,
                   EngineMode engine_mode) {
    CompilerCheck(catalog, sql, engine_mode, false);
}
void RequestSchemaCheck(std::shared_ptr<Catalog> catalog, const std::string sql,
                        const type::TableDef& exp_table_def) {
    SQLCompiler sql_compiler(catalog);
    SQLContext sql_context;
    sql_context.sql = sql;
    sql_context.db = "db";
    sql_context.engine_mode = kRequestMode;
    sql_context.is_performance_sensitive = false;
    base::Status compile_status;
    bool ok = sql_compiler.Compile(sql_context, compile_status);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(nullptr != sql_context.physical_plan);
    std::ostringstream oss;
    sql_context.physical_plan->Print(oss, "");
    std::cout << "physical plan:\n" << sql << "\n" << oss.str() << std::endl;

    std::ostringstream oss_schema;
    PrintSchema(oss_schema, sql_context.schema);
    std::cout << "schema:\n" << oss_schema.str();

    std::ostringstream oss_request_schema;
    PrintSchema(oss_schema, sql_context.request_schema);
    std::cout << "request schema:\n" << oss_request_schema.str();

    ASSERT_EQ(sql_context.request_name, exp_table_def.name());
    ASSERT_EQ(sql_context.request_schema.size(),
              exp_table_def.columns().size());
    for (int i = 0; i < sql_context.request_schema.size(); i++) {
        ASSERT_EQ(sql_context.request_schema.Get(i).DebugString(),
                  exp_table_def.columns().Get(i).DebugString());
    }
}

TEST_P(SQLCompilerTest, compile_request_mode_test) {
    if (boost::contains(GetParam().mode(), "request-unsupport")) {
        LOG(INFO) << "Skip sql case: request unsupport";
        return;
    }
    std::string sqlstr = GetParam().sql_str();
    LOG(INFO) << sqlstr;

    const fesql::base::Status exp_status(::fesql::common::kOk, "ok");
    boost::to_lower(sqlstr);
    LOG(INFO) << sqlstr;
    std::cout << sqlstr << std::endl;

    fesql::type::TableDef table_def;
    fesql::type::TableDef table_def2;
    fesql::type::TableDef table_def3;
    fesql::type::TableDef table_def4;
    fesql::type::TableDef table_def5;
    fesql::type::TableDef table_def6;

    BuildTableDef(table_def);
    BuildTableDef(table_def2);
    BuildTableDef(table_def3);
    BuildTableDef(table_def4);
    BuildTableDef(table_def5);
    BuildTableDef(table_def6);

    table_def.set_name("t1");
    table_def2.set_name("t2");
    table_def3.set_name("t3");
    table_def4.set_name("t4");
    table_def5.set_name("t5");
    table_def6.set_name("t6");

    std::shared_ptr<::fesql::storage::Table> table(
        new ::fesql::storage::Table(1, 1, table_def));
    std::shared_ptr<::fesql::storage::Table> table2(
        new ::fesql::storage::Table(2, 1, table_def2));
    std::shared_ptr<::fesql::storage::Table> table3(
        new ::fesql::storage::Table(3, 1, table_def3));
    std::shared_ptr<::fesql::storage::Table> table4(
        new ::fesql::storage::Table(4, 1, table_def4));
    std::shared_ptr<::fesql::storage::Table> table5(
        new ::fesql::storage::Table(5, 1, table_def5));
    std::shared_ptr<::fesql::storage::Table> table6(
        new ::fesql::storage::Table(6, 1, table_def6));

    ::fesql::type::IndexDef* index = table_def.add_indexes();
    index->set_name("index12");
    index->add_first_keys("col1");
    index->add_first_keys("col2");
    index->set_second_key("col5");
    auto catalog = BuildCommonCatalog(table_def, table);
    AddTable(catalog, table_def2, table2);
    AddTable(catalog, table_def3, table3);
    AddTable(catalog, table_def4, table4);
    AddTable(catalog, table_def5, table5);
    AddTable(catalog, table_def6, table6);
    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("tb");
        std::shared_ptr<::fesql::storage::Table> table(
            new fesql::storage::Table(1, 1, table_def));
        AddTable(catalog, table_def, table);
    }
    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("tc");
        std::shared_ptr<::fesql::storage::Table> table(
            new fesql::storage::Table(1, 1, table_def));
        AddTable(catalog, table_def, table);
    }
    CompilerCheck(catalog, sqlstr, kRequestMode);
    RequestSchemaCheck(catalog, sqlstr, table_def);
}

TEST_P(SQLCompilerTest, compile_batch_mode_test) {
    if (boost::contains(GetParam().mode(), "batch-unsupport")) {
        LOG(INFO) << "Skip sql case: batch unsupport";
        return;
    }
    std::string sqlstr = GetParam().sql_str();
    LOG(INFO) << sqlstr;

    const fesql::base::Status exp_status(::fesql::common::kOk, "ok");
    boost::to_lower(sqlstr);
    LOG(INFO) << sqlstr;
    std::cout << sqlstr << std::endl;

    fesql::type::TableDef table_def;
    fesql::type::TableDef table_def2;
    fesql::type::TableDef table_def3;
    fesql::type::TableDef table_def4;
    fesql::type::TableDef table_def5;
    fesql::type::TableDef table_def6;

    BuildTableDef(table_def);
    BuildTableDef(table_def2);
    BuildTableDef(table_def3);
    BuildTableDef(table_def4);
    BuildTableDef(table_def5);
    BuildTableDef(table_def6);

    table_def.set_name("t1");
    table_def2.set_name("t2");
    table_def3.set_name("t3");
    table_def4.set_name("t4");
    table_def5.set_name("t5");
    table_def6.set_name("t6");

    std::shared_ptr<::fesql::storage::Table> table(
        new ::fesql::storage::Table(1, 1, table_def));
    std::shared_ptr<::fesql::storage::Table> table2(
        new ::fesql::storage::Table(2, 1, table_def2));
    std::shared_ptr<::fesql::storage::Table> table3(
        new ::fesql::storage::Table(3, 1, table_def3));
    std::shared_ptr<::fesql::storage::Table> table4(
        new ::fesql::storage::Table(4, 1, table_def4));
    std::shared_ptr<::fesql::storage::Table> table5(
        new ::fesql::storage::Table(5, 1, table_def5));
    std::shared_ptr<::fesql::storage::Table> table6(
        new ::fesql::storage::Table(6, 1, table_def6));

    ::fesql::type::IndexDef* index = table_def.add_indexes();
    index->set_name("index12");
    index->add_first_keys("col1");
    index->add_first_keys("col2");
    index->set_second_key("col5");
    auto catalog = BuildCommonCatalog(table_def, table);
    AddTable(catalog, table_def2, table2);
    AddTable(catalog, table_def3, table3);
    AddTable(catalog, table_def4, table4);
    AddTable(catalog, table_def5, table5);
    AddTable(catalog, table_def6, table6);

    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("tb");
        std::shared_ptr<::fesql::storage::Table> table(
            new fesql::storage::Table(1, 1, table_def));
        AddTable(catalog, table_def, table);
    }
    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("tc");
        std::shared_ptr<::fesql::storage::Table> table(
            new fesql::storage::Table(1, 1, table_def));
        AddTable(catalog, table_def, table);
    }
    CompilerCheck(catalog, sqlstr, kBatchMode, false);

    // Check for work with simple catalog
    auto simple_catalog = std::make_shared<SimpleCatalog>();
    fesql::type::Database db;
    db.set_name("db");
    {
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def;
    }
    {
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def2;
    }
    {
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def3;
    }
    {
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def4;
    }
    {
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def5;
    }
    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("ta");
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def;
    }
    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("tb");
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def;
    }
    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("tc");
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def;
    }

    simple_catalog->AddDatabase(db);
    CompilerCheck(simple_catalog, sqlstr, kBatchMode, false);
}

TEST_P(SQLCompilerTest, compile_batch_mode_enable_window_paralled_test) {
    if (boost::contains(GetParam().mode(), "batch-unsupport")) {
        LOG(INFO) << "Skip sql case: batch unsupport";
        return;
    }
    std::string sqlstr = GetParam().sql_str();
    LOG(INFO) << sqlstr;

    const fesql::base::Status exp_status(::fesql::common::kOk, "ok");
    boost::to_lower(sqlstr);
    LOG(INFO) << sqlstr;
    std::cout << sqlstr << std::endl;

    fesql::type::TableDef table_def;
    fesql::type::TableDef table_def2;
    fesql::type::TableDef table_def3;
    fesql::type::TableDef table_def4;
    fesql::type::TableDef table_def5;
    fesql::type::TableDef table_def6;

    BuildTableDef(table_def);
    BuildTableDef(table_def2);
    BuildTableDef(table_def3);
    BuildTableDef(table_def4);
    BuildTableDef(table_def5);
    BuildTableDef(table_def6);

    table_def.set_name("t1");
    table_def2.set_name("t2");
    table_def3.set_name("t3");
    table_def4.set_name("t4");
    table_def5.set_name("t5");
    table_def6.set_name("t6");

    std::shared_ptr<::fesql::storage::Table> table(
        new ::fesql::storage::Table(1, 1, table_def));
    std::shared_ptr<::fesql::storage::Table> table2(
        new ::fesql::storage::Table(2, 1, table_def2));
    std::shared_ptr<::fesql::storage::Table> table3(
        new ::fesql::storage::Table(3, 1, table_def3));
    std::shared_ptr<::fesql::storage::Table> table4(
        new ::fesql::storage::Table(4, 1, table_def4));
    std::shared_ptr<::fesql::storage::Table> table5(
        new ::fesql::storage::Table(5, 1, table_def5));
    std::shared_ptr<::fesql::storage::Table> table6(
        new ::fesql::storage::Table(6, 1, table_def6));

    ::fesql::type::IndexDef* index = table_def.add_indexes();
    index->set_name("index12");
    index->add_first_keys("col1");
    index->add_first_keys("col2");
    index->set_second_key("col5");
    auto catalog = BuildCommonCatalog(table_def, table);
    AddTable(catalog, table_def2, table2);
    AddTable(catalog, table_def3, table3);
    AddTable(catalog, table_def4, table4);
    AddTable(catalog, table_def5, table5);
    AddTable(catalog, table_def6, table6);

    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("tb");
        std::shared_ptr<::fesql::storage::Table> table(
            new fesql::storage::Table(1, 1, table_def));
        AddTable(catalog, table_def, table);
    }
    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("tc");
        std::shared_ptr<::fesql::storage::Table> table(
            new fesql::storage::Table(1, 1, table_def));
        AddTable(catalog, table_def, table);
    }
    CompilerCheck(catalog, sqlstr, kBatchMode, true);

    // Check for work with simple catalog
    auto simple_catalog = std::make_shared<SimpleCatalog>();
    fesql::type::Database db;
    db.set_name("db");
    {
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def;
    }
    {
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def2;
    }
    {
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def3;
    }
    {
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def4;
    }
    {
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def5;
    }
    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("ta");
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def;
    }
    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("tb");
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def;
    }
    {
        fesql::type::TableDef table_def;
        BuildTableA(table_def);
        table_def.set_name("tc");
        ::fesql::type::TableDef* p_table = db.add_tables();
        *p_table = table_def;
    }

    simple_catalog->AddDatabase(db);
    CompilerCheck(simple_catalog, sqlstr, kBatchMode, true);
}

}  // namespace vm
}  // namespace fesql

int main(int argc, char** argv) {
    ::testing::GTEST_FLAG(color) = "yes";
    ::testing::InitGoogleTest(&argc, argv);
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    return RUN_ALL_TESTS();
}
