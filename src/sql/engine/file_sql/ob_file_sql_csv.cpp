/**
 * Copyright (c) 2021 OceanBase
 * SPDX-License-Identifier: Apache-2.0
 *
 * CSV reader with deterministic type inference.
 */

#include "sql/engine/file_sql/ob_file_sql_format.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

namespace oceanbase
{
namespace sql
{
namespace filesql
{

const char *value_type_name(ValueType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case ValueType::NULL_TYPE: name = "NULL"; break;
    case ValueType::BOOLEAN: name = "BOOLEAN"; break;
    case ValueType::BIGINT: name = "BIGINT"; break;
    case ValueType::DOUBLE: name = "DOUBLE"; break;
    case ValueType::DATE: name = "DATE"; break;
    case ValueType::DATETIME: name = "DATETIME"; break;
    case ValueType::VARCHAR: name = "VARCHAR"; break;
    default: name = "VARCHAR"; break;
  }
  return name;
}

int read_whole_file(const std::string &path, std::string &content, std::string &err)
{
  int ret = 0;
  content.clear();
  struct stat st;
  if (0 != ::stat(path.c_str(), &st)) {
    ret = -1;
    err = "cannot access file '" + path + "': " + ::strerror(errno);
  } else if (S_ISDIR(st.st_mode)) {
    ret = -1;
    err = "'" + path + "' is a directory, not a file";
  } else {
    FILE *fp = ::fopen(path.c_str(), "rb");
    if (NULL == fp) {
      ret = -1;
      err = "cannot open file '" + path + "': " + ::strerror(errno);
    } else {
      content.resize(static_cast<size_t>(st.st_size) + 1);
      size_t read_bytes = ::fread(&content[0], 1, static_cast<size_t>(st.st_size), fp);
      ::fclose(fp);
      content.resize(read_bytes);
    }
  }
  return ret;
}

namespace
{

inline bool is_space_char(char c)
{
  return ' ' == c || '\t' == c || '\r' == c || '\n' == c || '\v' == c || '\f' == c;
}

std::string trim_copy(const std::string &s)
{
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && is_space_char(s[begin])) {
    ++begin;
  }
  while (end > begin && is_space_char(s[end - 1])) {
    --end;
  }
  return s.substr(begin, end - begin);
}

bool parse_digits(const std::string &text, size_t begin, size_t end, int64_t &value)
{
  bool ok = begin < end;
  int64_t result = 0;
  for (size_t i = begin; ok && i < end; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      ok = false;
    } else {
      int digit = text[i] - '0';
      if (result > (INT64_MAX - digit) / 10) {
        ok = false;
      } else {
        result = result * 10 + digit;
      }
    }
  }
  if (ok) {
    value = result;
  }
  return ok;
}

// Strict YYYY-MM-DD checker with calendar validation.
bool parse_date_part(const std::string &text, size_t begin, int &year, int &month, int &day)
{
  int64_t y = 0;
  int64_t m = 0;
  int64_t d = 0;
  bool ok = text.size() >= begin + 10
            && '-' == text[begin + 4] && '-' == text[begin + 7]
            && parse_digits(text, begin, begin + 4, y)
            && parse_digits(text, begin + 5, begin + 7, m)
            && parse_digits(text, begin + 8, begin + 10, d);
  if (ok && (m < 1 || m > 12 || d < 1 || d > 31)) {
    ok = false;
  }
  if (ok) {
    static const int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (0 == y % 4 && 0 != y % 100) || 0 == y % 400;
    int limit = days_in_month[m - 1] + ((2 == m && leap) ? 1 : 0);
    if (d > limit) {
      ok = false;
    }
  }
  if (ok) {
    year = static_cast<int>(y);
    month = static_cast<int>(m);
    day = static_cast<int>(d);
  }
  return ok;
}

bool parse_time_part(const std::string &text, size_t begin)
{
  int64_t hour = 0;
  int64_t minute = 0;
  int64_t second = 0;
  bool ok = text.size() >= begin + 8
            && ':' == text[begin + 2] && ':' == text[begin + 5]
            && parse_digits(text, begin, begin + 2, hour)
            && parse_digits(text, begin + 3, begin + 5, minute)
            && parse_digits(text, begin + 6, begin + 8, second)
            && hour <= 23 && minute <= 59 && second <= 59;
  if (ok) {
    // optional fractional part
    size_t pos = begin + 8;
    if (pos < text.size() && '.' == text[pos]) {
      size_t digit_begin = ++pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        ++pos;
      }
      ok = pos > digit_begin;
    }
    ok = ok && pos == text.size();
  }
  return ok;
}

bool looks_like_date(const std::string &text)
{
  int y = 0;
  int m = 0;
  int d = 0;
  return 10 == text.size() && parse_date_part(text, 0, y, m, d);
}

bool looks_like_datetime(const std::string &text)
{
  int y = 0;
  int m = 0;
  int d = 0;
  return (text.size() > 11 && (' ' == text[10] || 'T' == text[10] || 't' == text[10])
          && parse_date_part(text, 0, y, m, d)
          && parse_time_part(text, 11));
}

bool looks_like_integer(const std::string &text, int64_t &value)
{
  bool ok = false;
  if (!text.empty()) {
    size_t begin = 0;
    bool negative = false;
    if ('+' == text[0] || '-' == text[0]) {
      negative = ('-' == text[0]);
      begin = 1;
    }
    int64_t magnitude = 0;
    ok = parse_digits(text, begin, text.size(), magnitude);
    if (ok) {
      value = negative ? -magnitude : magnitude;
    }
  }
  return ok;
}

bool looks_like_double(const std::string &text, double &value)
{
  bool ok = false;
  if (!text.empty()) {
    errno = 0;
    char *end = NULL;
    double parsed = ::strtod(text.c_str(), &end);
    ok = (NULL != end) && (end == text.c_str() + text.size()) && 0 == errno
         && !isnan(parsed) && !isinf(parsed);
    if (ok) {
      value = parsed;
    }
  }
  return ok;
}

bool looks_like_boolean(const std::string &text)
{
  std::string lower;
  lower.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    lower.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(text[i]))));
  }
  return "true" == lower || "false" == lower;
}

// Normalizes "2026-08-03T12:13:14" into "2026-08-03 12:13:14".
std::string normalize_datetime(const std::string &text)
{
  std::string out = text;
  if (out.size() > 10 && ('T' == out[10] || 't' == out[10])) {
    out[10] = ' ';
  }
  return out;
}

}  // namespace

ValueType detect_scalar_type(const std::string &raw_text)
{
  ValueType type = ValueType::VARCHAR;
  std::string text = trim_copy(raw_text);
  int64_t int_value = 0;
  double double_value = 0;
  if (text.empty()) {
    type = ValueType::NULL_TYPE;
  } else if (looks_like_boolean(text)) {
    type = ValueType::BOOLEAN;
  } else if (looks_like_date(text)) {
    type = ValueType::DATE;
  } else if (looks_like_datetime(text)) {
    type = ValueType::DATETIME;
  } else if (looks_like_integer(text, int_value)) {
    type = ValueType::BIGINT;
  } else if (looks_like_double(text, double_value)) {
    type = ValueType::DOUBLE;
  }
  return type;
}

ValueType widen_type(ValueType current, ValueType incoming)
{
  ValueType result = current;
  if (ValueType::NULL_TYPE == incoming) {
    result = current;
  } else if (ValueType::NULL_TYPE == current) {
    result = incoming;
  } else if (current == incoming) {
    result = current;
  } else if (ValueType::VARCHAR == current || ValueType::VARCHAR == incoming) {
    result = ValueType::VARCHAR;
  } else if ((ValueType::BIGINT == current && ValueType::DOUBLE == incoming)
             || (ValueType::DOUBLE == current && ValueType::BIGINT == incoming)) {
    result = ValueType::DOUBLE;
  } else if ((ValueType::DATE == current && ValueType::DATETIME == incoming)
             || (ValueType::DATETIME == current && ValueType::DATE == incoming)) {
    result = ValueType::DATETIME;
  } else {
    // Any other combination (boolean mixed with numbers, dates mixed with
    // numbers, ...) degrades to text instead of failing the whole file.
    result = ValueType::VARCHAR;
  }
  return result;
}

namespace
{

// Splits one CSV record starting at pos. Appends the fields to out_fields and
// leaves pos at the first character after the record terminator.
void parse_csv_record(const std::string &content, size_t &pos, char delimiter, char quote_char,
                      std::vector<std::string> &out_fields)
{
  out_fields.clear();
  std::string field;
  bool in_quote = false;
  bool field_finished = false;
  while (pos <= content.size()) {
    if (pos == content.size()) {
      out_fields.push_back(field);
      break;
    }
    char c = content[pos];
    if (in_quote) {
      if (c == quote_char) {
        if (pos + 1 < content.size() && content[pos + 1] == quote_char) {
          field.push_back(quote_char);
          pos += 2;
        } else {
          in_quote = false;
          ++pos;
        }
      } else {
        field.push_back(c);
        ++pos;
        field_finished = false;
      }
    } else if (c == quote_char && field.empty()) {
      in_quote = true;
      field_finished = false;
      ++pos;
    } else if (c == delimiter) {
      out_fields.push_back(field);
      field.clear();
      field_finished = true;
      ++pos;
    } else if ('\n' == c || '\r' == c) {
      out_fields.push_back(field);
      if ('\r' == c && pos + 1 < content.size() && '\n' == content[pos + 1]) {
        pos += 2;
      } else {
        ++pos;
      }
      break;
    } else {
      field.push_back(c);
      field_finished = false;
      ++pos;
    }
  }
  (void)field_finished;
}

std::string dedup_column_name(const std::string &base, int64_t occurrence)
{
  std::string name = base;
  if (occurrence > 1) {
    char buf[32];
    snprintf(buf, sizeof(buf), "__%ld", static_cast<long>(occurrence));
    name += buf;
  }
  return name;
}

void convert_value(const std::string &text, ValueType type, Value &value)
{
  value.set_null();
  if (ValueType::NULL_TYPE == type) {
    if (!text.empty()) {
      value.set_str(ValueType::VARCHAR, text);
    }
  } else if (text.empty()) {
    value.set_null();
  } else if (ValueType::BOOLEAN == type) {
    std::string lower;
    for (size_t i = 0; i < text.size(); ++i) {
      lower.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(text[i]))));
    }
    value.set_int(ValueType::BOOLEAN, ("true" == lower) ? 1 : 0);
  } else if (ValueType::BIGINT == type) {
    int64_t int_value = 0;
    std::string trimmed = trim_copy(text);
    if (looks_like_integer(trimmed, int_value)) {
      value.set_int(ValueType::BIGINT, int_value);
    } else {
      value.set_str(ValueType::VARCHAR, text);
    }
  } else if (ValueType::DOUBLE == type) {
    double double_value = 0;
    std::string trimmed = trim_copy(text);
    int64_t int_value = 0;
    if (looks_like_integer(trimmed, int_value)) {
      value.set_double(static_cast<double>(int_value));
    } else if (looks_like_double(trimmed, double_value)) {
      value.set_double(double_value);
    } else {
      value.set_str(ValueType::VARCHAR, text);
    }
  } else if (ValueType::DATE == type) {
    value.set_str(ValueType::DATE, trim_copy(text));
  } else if (ValueType::DATETIME == type) {
    value.set_str(ValueType::DATETIME, normalize_datetime(trim_copy(text)));
  } else {
    value.set_str(ValueType::VARCHAR, text);
  }
}

}  // namespace

int read_csv_file(const std::string &path, const CsvReadOptions &opt, Table &table,
                  std::string &err)
{
  int ret = 0;
  std::string content;
  table.reset();
  if (0 != (ret = read_whole_file(path, content, err))) {
    // keep err from read_whole_file
  } else {
    // strip a UTF-8 BOM if present
    size_t start = 0;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF
        && static_cast<unsigned char>(content[1]) == 0xBB
        && static_cast<unsigned char>(content[2]) == 0xBF) {
      start = 3;
    }
    std::vector<std::vector<std::string> > records;
    size_t pos = start;
    while (pos < content.size()) {
      std::vector<std::string> fields;
      parse_csv_record(content, pos, opt.field_delimiter_, opt.quote_char_, fields);
      bool blank = (1 == fields.size() && fields[0].empty());
      if (!blank) {
        records.push_back(fields);
      }
    }
    if (records.empty()) {
      ret = -1;
      err = "file '" + path + "' contains no data rows";
    } else {
      size_t first_data = 0;
      std::vector<std::string> names;
      if (opt.header_) {
        names = records[0];
        first_data = 1;
      } else {
        for (size_t i = 0; i < records[0].size(); ++i) {
          char buf[32];
          snprintf(buf, sizeof(buf), "column%ld", static_cast<long>(i));
          names.push_back(buf);
        }
      }
      // duplicate headers get a stable __N suffix (id, id__2, id__3, ...)
      std::vector<std::string> bases;
      std::vector<std::string> final_names;
      for (size_t i = 0; i < names.size(); ++i) {
        std::string base = trim_copy(names[i]);
        if (base.empty()) {
          char buf[32];
          snprintf(buf, sizeof(buf), "column%ld", static_cast<long>(i));
          base = buf;
        }
        int64_t occurrence = 1;
        for (size_t j = 0; j < bases.size(); ++j) {
          if (bases[j] == base) {
            ++occurrence;
          }
        }
        bases.push_back(base);
        final_names.push_back(dedup_column_name(base, occurrence));
      }
      if (first_data >= records.size()) {
        ret = -1;
        err = "file '" + path + "' contains a header but no data rows";
      } else {
        table.columns_.resize(final_names.size());
        for (size_t i = 0; i < final_names.size(); ++i) {
          table.columns_[i].name_ = final_names[i];
          table.columns_[i].type_ = ValueType::NULL_TYPE;
          table.columns_[i].nullable_ = false;
        }
        int64_t infer_rows = static_cast<int64_t>(records.size() - first_data);
        if (opt.sample_size_ > 0 && opt.sample_size_ < infer_rows) {
          infer_rows = opt.sample_size_;
        }
        for (int64_t r = 0; r < infer_rows; ++r) {
          const std::vector<std::string> &fields = records[first_data + r];
          for (size_t c = 0; c < table.columns_.size(); ++c) {
            ValueType detected = (c < fields.size())
                                     ? detect_scalar_type(fields[c])
                                     : ValueType::NULL_TYPE;
            table.columns_[c].type_ = widen_type(table.columns_[c].type_, detected);
          }
        }
        for (size_t c = 0; c < table.columns_.size(); ++c) {
          if (ValueType::NULL_TYPE == table.columns_[c].type_) {
            table.columns_[c].type_ = ValueType::VARCHAR;
          }
        }
        for (size_t r = first_data; r < records.size(); ++r) {
          const std::vector<std::string> &fields = records[r];
          std::vector<Value> row(table.columns_.size());
          for (size_t c = 0; c < table.columns_.size(); ++c) {
            if (c < fields.size()) {
              convert_value(fields[c], table.columns_[c].type_, row[c]);
            } else {
              row[c].set_null();
            }
          }
          table.rows_.push_back(row);
        }
        for (size_t c = 0; c < table.columns_.size(); ++c) {
          bool nullable = false;
          for (size_t r = 0; r < table.rows_.size() && !nullable; ++r) {
            nullable = table.rows_[r][c].is_null();
          }
          table.columns_[c].nullable_ = nullable;
        }
      }
    }
  }
  return ret;
}

}  // namespace filesql
}  // namespace sql
}  // namespace oceanbase
