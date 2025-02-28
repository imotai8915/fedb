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

package com._4paradigm.sql.jdbc;

import com._4paradigm.sql.*;
import com._4paradigm.sql.sdk.SdkOption;
import com._4paradigm.sql.sdk.SqlExecutor;
import com._4paradigm.sql.sdk.impl.SqlClusterExecutor;
import org.testng.Assert;
import org.testng.annotations.Test;

import java.sql.PreparedStatement;
import java.sql.Types;
import java.util.Random;

public class SQLRouterSmokeTest {

    private Random random = new Random(System.currentTimeMillis());

    @Test
    public void testSmoke() {
        SdkOption option = new SdkOption();
        option.setZkPath(TestConfig.ZK_PATH);
        option.setZkCluster(TestConfig.ZK_CLUSTER);
        option.setSessionTimeout(200000);
        try {
            SqlExecutor router = new SqlClusterExecutor(option);
            String dbname = "db" + random.nextInt(100000);
            // create db
            router.dropDB(dbname);
            boolean ok = router.createDB(dbname);
            Assert.assertTrue(ok);
            String ddl = "create table tsql1010 ( col1 bigint, col2 string, index(key=col2, ts=col1));";
            // create table
            ok = router.executeDDL(dbname, ddl);
            Assert.assertTrue(ok);
            // insert normal
            String insert = "insert into tsql1010 values(1000, 'hello');";
            ok = router.executeInsert(dbname, insert);
            Assert.assertTrue(ok);
            // insert placeholder
            String insertPlaceholder = "insert into tsql1010 values(?, ?);";
            SQLInsertRow insertRow = router.getInsertRow(dbname, insertPlaceholder);
            insertRow.Init(5);
            insertRow.AppendInt64(1001);
            insertRow.AppendString("world");
            PreparedStatement impl = router.getInsertPreparedStmt(dbname, insertPlaceholder);
            impl.setLong(1, 1001);
            impl.setString(2, "world");
            ok = impl.execute();
            // ok = router.executeInsert(dbname, insertPlaceholder, insertRow);
            Assert.assertTrue(ok);
            // insert placeholder batch
            SQLInsertRows insertRows = router.getInsertRows(dbname, insertPlaceholder);
            SQLInsertRow row1 = insertRows.NewRow();
            row1.Init(2);
            row1.AppendInt64(1002);
            row1.AppendString("hi");
            SQLInsertRow row2 = insertRows.NewRow();
            row2.Init(4);
            row2.AppendInt64(1003);
            row2.AppendString("word");
            ok = router.executeInsert(dbname, insertPlaceholder, insertRows);
            Assert.assertTrue(ok);
            // select
            String select1 = "select * from tsql1010;";
            ResultSet rs1 = (ResultSet) router.executeSQL(dbname, select1);
            Assert.assertEquals(4, rs1.Size());
            Assert.assertEquals(2, rs1.GetSchema().GetColumnCnt());
            Assert.assertEquals("kTypeInt64", rs1.GetSchema().GetColumnType(0).toString());
            Assert.assertEquals("kTypeString", rs1.GetSchema().GetColumnType(1).toString());
            Assert.assertTrue(rs1.Next());
            Assert.assertEquals("hello", rs1.GetStringUnsafe(1));
            Assert.assertEquals(1000, rs1.GetInt64Unsafe(0));
            Assert.assertTrue(rs1.Next());
            Assert.assertEquals("world", rs1.GetStringUnsafe(1));
            Assert.assertEquals(1001, rs1.GetInt64Unsafe(0));

            String select2 = "select col1 from tsql1010;";
            ResultSet rs2 = (ResultSet) router.executeSQL(dbname, select2);
            Assert.assertEquals(4, rs2.Size());
            Assert.assertEquals(1, rs2.GetSchema().GetColumnCnt());
            Assert.assertEquals("kTypeInt64", rs2.GetSchema().GetColumnType(0).toString());
            Assert.assertTrue(rs2.Next());
            Assert.assertEquals(1000, rs2.GetInt64Unsafe(0));
            Assert.assertTrue(rs2.Next());
            Assert.assertEquals(1001, rs2.GetInt64Unsafe(0));

            String select3 = "select col2 from tsql1010;";
            ResultSet rs3 = (ResultSet) router.executeSQL(dbname, select3);
            Assert.assertEquals(4, rs3.Size());
            Assert.assertEquals(1, rs3.GetSchema().GetColumnCnt());
            Assert.assertEquals("kTypeString", rs3.GetSchema().GetColumnType(0).toString());
            Assert.assertTrue(rs3.Next());
            Assert.assertEquals("hello", rs3.GetStringUnsafe(0));
            Assert.assertTrue(rs3.Next());
            Assert.assertEquals("world", rs3.GetStringUnsafe(0));
            // drop table
            String drop = "drop table tsql1010;";
            ok = router.executeDDL(dbname, drop);
            Assert.assertTrue(ok);
            // insert into deleted table
            ok = router.executeInsert(dbname, insertPlaceholder, insertRow);
            Assert.assertFalse(ok);
            // drop database
            ok = router.dropDB(dbname);
            Assert.assertTrue(ok);
        } catch (Exception e) {
            e.printStackTrace();
            Assert.fail();
        }
    }

    @Test
    public void testInsertMeta() {
        SdkOption option = new SdkOption();
        option.setZkPath(TestConfig.ZK_PATH);
        option.setZkCluster(TestConfig.ZK_CLUSTER);
        option.setSessionTimeout(200000);
        SqlExecutor router = null;
        try {
            router = new SqlClusterExecutor(option);
        }catch (Exception e) {
            Assert.fail();
        }
        String dbname = "db" + random.nextInt(100000);
        // create db
        router.dropDB(dbname);
        boolean ok = router.createDB(dbname);
        Assert.assertTrue(ok);
        String ddl = "create table tsql1010 ( col1 bigint, col2 date, col3 string, col4 string, col5 int, index(key=col3, ts=col1));";
        // create table
        ok = router.executeDDL(dbname, ddl);
        Assert.assertTrue(ok);
        java.sql.Date d1 = new java.sql.Date(2019, 1, 1);
        java.sql.Date d2 = new java.sql.Date(2019, 2, 2);
        java.sql.Date d3 = new java.sql.Date(2019, 3, 3);
        java.sql.Date d4 = new java.sql.Date(2019, 4, 4);
        java.sql.Date d5 = new java.sql.Date(2019, 5, 5);
        String date1 = String.format("%s-%02d-%02d", d1.getYear() + 1900, d1.getMonth(), d1.getDate());
        String fullInsert = String.format("insert into tsql1010 values(1000, '%s', 'guangdong', '广州', 1);", date1);
        try {

            PreparedStatement ps = router.getInsertPreparedStmt(dbname, fullInsert);
            Assert.assertEquals(ps.getMetaData().getColumnCount(), 0);
            ps.close();
        } catch (Exception e) {
            Assert.fail();
        }

        String insert1 = "insert into tsql1010 values(?, '2019-01-12', 'xx', 'xx', 1);";
        try {
            PreparedStatement ps = router.getInsertPreparedStmt(dbname, insert1);
            Assert.assertEquals(ps.getMetaData().getColumnCount(), 1);
            String col = ps.getMetaData().getColumnName(1);
            Assert.assertEquals(col, "col1");
            int type = ps.getMetaData().getColumnType(1);
            Assert.assertEquals(Types.BIGINT, type);
        } catch (Exception e) {
            Assert.fail();
        }

    }

    @Test
    public void testInsertPreparedState() {
        SdkOption option = new SdkOption();
        option.setZkPath(TestConfig.ZK_PATH);
        option.setZkCluster(TestConfig.ZK_CLUSTER);
        option.setSessionTimeout(200000);
        try {
            SqlExecutor router = new SqlClusterExecutor(option);
            String dbname = "db" + random.nextInt(100000);
            // create db
            router.dropDB(dbname);
            boolean ok = router.createDB(dbname);
            Assert.assertTrue(ok);
            String ddl = "create table tsql1010 ( col1 bigint, col2 date, col3 string, col4 string, col5 int, index(key=col3, ts=col1));";
            // create table
            ok = router.executeDDL(dbname, ddl);
            Assert.assertTrue(ok);
            // insert normal
            java.sql.Date d1 = new java.sql.Date(2019, 1, 1);
            java.sql.Date d2 = new java.sql.Date(2019, 2, 2);
            java.sql.Date d3 = new java.sql.Date(2019, 3, 3);
            java.sql.Date d4 = new java.sql.Date(2019, 4, 4);
            java.sql.Date d5 = new java.sql.Date(2019, 5, 5);
            String date1 = String.format("%s-%02d-%02d", d1.getYear() + 1900, d1.getMonth(), d1.getDate());
            String fullInsert = String.format("insert into tsql1010 values(1000, '%s', 'guangdong', '广州', 1);", date1);
            ok = router.executeInsert(dbname, fullInsert);
            Assert.assertTrue(ok);
            Object[][]datas = new Object[][]{
                    {1000l, d1, "guangdong", "广州", 1},
                    {1001l, d2, "jiangsu", "nanjing", 2},
                    {1002l, d3, "sandong", "jinan", 3},
                    {1003l, d4, "zhejiang", "hangzhou", 4},
                    {1004l, d5, "henan", "zhenzhou", 5},
            };
            // insert placeholder
            String date2 = String.format("%s-%s-%s", d2.getYear() + 1900, d2.getMonth(), d2.getDate());
            String insert = String.format("insert into tsql1010 values(?, '%s', 'jiangsu', 'nanjing', 2);", date2);
            PreparedStatement impl = router.getInsertPreparedStmt(dbname, insert);
            impl.setLong(1, 1001);
            try {
                impl.setInt(2, 1002);
            } catch (Exception e) {
                Assert.assertEquals("out of data range", e.getMessage());
            }
            ok = impl.execute();
            Assert.assertTrue(ok);
            insert = "insert into tsql1010 values(1002, ?, ?, 'jinan', 3);";
            PreparedStatement impl2 = router.getInsertPreparedStmt(dbname, insert);
            try {
                impl2.execute();
            } catch (Exception e) {
                Assert.assertEquals("data not enough", e.getMessage());
            }
            try {
                impl2.setString(1, "c");
            } catch (Exception e) {
                Assert.assertEquals("data type not match", e.getMessage());
            }
            impl2.setDate(1, null);
            impl2.setDate(1, d3);
            impl2.setString(2, "sandong");
            ok = impl2.execute();
            Assert.assertTrue(ok);

            insert = "insert into tsql1010 values(?, ?, ?, ?, ?);";
            PreparedStatement impl3 = router.getInsertPreparedStmt(dbname, insert);
            impl3.setLong(1, 1003);
            impl3.setString(3, "zhejiangxx");
            impl3.setString(3, "zhejiang");
            impl3.setString(4, "xxhangzhou");
            impl3.setString(4, "hangzhou");
            impl3.setDate(2, d4);
            impl3.setInt(5, 4);
            impl3.closeOnCompletion();
            Assert.assertTrue(impl3.isCloseOnCompletion());
            ok = impl3.execute();
            Assert.assertTrue(ok);
            try {
                impl3.execute();
            } catch (Exception e) {
                Assert.assertEquals("preparedstatement closed", e.getMessage());
            }
            insert = "insert into tsql1010 values(?, ?, ?, 'zhenzhou', 5);";
            PreparedStatement impl4 = router.getInsertPreparedStmt(dbname, insert);
            impl4.close();
            Assert.assertTrue(impl4.isClosed());
            PreparedStatement impl5 = router.getInsertPreparedStmt(dbname, insert);
            impl5.setLong(1, 1004);
            impl5.setDate(2, d5);
            impl5.setString(3, "henan");
            ok = impl5.execute();
            Assert.assertTrue(ok);
            // select
            String select1 = "select * from tsql1010;";
            ResultSet rs1 = (ResultSet) router.executeSQL(dbname, select1);
            Assert.assertEquals(5, rs1.Size());
            Assert.assertEquals(5, rs1.GetSchema().GetColumnCnt());
            Assert.assertEquals("kTypeInt64", rs1.GetSchema().GetColumnType(0).toString());
            Assert.assertEquals("kTypeDate", rs1.GetSchema().GetColumnType(1).toString());
            Assert.assertEquals("kTypeString", rs1.GetSchema().GetColumnType(2).toString());
            Assert.assertEquals("kTypeString", rs1.GetSchema().GetColumnType(3).toString());
            Assert.assertEquals("kTypeInt32", rs1.GetSchema().GetColumnType(4).toString());
            for (int i = 0; i < rs1.Size(); i++) {
                Assert.assertTrue(rs1.Next());
                int idx = rs1.GetInt32Unsafe(4);
                int suffix = idx - 1;
                Assert.assertTrue(suffix < datas.length);
                Assert.assertEquals(datas[suffix][0], rs1.GetInt64Unsafe(0));
                Assert.assertEquals(datas[suffix][4], idx);
                Assert.assertEquals(datas[suffix][2], rs1.GetStringUnsafe(2));
                Assert.assertEquals(datas[suffix][3], rs1.GetStringUnsafe(3));
                java.sql.Date tmpDate = (java.sql.Date)datas[suffix][1];
                Date getData = rs1.GetStructDateUnsafe(1);
                Assert.assertEquals(tmpDate.getYear() + 1900, getData.getYear());
                Assert.assertEquals(tmpDate.getDate(), getData.getDay());
                Assert.assertEquals(tmpDate.getMonth(), getData.getMonth());
            }

            String select2 = "select col1 from tsql1010;";
            ResultSet rs2 = (ResultSet) router.executeSQL(dbname, select2);
            Assert.assertEquals(5, rs2.Size());
            Assert.assertEquals(1, rs2.GetSchema().GetColumnCnt());
            Assert.assertEquals("kTypeInt64", rs2.GetSchema().GetColumnType(0).toString());

            // drop table
            String drop = "drop table tsql1010;";
            ok = router.executeDDL(dbname, drop);
            Assert.assertTrue(ok);
            // insert into deleted table
            ok = router.executeInsert(dbname, fullInsert);
            Assert.assertFalse(ok);
            // drop database
            ok = router.dropDB(dbname);
            Assert.assertTrue(ok);
        } catch (Exception e) {
            e.printStackTrace();
            Assert.fail();
        }
    }

    @Test
    public void testInsertPreparedStateBatch() {
        Object[][] batchData = new Object[][] {
                {
                        "insert into tsql1010 values(?, ?, 'zhao', 1.0, null, 'z');",
                        new Object[][]{
                                {1000l, 1l}, {1001l, 2l}, {1002l, 3l}, {1003l, 4l},}
                },
                {
                        "insert into tsql1010 values(?, ?, 'zhao', 1.0, null, 'z');",
                        new Object[][] {
                                {1004l, 5l}, {1005l, 6l}, {1006l, 7l}, {1007l, 8l},}
                },
                {
                    "insert into tsql1010 values(?, ?, ?, 2.0, null, ?);",
                    "insert into tsql1010 values(1008, 9, 'zhao', 2.0, null, 'z');",
                    "insert into tsql1010 values(1009, 10, 'zhao', 2.0, null, 'z');",
                    "insert into tsql1010 values(1010, 11, 'zhao', 2.0, null, 'z');",
                }
        };
        SdkOption option = new SdkOption();
        option.setZkPath(TestConfig.ZK_PATH);
        option.setZkCluster(TestConfig.ZK_CLUSTER);
        option.setSessionTimeout(200000);
        try {
            SqlExecutor router = new SqlClusterExecutor(option);
            String dbname = "db" + random.nextInt(100000);
            // create db
            router.dropDB(dbname);
            boolean ok = router.createDB(dbname);
            Assert.assertTrue(ok);
            String ddl = "create table tsql1010 ( col1 bigint, col2 bigint, col3 string, col4 float, col5 date, col6 string, index(key=col2, ts=col1));";
            // create table
            ok = router.executeDDL(dbname, ddl);
            Assert.assertTrue(ok);
            // insert normal
            int i = 0;
            String insertPlaceholder = "insert into tsql1010 values(?, 2, 'taiyuan', 2.0);";
            PreparedStatement impl = router.getInsertPreparedStmt(dbname, (String) batchData[i][0]);
            Object[][] datas1 = (Object[][]) batchData[i][1];
            for (int j = 0; j < datas1.length; j++) {
                try {
                    impl.setInt(2, 1002);
                } catch (Exception e) {
                    Assert.assertEquals("data type not match", e.getMessage());
                }
                try {
                    impl.execute();
                } catch (Exception e) {
                    if (j > 1) {
                        Assert.assertEquals("please use executeBatch", e.getMessage());
                    } else {
                        Assert.assertEquals("data not enough", e.getMessage());
                    }
                }
                impl.setLong(1, (Long) datas1[j][0]);
                impl.setLong(2, (Long) datas1[j][1]);
                impl.addBatch();
            }
            try {
                ok = impl.execute();
            } catch (Exception e) {
                Assert.assertEquals("please use executeBatch", e.getMessage());
            }
            impl.executeBatch();
            Assert.assertTrue(ok);
            String select1 = "select * from tsql1010;";
            ResultSet rs1 = (ResultSet) router.executeSQL(dbname, select1);
            Assert.assertEquals(datas1.length, rs1.Size());
            Assert.assertEquals(6, rs1.GetSchema().GetColumnCnt());

            i++;
            PreparedStatement impl2 = router.getInsertPreparedStmt(dbname, (String) batchData[i][0]);
            datas1 = (Object[][]) batchData[i][1];
            for (int j = 0; j < datas1.length; j++) {
                try {
                    impl2.setInt(2, 1002);
                } catch (Exception e) {
                    Assert.assertEquals("data type not match", e.getMessage());
                }
                try {
                    impl2.execute();
                } catch (Exception e) {
                    if (j > 1) {
                        Assert.assertEquals("please use executeBatch", e.getMessage());
                    } else {
                        Assert.assertEquals("data not enough", e.getMessage());
                    }
                }
                impl2.setLong(1, (Long) datas1[j][0]);
                impl2.setLong(2, (Long) datas1[j][1]);
                impl2.addBatch();
            }
            try {
                ok = impl2.execute();
            } catch (Exception e) {
                Assert.assertEquals("please use executeBatch", e.getMessage());
            }
            i++;
            Object[] datas2 = (Object[]) batchData[i];
            try {
                impl2.addBatch((String) datas2[0]);
            } catch (Exception e) {
                Assert.assertEquals("this sql need data", e.getMessage());
            }
            for (int j = 1; i < datas2.length; i++) {
                impl2.addBatch((String) datas2[j]);
            }
            impl.executeBatch();
            Assert.assertTrue(ok);
            String select2 = "select * from tsql1010;";
            ResultSet rs2 = (ResultSet) router.executeSQL(dbname, select1);
            Assert.assertEquals(datas1.length + datas2.length, rs2.Size());
            Assert.assertEquals(6, rs1.GetSchema().GetColumnCnt());

            // drop table
            String drop = "drop table tsql1010;";
            ok = router.executeDDL(dbname, drop);
            Assert.assertTrue(ok);
            // insert into deleted table
            ok = router.executeInsert(dbname, "insert into tsql1010 values(1009, 10, 'zhao', 2.0, null, 'z')");
            Assert.assertFalse(ok);
            // drop database
            ok = router.dropDB(dbname);
            Assert.assertTrue(ok);
        } catch (Exception e) {
            e.printStackTrace();
            Assert.fail();
        }
    }
}
