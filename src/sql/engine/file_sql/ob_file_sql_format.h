/**
 * Copyright (c) 2021 OceanBase
 * SPDX-License-Identifier: Apache-2.0
 *
 * File SQL: read local CSV / JSONL / Parquet files as tables.
 *
 * This header is deliberately free of any observer dependency so that the
 * readers can be unit tested and reused outside of the SQL engine.
 */

#ifndef OBDEV_SRC_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_FORMAT_H_
#define OBDEV_SRC_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_FORMAT_H_

#include <stdint.h>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace filesql
{

// Logical column type of a file table. The names are rendered as-is by
// DESCRIBE, so they follow the DuckDB spelling ("BIGINT", "DOUBLE", ...).
enum class ValueType : int8_t
{
  NULL_TYPE = 0,
  BOOLEAN = 1,
  BIGINT = 2,
  DOUBLE = 3,
  DATE = 4,
  DATETIME = 5,
  VARCHAR = 6,
  MAX_TYPE
};

const char *value_type_name(ValueType type);

struct Value
{
  ValueType type_;
  int64_t int_value_;   // BOOLEAN (0/1) and BIGINT
  double double_value_;
  std::string str_value_;  // VARCHAR / DATE / DATETIME (already normalized)

  Value() : type_(ValueType::NULL_TYPE), int_value_(0), double_value_(0) {}
  void set_null() { type_ = ValueType::NULL_TYPE; int_value_ = 0; double_value_ = 0; str_value_.clear(); }
  void set_int(ValueType type, int64_t v) { type_ = type; int_value_ = v; }
  void set_double(double v) { type_ = ValueType::DOUBLE; double_value_ = v; }
  void set_str(ValueType type, const std::string &v) { type_ = type; str_value_ = v; }
  bool is_null() const { return ValueType::NULL_TYPE == type_; }
};

struct Column
{
  std::string name_;
  ValueType type_;
  bool nullable_;

  Column() : type_(ValueType::VARCHAR), nullable_(false) {}
};

struct Table
{
  std::vector<Column> columns_;
  std::vector<std::vector<Value> > rows_;

  void reset() { columns_.clear(); rows_.clear(); }
  int64_t column_count() const { return static_cast<int64_t>(columns_.size()); }
  int64_t row_count() const { return static_cast<int64_t>(rows_.size()); }
};

struct CsvReadOptions
{
  bool header_;
  int64_t sample_size_;  // -1 means "infer from the whole file"
  char field_delimiter_;
  char quote_char_;

  CsvReadOptions() : header_(true), sample_size_(-1), field_delimiter_(','), quote_char_('"') {}
};

// Diagnostics carried back to the SQL layer. All readers return 0 on success
// and a negative value on failure with a human readable message in err.
int read_csv_file(const std::string &path, const CsvReadOptions &opt, Table &table, std::string &err);
int read_jsonl_file(const std::string &path, bool newline_delimited, Table &table, std::string &err);
int read_parquet_file(const std::string &path, Table &table, std::string &err);

// Reads a whole file into memory. Returns 0 on success.
int read_whole_file(const std::string &path, std::string &content, std::string &err);

// Type inference helpers shared by the CSV reader (exposed for testing).
ValueType detect_scalar_type(const std::string &text);
ValueType widen_type(ValueType current, ValueType incoming);

}  // namespace filesql
}  // namespace sql
}  // namespace oceanbase

#endif  // OBDEV_SRC_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_FORMAT_H_
