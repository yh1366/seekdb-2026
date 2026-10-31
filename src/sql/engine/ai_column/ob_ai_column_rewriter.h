/**
 * Copyright (c) 2021 OceanBase
 * SPDX-License-Identifier: Apache-2.0
 *
 * AI COLUMN: asynchronous generated columns backed by AI functions.
 *
 * Declared as
 *   tags JSON AI COLUMN (AI_COMPLETE('text_model', CONCAT('...', content)))
 *        WITH (sync_mode='async', max_retries=2)
 *
 * The switch is implemented on top of plain columns: the declaration is turned
 * into the user visible result column plus a set of invisible bookkeeping
 * columns, and the generation rule itself is persisted in the column comment so
 * that SHOW AI COLUMNS and the background worker can recover it after a
 * restart.
 *
 * Supported syntax handled here:
 *   CREATE TABLE t (..., c <type> AI COLUMN (<expr>) [WITH (<options>)], ...)
 *   ALTER TABLE t ADD [COLUMN] c <type> AI COLUMN (<expr>) [WITH (<options>)]
 *   ALTER TABLE t MODIFY [COLUMN] c <type> AI COLUMN (<expr>) [WITH (<options>)]
 *   ALTER TABLE t RETRY AI COLUMN c [WHERE <cond>]
 *   SHOW AI COLUMNS FROM t
 *   SET [SESSION] ai_column_strict_read = ON|OFF
 */

#ifndef OBDEV_SRC_SQL_ENGINE_AI_COLUMN_OB_AI_COLUMN_REWRITER_H_
#define OBDEV_SRC_SQL_ENGINE_AI_COLUMN_OB_AI_COLUMN_REWRITER_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;

namespace aicolumn
{

// Suffixes of the invisible bookkeeping columns that belong to one AI column.
extern const char *const AI_COLUMN_STATUS_SUFFIX;
extern const char *const AI_COLUMN_SRC_SUFFIX;
extern const char *const AI_COLUMN_SEQ_SUFFIX;
// The column comment prefix that marks a column as an AI column.
extern const char *const AI_COLUMN_COMMENT_PREFIX;

class ObAiColumnRewriter
{
public:
  // Rewrites `stmt` when it involves an AI column. When nothing needs to be
  // rewritten the original statement is returned unchanged.
  static int rewrite(common::ObIAllocator &alloc,
                     const common::ObString &stmt,
                     ObSQLSessionInfo &session,
                     common::ObString &rewritten);
};

}  // namespace aicolumn
}  // namespace sql
}  // namespace oceanbase

#endif  // OBDEV_SRC_SQL_ENGINE_AI_COLUMN_OB_AI_COLUMN_REWRITER_H_
