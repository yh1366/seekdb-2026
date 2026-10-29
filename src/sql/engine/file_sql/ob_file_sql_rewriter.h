/**
 * Copyright (c) 2021 OceanBase
 * SPDX-License-Identifier: Apache-2.0
 *
 * File SQL: rewrites DuckDB style file reading functions into native seekdb
 * SQL so that the regular engine (projection, filter, sort, group by, join)
 * executes the query. No external query engine is involved and the rows are
 * produced from the parsed file content directly, so no table or import step
 * is required.
 *
 * Supported syntax:
 *   SELECT ... FROM read_csv('path'[, header = true][, sample_size = -1])
 *   SELECT ... FROM read_json('path'[, format = 'newline_delimited'])
 *   SELECT ... FROM read_ndjson('path')
 *   SELECT ... FROM read_parquet('path')
 *   SELECT ... FROM glob('pattern')
 *   DESCRIBE SELECT * FROM read_csv('path')
 *   COPY (query) TO 'path' (format csv, header, nullstr 'NULL')
 */

#ifndef OBDEV_SRC_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_REWRITER_H_
#define OBDEV_SRC_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_REWRITER_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "sql/engine/file_sql/ob_file_sql_format.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;

namespace filesql
{

class ObFileSqlRewriter
{
public:
  // Rewrites `stmt` when it uses file reading functions. When nothing needs to
  // be rewritten the original statement is returned unchanged.
  static int rewrite(common::ObIAllocator &alloc,
                     const common::ObString &stmt,
                     ObSQLSessionInfo &session,
                     common::ObString &rewritten);
};

}  // namespace filesql
}  // namespace sql
}  // namespace oceanbase

#endif  // OBDEV_SRC_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_REWRITER_H_
