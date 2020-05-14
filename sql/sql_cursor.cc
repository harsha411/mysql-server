/* Copyright (c) 2005, 2020, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/sql_cursor.h"

#include <sys/types.h>

#include <utility>  // move

#include "memory_debugging.h"
#include "my_alloc.h"
#include "my_base.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "mysql/components/services/psi_statement_bits.h"
#include "mysql_com.h"
#include "sql/debug_sync.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/parse_tree_node_base.h"
#include "sql/protocol.h"
#include "sql/query_options.h"
#include "sql/query_result.h"
#include "sql/sql_cmd_dml.h"  // Sql_cmd_dml
#include "sql/sql_digest_stream.h"
#include "sql/sql_lex.h"
#include "sql/sql_list.h"
#include "sql/sql_parse.h"      // mysql_execute_command
#include "sql/sql_tmp_table.h"  // tmp tables
#include "sql/sql_union.h"      // Query_result_union
#include "sql/system_variables.h"
#include "sql/table.h"
#include "sql/thd_raii.h"  // Prepared_stmt_arena_holder

/****************************************************************************
  Declarations.
****************************************************************************/

/**
  Materialized_cursor -- an insensitive materialized server-side
  cursor. The result set of this cursor is saved in a temporary
  table at open. The cursor itself is simply an interface for the
  handler of the temporary table.

  The materialized cursor is usually attached to a preparable statement
  through a query result object. The lifetime of the cursor is the same
  as the lifetime of the preparable statement. When the preparable statement
  is destroyed, the materialized cursor (including the temporary table) is
  also destroyed.
*/

class Materialized_cursor final : public Server_side_cursor {
  /// A fake unit to supply to Query_result_send when fetching
  SELECT_LEX_UNIT fake_unit;
  /// Cursor to the table that contains the materialized result
  TABLE *table{nullptr};
  /**
    List of items to send to client, copy of original items, but created in
    the cursor object's mem_root.
  */
  List<Item> item_list;
  ulong fetch_limit{0};
  ulong fetch_count{0};
  bool is_rnd_inited{false};

 public:
  Materialized_cursor(Query_result *result);
  void set_table(TABLE *table_arg);
  void set_result(Query_result *result_arg) { result = result_arg; }
  int send_result_set_metadata(THD *thd, List<Item> &send_result_set_metadata);
  bool is_open() const override { return table->has_storage_handler(); }
  bool open(THD *) override;
  bool fetch(ulong num_rows) override;
  void close() override;
  ~Materialized_cursor() override;
};

/**
  Query_result_materialize -- a mediator between a cursor query and the
  protocol. In case we were not able to open a non-materialzed
  cursor, it creates an internal temporary memory table, and inserts
  all rows into it. If the table is in the Heap engine and if it reaches
  maximum Heap table size, it's converted to a disk-based temporary
  table. Later this table is used to create a Materialized_cursor.
*/

class Query_result_materialize final : public Query_result_union {
  Query_result *result; /**< the result object of the caller (PS or SP) */
 public:
  Materialized_cursor *materialized_cursor;
  Query_result_materialize(Query_result *result_arg)
      : Query_result_union(),
        result(result_arg),
        materialized_cursor(nullptr) {}
  ~Query_result_materialize() override { delete materialized_cursor; }
  void set_result(Query_result *result_arg) {
    result = result_arg;
    if (materialized_cursor != nullptr)
      materialized_cursor->set_result(result_arg);
  }
  bool check_simple_select() const override { return false; }
  bool prepare(THD *thd, List<Item> &list, SELECT_LEX_UNIT *u) override;
  bool start_execution(THD *thd) override;
  bool send_result_set_metadata(THD *thd, List<Item> &list,
                                uint flags) override;
  void cleanup(THD *) override {}
};

/**************************************************************************/

/**
  Attempt to open a materialized cursor.

  @param      thd           thread handle
  @param[in]  result        result class of the caller used as a destination
                            for the rows fetched from the cursor
  @param[in,out] pcursor    a pointer to store a pointer to cursor in
                            If non-NULL on entry, use the supplied cursor.
                            Must be NULL on first invocation.

  @return Error status

  @returns false on success, true on error

  @note
  On first invocation, mysql_open_cursor creates a query result object
  for management of the materialized result. When this cursor is prepared,
  it creates a materialized cursor object (Materialized_cursor) inside
  the cursor. In addition, an application specific result object supplied
  as argument is attached to the query result object.
  The query result object is also attached to the current prepared statement.
  A reference to the cursor object is returned in pcursor.
  The statement may or may not be prepared on first invocation,
  it is prepared if necessary.

  On subsequent invocations, the query result object is located inside
  the preparable statement and the cursor object is located inside this.
  A reference to the cursor object is returned in pcursor.

  On all invocations, the statement is executed and a temporary table managed
  by the cursor object is populated with the result set.
*/

bool mysql_open_cursor(THD *thd, Query_result *result,
                       Server_side_cursor **pcursor) {
  sql_digest_state *parent_digest;
  PSI_statement_locker *parent_locker;
  Query_result_materialize *result_materialize = nullptr;
  LEX *lex = thd->lex;

  Sql_cmd_dml *sql_cmd = lex->m_sql_cmd != nullptr && lex->m_sql_cmd->is_dml()
                             ? down_cast<Sql_cmd_dml *>(lex->m_sql_cmd)
                             : nullptr;

  // Only DML statements may have assigned a cursor.
  if (sql_cmd == nullptr) {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), "with cursor");
    return true;
  }

  /*
    Create the result object for materialization.
    Three situations are possible here:
    1. If this is a preparable un-prepared statement (may happen for statements
       that are part of stored procedures), create object in statement mem_root.
    2. If this is a prepared statement but no result object for materialization
       exists, create object in statement mem_root.
       Since the statement is already prepared, explicitly prepare the
       result object, which includes creating the temporary table.
    3. If this is a prepared statement for which a result object for
       materialization exists, reuse this object.

    Cursors are not supported for regular (non-prepared, non-SP) statements,
    and the statement must return data (usually a SELECT statement).
  */
  if (!sql_cmd->may_use_cursor() || sql_cmd->is_regular()) {
  } else if (!sql_cmd->is_prepared()) {
    Prepared_stmt_arena_holder ps_arena_holder(thd);

    result_materialize = new (thd->mem_root) Query_result_materialize(result);
    if (result_materialize == nullptr) return true;
  } else if (lex->result == nullptr) {
    Prepared_stmt_arena_holder ps_arena_holder(thd);

    result_materialize = new (thd->mem_root) Query_result_materialize(result);
    if (result_materialize == nullptr) return true;

    sql_cmd->set_query_result(result_materialize);

    // Signal that query result must be prepared on execution
    sql_cmd->set_lazy_result();
  } else {
    result_materialize =
        down_cast<Query_result_materialize *>(sql_cmd->query_result());
    DBUG_ASSERT(sql_cmd->query_result() == result_materialize);
    result_materialize->set_result(result);
  }

  // Pass the Query_result_materialize object to the query
  lex->result = result_materialize;

  parent_digest = thd->m_digest;
  parent_locker = thd->m_statement_psi;
  thd->m_digest = nullptr;
  thd->m_statement_psi = nullptr;

  bool rc = mysql_execute_command(thd);

  thd->m_digest = parent_digest;
  DEBUG_SYNC(thd, "after_table_close");
  thd->m_statement_psi = parent_locker;

  /*
    Possible options here:
    - a materialized cursor is open. In this case rc is 0 and
      result_materialize->materialized is not NULL
    - an error occurred during materialization.
      result_materialize->materialized_cursor is not NULL, but rc != 0
    - successful completion of mysql_execute_command without
      a cursor: rc is 0, result_materialize->materialized_cursor is NULL.
      This is possible if some command writes directly to the
      network, bypassing Query_result mechanism. An example of
      such command is SHOW VARIABLES or SHOW STATUS.
  */
  if (rc) {
    if (result_materialize->materialized_cursor) {
      /* Rollback metadata in the client-server protocol. */
      result_materialize->abort_result_set(thd);

      delete result_materialize->materialized_cursor;
      result_materialize->materialized_cursor = nullptr;
    }

    return true;
  }

  if (result_materialize != nullptr &&
      result_materialize->materialized_cursor) {
    Materialized_cursor *materialized_cursor =
        result_materialize->materialized_cursor;

    /*
      NOTE: close_thread_tables() has been called in
      mysql_execute_command(), so all tables except from the cursor
      temporary table have been closed.
    */

    if (materialized_cursor->open(thd)) {
      delete materialized_cursor;
      return true;
    }

    if (*pcursor == nullptr) *pcursor = materialized_cursor;
  }

  return false;
}

/****************************************************************************
  Server_side_cursor
****************************************************************************/

void Server_side_cursor::operator delete(void *, size_t) {}

/***************************************************************************
 Materialized_cursor
****************************************************************************/

Materialized_cursor::Materialized_cursor(Query_result *result_arg)
    : Server_side_cursor(result_arg), fake_unit(CTX_NONE) {}

/// Bind a temporary table with a materialized cursor.
void Materialized_cursor::set_table(TABLE *table_arg) { table = table_arg; }

/**
  Preserve the original metadata to be sent to the client.
  Initiate sending of the original metadata to the client
  (call Protocol::send_result_set_metadata()).

  @param thd Thread identifier.
  @param send_result_set_metadata List of fields that would be sent.
*/

int Materialized_cursor::send_result_set_metadata(
    THD *thd, List<Item> &send_result_set_metadata) {
  /*
    Create objects in the mem_root of the cursor. The item list will be
    referenced after the execution of the current statement, so it cannot
    created on the execution mem_root.
  */
  Query_arena backup_arena;
  thd->swap_query_arena(m_arena, &backup_arena);
  if (item_list.is_empty()) {
    if (table->fill_item_list(&item_list)) {
      thd->swap_query_arena(backup_arena, &m_arena);
      return true;
    }

    DBUG_ASSERT(send_result_set_metadata.elements == item_list.elements);
    /*
      Unless we preserve the original metadata, it will be lost,
      since new fields describe columns of the temporary table.
      Allocate a copy of the name for safety only. Currently
      items with original names are always kept in memory,
      but in case this changes a memory leak may be hard to notice.
    */
    List_iterator_fast<Item> it_org(send_result_set_metadata);
    List_iterator_fast<Item> it_dst(item_list);
    Item *item_org;
    Item *item_dst;
    while ((item_dst = it_dst++, item_org = it_org++)) {
      Send_field send_field;
      Item_ident *ident = down_cast<Item_ident *>(item_dst);
      item_org->make_field(&send_field);

      ident->db_name = thd->mem_strdup(send_field.db_name);
      ident->table_name = thd->mem_strdup(send_field.table_name);
    }
  }

  /*
    Original metadata result set should be sent here. After
    mysql_execute_command() is finished, item_list can not be used for
    sending metadata, because it references closed table.
  */
  if (result->send_result_set_metadata(thd, item_list,
                                       Protocol::SEND_NUM_ROWS)) {
    thd->swap_query_arena(backup_arena, &m_arena);
    return true;
  }

  thd->swap_query_arena(backup_arena, &m_arena);

  DBUG_ASSERT(!thd->is_error());

  return false;
}

bool Materialized_cursor::open(THD *thd) {
  bool rc;
  Query_arena backup_arena;

  thd->swap_query_arena(m_arena, &backup_arena);

  /* Create a list of fields and start sequential scan. */

  rc = result->prepare(thd, item_list, &fake_unit);
  rc = !rc && table->file->ha_rnd_init(true);
  is_rnd_inited = !rc;

  thd->swap_query_arena(backup_arena, &m_arena);

  /* Commit or rollback metadata in the client-server protocol. */

  if (!rc) {
    thd->server_status |= SERVER_STATUS_CURSOR_EXISTS;
    result->send_eof(thd);
  } else {
    result->abort_result_set(thd);
  }

  fetch_limit = 0;
  fetch_count = 0;

  return rc;
}

/**
  Fetch up to the given number of rows from a materialized cursor.

    Precondition: the cursor is open.

    If the cursor points after the last row, the fetch will automatically
    close the cursor and not send any data (except the 'EOF' packet
    with SERVER_STATUS_LAST_ROW_SENT). This is an extra round trip
    and probably should be improved to return
    SERVER_STATUS_LAST_ROW_SENT along with the last row.
*/

bool Materialized_cursor::fetch(ulong num_rows) {
  THD *thd = table->in_use;

  int res = 0;
  result->begin_dataset();
  for (fetch_limit += num_rows; fetch_count < fetch_limit; fetch_count++) {
    if ((res = table->file->ha_rnd_next(table->record[0]))) break;
    /* Send data only if the read was successful. */
    /*
      If network write failed (i.e. due to a closed socked),
      the error has already been set. Return true if the error
      is set.
    */
    if (result->send_data(thd, item_list)) return true;
  }

  switch (res) {
    case 0:
      thd->server_status |= SERVER_STATUS_CURSOR_EXISTS;
      result->send_eof(thd);
      break;
    case HA_ERR_END_OF_FILE:
      thd->server_status |= SERVER_STATUS_LAST_ROW_SENT;
      result->send_eof(thd);
      close();
      break;
    default:
      table->file->print_error(res, MYF(0));
      close();
      return true;
  }

  return false;
}

void Materialized_cursor::close() {
  if (is_rnd_inited) {
    (void)table->file->ha_rnd_end();
    is_rnd_inited = false;
  }
  close_tmp_table(table->in_use, table);
  m_arena.free_items();
  item_list.empty();
  mem_root.ClearForReuse();
}

Materialized_cursor::~Materialized_cursor() {
  DBUG_ASSERT(!is_open());
  if (table != nullptr) free_tmp_table(table);
}

/***************************************************************************
 Query_result_materialize
****************************************************************************/

bool Query_result_materialize::prepare(THD *thd, List<Item> &list,
                                       SELECT_LEX_UNIT *u) {
  unit = u;

  if (result->prepare(thd, list, u)) return true;

  DBUG_ASSERT(table == nullptr && materialized_cursor == nullptr);

  materialized_cursor = new (thd->mem_root) Materialized_cursor(result);
  if (materialized_cursor == nullptr) return true;
  /*
    Objects associated with the temporary table should be created as follows:
    - Metadata about the temporary table are created on the Statement mem_root.
      This mem_root should be bound to THD when this function is called.
    - HANDLER objects are created on the mem_root of the materialized cursor,
      since the handler must be kept open for subsequent FETCH operations.
      This must be ensured when the temporary table is instantiated.
  */
  // @todo Replace "columns" with u->get_unit_column_types() when assert fixed
  List<Item> *columns =
      u->is_union() ? &u->types : &u->first_select()->fields_list;
  if (create_result_table(thd, columns, false,
                          thd->variables.option_bits | TMP_TABLE_ALL_COLUMNS,
                          "", false, false)) {
    delete materialized_cursor;
    return true;
  }
  materialized_cursor->set_table(table);

  return false;
}

bool Query_result_materialize::start_execution(THD *thd) {
  // If UNION, we may call this function multiple times.
  if (table->is_created()) return false;

  MEM_ROOT *saved_mem_root = thd->mem_root;
  thd->mem_root = &materialized_cursor->mem_root;
  if (instantiate_tmp_table(thd, table)) {
    thd->mem_root = saved_mem_root;
    return true;
  }

  table->file->ha_extra(HA_EXTRA_IGNORE_DUP_KEY);
  if (table->hash_field) table->file->ha_index_init(0, false);
  thd->mem_root = saved_mem_root;

  return false;
}

bool Query_result_materialize::send_result_set_metadata(THD *thd,
                                                        List<Item> &list,
                                                        uint) {
  if (materialized_cursor->send_result_set_metadata(thd, list)) {
    return true;
  }

  return false;
}
