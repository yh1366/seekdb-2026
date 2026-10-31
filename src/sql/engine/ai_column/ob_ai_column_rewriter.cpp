/**
 * Copyright (c) 2021 OceanBase
 * SPDX-License-Identifier: Apache-2.0
 *
 * AI COLUMN rewriter implementation.
 */

#include "sql/engine/ai_column/ob_ai_column_rewriter.h"

#include <ctype.h>
#include <string.h>

#include <string>
#include <vector>

#include "share/ob_errno.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{
namespace aicolumn
{

const char *const AI_COLUMN_STATUS_SUFFIX = "_status";
const char *const AI_COLUMN_SRC_SUFFIX = "_src";
const char *const AI_COLUMN_SEQ_SUFFIX = "_seq";
const char *const AI_COLUMN_COMMENT_PREFIX = "{\"ai_column\":1";

namespace
{

const char *const AI_COLUMN_KW = "ai column";
const char *const HIDDEN_PREFIX = "__ai_";
const char *const STRICT_VAR_NAME = "ob_ai_column_strict_flag";
const char *const STRICT_SYSVAR_NAME = "ai_column_strict_read";
const int64_t MAX_SQL_TEXT_FOR_AI_COLUMN = 512 * 1024;

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
  const size_t len = ::strlen(prefix);
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
  const size_t needle_len = ::strlen(needle);
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

bool is_ident_char(const char c)
{
  return 0 != ::isalnum(static_cast<unsigned char>(c)) || '_' == c || '$' == c;
}

// Matches `kw` at `pos` of an already lower cased text, on identifier
// boundaries.
bool match_kw_at(const std::string &lower, const size_t pos, const char *kw)
{
  const size_t len = ::strlen(kw);
  bool matched = false;
  if (pos + len <= lower.size() && 0 == lower.compare(pos, len, kw)) {
    matched = true;
    if (pos > 0 && (is_ident_char(lower[pos - 1]) || '.' == lower[pos - 1])) {
      matched = false;
    }
    if (matched && pos + len < lower.size() && is_ident_char(lower[pos + len])) {
      matched = false;
    }
  }
  return matched;
}

// Returns the position of the matching ')' of the '(' at `open_pos`.
size_t find_matching_paren(const std::string &text, const size_t open_pos)
{
  if (open_pos >= text.size() || '(' != text[open_pos]) {
    return std::string::npos;
  }
  int64_t depth = 0;
  char quote = 0;
  size_t result = std::string::npos;
  for (size_t i = open_pos; i < text.size() && std::string::npos == result; ++i) {
    const char c = text[i];
    if (0 != quote) {
      if ('\\' == c) {
        ++i;
      } else if (c == quote) {
        quote = 0;
      }
    } else if ('\'' == c || '"' == c || '`' == c) {
      quote = c;
    } else if ('(' == c) {
      ++depth;
    } else if (')' == c) {
      --depth;
      if (0 == depth) {
        result = i;
      }
    }
  }
  return result;
}

// Splits `text` on `sep` occurrences that are not nested in parens or quotes.
void split_top_level(const std::string &text, const char sep, std::vector<std::string> &out)
{
  size_t start = 0;
  int64_t depth = 0;
  char quote = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (0 != quote) {
      if ('\\' == c) {
        ++i;
      } else if (c == quote) {
        quote = 0;
      }
    } else if ('\'' == c || '"' == c || '`' == c) {
      quote = c;
    } else if ('(' == c) {
      ++depth;
    } else if (')' == c) {
      --depth;
    } else if (sep == c && 0 == depth) {
      out.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  out.push_back(text.substr(start));
}

// Finds `kw` at paren depth 0 of an already lower cased text.
size_t find_kw_top_level(const std::string &lower, const char *kw, const size_t from)
{
  int64_t depth = 0;
  char quote = 0;
  size_t result = std::string::npos;
  for (size_t i = from; i < lower.size() && std::string::npos == result; ++i) {
    const char c = lower[i];
    if (0 != quote) {
      if ('\\' == c) {
        ++i;
      } else if (c == quote) {
        quote = 0;
      }
    } else if ('\'' == c || '"' == c || '`' == c) {
      quote = c;
    } else if ('(' == c) {
      ++depth;
    } else if (')' == c) {
      --depth;
    } else if (0 == depth && match_kw_at(lower, i, kw)) {
      result = i;
    }
  }
  return result;
}

// Reads the first whitespace separated token, backquotes are kept as is.
std::string first_token(const std::string &text)
{
  const std::string trimmed = trim_copy(text);
  size_t end = 0;
  bool in_quote = false;
  while (end < trimmed.size()) {
    const char c = trimmed[end];
    if ('`' == c) {
      in_quote = !in_quote;
    } else if (!in_quote && isspace(static_cast<unsigned char>(c))) {
      break;
    }
    ++end;
  }
  return trimmed.substr(0, end);
}

std::string bare_name(const std::string &raw)
{
  std::string name = trim_copy(raw);
  if (name.size() >= 2 && '`' == name[0] && '`' == name[name.size() - 1]) {
    name = name.substr(1, name.size() - 2);
  }
  return name;
}

std::string column_name_of(const std::string &text)
{
  const std::string trimmed = trim_copy(text);
  std::string name;
  if (!trimmed.empty()) {
    if ('`' == trimmed[0]) {
      const size_t close = trimmed.find('`', 1);
      if (std::string::npos != close) {
        name = trimmed.substr(1, close - 1);
      }
    } else {
      size_t end = 0;
      while (end < trimmed.size() && is_ident_char(trimmed[end])) {
        ++end;
      }
      name = trimmed.substr(0, end);
    }
  }
  return name;
}

std::string escape_json(const std::string &in)
{
  std::string out;
  out.reserve(in.size() + 8);
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string escape_sql_string(const std::string &in)
{
  std::string out;
  out.reserve(in.size() + 8);
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if ('\'' == c) {
      out += "''";
    } else if ('\\' == c) {
      out += "\\\\";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

struct AiColumnDef
{
  AiColumnDef() : name_(), prefix_(), expr_(), options_() {}
  std::string name_;     // bare column name
  std::string prefix_;   // original text of the column definition before AI COLUMN
  std::string expr_;     // AI expression, e.g. AI_COMPLETE('m', content)
  std::string options_;  // raw text inside WITH (...)
};

// Parses one column definition. Returns true when it declares an AI column.
bool parse_ai_column_def(const std::string &def, AiColumnDef &out)
{
  bool found = false;
  const std::string lower = to_lower(def);
  const size_t pos = find_kw_top_level(lower, AI_COLUMN_KW, 0);
  if (std::string::npos != pos) {
    const std::string prefix = trim_copy(def.substr(0, pos));
    const std::string rest = trim_copy(def.substr(pos + ::strlen(AI_COLUMN_KW)));
    if (!prefix.empty() && !rest.empty() && '(' == rest[0]) {
      const size_t rp = find_matching_paren(rest, 0);
      if (std::string::npos != rp) {
        const std::string expr = trim_copy(rest.substr(1, rp - 1));
        const std::string after = trim_copy(rest.substr(rp + 1));
        std::string options;
        if (starts_with_ci(after, "with")) {
          const size_t lp = after.find('(');
          if (std::string::npos != lp) {
            const size_t arp = find_matching_paren(after, lp);
            if (std::string::npos != arp) {
              options = trim_copy(after.substr(lp + 1, arp - lp - 1));
            }
          }
        }
        const std::string name = column_name_of(prefix);
        if (!name.empty() && !expr.empty()) {
          out.name_ = name;
          out.prefix_ = prefix;
          out.expr_ = expr;
          out.options_ = options;
          found = true;
        }
      }
    }
  }
  return found;
}

std::string build_comment(const AiColumnDef &def)
{
  std::string json = AI_COLUMN_COMMENT_PREFIX;
  json += ",\"expr\":\"";
  json += escape_json(def.expr_);
  json += "\"";
  if (!def.options_.empty()) {
    json += ",\"options\":\"";
    json += escape_json(def.options_);
    json += "\"";
  }
  json += "}";
  return json;
}

void build_hidden_defs(const std::string &name, std::vector<std::string> &out)
{
  const std::string base = std::string(HIDDEN_PREFIX) + name;
  out.push_back(base + AI_COLUMN_STATUS_SUFFIX + " VARCHAR(16) DEFAULT 'PENDING' INVISIBLE");
  out.push_back(base + AI_COLUMN_SRC_SUFFIX + " VARCHAR(64) DEFAULT '' INVISIBLE");
  out.push_back(base + AI_COLUMN_SEQ_SUFFIX + " BIGINT DEFAULT 0 INVISIBLE");
}

std::string table_name_of(const std::string &text)
{
  std::string name;
  const std::string lower = to_lower(text);
  const size_t pos = find_kw_top_level(lower, "table", 0);
  if (std::string::npos != pos) {
    name = first_token(text.substr(pos + ::strlen("table")));
  }
  return name;
}

// Replaces the bare `status` pseudo column with the real state column, while
// back quoted `status` keeps referring to the business column.
std::string rewrite_status_pseudo(const std::string &cond, const std::string &status_col)
{
  std::string out;
  const std::string lower = to_lower(cond);
  size_t i = 0;
  while (i < cond.size()) {
    const char c = cond[i];
    if ('`' == c) {
      const size_t close = cond.find('`', i + 1);
      if (std::string::npos == close) {
        out += cond.substr(i);
        break;
      }
      out += cond.substr(i, close - i + 1);
      i = close + 1;
    } else if (match_kw_at(lower, i, "status")) {
      out += status_col;
      i += ::strlen("status");
    } else {
      out.push_back(c);
      ++i;
    }
  }
  return out;
}

int copy_out(common::ObIAllocator &alloc, const std::string &text, common::ObString &out)
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

int rewrite_create_table(const std::string &text, std::string &result)
{
  int ret = OB_SUCCESS;
  const std::string lower = to_lower(text);
  const size_t lp = lower.find('(');
  if (std::string::npos == lp) {
    ret = OB_ERR_PARSE_SQL;
  } else {
    const size_t rp = find_matching_paren(text, lp);
    if (std::string::npos == rp) {
      ret = OB_ERR_PARSE_SQL;
    } else {
      const std::string head = text.substr(0, lp);
      const std::string body = text.substr(lp + 1, rp - lp - 1);
      const std::string tail = text.substr(rp + 1);
      std::vector<std::string> defs;
      split_top_level(body, ',', defs);
      std::vector<std::string> out_defs;
      bool any_ai = false;
      for (size_t i = 0; i < defs.size(); ++i) {
        AiColumnDef def;
        if (parse_ai_column_def(defs[i], def)) {
          any_ai = true;
          out_defs.push_back(def.prefix_ + " COMMENT '"
                             + escape_sql_string(build_comment(def)) + "'");
          std::vector<std::string> hidden;
          build_hidden_defs(def.name_, hidden);
          for (size_t k = 0; k < hidden.size(); ++k) {
            out_defs.push_back(hidden[k]);
          }
        } else {
          out_defs.push_back(trim_copy(defs[i]));
        }
      }
      if (!any_ai) {
        result = text;
      } else {
        result.clear();
        result += head;
        result += "(";
        for (size_t i = 0; i < out_defs.size(); ++i) {
          if (0 != i) {
            result += ", ";
          }
          result += out_defs[i];
        }
        result += ")";
        result += tail;
      }
    }
  }
  return ret;
}

// ALTER TABLE t RETRY AI COLUMN c [WHERE cond]
int rewrite_alter_retry(const std::string &text, std::string &result)
{
  int ret = OB_SUCCESS;
  const std::string lower = to_lower(text);
  const std::string table_name = table_name_of(text);
  const size_t kw_pos = find_kw_top_level(lower, AI_COLUMN_KW, 0);
  if (table_name.empty() || std::string::npos == kw_pos) {
    ret = OB_ERR_PARSE_SQL;
  } else {
    const std::string after = trim_copy(text.substr(kw_pos + ::strlen(AI_COLUMN_KW)));
    const std::string col_token = first_token(after);
    const std::string col_name = bare_name(col_token);
    if (col_name.empty()) {
      ret = OB_ERR_PARSE_SQL;
    } else {
      const std::string base = std::string(HIDDEN_PREFIX) + col_name;
      const std::string status_col = base + AI_COLUMN_STATUS_SUFFIX;
      const std::string seq_col = base + AI_COLUMN_SEQ_SUFFIX;
      result = "update " + table_name + " set " + status_col + " = 'PENDING', "
               + seq_col + " = " + seq_col + " + 1";
      const size_t where_pos = find_kw_top_level(to_lower(after.substr(col_token.size())),
                                                 "where", 0);
      if (std::string::npos != where_pos) {
        const std::string cond = trim_copy(after.substr(col_token.size() + where_pos
                                                        + ::strlen("where")));
        if (cond.empty()) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          result += " where ";
          result += rewrite_status_pseudo(cond, status_col);
        }
      }
    }
  }
  return ret;
}

// ALTER TABLE t ADD [COLUMN] c <type> AI COLUMN (...) [WITH (...)]
// ALTER TABLE t MODIFY [COLUMN] c <type> AI COLUMN (...) [WITH (...)]
int rewrite_alter_add_modify(const std::string &text, std::string &result, bool &handled)
{
  int ret = OB_SUCCESS;
  handled = false;
  const std::string lower = to_lower(text);
  const std::string table_name = table_name_of(text);
  if (table_name.empty()) {
    // not a shape we understand, leave it to the native resolver
  } else {
    const size_t add_pos = find_kw_top_level(lower, "add", 0);
    const size_t modify_pos = find_kw_top_level(lower, "modify", 0);
    bool is_add = false;
    size_t def_pos = std::string::npos;
    if (std::string::npos != add_pos
        && (std::string::npos == modify_pos || add_pos < modify_pos)) {
      is_add = true;
      def_pos = add_pos + ::strlen("add");
    } else if (std::string::npos != modify_pos) {
      def_pos = modify_pos + ::strlen("modify");
    }
    if (std::string::npos != def_pos) {
      std::string def_text = trim_copy(text.substr(def_pos));
      if (starts_with_ci(def_text, "column")) {
        def_text = trim_copy(def_text.substr(::strlen("column")));
      }
      AiColumnDef def;
      if (parse_ai_column_def(def_text, def)) {
        const std::string with_comment = def.prefix_ + " COMMENT '"
                                         + escape_sql_string(build_comment(def)) + "'";
        result = "alter table " + table_name;
        result += is_add ? " add column " : " modify column ";
        result += with_comment;
        if (is_add) {
          std::vector<std::string> hidden;
          build_hidden_defs(def.name_, hidden);
          for (size_t k = 0; k < hidden.size(); ++k) {
            result += ", add column ";
            result += hidden[k];
          }
        }
        handled = true;
      }
    }
  }
  return ret;
}

int rewrite_alter(const std::string &text, std::string &result)
{
  int ret = OB_SUCCESS;
  bool handled = false;
  const std::string lower = to_lower(text);
  if (std::string::npos != find_kw_top_level(lower, "retry", 0)) {
    ret = rewrite_alter_retry(text, result);
    handled = (OB_SUCCESS == ret);
  } else {
    ret = rewrite_alter_add_modify(text, result, handled);
  }
  if (OB_SUCC(ret) && !handled) {
    result = text;
  }
  return ret;
}

// SHOW AI COLUMNS [FROM|IN] t
int rewrite_show(const std::string &text, std::string &result)
{
  int ret = OB_SUCCESS;
  const std::string lower = to_lower(text);
  // `show ai columns` uses the plural form, take the longest match first so
  // that the trailing 's' does not end up in the object name.
  size_t kw_pos = lower.find("ai columns");
  size_t kw_len = ::strlen("ai columns");
  if (std::string::npos == kw_pos) {
    kw_pos = find_kw_top_level(lower, AI_COLUMN_KW, 0);
    kw_len = ::strlen(AI_COLUMN_KW);
  }
  if (std::string::npos == kw_pos) {
    result = text;
  } else {
    std::string rest = trim_copy(text.substr(kw_pos + kw_len));
    const std::string rest_lower = to_lower(rest);
    if (starts_with_ci(rest, "from") || starts_with_ci(rest, "in")) {
      rest = trim_copy(rest.substr(rest_lower.compare(0, 2, "in") == 0 ? 2 : 4));
    }
    const std::string full_name = bare_name(first_token(rest));
    std::string db_name;
    std::string table_name = full_name;
    const size_t dot = full_name.find('.');
    if (std::string::npos != dot) {
      db_name = bare_name(full_name.substr(0, dot));
      table_name = bare_name(full_name.substr(dot + 1));
    }
    if (table_name.empty()) {
      ret = OB_ERR_PARSE_SQL;
    } else {
      result = "select column_name as column_name, column_type as column_type, "
               "column_comment as column_comment from information_schema.columns where ";
      if (db_name.empty()) {
        result += "table_schema = database() and ";
      } else {
        result += "table_schema = '" + escape_sql_string(db_name) + "' and ";
      }
      result += "table_name = '" + escape_sql_string(table_name) + "' and ";
      result += "column_comment like '";
      result += escape_sql_string(AI_COLUMN_COMMENT_PREFIX);
      result += "%' order by ordinal_position";
    }
  }
  return ret;
}

// SET [SESSION] ai_column_strict_read = ON|OFF|TRUE|FALSE|1|0
int rewrite_set_strict(const std::string &text, std::string &result)
{
  int ret = OB_SUCCESS;
  const std::string lower = to_lower(text);
  const size_t pos = lower.find(STRICT_SYSVAR_NAME);
  if (std::string::npos == pos) {
    result = text;
  } else {
    size_t p = pos + ::strlen(STRICT_SYSVAR_NAME);
    while (p < lower.size() && isspace(static_cast<unsigned char>(lower[p]))) {
      ++p;
    }
    if (p < lower.size() && ':' == lower[p]) {
      ++p;
    }
    if (p < lower.size() && '=' == lower[p]) {
      ++p;
    }
    std::string value = trim_copy(lower.substr(p));
    if (!value.empty() && (('\'' == value[0] && '\'' == value[value.size() - 1])
                           || ('"' == value[0] && '"' == value[value.size() - 1]))) {
      value = trim_copy(value.substr(1, value.size() - 2));
    }
    const char *literal = NULL;
    if ("on" == value || "true" == value || "1" == value) {
      literal = "1";
    } else if ("off" == value || "false" == value || "0" == value
               || "default" == value) {
      literal = "0";
    }
    if (NULL == literal) {
      ret = OB_ERR_WRONG_VALUE_FOR_VAR;
    } else {
      result = "set @";
      result += STRICT_VAR_NAME;
      result += " = ";
      result += literal;
    }
  }
  return ret;
}

}  // namespace

int ObAiColumnRewriter::rewrite(common::ObIAllocator &alloc,
                                const common::ObString &stmt,
                                ObSQLSessionInfo &session,
                                common::ObString &rewritten)
{
  int ret = OB_SUCCESS;
  (void)session;
  rewritten = stmt;
  if (stmt.empty() || stmt.length() > MAX_SQL_TEXT_FOR_AI_COLUMN) {
    // nothing to do
  } else {
    const std::string text(stmt.ptr(), static_cast<size_t>(stmt.length()));
    std::string result;
    bool handled = false;
    if (contains_ci(text, AI_COLUMN_KW) || contains_ci(text, STRICT_SYSVAR_NAME)) {
      const std::string trimmed = trim_copy(text);
      if (starts_with_ci(trimmed, "create")) {
        ret = rewrite_create_table(text, result);
        handled = true;
      } else if (starts_with_ci(trimmed, "alter")) {
        ret = rewrite_alter(text, result);
        handled = true;
      } else if (starts_with_ci(trimmed, "show")) {
        ret = rewrite_show(text, result);
        handled = true;
      } else if (starts_with_ci(trimmed, "set")) {
        ret = rewrite_set_strict(text, result);
        handled = true;
      } else {
        // other statements only mention the phrase in a literal
      }
    }
    if (OB_SUCC(ret) && handled && !result.empty() && result != text) {
      ret = copy_out(alloc, result, rewritten);
    }
  }
  return ret;
}

}  // namespace aicolumn
}  // namespace sql
}  // namespace oceanbase
