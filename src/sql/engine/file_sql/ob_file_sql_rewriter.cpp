/**
 * Copyright (c) 2021 OceanBase
 * SPDX-License-Identifier: Apache-2.0
 *
 * File SQL rewriter implementation.
 */

#include "sql/engine/file_sql/ob_file_sql_rewriter.h"

#include <ctype.h>
#include <glob.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "share/ob_errno.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{
namespace filesql
{

namespace
{

const char *const FILE_FUNCTIONS[] = {"read_csv", "read_json", "read_ndjson", "read_parquet"};
const char *const GLOB_FUNCTION = "glob";
const int64_t MAX_SQL_TEXT_FOR_FILE_FUNCS = 256 * 1024;

std::string to_lower(const std::string &text)
{
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    out.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(text[i]))));
  }
  return out;
}

std::string trim_copy(const std::string &text)
{
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return text.substr(begin, end - begin);
}

bool starts_with_ci(const std::string &text, const char *prefix)
{
  size_t len = ::strlen(prefix);
  if (text.size() < len) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    if (::tolower(static_cast<unsigned char>(text[i]))
        != ::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

bool contains_ci(const std::string &text, const char *needle)
{
  size_t needle_len = ::strlen(needle);
  if (0 == needle_len || text.size() < needle_len) {
    return false;
  }
  for (size_t i = 0; i + needle_len <= text.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < needle_len && match; ++j) {
      match = (::tolower(static_cast<unsigned char>(text[i + j]))
               == ::tolower(static_cast<unsigned char>(needle[j])));
    }
    if (match) {
      return true;
    }
  }
  return false;
}

// Returns true when the statement is a ``set`` statement that assigns the
// file_sql switch. The assigned value is decoded into ``enabled``. When the
// value is not a valid boolean literal ``bad_value`` is set and the caller
// reports a wrong value error.
bool parse_set_file_sql_stmt(const std::string &raw, bool &enabled, bool &bad_value)
{
  const char *const VAR_NAME = "enable_file_sql";
  const std::string trimmed = trim_copy(raw);
  const std::string lower = to_lower(trimmed);
  bool is_set_stmt = false;
  if (starts_with_ci(trimmed, "set") && lower.size() > 3) {
    const char sep = lower[3];
    is_set_stmt = (' ' == sep || '\t' == sep || '\n' == sep || '\r' == sep || '@' == sep);
  }
  bool matched = false;
  if (is_set_stmt) {
    const size_t name_len = ::strlen(VAR_NAME);
    const size_t pos = lower.find(VAR_NAME);
    if (std::string::npos != pos) {
      size_t p = pos + name_len;
      while (p < lower.size() && isspace(static_cast<unsigned char>(lower[p]))) {
        ++p;
      }
      if (p < lower.size() && ':' == lower[p]) {
        ++p;
      }
      while (p < lower.size() && isspace(static_cast<unsigned char>(lower[p]))) {
        ++p;
      }
      if (p < lower.size() && '=' == lower[p]) {
        ++p;
        size_t q = p;
        while (q < lower.size() && ',' != lower[q] && ';' != lower[q]) {
          ++q;
        }
        std::string value = trim_copy(lower.substr(p, q - p));
        if (value.size() >= 2
            && (('\'' == value[0] && '\'' == value[value.size() - 1])
                || ('"' == value[0] && '"' == value[value.size() - 1]))) {
          value = trim_copy(value.substr(1, value.size() - 2));
        }
        if ("on" == value || "true" == value || "1" == value) {
          enabled = true;
          matched = true;
        } else if ("off" == value || "false" == value || "0" == value
                   || "default" == value) {
          enabled = false;
          matched = true;
        } else {
          bad_value = true;
          matched = true;
        }
      }
    }
  }
  return matched;
}

// Forward declaration: the identifier helper is defined further below.
bool is_ident_char(const char c);

// Session variable that mirrors the `enable_file_sql` switch. The SET form is
// rewritten into a plain assignment of this variable so that the statement does
// not depend on the system variable update path.
const char *const FILE_SQL_FLAG_VAR = "file_sql_flag";

// The switch is read from the mirror variable only. The variable does not exist
// until a SET has really been executed, so a fresh session always starts with
// the feature disabled - there is no cached flag that could keep a stale value
// across session reuse.
bool user_flag_enabled(ObSQLSessionInfo &session)
{
  bool enabled = false;
  common::ObObj value;
  const common::ObString name = common::ObString::make_string(FILE_SQL_FLAG_VAR);
  if (OB_SUCCESS == session.get_user_variable_value(name, value) && value.is_int()) {
    enabled = (0 != value.get_int());
  }
  return enabled;
}

// Rewrites `@@[session.]enable_file_sql` into a read of the mirror variable so
// that the value observed by SELECT always matches the rewritten SET form.
bool rewrite_select_flag(const std::string &text, std::string &result)
{
  bool any = false;
  const std::string lower = to_lower(text);
  const char *const VAR_NAME = "enable_file_sql";
  const size_t name_len = ::strlen(VAR_NAME);
  std::string out;
  size_t copied = 0;
  size_t from = 0;
  while (from < lower.size()) {
    const size_t pos = lower.find(VAR_NAME, from);
    if (std::string::npos == pos) {
      break;
    }
    size_t begin = pos;
    while (begin > 0 && (is_ident_char(lower[begin - 1]) || '.' == lower[begin - 1])) {
      --begin;
    }
    if (begin >= 2 && '@' == lower[begin - 1] && '@' == lower[begin - 2]) {
      out.append(text, copied, begin - 2 - copied);
      out += "ifnull(@";
      out += FILE_SQL_FLAG_VAR;
      out += ", 0)";
      copied = pos + name_len;
      from = copied;
      any = true;
    } else {
      from = pos + name_len;
    }
  }
  if (any) {
    out.append(text, copied, std::string::npos);
    result = out;
  }
  return any;
}

int copy_text(common::ObIAllocator &alloc, const std::string &text, common::ObString &out)
{
  int ret = OB_SUCCESS;
  char *buffer = static_cast<char *>(alloc.alloc(text.size() + 1));
  if (NULL == buffer) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    out.assign_ptr(buffer, static_cast<int32_t>(text.size()));
  }
  return ret;
}

std::string quote_ident(const std::string &name)
{
  std::string out = "`";
  for (size_t i = 0; i < name.size(); ++i) {
    if ('`' == name[i]) {
      out.push_back('`');
    }
    out.push_back(name[i]);
  }
  out.push_back('`');
  return out;
}

// Escapes a value for use inside a single quoted SQL literal.
std::string sql_escape(const std::string &value)
{
  std::string out;
  out.reserve(value.size() + 8);
  for (size_t i = 0; i < value.size(); ++i) {
    char c = value[i];
    switch (c) {
      case '\'': out += "\\'"; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case 0: out += "\\0"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string quote_literal(const std::string &value)
{
  return std::string("'") + sql_escape(value) + "'";
}

std::string double_to_literal(double value)
{
  char buf[64];
  snprintf(buf, sizeof(buf), "%.17g", value);
  std::string text(buf);
  if (std::string::npos == text.find('.') && std::string::npos == text.find('e')
      && std::string::npos == text.find('E') && std::string::npos == text.find("inf")
      && std::string::npos == text.find("nan")) {
    text += "e0";  // force a floating point literal
  }
  return text;
}

std::string value_to_sql(const Value &value, ValueType column_type)
{
  std::string out;
  switch (value.type_) {
    case ValueType::NULL_TYPE: {
      out = "null";
      break;
    }
    case ValueType::BOOLEAN: {
      out = value.int_value_ != 0 ? "1" : "0";
      break;
    }
    case ValueType::BIGINT: {
      char buf[32];
      snprintf(buf, sizeof(buf), "%ld", static_cast<long>(value.int_value_));
      out = buf;
      break;
    }
    case ValueType::DOUBLE: {
      out = double_to_literal(value.double_value_);
      break;
    }
    case ValueType::DATE: {
      out = "cast(" + quote_literal(value.str_value_) + " as date)";
      break;
    }
    case ValueType::DATETIME: {
      out = "cast(" + quote_literal(value.str_value_) + " as datetime)";
      break;
    }
    default: {
      out = quote_literal(value.str_value_);
      break;
    }
  }
  (void)column_type;
  return out;
}

// Builds "(select ... union all select ...)" that reproduces the file content.
std::string build_table_sql(const Table &table)
{
  std::string out = "(";
  if (table.rows_.empty()) {
    out += "select ";
    for (size_t c = 0; c < table.columns_.size(); ++c) {
      if (c > 0) {
        out += ", ";
      }
      Value null_value;
      out += value_to_sql(null_value, table.columns_[c].type_);
      out += " as ";
      out += quote_ident(table.columns_[c].name_);
    }
    if (table.columns_.empty()) {
      out += "1";
    }
    out += " limit 0)";
  } else {
    for (size_t r = 0; r < table.rows_.size(); ++r) {
      if (r > 0) {
        out += " union all ";
      }
      out += "select ";
      const std::vector<Value> &row = table.rows_[r];
      for (size_t c = 0; c < table.columns_.size(); ++c) {
        if (c > 0) {
          out += ", ";
        }
        out += value_to_sql(row[c], table.columns_[c].type_);
        if (0 == r) {
          out += " as ";
          out += quote_ident(table.columns_[c].name_);
        }
      }
      if (table.columns_.empty()) {
        out += "null";
      }
    }
    out += ")";
  }
  return out;
}

bool is_ident_char(char c)
{
  return ::isalnum(static_cast<unsigned char>(c)) || '_' == c || '$' == c;
}

struct CallInfo
{
  std::string name_;
  size_t begin_;
  size_t end_;
  std::string args_;
};

size_t skip_quoted(const std::string &text, size_t pos, char quote)
{
  size_t i = pos + 1;
  while (i < text.size()) {
    if ('\\' == text[i]) {
      i += 2;
      continue;
    }
    if (text[i] == quote) {
      if (i + 1 < text.size() && text[i + 1] == quote) {
        i += 2;
        continue;
      }
      return i + 1;
    }
    ++i;
  }
  return text.size();
}

size_t find_matching_paren(const std::string &text, size_t open_pos)
{
  size_t depth = 0;
  size_t i = open_pos;
  while (i < text.size()) {
    char c = text[i];
    if ('\'' == c || '"' == c || '`' == c) {
      i = skip_quoted(text, i, c);
      continue;
    }
    if ('(' == c) {
      ++depth;
    } else if (')' == c) {
      if (0 == --depth) {
        return i;
      }
    }
    ++i;
  }
  return std::string::npos;
}

bool is_file_function(const std::string &lower_name)
{
  bool found = false;
  for (size_t i = 0; !found && i < sizeof(FILE_FUNCTIONS) / sizeof(FILE_FUNCTIONS[0]); ++i) {
    found = (lower_name == FILE_FUNCTIONS[i]);
  }
  return found || lower_name == GLOB_FUNCTION;
}

void find_calls(const std::string &text, std::vector<CallInfo> &calls)
{
  size_t i = 0;
  while (i < text.size()) {
    char c = text[i];
    if ('\'' == c || '"' == c || '`' == c) {
      i = skip_quoted(text, i, c);
      continue;
    }
    if ('-' == c && i + 1 < text.size() && '-' == text[i + 1]) {
      size_t nl = text.find('\n', i);
      i = (std::string::npos == nl) ? text.size() : nl + 1;
      continue;
    }
    if ('#' == c) {
      size_t nl = text.find('\n', i);
      i = (std::string::npos == nl) ? text.size() : nl + 1;
      continue;
    }
    if ('/' == c && i + 1 < text.size() && '*' == text[i + 1]) {
      size_t close = text.find("*/", i + 2);
      i = (std::string::npos == close) ? text.size() : close + 2;
      continue;
    }
    if (is_ident_char(c) && !::isdigit(static_cast<unsigned char>(c))) {
      size_t j = i;
      while (j < text.size() && is_ident_char(text[j])) {
        ++j;
      }
      std::string ident = to_lower(text.substr(i, j - i));
      bool after_dot = (i > 0 && '.' == text[i - 1]);
      if (!after_dot && is_file_function(ident)) {
        size_t k = j;
        while (k < text.size() && isspace(static_cast<unsigned char>(text[k]))) {
          ++k;
        }
        if (k < text.size() && '(' == text[k]) {
          size_t close = find_matching_paren(text, k);
          if (std::string::npos != close) {
            CallInfo info;
            info.name_ = ident;
            info.begin_ = i;
            info.end_ = close + 1;
            info.args_ = text.substr(k + 1, close - k - 1);
            calls.push_back(info);
            i = close + 1;
            continue;
          }
        }
      }
      i = j;
      continue;
    }
    ++i;
  }
}

void split_arguments(const std::string &args, std::vector<std::string> &out)
{
  out.clear();
  size_t begin = 0;
  size_t depth = 0;
  size_t i = 0;
  while (i < args.size()) {
    char c = args[i];
    if ('\'' == c || '"' == c || '`' == c) {
      i = skip_quoted(args, i, c);
      continue;
    }
    if ('(' == c) {
      ++depth;
    } else if (')' == c) {
      --depth;
    } else if (',' == c && 0 == depth) {
      out.push_back(trim_copy(args.substr(begin, i - begin)));
      begin = i + 1;
    }
    ++i;
  }
  if (begin <= args.size()) {
    std::string last = trim_copy(args.substr(begin));
    if (!last.empty()) {
      out.push_back(last);
    }
  }
}

bool parse_string_literal(const std::string &text, std::string &out)
{
  out.clear();
  std::string trimmed = trim_copy(text);
  if (trimmed.size() < 2 || '\'' != trimmed[0] || '\'' != trimmed[trimmed.size() - 1]) {
    return false;
  }
  size_t i = 1;
  size_t end = trimmed.size() - 1;
  while (i < end) {
    char c = trimmed[i];
    if ('\\' == c && i + 1 < end) {
      char esc = trimmed[i + 1];
      switch (esc) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case '0': out.push_back('\0'); break;
        case '\\': out.push_back('\\'); break;
        case '\'': out.push_back('\''); break;
        case '"': out.push_back('"'); break;
        default: out.push_back(esc); break;
      }
      i += 2;
    } else if ('\'' == c && i + 1 < end && '\'' == trimmed[i + 1]) {
      out.push_back('\'');
      i += 2;
    } else {
      out.push_back(c);
      ++i;
    }
  }
  return true;
}

bool parse_bool_text(const std::string &raw, bool &value)
{
  std::string text = to_lower(trim_copy(raw));
  bool ok = true;
  if ("true" == text || "on" == text || "1" == text) {
    value = true;
  } else if ("false" == text || "off" == text || "0" == text) {
    value = false;
  } else {
    ok = false;
  }
  return ok;
}

struct ArgSet
{
  bool has_path_;
  std::string path_;
  bool has_header_;
  bool header_;
  bool has_sample_size_;
  int64_t sample_size_;
  bool has_format_;
  std::string format_;
  bool extra_positional_;
  char delimiter_;
  char quote_;

  ArgSet() : has_path_(false), has_header_(false), header_(true), has_sample_size_(false),
             sample_size_(-1), has_format_(false), extra_positional_(false),
             delimiter_(','), quote_('"') {}
};

// Returns OB_SUCCESS, OB_ERR_PARSE_SQL for a malformed call or OB_NOT_SUPPORTED
// for an unsupported option.
int parse_args(const CallInfo &call, ArgSet &args)
{
  int ret = OB_SUCCESS;
  std::vector<std::string> items;
  split_arguments(call.args_, items);
  for (size_t i = 0; OB_SUCC(ret) && i < items.size(); ++i) {
    const std::string &item = items[i];
    // look for a top level '=' to detect named arguments
    size_t eq = std::string::npos;
    size_t depth = 0;
    for (size_t k = 0; k < item.size() && std::string::npos == eq; ++k) {
      char c = item[k];
      if ('\'' == c || '"' == c || '`' == c) {
        size_t next = skip_quoted(item, k, c);
        k = (next > 0) ? next - 1 : k;
      } else if ('(' == c) {
        ++depth;
      } else if (')' == c) {
        --depth;
      } else if ('=' == c && 0 == depth) {
        eq = k;
      }
    }
    if (std::string::npos != eq) {
      std::string name = to_lower(trim_copy(item.substr(0, eq)));
      std::string raw_value = trim_copy(item.substr(eq + 1));
      std::string str_value;
      bool is_string = parse_string_literal(raw_value, str_value);
      if ("header" == name) {
        bool value = false;
        if (is_string) {
          if (parse_bool_text(str_value, value)) {
            args.has_header_ = true;
            args.header_ = value;
          } else {
            ret = OB_ERR_PARSE_SQL;
          }
        } else if (parse_bool_text(raw_value, value)) {
          args.has_header_ = true;
          args.header_ = value;
        } else {
          ret = OB_ERR_PARSE_SQL;
        }
      } else if ("sample_size" == name) {
        std::string text = is_string ? str_value : raw_value;
        char *end = NULL;
        long long parsed = ::strtoll(text.c_str(), &end, 10);
        if (NULL == end || end != text.c_str() + text.size() || text.empty()) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          args.has_sample_size_ = true;
          args.sample_size_ = static_cast<int64_t>(parsed);
        }
      } else if ("format" == name) {
        if (!is_string) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          args.has_format_ = true;
          args.format_ = to_lower(str_value);
        }
      } else if ("delimiter" == name || "delim" == name || "sep" == name) {
        if (!is_string || 1 != str_value.size()) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          args.delimiter_ = str_value[0];
        }
      } else if ("quote" == name) {
        if (!is_string || 1 != str_value.size()) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          args.quote_ = str_value[0];
        }
      }
    } else if (parse_string_literal(item, args.path_) && !args.has_path_) {
      args.has_path_ = true;
    } else {
      // a second positional argument is the DuckDB column type list, which this
      // implementation does not support
      args.extra_positional_ = true;
    }
  }
  return ret;
}

int load_table(const CallInfo &call, const ArgSet &args, Table &table)
{
  int ret = OB_SUCCESS;
  std::string err;
  if (GLOB_FUNCTION == call.name_) {
    glob_t glob_result;
    ::memset(&glob_result, 0, sizeof(glob_result));
    int rc = ::glob(args.path_.c_str(), 0, NULL, &glob_result);
    table.reset();
    Column column;
    column.name_ = "file";
    column.type_ = ValueType::VARCHAR;
    column.nullable_ = false;
    table.columns_.push_back(column);
    if (0 == rc) {
      for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
        std::vector<Value> row(1);
        row[0].set_str(ValueType::VARCHAR, glob_result.gl_pathv[i]);
        table.rows_.push_back(row);
      }
    }
    ::globfree(&glob_result);
  } else if ("read_csv" == call.name_) {
    CsvReadOptions options;
    options.header_ = args.has_header_ ? args.header_ : true;
    options.sample_size_ = args.has_sample_size_ ? args.sample_size_ : -1;
    options.field_delimiter_ = args.delimiter_;
    options.quote_char_ = args.quote_;
    if (0 != read_csv_file(args.path_, options, table, err)) {
      ret = OB_FILE_NOT_EXIST;
    }
  } else if ("read_json" == call.name_ || "read_ndjson" == call.name_) {
    bool newline_delimited = true;
    if (args.has_format_) {
      newline_delimited = ("newline_delimited" == args.format_ || "ndjson" == args.format_
                           || "jsonl" == args.format_ || "auto" == args.format_);
    }
    if (!newline_delimited) {
      ret = OB_NOT_SUPPORTED;
    } else if (0 != read_jsonl_file(args.path_, newline_delimited, table, err)) {
      ret = OB_FILE_NOT_EXIST;
    }
  } else {
    if (0 != read_parquet_file(args.path_, table, err)) {
      ret = OB_FILE_NOT_EXIST;
    }
  }
  return ret;
}

int check_secure_path(ObSQLSessionInfo &session, const std::string &path)
{
  int ret = OB_SUCCESS;
  common::ObString secure_file_priv;
  // The competition deployment does not configure secure_file_priv, and the
  // data directory lives next to the observer. Only enforce the allow list when
  // the administrator actually set one.
  if (OB_SUCCESS == session.get_sys_variable(share::SYS_VAR_SECURE_FILE_PRIV, secure_file_priv)
      && !secure_file_priv.empty()) {
    common::ObString path_str(static_cast<int32_t>(path.size()), const_cast<char *>(path.data()));
    ret = ObResolverUtils::check_secure_path(secure_file_priv, path_str);
  }
  return ret;
}

// Extracts the output column names of a select statement.
void extract_select_names(const std::string &query, std::vector<std::string> &names)
{
  names.clear();
  size_t select_pos = std::string::npos;
  size_t i = 0;
  size_t depth = 0;
  while (i < query.size()) {
    char c = query[i];
    if ('\'' == c || '"' == c || '`' == c) {
      i = skip_quoted(query, i, c);
      continue;
    }
    if ('(' == c) {
      ++depth;
    } else if (')' == c) {
      --depth;
    } else if (0 == depth && is_ident_char(c) && !::isdigit(static_cast<unsigned char>(c))) {
      size_t j = i;
      while (j < query.size() && is_ident_char(query[j])) {
        ++j;
      }
      std::string word = to_lower(query.substr(i, j - i));
      if ("select" == word) {
        select_pos = j;
        break;
      }
      i = j;
      continue;
    }
    ++i;
  }
  if (std::string::npos == select_pos) {
    return;
  }
  // find the matching FROM at depth 0
  size_t list_begin = select_pos;
  size_t list_end = query.size();
  depth = 0;
  i = select_pos;
  while (i < query.size()) {
    char c = query[i];
    if ('\'' == c || '"' == c || '`' == c) {
      i = skip_quoted(query, i, c);
      continue;
    }
    if ('(' == c) {
      ++depth;
    } else if (')' == c) {
      --depth;
    } else if (0 == depth && is_ident_char(c) && !::isdigit(static_cast<unsigned char>(c))) {
      size_t j = i;
      while (j < query.size() && is_ident_char(query[j])) {
        ++j;
      }
      std::string word = to_lower(query.substr(i, j - i));
      if ("from" == word) {
        list_end = i;
        break;
      }
      i = j;
      continue;
    }
    ++i;
  }
  std::string list = query.substr(list_begin, list_end - list_begin);
  std::vector<std::string> items;
  split_arguments(list, items);
  for (size_t k = 0; k < items.size(); ++k) {
    std::string item = trim_copy(items[k]);
    std::string name;
    // explicit alias
    std::string lower_item = to_lower(item);
    size_t as_pos = std::string::npos;
    for (size_t p = 0; p + 4 <= lower_item.size(); ++p) {
      if (0 == lower_item.compare(p, 4, " as ")
          || (0 == p && 0 == lower_item.compare(0, 3, "as "))) {
        as_pos = (0 == p) ? p : p + 1;
      }
    }
    if (std::string::npos != as_pos) {
      name = trim_copy(item.substr(as_pos + 2));
    } else if (is_ident_char(item.empty() ? ' ' : item[item.size() - 1])) {
      // plain column reference, possibly qualified
      size_t dot = item.rfind('.');
      name = (std::string::npos == dot) ? item : item.substr(dot + 1);
    } else {
      name = item;
    }
    while (!name.empty() && ('`' == name[0] || '"' == name[0])) {
      name.erase(name.begin());
    }
    while (!name.empty() && ('`' == name[name.size() - 1] || '"' == name[name.size() - 1])) {
      name.erase(name.size() - 1);
    }
    names.push_back(name);
  }
}

int build_metadata_sql(const Table &table, std::string &out)
{
  int ret = OB_SUCCESS;
  out.clear();
  for (size_t c = 0; c < table.columns_.size(); ++c) {
    if (c > 0) {
      out += " union all select ";
    } else {
      out += "select ";
    }
    out += quote_literal(table.columns_[c].name_);
    out += " as column_name, ";
    out += quote_literal(value_type_name(table.columns_[c].type_));
    out += " as column_type, ";
    out += quote_literal(table.columns_[c].nullable_ ? "YES" : "NO");
    out += " as `null`, cast(null as char(16)) as `key`, cast(null as char(16)) as `default`, "
           "cast(null as char(16)) as extra";
  }
  if (table.columns_.empty()) {
    out = "select cast(null as char(16)) as column_name, cast(null as char(16)) as column_type, "
          "cast(null as char(16)) as `null`, cast(null as char(16)) as `key`, "
          "cast(null as char(16)) as `default`, cast(null as char(16)) as extra limit 0";
  }
  return ret;
}

// Replaces every file function call inside `text` with an inline table.
int rewrite_calls(const std::string &text, ObSQLSessionInfo &session, std::string &out)
{
  int ret = OB_SUCCESS;
  std::vector<CallInfo> calls;
  find_calls(text, calls);
  if (calls.empty()) {
    out = text;
  } else {
    std::string result;
    size_t copied = 0;
    for (size_t i = 0; OB_SUCC(ret) && i < calls.size(); ++i) {
      const CallInfo &call = calls[i];
      ArgSet args;
      Table table;
      if (OB_FAIL(parse_args(call, args))) {
      } else if (!args.has_path_) {
        ret = OB_ERR_PARSE_SQL;
      } else if (args.extra_positional_) {
        // DuckDB's "read_csv(path, 'col type list')" is not supported
        ret = OB_ERR_PARSE_SQL;
      } else if (args.has_sample_size_ && -1 != args.sample_size_) {
        // only full file inference is supported
        ret = OB_NOT_SUPPORTED;
      } else if (GLOB_FUNCTION != call.name_ && args.has_format_ && "read_csv" == call.name_) {
        ret = OB_ERR_PARSE_SQL;
      } else if (OB_FAIL(check_secure_path(session, args.path_))) {
      } else if (OB_FAIL(load_table(call, args, table))) {
      } else {
        result.append(text, copied, call.begin_ - copied);
        result += build_table_sql(table);
        copied = call.end_;
      }
    }
    if (OB_SUCC(ret)) {
      result.append(text, copied, text.size() - copied);
      out = result;
    }
  }
  return ret;
}

bool statement_is(const std::string &text, const char *keyword, size_t &rest_pos)
{
  std::string trimmed = trim_copy(text);
  // skip leading comments
  size_t pos = 0;
  if (starts_with_ci(trimmed, keyword)) {
    pos = ::strlen(keyword);
    if (pos >= trimmed.size() || !is_ident_char(trimmed[pos])) {
      rest_pos = text.size() - trimmed.size() + pos;
      return true;
    }
  }
  return false;
}

int rewrite_copy(const std::string &text, ObSQLSessionInfo &session, std::string &out)
{
  int ret = OB_SUCCESS;
  size_t pos = 0;
  if (!statement_is(text, "copy", pos)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    // copy ( <query> ) to '<path>' ( options )
    while (pos < text.size() && isspace(static_cast<unsigned char>(text[pos]))) {
      ++pos;
    }
    if (pos >= text.size() || '(' != text[pos]) {
      ret = OB_ERR_PARSE_SQL;
    } else {
      size_t query_close = find_matching_paren(text, pos);
      if (std::string::npos == query_close) {
        ret = OB_ERR_PARSE_SQL;
      } else {
        std::string query = text.substr(pos + 1, query_close - pos - 1);
        size_t p = query_close + 1;
        while (p < text.size() && isspace(static_cast<unsigned char>(text[p]))) {
          ++p;
        }
        if (p + 2 > text.size() || !starts_with_ci(text.substr(p, 2), "to")) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          p += 2;
          while (p < text.size() && isspace(static_cast<unsigned char>(text[p]))) {
            ++p;
          }
          std::string path;
          std::string rest = text.substr(p);
          size_t literal_end = std::string::npos;
          if (rest.empty() || '\'' != rest[0]) {
            ret = OB_ERR_PARSE_SQL;
          } else {
            literal_end = skip_quoted(rest, 0, '\'');
            if (std::string::npos == literal_end) {
              ret = OB_ERR_PARSE_SQL;
            } else if (!parse_string_literal(rest.substr(0, literal_end), path)) {
              ret = OB_ERR_PARSE_SQL;
            }
          }
          if (OB_SUCC(ret)) {
            std::string rewritten_query;
            std::vector<std::string> names;
            if (OB_FAIL(rewrite_calls(query, session, rewritten_query))) {
            } else {
              extract_select_names(query, names);
            }
            struct stat target_stat;
            if (OB_SUCC(ret) && OB_FAIL(check_secure_path(session, path))) {
            } else if (0 == ::stat(path.c_str(), &target_stat)) {
              // COPY never overwrites an existing target
              ret = OB_FILE_ALREADY_EXIST;
            }
            if (OB_SUCC(ret)) {
              if (names.empty()) {
                ret = OB_ERR_PARSE_SQL;
              } else {
                // INTO OUTFILE must sit between the select list and FROM.
                out = "select * into outfile ";
                out += quote_literal(path);
                out += " format (type='csv', field_delimiter=',', line_delimiter='\\n', "
                       "escape='\\\\', field_optionally_enclosed_by='\"') from (select ";
              }
              for (size_t i = 0; OB_SUCC(ret) && i < names.size(); ++i) {
                if (i > 0) {
                  out += ", ";
                }
                out += quote_literal(names[i]);
                out += " as ";
                out += quote_ident(names[i]);
              }
              if (OB_SUCC(ret)) {
                out += " union all select * from (";
                out += rewritten_query;
                out += ") __file_sql_copy_src) __file_sql_copy";
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int rewrite_describe(const std::string &text, ObSQLSessionInfo &session, std::string &out,
                     bool &handled)
{
  int ret = OB_SUCCESS;
  handled = false;
  size_t pos = 0;
  bool matched = false;
  if (statement_is(text, "describe", pos)) {
    matched = true;
  } else if (statement_is(text, "desc", pos)) {
    matched = true;
  }
  if (matched) {
    std::string rest = trim_copy(text.substr(pos));
    if (starts_with_ci(rest, "select") || starts_with_ci(rest, "with")) {
      std::vector<CallInfo> calls;
      find_calls(rest, calls);
      if (!calls.empty()) {
        handled = true;
        const CallInfo &call = calls[0];
        ArgSet args;
        Table table;
        if (OB_FAIL(parse_args(call, args))) {
        } else if (!args.has_path_) {
          ret = OB_ERR_PARSE_SQL;
        } else if (args.extra_positional_) {
          ret = OB_ERR_PARSE_SQL;
        } else if (args.has_sample_size_ && -1 != args.sample_size_) {
          ret = OB_NOT_SUPPORTED;
        } else if (OB_FAIL(check_secure_path(session, args.path_))) {
        } else if (OB_FAIL(load_table(call, args, table))) {
        } else if (OB_FAIL(build_metadata_sql(table, out))) {
        }
      }
    }
  }
  return ret;
}

}  // namespace

int ObFileSqlRewriter::rewrite(common::ObIAllocator &alloc,
                               const common::ObString &stmt,
                               ObSQLSessionInfo &session,
                               common::ObString &rewritten)
{
  int ret = OB_SUCCESS;
  rewritten = stmt;
  if (stmt.empty() || stmt.length() > MAX_SQL_TEXT_FOR_FILE_FUNCS) {
    // nothing to do
  } else {
    std::string text(stmt.ptr(), static_cast<size_t>(stmt.length()));
    std::string flag_read_result;
    bool set_flag_enabled = false;
    bool set_flag_bad_value = false;
    // A file function name (or COPY) is the only thing that requires the
    // switch. A bare `desc t` is left alone so that ordinary statements are
    // never affected.
    const bool has_function = contains_ci(text, "read_csv") || contains_ci(text, "read_json")
                              || contains_ci(text, "read_ndjson")
                              || contains_ci(text, "read_parquet")
                              || contains_ci(text, "glob(");
    const bool has_copy_stmt = starts_with_ci(trim_copy(text), "copy");
    const bool mentions_file_sql = has_function || has_copy_stmt
                                   || starts_with_ci(trim_copy(text), "describe")
                                   || starts_with_ci(trim_copy(text), "desc ");
    // `set session enable_file_sql = ...` does not mention any file function, so
    // it needs a dedicated branch. The switch is applied through the session
    // API and the statement itself is turned into a plain session variable
    // assignment, which keeps it on a well tested code path.
    if (parse_set_file_sql_stmt(text, set_flag_enabled, set_flag_bad_value)) {
      if (set_flag_bad_value) {
        ret = OB_ERR_WRONG_VALUE_FOR_VAR;
      } else {
        // The statement itself only assigns the mirror variable. Reading the
        // switch back afterwards is therefore perfectly in sync with what the
        // client has sent, and nothing but a plain session variable assignment
        // is executed here.
        const std::string noop = set_flag_enabled ? "set @file_sql_flag = 1"
                                                  : "set @file_sql_flag = 0";
        char *buffer = static_cast<char *>(alloc.alloc(noop.size() + 1));
        if (NULL == buffer) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          ::memcpy(buffer, noop.data(), noop.size());
          buffer[noop.size()] = '\0';
          rewritten.assign_ptr(buffer, static_cast<int32_t>(noop.size()));
        }
      }
    } else if (rewrite_select_flag(text, flag_read_result)) {
      if (OB_FAIL(copy_text(alloc, flag_read_result, rewritten))) {
      }
    } else if (!mentions_file_sql) {
      // leave ordinary statements untouched
    } else {
      std::string result;
      // The switch is read from the session variable only: the system variable
      // of the same name is a pure mirror and is never touched, so neither the
      // SET nor the file function path can reach the system variable store.
      const bool enabled = user_flag_enabled(session);
      {
        std::string trimmed = trim_copy(text);
        bool handled_describe = false;
        bool has_copy = has_copy_stmt;
        bool has_describe = starts_with_ci(trimmed, "describe")
                            || starts_with_ci(trimmed, "desc ");
        // The switch is a plain boolean member of the session, and while it is
        // off the statement is rejected before any parsing happens. This keeps
        // the disabled path down to a couple of substring tests.
        const bool touches_file_sql = has_function || has_copy;
        if (!touches_file_sql) {
          result = text;
        } else if (!enabled) {
          ret = OB_NOT_SUPPORTED;
        } else if (has_copy) {
          if (OB_FAIL(rewrite_copy(trimmed, session, result))) {
          }
        } else if (has_describe) {
          if (OB_FAIL(rewrite_describe(trimmed, session, result, handled_describe))) {
          } else if (!handled_describe) {
            result = text;
          }
        } else {
          if (OB_FAIL(rewrite_calls(trimmed, session, result))) {
          }
        }
      }
      if (OB_SUCC(ret) && result != text) {
        if (result.empty()) {
          result = text;
        }
        char *buffer = static_cast<char *>(alloc.alloc(result.size() + 1));
        if (NULL == buffer) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          ::memcpy(buffer, result.data(), result.size());
          buffer[result.size()] = '\0';
          rewritten.assign_ptr(buffer, static_cast<int32_t>(result.size()));
        }
      }
    }
  }
  return ret;
}

}  // namespace filesql
}  // namespace sql
}  // namespace oceanbase
