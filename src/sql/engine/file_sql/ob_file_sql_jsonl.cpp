/**
 * Copyright (c) 2021 OceanBase
 * SPDX-License-Identifier: Apache-2.0
 *
 * JSON Lines (NDJSON) reader with union-schema inference.
 *
 * Every non empty line must contain a flat JSON object. Missing fields become
 * NULL, and the column set is the union of all keys in first-seen order.
 */

#include "sql/engine/file_sql/ob_file_sql_format.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace oceanbase
{
namespace sql
{
namespace filesql
{

namespace
{

struct JsonValue
{
  enum Kind
  {
    JNULL = 0,
    JBOOL,
    JINT,
    JDOUBLE,
    JSTR,
    JOBJ,
    JARR
  };

  Kind kind_;
  int64_t int_value_;
  double double_value_;
  std::string str_value_;

  JsonValue() : kind_(JNULL), int_value_(0), double_value_(0) {}
};

struct JsonMember
{
  std::string key_;
  JsonValue value_;
};

struct JsonDoc
{
  std::vector<JsonMember> members_;
};

class JsonParser
{
public:
  JsonParser(const char *begin, const char *end) : pos_(begin), end_(end) {}

  // Parses a flat object. Nested objects/arrays are rejected as an error so a
  // caller can report "nested structures are not supported".
  int parse_object(JsonDoc &doc, std::string &err)
  {
    int ret = 0;
    skip_ws();
    if (pos_ >= end_ || '{' != *pos_) {
      ret = -1;
      err = "expected a JSON object at the beginning of the line";
    } else {
      ++pos_;
      skip_ws();
      if (pos_ < end_ && '}' == *pos_) {
        ++pos_;
      } else {
        while (0 == ret) {
          JsonMember member;
          if (0 != (ret = parse_string(member.key_, err))) {
            // ret already set
          } else if (0 != (ret = expect(':', err))) {
            // ret already set
          } else if (0 != (ret = parse_value(member.value_, err))) {
            // ret already set
          } else if (JsonValue::JOBJ == member.value_.kind_
                     || JsonValue::JARR == member.value_.kind_) {
            ret = -1;
            err = "nested objects and arrays are not supported";
          } else {
            doc.members_.push_back(member);
            skip_ws();
            if (pos_ < end_ && ',' == *pos_) {
              ++pos_;
              skip_ws();
            } else if (pos_ < end_ && '}' == *pos_) {
              ++pos_;
              break;
            } else {
              ret = -1;
              err = "expected ',' or '}' in JSON object";
            }
          }
        }
      }
      if (0 == ret) {
        skip_ws();
        if (pos_ != end_) {
          ret = -1;
          err = "unexpected trailing characters after the JSON object";
        }
      }
    }
    return ret;
  }

private:
  void skip_ws()
  {
    while (pos_ < end_ && (' ' == *pos_ || '\t' == *pos_ || '\r' == *pos_ || '\n' == *pos_)) {
      ++pos_;
    }
  }

  int expect(char c, std::string &err)
  {
    int ret = 0;
    skip_ws();
    if (pos_ < end_ && c == *pos_) {
      ++pos_;
    } else {
      ret = -1;
      char buf[64];
      snprintf(buf, sizeof(buf), "expected '%c' in JSON object", c);
      err = buf;
    }
    return ret;
  }

  static void append_utf8(std::string &out, uint32_t cp)
  {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  int parse_hex4(uint32_t &value, std::string &err)
  {
    int ret = 0;
    uint32_t result = 0;
    if (end_ - pos_ < 4) {
      ret = -1;
      err = "truncated \\u escape in JSON string";
    } else {
      for (int i = 0; 0 == ret && i < 4; ++i) {
        char c = pos_[i];
        int digit = -1;
        if (c >= '0' && c <= '9') {
          digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
          digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
          digit = c - 'A' + 10;
        }
        if (digit < 0) {
          ret = -1;
          err = "invalid \\u escape in JSON string";
        } else {
          result = (result << 4) | static_cast<uint32_t>(digit);
        }
      }
      if (0 == ret) {
        value = result;
        pos_ += 4;
      }
    }
    return ret;
  }

  int parse_string(std::string &out, std::string &err)
  {
    int ret = 0;
    out.clear();
    skip_ws();
    if (pos_ >= end_ || '"' != *pos_) {
      ret = -1;
      err = "expected a JSON string";
    } else {
      ++pos_;
      bool closed = false;
      while (0 == ret && pos_ < end_ && !closed) {
        char c = *pos_++;
        if ('"' == c) {
          closed = true;
        } else if ('\\' == c) {
          if (pos_ >= end_) {
            ret = -1;
            err = "truncated escape in JSON string";
          } else {
            char esc = *pos_++;
            switch (esc) {
              case '"': out.push_back('"'); break;
              case '\\': out.push_back('\\'); break;
              case '/': out.push_back('/'); break;
              case 'b': out.push_back('\b'); break;
              case 'f': out.push_back('\f'); break;
              case 'n': out.push_back('\n'); break;
              case 'r': out.push_back('\r'); break;
              case 't': out.push_back('\t'); break;
              case 'u': {
                uint32_t cp = 0;
                if (0 != (ret = parse_hex4(cp, err))) {
                  // ret already set
                } else if (cp >= 0xD800 && cp <= 0xDBFF && end_ - pos_ >= 6
                           && '\\' == pos_[0] && 'u' == pos_[1]) {
                  uint32_t low = 0;
                  const char *saved = pos_;
                  pos_ += 2;
                  if (0 != (ret = parse_hex4(low, err))) {
                    // ret already set
                  } else if (low >= 0xDC00 && low <= 0xDFFF) {
                    append_utf8(out, 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00));
                  } else {
                    pos_ = saved;
                    append_utf8(out, cp);
                  }
                } else {
                  append_utf8(out, cp);
                }
                break;
              }
              default:
                ret = -1;
                err = "unsupported escape sequence in JSON string";
                break;
            }
          }
        } else {
          out.push_back(c);
        }
      }
      if (0 == ret && !closed) {
        ret = -1;
        err = "unterminated JSON string";
      }
    }
    return ret;
  }

  int parse_number(JsonValue &value)
  {
    int ret = 0;
    const char *begin = pos_;
    bool is_double = false;
    if (pos_ < end_ && ('-' == *pos_ || '+' == *pos_)) {
      ++pos_;
    }
    while (pos_ < end_) {
      char c = *pos_;
      if (c >= '0' && c <= '9') {
        ++pos_;
      } else if ('.' == c || 'e' == c || 'E' == c || '+' == c || '-' == c) {
        is_double = true;
        ++pos_;
      } else {
        break;
      }
    }
    std::string text(begin, pos_);
    if (text.empty()) {
      ret = -1;
    } else if (!is_double) {
      errno = 0;
      char *end_ptr = NULL;
      long long parsed = ::strtoll(text.c_str(), &end_ptr, 10);
      if (0 == errno && end_ptr == text.c_str() + text.size()) {
        value.kind_ = JsonValue::JINT;
        value.int_value_ = static_cast<int64_t>(parsed);
      } else {
        value.kind_ = JsonValue::JDOUBLE;
        value.double_value_ = ::strtod(text.c_str(), NULL);
      }
    } else {
      errno = 0;
      value.kind_ = JsonValue::JDOUBLE;
      value.double_value_ = ::strtod(text.c_str(), NULL);
    }
    return ret;
  }

  int parse_value(JsonValue &value, std::string &err)
  {
    int ret = 0;
    skip_ws();
    if (pos_ >= end_) {
      ret = -1;
      err = "unexpected end of JSON line";
    } else if ('"' == *pos_) {
      value.kind_ = JsonValue::JSTR;
      ret = parse_string(value.str_value_, err);
    } else if ('{' == *pos_ || '[' == *pos_) {
      // remember the kind so the caller can reject nesting
      value.kind_ = ('{' == *pos_) ? JsonValue::JOBJ : JsonValue::JARR;
      ret = skip_container(err);
    } else if (0 == ::strncmp(pos_, "true", 4)) {
      pos_ += 4;
      value.kind_ = JsonValue::JBOOL;
      value.int_value_ = 1;
    } else if (0 == ::strncmp(pos_, "false", 5)) {
      pos_ += 5;
      value.kind_ = JsonValue::JBOOL;
      value.int_value_ = 0;
    } else if (0 == ::strncmp(pos_, "null", 4)) {
      pos_ += 4;
      value.kind_ = JsonValue::JNULL;
    } else {
      ret = parse_number(value);
      if (0 != ret) {
        err = "invalid JSON value";
      }
    }
    return ret;
  }

  int skip_container(std::string &err)
  {
    int ret = 0;
    int depth = 0;
    bool in_string = false;
    while (pos_ < end_ && 0 == ret) {
      char c = *pos_;
      if (in_string) {
        if ('\\' == c) {
          pos_ += 2;
          continue;
        } else if ('"' == c) {
          in_string = false;
        }
      } else if ('"' == c) {
        in_string = true;
      } else if ('{' == c || '[' == c) {
        ++depth;
      } else if ('}' == c || ']' == c) {
        if (--depth <= 0) {
          ++pos_;
          break;
        }
      }
      ++pos_;
    }
    if (depth > 0) {
      ret = -1;
      err = "unterminated nested JSON structure";
    }
    return ret;
  }

  const char *pos_;
  const char *end_;
};

ValueType json_value_type(const JsonValue &value)
{
  ValueType type = ValueType::VARCHAR;
  switch (value.kind_) {
    case JsonValue::JNULL: type = ValueType::NULL_TYPE; break;
    case JsonValue::JBOOL: type = ValueType::BOOLEAN; break;
    case JsonValue::JINT: type = ValueType::BIGINT; break;
    case JsonValue::JDOUBLE: type = ValueType::DOUBLE; break;
    case JsonValue::JSTR: type = ValueType::VARCHAR; break;
    default: type = ValueType::VARCHAR; break;
  }
  return type;
}

void json_value_to_value(const JsonValue &json, ValueType type, Value &value)
{
  value.set_null();
  if (JsonValue::JNULL == json.kind_) {
    value.set_null();
  } else if (ValueType::BOOLEAN == type) {
    value.set_int(ValueType::BOOLEAN, JsonValue::JBOOL == json.kind_ ? json.int_value_
                                                                    : (json.int_value_ != 0));
  } else if (ValueType::BIGINT == type) {
    if (JsonValue::JINT == json.kind_) {
      value.set_int(ValueType::BIGINT, json.int_value_);
    } else if (JsonValue::JDOUBLE == json.kind_) {
      value.set_int(ValueType::BIGINT, static_cast<int64_t>(json.double_value_));
    } else {
      value.set_str(ValueType::VARCHAR, json.str_value_);
    }
  } else if (ValueType::DOUBLE == type) {
    if (JsonValue::JDOUBLE == json.kind_) {
      value.set_double(json.double_value_);
    } else if (JsonValue::JINT == json.kind_) {
      value.set_double(static_cast<double>(json.int_value_));
    } else {
      value.set_str(ValueType::VARCHAR, json.str_value_);
    }
  } else {
    // VARCHAR fallback: render scalars as text so mixed-type columns stay readable
    if (JsonValue::JSTR == json.kind_) {
      value.set_str(ValueType::VARCHAR, json.str_value_);
    } else if (JsonValue::JBOOL == json.kind_) {
      value.set_str(ValueType::VARCHAR, json.int_value_ ? "true" : "false");
    } else if (JsonValue::JINT == json.kind_) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%ld", static_cast<long>(json.int_value_));
      value.set_str(ValueType::VARCHAR, buf);
    } else {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.17g", json.double_value_);
      value.set_str(ValueType::VARCHAR, buf);
    }
  }
}

}  // namespace

int read_jsonl_file(const std::string &path, bool newline_delimited, Table &table,
                    std::string &err)
{
  int ret = 0;
  std::string content;
  table.reset();
  if (!newline_delimited) {
    ret = -1;
    err = "only newline_delimited JSON is supported";
  } else if (0 != (ret = read_whole_file(path, content, err))) {
    // keep err from read_whole_file
  } else {
    size_t start = 0;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF
        && static_cast<unsigned char>(content[1]) == 0xBB
        && static_cast<unsigned char>(content[2]) == 0xBF) {
      start = 3;
    }
    std::vector<JsonDoc> docs;
    size_t pos = start;
    int64_t line_no = 0;
    while (0 == ret && pos <= content.size()) {
      size_t line_begin = pos;
      size_t line_end = content.find('\n', pos);
      if (std::string::npos == line_end) {
        line_end = content.size();
        pos = content.size() + 1;
      } else {
        pos = line_end + 1;
      }
      ++line_no;
      size_t real_end = line_end;
      if (real_end > line_begin && '\r' == content[real_end - 1]) {
        --real_end;
      }
      // skip blank lines
      size_t first = line_begin;
      while (first < real_end && (' ' == content[first] || '\t' == content[first])) {
        ++first;
      }
      if (first == real_end) {
        continue;
      }
      JsonDoc doc;
      JsonParser parser(content.data() + line_begin, content.data() + real_end);
      std::string parse_err;
      if (0 != parser.parse_object(doc, parse_err)) {
        ret = -1;
        char buf[64];
        snprintf(buf, sizeof(buf), "line %ld: ", static_cast<long>(line_no));
        err = std::string(buf) + parse_err;
      } else {
        docs.push_back(doc);
      }
    }
    if (0 == ret && docs.empty()) {
      ret = -1;
      err = "file '" + path + "' contains no JSON records";
    }
    if (0 == ret) {
      // union schema, in first seen order
      std::vector<std::string> names;
      for (size_t d = 0; d < docs.size(); ++d) {
        for (size_t m = 0; m < docs[d].members_.size(); ++m) {
          const std::string &key = docs[d].members_[m].key_;
          bool found = false;
          for (size_t n = 0; n < names.size() && !found; ++n) {
            found = (names[n] == key);
          }
          if (!found) {
            names.push_back(key);
          }
        }
      }
      table.columns_.resize(names.size());
      for (size_t c = 0; c < names.size(); ++c) {
        table.columns_[c].name_ = names[c];
        table.columns_[c].type_ = ValueType::NULL_TYPE;
        table.columns_[c].nullable_ = false;
      }
      for (size_t d = 0; d < docs.size(); ++d) {
        for (size_t m = 0; m < docs[d].members_.size(); ++m) {
          const std::string &key = docs[d].members_[m].key_;
          ValueType incoming = json_value_type(docs[d].members_[m].value_);
          for (size_t c = 0; c < names.size(); ++c) {
            if (names[c] == key) {
              table.columns_[c].type_ = widen_type(table.columns_[c].type_, incoming);
              break;
            }
          }
        }
      }
      for (size_t c = 0; c < names.size(); ++c) {
        if (ValueType::NULL_TYPE == table.columns_[c].type_) {
          table.columns_[c].type_ = ValueType::VARCHAR;
        }
      }
      for (size_t d = 0; d < docs.size(); ++d) {
        std::vector<Value> row(names.size());
        for (size_t c = 0; c < names.size(); ++c) {
          row[c].set_null();
          for (size_t m = 0; m < docs[d].members_.size(); ++m) {
            if (docs[d].members_[m].key_ == names[c]) {
              json_value_to_value(docs[d].members_[m].value_, table.columns_[c].type_, row[c]);
              break;
            }
          }
        }
        table.rows_.push_back(row);
      }
      for (size_t c = 0; c < names.size(); ++c) {
        bool nullable = false;
        for (size_t r = 0; r < table.rows_.size() && !nullable; ++r) {
          nullable = table.rows_[r][c].is_null();
        }
        table.columns_[c].nullable_ = nullable;
      }
    }
  }
  return ret;
}

}  // namespace filesql
}  // namespace sql
}  // namespace oceanbase
