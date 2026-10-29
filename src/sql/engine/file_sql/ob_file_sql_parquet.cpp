/**
 * Copyright (c) 2021 OceanBase
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal Parquet reader: thrift compact footer metadata, multiple row groups,
 * optional columns (definition levels), PLAIN / RLE_DICTIONARY values and
 * UNCOMPRESSED / SNAPPY / GZIP-free compression (gzip falls back to an error).
 */

#include "sql/engine/file_sql/ob_file_sql_format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace oceanbase
{
namespace sql
{
namespace filesql
{

namespace
{

// ---------------------------------------------------------------- thrift ---

enum ThriftType
{
  TT_STOP = 0,
  TT_TRUE = 1,
  TT_FALSE = 2,
  TT_BYTE = 3,
  TT_I16 = 4,
  TT_I32 = 5,
  TT_I64 = 6,
  TT_DOUBLE = 7,
  TT_BINARY = 8,
  TT_LIST = 9,
  TT_SET = 10,
  TT_MAP = 11,
  TT_STRUCT = 12
};

class CompactReader
{
public:
  CompactReader(const uint8_t *data, size_t size) : data_(data), size_(size), pos_(0) {}

  bool eof() const { return pos_ >= size_; }
  size_t pos() const { return pos_; }
  void seek(size_t pos) { pos_ = pos; }

  bool read_byte(uint8_t &value)
  {
    bool ok = pos_ < size_;
    if (ok) {
      value = data_[pos_++];
    }
    return ok;
  }

  bool read_varint(uint64_t &value)
  {
    uint64_t result = 0;
    int shift = 0;
    bool ok = false;
    while (pos_ < size_ && shift <= 63) {
      uint8_t byte = data_[pos_++];
      result |= (static_cast<uint64_t>(byte & 0x7F) << shift);
      if (0 == (byte & 0x80)) {
        ok = true;
        break;
      }
      shift += 7;
    }
    if (ok) {
      value = result;
    }
    return ok;
  }

  bool read_zigzag(int64_t &value)
  {
    uint64_t raw = 0;
    bool ok = read_varint(raw);
    if (ok) {
      value = static_cast<int64_t>(raw >> 1) ^ -static_cast<int64_t>(raw & 1);
    }
    return ok;
  }

  bool read_i32(int32_t &value)
  {
    int64_t tmp = 0;
    bool ok = read_zigzag(tmp);
    if (ok) {
      value = static_cast<int32_t>(tmp);
    }
    return ok;
  }

  bool read_i64(int64_t &value) { return read_zigzag(value); }

  bool read_double(double &value)
  {
    bool ok = pos_ + 8 <= size_;
    if (ok) {
      uint64_t bits = 0;
      for (int i = 7; i >= 0; --i) {
        bits = (bits << 8) | data_[pos_ + i];
      }
      ::memcpy(&value, &bits, sizeof(value));
      pos_ += 8;
    }
    return ok;
  }

  bool read_binary(std::string &value)
  {
    uint64_t len = 0;
    bool ok = read_varint(len) && pos_ + len <= size_;
    if (ok) {
      value.assign(reinterpret_cast<const char *>(data_ + pos_), static_cast<size_t>(len));
      pos_ += static_cast<size_t>(len);
    }
    return ok;
  }

  // Reads a field header. Returns false when the struct is finished (STOP).
  bool read_field(uint8_t &type, int16_t &id, bool &stop)
  {
    stop = false;
    uint8_t header = 0;
    bool ok = read_byte(header);
    if (ok) {
      if (0 == header) {
        stop = true;
      } else {
        type = header & 0x0F;
        int16_t delta = static_cast<int16_t>((header & 0xF0) >> 4);
        if (0 == delta) {
          int64_t explicit_id = 0;
          ok = read_zigzag(explicit_id);
          id = static_cast<int16_t>(explicit_id);
        } else {
          id = static_cast<int16_t>(last_id_ + delta);
        }
        if (ok) {
          last_id_ = id;
        }
      }
    }
    return ok;
  }

  void push_id() { id_stack_[depth_++] = last_id_; last_id_ = 0; }
  void pop_id() { last_id_ = id_stack_[--depth_]; }

  bool read_list(uint8_t &elem_type, uint32_t &count)
  {
    uint8_t header = 0;
    bool ok = read_byte(header);
    if (ok) {
      elem_type = header & 0x0F;
      uint32_t size = (header & 0xF0) >> 4;
      if (15 == size) {
        uint64_t extended = 0;
        ok = read_varint(extended);
        size = static_cast<uint32_t>(extended);
      }
      count = size;
    }
    return ok;
  }

  bool skip(uint8_t type)
  {
    bool ok = true;
    switch (type) {
      case TT_TRUE:
      case TT_FALSE: {
        // inside a struct a bool carries no payload
        break;
      }
      case TT_BYTE: {
        uint8_t ignored = 0;
        ok = read_byte(ignored);
        break;
      }
      case TT_I16:
      case TT_I32:
      case TT_I64: {
        int64_t ignored = 0;
        ok = read_zigzag(ignored);
        break;
      }
      case TT_DOUBLE: {
        double ignored = 0;
        ok = read_double(ignored);
        break;
      }
      case TT_BINARY: {
        std::string ignored;
        ok = read_binary(ignored);
        break;
      }
      case TT_LIST:
      case TT_SET: {
        uint8_t elem_type = 0;
        uint32_t count = 0;
        ok = read_list(elem_type, count);
        for (uint32_t i = 0; ok && i < count; ++i) {
          ok = skip_element(elem_type);
        }
        break;
      }
      case TT_MAP: {
        uint8_t header = 0;
        ok = read_byte(header);
        if (ok && header != 0) {
          uint32_t count = 0;
          uint8_t key_type = header & 0x0F;
          uint8_t value_type = (header & 0xF0) >> 4;
          uint64_t extended = 0;
          ok = read_varint(extended);
          count = static_cast<uint32_t>(extended);
          for (uint32_t i = 0; ok && i < count; ++i) {
            ok = skip_element(key_type) && skip_element(value_type);
          }
        }
        break;
      }
      case TT_STRUCT: {
        push_id();
        bool stop = false;
        while (ok && !stop) {
          uint8_t field_type = 0;
          int16_t field_id = 0;
          ok = read_field(field_type, field_id, stop);
          if (ok && !stop) {
            ok = skip(field_type);
          }
        }
        pop_id();
        break;
      }
      default:
        ok = false;
        break;
    }
    return ok;
  }

private:
  bool skip_element(uint8_t type)
  {
    bool ok = true;
    if (TT_TRUE == type || TT_FALSE == type) {
      uint8_t ignored = 0;
      ok = read_byte(ignored);
    } else {
      ok = skip(type);
    }
    return ok;
  }

  const uint8_t *data_;
  size_t size_;
  size_t pos_;
  int16_t last_id_ = 0;
  int16_t id_stack_[32];
  int depth_ = 0;
};

// ------------------------------------------------------------- metadata ----

enum ParquetType
{
  PQ_BOOLEAN = 0,
  PQ_INT32 = 1,
  PQ_INT64 = 2,
  PQ_INT96 = 3,
  PQ_FLOAT = 4,
  PQ_DOUBLE = 5,
  PQ_BYTE_ARRAY = 6,
  PQ_FIXED_LEN_BYTE_ARRAY = 7
};

enum ParquetCodec
{
  PQ_UNCOMPRESSED = 0,
  PQ_SNAPPY = 1,
  PQ_GZIP = 2,
  PQ_LZO = 3,
  PQ_BROTLI = 4,
  PQ_LZ4 = 5,
  PQ_ZSTD = 6,
  PQ_LZ4_RAW = 7
};

enum ParquetEncoding
{
  PQ_PLAIN = 0,
  PQ_GROUP_VAR_INT = 1,
  PQ_PLAIN_DICTIONARY = 2,
  PQ_RLE = 3,
  PQ_BIT_PACKED = 4,
  PQ_DELTA_BINARY_PACKED = 5,
  PQ_DELTA_LENGTH_BYTE_ARRAY = 6,
  PQ_DELTA_BYTE_ARRAY = 7,
  PQ_RLE_DICTIONARY = 8,
  PQ_BYTE_STREAM_SPLIT = 9
};

enum PageType
{
  PAGE_DATA = 0,
  PAGE_INDEX = 1,
  PAGE_DICTIONARY = 2,
  PAGE_DATA_V2 = 3
};

struct SchemaElement
{
  int32_t type_ = -1;
  int32_t type_length_ = 0;
  int32_t repetition_ = 0;
  std::string name_;
  int32_t num_children_ = 0;
  int32_t converted_type_ = -1;
  int32_t max_def_level_ = 0;
  int32_t max_rep_level_ = 0;
};

struct ColumnChunkMeta
{
  int32_t type_ = -1;
  int32_t codec_ = PQ_UNCOMPRESSED;
  int64_t num_values_ = 0;
  int64_t total_compressed_size_ = 0;
  int64_t data_page_offset_ = -1;
  int64_t dictionary_page_offset_ = -1;
  int32_t max_def_level_ = 0;
  int32_t max_rep_level_ = 0;
  std::string name_;
};

struct RowGroupMeta
{
  int64_t num_rows_ = 0;
  std::vector<ColumnChunkMeta> columns_;
};

bool parse_schema_element(CompactReader &reader, SchemaElement &element)
{
  bool ok = true;
  bool stop = false;
  reader.push_id();
  while (ok && !stop) {
    uint8_t type = 0;
    int16_t id = 0;
    ok = reader.read_field(type, id, stop);
    if (!ok || stop) {
      break;
    }
    switch (id) {
      case 1: { int32_t v = 0; ok = reader.read_i32(v); element.type_ = v; break; }
      case 2: { int32_t v = 0; ok = reader.read_i32(v); element.type_length_ = v; break; }
      case 3: { int32_t v = 0; ok = reader.read_i32(v); element.repetition_ = v; break; }
      case 4: ok = reader.read_binary(element.name_); break;
      case 5: { int32_t v = 0; ok = reader.read_i32(v); element.num_children_ = v; break; }
      case 6: { int32_t v = 0; ok = reader.read_i32(v); element.converted_type_ = v; break; }
      default: ok = reader.skip(type); break;
    }
  }
  reader.pop_id();
  return ok;
}

bool parse_column_meta(CompactReader &reader, ColumnChunkMeta &meta)
{
  bool ok = true;
  bool stop = false;
  reader.push_id();
  while (ok && !stop) {
    uint8_t type = 0;
    int16_t id = 0;
    ok = reader.read_field(type, id, stop);
    if (!ok || stop) {
      break;
    }
    switch (id) {
      case 1: { int32_t v = 0; ok = reader.read_i32(v); meta.type_ = v; break; }
      case 3: {  // path_in_schema
        uint8_t elem_type = 0;
        uint32_t count = 0;
        ok = reader.read_list(elem_type, count);
        for (uint32_t i = 0; ok && i < count; ++i) {
          std::string part;
          ok = reader.read_binary(part);
          if (ok) {
            if (!meta.name_.empty()) {
              meta.name_ += ".";
            }
            meta.name_ += part;
          }
        }
        break;
      }
      case 4: { int32_t v = 0; ok = reader.read_i32(v); meta.codec_ = v; break; }
      case 5: ok = reader.read_i64(meta.num_values_); break;
      case 7: ok = reader.read_i64(meta.total_compressed_size_); break;
      case 9: ok = reader.read_i64(meta.data_page_offset_); break;
      case 11: ok = reader.read_i64(meta.dictionary_page_offset_); break;
      default: ok = reader.skip(type); break;
    }
  }
  reader.pop_id();
  return ok;
}

bool parse_column_chunk(CompactReader &reader, ColumnChunkMeta &meta)
{
  bool ok = true;
  bool stop = false;
  reader.push_id();
  while (ok && !stop) {
    uint8_t type = 0;
    int16_t id = 0;
    ok = reader.read_field(type, id, stop);
    if (!ok || stop) {
      break;
    }
    if (3 == id) {
      ok = parse_column_meta(reader, meta);
    } else {
      ok = reader.skip(type);
    }
  }
  reader.pop_id();
  return ok;
}

bool parse_row_group(CompactReader &reader, RowGroupMeta &group)
{
  bool ok = true;
  bool stop = false;
  reader.push_id();
  while (ok && !stop) {
    uint8_t type = 0;
    int16_t id = 0;
    ok = reader.read_field(type, id, stop);
    if (!ok || stop) {
      break;
    }
    if (1 == id) {
      uint8_t elem_type = 0;
      uint32_t count = 0;
      ok = reader.read_list(elem_type, count);
      for (uint32_t i = 0; ok && i < count; ++i) {
        ColumnChunkMeta meta;
        ok = parse_column_chunk(reader, meta);
        if (ok) {
          group.columns_.push_back(meta);
        }
      }
    } else if (3 == id) {
      ok = reader.read_i64(group.num_rows_);
    } else {
      ok = reader.skip(type);
    }
  }
  reader.pop_id();
  return ok;
}

struct PageHeader
{
  int32_t type_ = -1;
  int32_t uncompressed_size_ = 0;
  int32_t compressed_size_ = 0;
  int32_t num_values_ = 0;
  int32_t encoding_ = -1;
  int32_t def_level_encoding_ = -1;
  int32_t rep_level_encoding_ = -1;
  int32_t dict_num_values_ = 0;
  int32_t v2_num_values_ = 0;
  int32_t v2_num_nulls_ = 0;
  int32_t v2_num_rows_ = 0;
  int32_t v2_encoding_ = -1;
  int32_t v2_def_len_ = 0;
  int32_t v2_rep_len_ = 0;
  bool v2_is_compressed_ = true;
};

bool parse_data_page_header(CompactReader &reader, PageHeader &header, bool v2)
{
  bool ok = true;
  bool stop = false;
  reader.push_id();
  while (ok && !stop) {
    uint8_t type = 0;
    int16_t id = 0;
    ok = reader.read_field(type, id, stop);
    if (!ok || stop) {
      break;
    }
    if (!v2) {
      switch (id) {
        case 1: { int32_t v = 0; ok = reader.read_i32(v); header.num_values_ = v; break; }
        case 2: { int32_t v = 0; ok = reader.read_i32(v); header.encoding_ = v; break; }
        case 3: { int32_t v = 0; ok = reader.read_i32(v); header.def_level_encoding_ = v; break; }
        case 4: { int32_t v = 0; ok = reader.read_i32(v); header.rep_level_encoding_ = v; break; }
        default: ok = reader.skip(type); break;
      }
    } else {
      switch (id) {
        case 1: { int32_t v = 0; ok = reader.read_i32(v); header.v2_num_values_ = v; break; }
        case 2: { int32_t v = 0; ok = reader.read_i32(v); header.v2_num_nulls_ = v; break; }
        case 3: { int32_t v = 0; ok = reader.read_i32(v); header.v2_num_rows_ = v; break; }
        case 4: { int32_t v = 0; ok = reader.read_i32(v); header.v2_encoding_ = v; break; }
        case 5: { int32_t v = 0; ok = reader.read_i32(v); header.v2_def_len_ = v; break; }
        case 6: { int32_t v = 0; ok = reader.read_i32(v); header.v2_rep_len_ = v; break; }
        case 7: {
          // boolean fields carry their value in the compact protocol type
          header.v2_is_compressed_ = (TT_TRUE == type);
          break;
        }
        default: ok = reader.skip(type); break;
      }
    }
  }
  reader.pop_id();
  return ok;
}

bool parse_dictionary_header(CompactReader &reader, PageHeader &header)
{
  bool ok = true;
  bool stop = false;
  reader.push_id();
  while (ok && !stop) {
    uint8_t type = 0;
    int16_t id = 0;
    ok = reader.read_field(type, id, stop);
    if (!ok || stop) {
      break;
    }
    if (1 == id) {
      int32_t v = 0;
      ok = reader.read_i32(v);
      header.dict_num_values_ = v;
    } else {
      ok = reader.skip(type);
    }
  }
  reader.pop_id();
  return ok;
}

bool parse_page_header(CompactReader &reader, PageHeader &header)
{
  bool ok = true;
  bool stop = false;
  reader.push_id();
  while (ok && !stop) {
    uint8_t type = 0;
    int16_t id = 0;
    ok = reader.read_field(type, id, stop);
    if (!ok || stop) {
      break;
    }
    switch (id) {
      case 1: { int32_t v = 0; ok = reader.read_i32(v); header.type_ = v; break; }
      case 2: { int32_t v = 0; ok = reader.read_i32(v); header.uncompressed_size_ = v; break; }
      case 3: { int32_t v = 0; ok = reader.read_i32(v); header.compressed_size_ = v; break; }
      case 5: ok = parse_data_page_header(reader, header, false); break;
      case 7: ok = parse_dictionary_header(reader, header); break;
      case 8: ok = parse_data_page_header(reader, header, true); break;
      default: ok = reader.skip(type); break;
    }
  }
  reader.pop_id();
  return ok;
}

// ------------------------------------------------------------ decoders -----

// Snappy raw block decompression (used by SNAPPY compressed parquet pages).
bool snappy_decompress(const uint8_t *input, size_t input_size, std::string &output)
{
  bool ok = true;
  output.clear();
  size_t pos = 0;
  uint64_t expected = 0;
  int shift = 0;
  while (pos < input_size && shift <= 63) {
    uint8_t byte = input[pos++];
    expected |= (static_cast<uint64_t>(byte & 0x7F) << shift);
    if (0 == (byte & 0x80)) {
      break;
    }
    shift += 7;
  }
  output.reserve(static_cast<size_t>(expected));
  while (ok && pos < input_size) {
    uint8_t tag = input[pos++];
    uint32_t kind = tag & 0x03;
    if (0 == kind) {
      uint32_t len = tag >> 2;
      if (len < 60) {
        len += 1;
      } else {
        uint32_t extra = len - 59;
        len = 0;
        if (pos + extra > input_size) {
          ok = false;
        } else {
          for (uint32_t i = 0; i < extra; ++i) {
            len |= (static_cast<uint32_t>(input[pos + i]) << (8 * i));
          }
          pos += extra;
          len += 1;
        }
      }
      if (ok && pos + len > input_size) {
        ok = false;
      }
      if (ok) {
        output.append(reinterpret_cast<const char *>(input + pos), len);
        pos += len;
      }
    } else {
      uint32_t len = 0;
      uint32_t offset = 0;
      if (1 == kind) {
        if (pos >= input_size) {
          ok = false;
        } else {
          len = ((tag >> 2) & 0x07) + 4;
          offset = ((static_cast<uint32_t>(tag) & 0xE0) << 3) | input[pos];
          ++pos;
        }
      } else if (2 == kind) {
        if (pos + 2 > input_size) {
          ok = false;
        } else {
          len = (tag >> 2) + 1;
          offset = static_cast<uint32_t>(input[pos]) | (static_cast<uint32_t>(input[pos + 1]) << 8);
          pos += 2;
        }
      } else {
        if (pos + 4 > input_size) {
          ok = false;
        } else {
          len = (tag >> 2) + 1;
          offset = static_cast<uint32_t>(input[pos]) | (static_cast<uint32_t>(input[pos + 1]) << 8)
                   | (static_cast<uint32_t>(input[pos + 2]) << 16)
                   | (static_cast<uint32_t>(input[pos + 3]) << 24);
          pos += 4;
        }
      }
      if (ok) {
        if (0 == offset || offset > output.size() || output.size() + len > expected) {
          ok = false;
        } else {
          size_t from = output.size() - offset;
          for (uint32_t i = 0; i < len; ++i) {
            output.push_back(output[from + i]);
          }
        }
      }
    }
  }
  if (ok && output.size() != expected) {
    ok = false;
  }
  return ok;
}

uint32_t read_le32(const uint8_t *p)
{
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_le64(const uint8_t *p)
{
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | p[i];
  }
  return value;
}

int bit_width_for(uint32_t max_value)
{
  int width = 0;
  while (max_value > 0) {
    ++width;
    max_value >>= 1;
  }
  return width;
}

// Decodes a RLE / bit-packed hybrid run of `count` values.
bool decode_rle_bitpacked(const uint8_t *data, size_t size, int bit_width, size_t count,
                          std::vector<uint32_t> &out)
{
  bool ok = true;
  out.clear();
  out.reserve(count);
  size_t pos = 0;
  while (ok && out.size() < count && pos < size) {
    uint64_t header = 0;
    int shift = 0;
    while (pos < size && shift <= 63) {
      uint8_t byte = data[pos++];
      header |= (static_cast<uint64_t>(byte & 0x7F) << shift);
      if (0 == (byte & 0x80)) {
        break;
      }
      shift += 7;
    }
    if (header & 1) {
      size_t groups = static_cast<size_t>(header >> 1);
      size_t values = groups * 8;
      size_t bytes = (values * static_cast<size_t>(bit_width) + 7) / 8;
      if (pos + bytes > size) {
        ok = false;
      } else if (bit_width > 0) {
        uint64_t buffer = 0;
        int bits = 0;
        size_t consumed = 0;
        for (size_t i = 0; i < values && out.size() < count; ++i) {
          while (bits < bit_width && consumed < bytes) {
            buffer |= (static_cast<uint64_t>(data[pos + consumed]) << bits);
            bits += 8;
            ++consumed;
          }
          uint32_t value = static_cast<uint32_t>(buffer & ((1ULL << bit_width) - 1));
          buffer >>= bit_width;
          bits -= bit_width;
          out.push_back(value);
        }
      } else {
        for (size_t i = 0; i < values && out.size() < count; ++i) {
          out.push_back(0);
        }
      }
      pos += bytes;
    } else {
      size_t run = static_cast<size_t>(header >> 1);
      size_t bytes = (static_cast<size_t>(bit_width) + 7) / 8;
      if (pos + bytes > size) {
        ok = false;
      } else {
        uint32_t value = 0;
        for (size_t i = 0; i < bytes; ++i) {
          value |= (static_cast<uint32_t>(data[pos + i]) << (8 * i));
        }
        pos += bytes;
        for (size_t i = 0; i < run && out.size() < count; ++i) {
          out.push_back(value);
        }
      }
    }
  }
  return ok;
}

// Reader over the uncompressed payload of a page.
class PageCursor
{
public:
  PageCursor(const std::string &payload) : data_(payload.data()), size_(payload.size()), pos_(0) {}
  PageCursor(const uint8_t *data, size_t size) : data_(reinterpret_cast<const char *>(data)),
                                                 size_(size), pos_(0) {}
  size_t remain() const { return size_ - pos_; }
  const char *ptr() const { return data_ + pos_; }
  void advance(size_t n) { pos_ += n; }

private:
  const char *data_;
  size_t size_;
  size_t pos_;
};

bool decode_levels(const uint8_t *data, size_t size, int max_level, size_t count,
                   std::vector<uint32_t> &levels, size_t &consumed)
{
  bool ok = true;
  levels.clear();
  consumed = 0;
  if (max_level <= 0) {
    levels.assign(count, 0);
  } else if (size < 4) {
    ok = false;
  } else {
    uint32_t encoded_size = read_le32(data);
    if (encoded_size > size - 4) {
      ok = false;
    } else {
      ok = decode_rle_bitpacked(data + 4, encoded_size, bit_width_for(static_cast<uint32_t>(max_level)),
                                count, levels);
      consumed = 4 + encoded_size;
      if (ok) {
        // short level streams are legal: pad with the maximum level
        while (levels.size() < count) {
          levels.push_back(static_cast<uint32_t>(max_level));
        }
      }
    }
  }
  return ok;
}

void plain_decode_values(const uint8_t *data, size_t size, int32_t type, int32_t type_length,
                         size_t count, std::vector<Value> &values)
{
  values.clear();
  values.reserve(count);
  size_t pos = 0;
  if (PQ_BOOLEAN == type) {
    for (size_t i = 0; i < count; ++i) {
      Value value;
      size_t byte_index = i / 8;
      if (byte_index < size) {
        bool bit = (data[byte_index] >> (i % 8)) & 1;
        value.set_int(ValueType::BOOLEAN, bit ? 1 : 0);
      } else {
        value.set_null();
      }
      values.push_back(value);
    }
  } else if (PQ_INT32 == type || PQ_INT64 == type) {
    size_t width = (PQ_INT32 == type) ? 4 : 8;
    for (size_t i = 0; i < count && pos + width <= size; ++i, pos += width) {
      Value value;
      if (4 == width) {
        value.set_int(ValueType::BIGINT, static_cast<int32_t>(read_le32(data + pos)));
      } else {
        value.set_int(ValueType::BIGINT, static_cast<int64_t>(read_le64(data + pos)));
      }
      values.push_back(value);
    }
  } else if (PQ_FLOAT == type) {
    for (size_t i = 0; i < count && pos + 4 <= size; ++i, pos += 4) {
      uint32_t bits = read_le32(data + pos);
      float f = 0;
      ::memcpy(&f, &bits, sizeof(f));
      Value value;
      value.set_double(static_cast<double>(f));
      values.push_back(value);
    }
  } else if (PQ_DOUBLE == type) {
    for (size_t i = 0; i < count && pos + 8 <= size; ++i, pos += 8) {
      uint64_t bits = read_le64(data + pos);
      double d = 0;
      ::memcpy(&d, &bits, sizeof(d));
      Value value;
      value.set_double(d);
      values.push_back(value);
    }
  } else if (PQ_BYTE_ARRAY == type) {
    for (size_t i = 0; i < count && pos + 4 <= size; ++i) {
      uint32_t len = read_le32(data + pos);
      pos += 4;
      if (pos + len > size) {
        break;
      }
      Value value;
      value.set_str(ValueType::VARCHAR, std::string(reinterpret_cast<const char *>(data + pos), len));
      values.push_back(value);
      pos += len;
    }
  } else if (PQ_FIXED_LEN_BYTE_ARRAY == type) {
    size_t width = static_cast<size_t>(type_length > 0 ? type_length : 0);
    for (size_t i = 0; i < count && width > 0 && pos + width <= size; ++i, pos += width) {
      Value value;
      value.set_str(ValueType::VARCHAR,
                    std::string(reinterpret_cast<const char *>(data + pos), width));
      values.push_back(value);
    }
  }
}

ValueType map_parquet_type(int32_t type, int32_t converted_type)
{
  ValueType result = ValueType::VARCHAR;
  if (PQ_BOOLEAN == type) {
    result = ValueType::BOOLEAN;
  } else if (PQ_INT32 == type || PQ_INT64 == type) {
    result = ValueType::BIGINT;
  } else if (PQ_FLOAT == type || PQ_DOUBLE == type) {
    result = ValueType::DOUBLE;
  } else if (6 == converted_type) {  // DATE
    result = ValueType::DATE;
  } else if (9 == converted_type || 10 == converted_type) {  // TIMESTAMP_*
    result = ValueType::DATETIME;
  } else {
    result = ValueType::VARCHAR;
  }
  return result;
}

std::string format_date(int32_t days_since_epoch)
{
  // civil_from_days (Howard Hinnant)
  int64_t z = static_cast<int64_t>(days_since_epoch) + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int64_t d = doy - (153 * mp + 2) / 5 + 1;
  int64_t m = mp + (mp < 10 ? 3 : -9);
  int64_t year = y + (m <= 2 ? 1 : 0);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04ld-%02ld-%02ld", static_cast<long>(year), static_cast<long>(m),
           static_cast<long>(d));
  return std::string(buf);
}

std::string format_timestamp_millis(int64_t millis)
{
  int64_t seconds = millis / 1000;
  int64_t ms = millis % 1000;
  if (ms < 0) {
    ms += 1000;
    --seconds;
  }
  int64_t days = seconds / 86400;
  int64_t rest = seconds % 86400;
  if (rest < 0) {
    rest += 86400;
    --days;
  }
  std::string date = format_date(static_cast<int32_t>(days));
  char buf[64];
  snprintf(buf, sizeof(buf), "%s %02ld:%02ld:%02ld", date.c_str(),
           static_cast<long>(rest / 3600), static_cast<long>((rest % 3600) / 60),
           static_cast<long>(rest % 60));
  return std::string(buf);
}

}  // namespace

int read_parquet_file(const std::string &path, Table &table, std::string &err)
{
  int ret = 0;
  std::string content;
  table.reset();
  if (0 != (ret = read_whole_file(path, content, err))) {
    // err already set
  } else if (content.size() < 12 || 0 != ::memcmp(content.data(), "PAR1", 4)
             || 0 != ::memcmp(content.data() + content.size() - 4, "PAR1", 4)) {
    ret = -1;
    err = "'" + path + "' is not a parquet file (missing PAR1 magic)";
  } else {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(content.data());
    uint32_t footer_size = read_le32(bytes + content.size() - 8);
    if (static_cast<size_t>(footer_size) + 8 > content.size()) {
      ret = -1;
      err = "corrupted parquet footer in '" + path + "'";
    } else {
      size_t footer_begin = content.size() - 8 - footer_size;
      CompactReader reader(bytes + footer_begin, footer_size);
      std::vector<SchemaElement> schema;
      std::vector<RowGroupMeta> row_groups;
      int64_t num_rows = 0;
      bool ok = true;
      bool stop = false;
      reader.push_id();
      while (ok && !stop) {
        uint8_t type = 0;
        int16_t id = 0;
        ok = reader.read_field(type, id, stop);
        if (!ok || stop) {
          break;
        }
        if (2 == id) {
          uint8_t elem_type = 0;
          uint32_t count = 0;
          ok = reader.read_list(elem_type, count);
          for (uint32_t i = 0; ok && i < count; ++i) {
            SchemaElement element;
            ok = parse_schema_element(reader, element);
            if (ok) {
              schema.push_back(element);
            }
          }
        } else if (3 == id) {
          ok = reader.read_i64(num_rows);
        } else if (4 == id) {
          uint8_t elem_type = 0;
          uint32_t count = 0;
          ok = reader.read_list(elem_type, count);
          for (uint32_t i = 0; ok && i < count; ++i) {
            RowGroupMeta group;
            ok = parse_row_group(reader, group);
            if (ok) {
              row_groups.push_back(group);
            }
          }
        } else {
          ok = reader.skip(type);
        }
      }
      reader.pop_id();
      if (NULL != ::getenv("FS_PQ_DEBUG")) {
        fprintf(stderr, "[pq] rows=%ld schema=%ld row_groups=%ld\n", static_cast<long>(num_rows),
                static_cast<long>(schema.size()), static_cast<long>(row_groups.size()));
        for (size_t i = 0; i < schema.size(); ++i) {
          fprintf(stderr, "[pq] schema[%ld] name=%s type=%d rep=%d children=%d conv=%d\n",
                  static_cast<long>(i), schema[i].name_.c_str(), schema[i].type_,
                  schema[i].repetition_, schema[i].num_children_, schema[i].converted_type_);
        }
        for (size_t g = 0; g < row_groups.size(); ++g) {
          fprintf(stderr, "[pq] rg[%ld] rows=%ld cols=%ld\n", static_cast<long>(g),
                  static_cast<long>(row_groups[g].num_rows_),
                  static_cast<long>(row_groups[g].columns_.size()));
          for (size_t c = 0; c < row_groups[g].columns_.size(); ++c) {
            const ColumnChunkMeta &m = row_groups[g].columns_[c];
            fprintf(stderr, "[pq]   col[%ld] name=%s type=%d codec=%d nvals=%ld csize=%ld "
                    "data_off=%ld dict_off=%ld\n", static_cast<long>(c), m.name_.c_str(),
                    m.type_, m.codec_, static_cast<long>(m.num_values_),
                    static_cast<long>(m.total_compressed_size_),
                    static_cast<long>(m.data_page_offset_),
                    static_cast<long>(m.dictionary_page_offset_));
          }
        }
      }
      if (!ok) {
        ret = -1;
        err = "failed to parse parquet metadata of '" + path + "'";
      } else if (schema.empty()) {        ret = -1;
        err = "parquet file '" + path + "' has an empty schema";
      } else {
        // Flatten the schema tree: the first element is the root group.
        // Each leaf keeps its own definition / repetition level depth.
        struct Leaf
        {
          int32_t index_;
          int32_t def_level_;
          int32_t rep_level_;
        };
        std::vector<Leaf> leaves;
        int32_t index = 0;
        // recursive descent over the flat schema list
        struct Walker
        {
          static void walk(const std::vector<SchemaElement> &schema, int32_t &index,
                           int32_t def_level, int32_t rep_level, std::vector<Leaf> &leaves)
          {
            if (index >= static_cast<int32_t>(schema.size())) {
              return;
            }
            int32_t self = index;
            const SchemaElement &element = schema[self];
            int32_t next_def = def_level + (1 == element.repetition_ || 2 == element.repetition_ ? 1 : 0);
            int32_t next_rep = rep_level + (2 == element.repetition_ ? 1 : 0);
            ++index;
            if (element.num_children_ > 0) {
              for (int32_t i = 0; i < element.num_children_; ++i) {
                walk(schema, index, next_def, next_rep, leaves);
              }
            } else {
              Leaf leaf;
              leaf.index_ = self;
              leaf.def_level_ = next_def;
              leaf.rep_level_ = next_rep;
              leaves.push_back(leaf);
            }
          }
        };
        Walker::walk(schema, index, 0, 0, leaves);

        table.columns_.resize(leaves.size());
        for (size_t i = 0; i < leaves.size(); ++i) {
          const SchemaElement &element = schema[leaves[i].index_];
          table.columns_[i].name_ = element.name_;
          table.columns_[i].type_ = map_parquet_type(element.type_, element.converted_type_);
          table.columns_[i].nullable_ = (1 == element.repetition_);
        }

        // Decode every column chunk of every row group.
        for (size_t g = 0; ok && g < row_groups.size(); ++g) {
          const RowGroupMeta &group = row_groups[g];
          std::vector<std::vector<Value> > column_values(leaves.size());
          for (size_t c = 0; ok && c < leaves.size() && c < group.columns_.size(); ++c) {
            const ColumnChunkMeta &meta = group.columns_[c];
            const SchemaElement &element = schema[leaves[c].index_];
            int32_t max_def = leaves[c].def_level_;
            if (leaves[c].rep_level_ > 0) {
              ret = -1;
              err = "repeated (nested) parquet columns are not supported";
              ok = false;
              break;
            }
            size_t page_pos = static_cast<size_t>(meta.dictionary_page_offset_ >= 0
                                                      ? meta.dictionary_page_offset_
                                                      : meta.data_page_offset_);
            if (page_pos <= 0 || page_pos >= content.size()) {
              ret = -1;
              err = "invalid column page offset in '" + path + "'";
              ok = false;
              break;
            }
            std::vector<Value> dictionary;
            size_t value_pos = 0;
            while (ok && value_pos < static_cast<size_t>(meta.num_values_)
                   && page_pos < content.size()) {
              CompactReader page_reader(bytes + page_pos, content.size() - page_pos);
              PageHeader header;
              ok = parse_page_header(page_reader, header);
              size_t header_size = page_reader.pos();
              size_t page_size = static_cast<size_t>(header.compressed_size_ < 0
                                                         ? 0 : header.compressed_size_);
              if (!ok || header.compressed_size_ < 0
                  || page_pos + header_size + page_size > content.size()) {
                ok = false;
                break;
              }
              const uint8_t *page_data = bytes + page_pos + header_size;

              // Data page v1 compresses the whole body (levels included), while
              // v2 keeps the level sections raw and only compresses the values.
              const uint8_t *levels_ptr = page_data;
              size_t rep_len = 0;
              size_t def_len = page_size;
              const uint8_t *values_ptr = page_data;
              size_t values_size = page_size;
              std::string inflated;
              if (PAGE_DATA_V2 == header.type_) {
                rep_len = static_cast<size_t>(header.v2_rep_len_);
                def_len = static_cast<size_t>(header.v2_def_len_);
                size_t raw_len = rep_len + def_len;
                if (raw_len > page_size) {
                  ok = false;
                  break;
                }
                values_ptr = page_data + raw_len;
                values_size = page_size - raw_len;
                if (PQ_UNCOMPRESSED != meta.codec_ && header.v2_is_compressed_ && values_size > 0) {
                  size_t expected = static_cast<size_t>(header.uncompressed_size_) - raw_len;
                  if (PQ_SNAPPY != meta.codec_
                      || !snappy_decompress(values_ptr, values_size, inflated)
                      || inflated.size() != expected) {
                    ret = -1;
                    err = "failed to decompress snappy page in '" + path + "'";
                    ok = false;
                    break;
                  }
                  values_ptr = reinterpret_cast<const uint8_t *>(inflated.data());
                  values_size = inflated.size();
                }
              } else if (PQ_UNCOMPRESSED != meta.codec_ && page_size > 0) {
                if (PQ_SNAPPY != meta.codec_
                    || !snappy_decompress(page_data, page_size, inflated)
                    || inflated.size() != static_cast<size_t>(header.uncompressed_size_)) {
                  ret = -1;
                  err = "failed to decompress snappy page in '" + path + "'";
                  ok = false;
                  break;
                }
                levels_ptr = values_ptr = reinterpret_cast<const uint8_t *>(inflated.data());
                def_len = values_size = inflated.size();
              }

              if (PAGE_DICTIONARY == header.type_) {
                plain_decode_values(values_ptr, values_size, meta.type_, element.type_length_,
                                    static_cast<size_t>(header.dict_num_values_), dictionary);
              } else if (PAGE_DATA == header.type_ || PAGE_DATA_V2 == header.type_) {
                size_t num_values = (PAGE_DATA == header.type_)
                                        ? static_cast<size_t>(header.num_values_)
                                        : static_cast<size_t>(header.v2_num_values_);
                int32_t encoding = (PAGE_DATA == header.type_) ? header.encoding_ : header.v2_encoding_;
                std::vector<uint32_t> def_levels;
                if (page_size > 0 && max_def > 0) {
                  if (PAGE_DATA == header.type_) {
                    // v1: 4 byte length prefix followed by a RLE/bit-packed run
                    size_t consumed = 0;
                    if (!decode_levels(levels_ptr, def_len, max_def, num_values, def_levels,
                                       consumed)) {
                      ret = -1;
                      err = "failed to decode parquet definition levels";
                      ok = false;
                      break;
                    }
                    values_ptr += consumed;
                    values_size = (consumed < values_size) ? (values_size - consumed) : 0;
                  } else if (!decode_rle_bitpacked(levels_ptr + rep_len, def_len,
                                                   bit_width_for(static_cast<uint32_t>(max_def)),
                                                   num_values, def_levels)) {
                    ret = -1;
                    err = "failed to decode parquet definition levels";
                    ok = false;
                    break;
                  }
                  if (def_levels.size() < num_values) {
                    def_levels.resize(num_values, static_cast<uint32_t>(max_def));
                  }
                } else {
                  def_levels.assign(num_values, 0);
                }
                size_t present = 0;
                for (size_t i = 0; i < def_levels.size(); ++i) {
                  if (static_cast<int32_t>(def_levels[i]) >= max_def) {
                    ++present;
                  }
                }
                std::vector<Value> decoded;
                if (PQ_PLAIN_DICTIONARY == encoding || PQ_RLE_DICTIONARY == encoding) {
                  if (values_size < 1) {
                    ok = false;
                    break;
                  }
                  int bit_width = values_ptr[0];
                  std::vector<uint32_t> indices;
                  ok = decode_rle_bitpacked(values_ptr + 1, values_size - 1, bit_width,
                                            present, indices);
                  if (ok) {
                    for (size_t i = 0; i < indices.size(); ++i) {
                      if (indices[i] < dictionary.size()) {
                        decoded.push_back(dictionary[indices[i]]);
                      } else {
                        Value value;
                        value.set_null();
                        decoded.push_back(value);
                      }
                    }
                  }
                } else if (PQ_PLAIN == encoding) {
                  plain_decode_values(values_ptr, values_size, meta.type_, element.type_length_,
                                      present, decoded);
                } else {
                  ret = -1;
                  err = "unsupported parquet value encoding";
                  ok = false;
                  break;
                }
                if (ok) {
                  size_t next = 0;
                  for (size_t i = 0; i < def_levels.size(); ++i) {
                    Value value;
                    if (static_cast<int32_t>(def_levels[i]) >= max_def) {
                      if (next < decoded.size()) {
                        value = decoded[next++];
                      } else {
                        value.set_null();
                      }
                    } else {
                      value.set_null();
                    }
                    // converted logical types
                    if (!value.is_null() && ValueType::BIGINT == value.type_) {
                      if (6 == element.converted_type_) {
                        value.set_str(ValueType::DATE,
                                      format_date(static_cast<int32_t>(value.int_value_)));
                      } else if (9 == element.converted_type_) {
                        value.set_str(ValueType::DATETIME,
                                      format_timestamp_millis(value.int_value_));
                      }
                    }
                    column_values[c].push_back(value);
                  }
                  value_pos += num_values;
                }
              }
              page_pos += header_size + page_size;
            }
          }
          if (ok) {
            size_t rows = static_cast<size_t>(group.num_rows_);
            for (size_t r = 0; r < rows; ++r) {
              std::vector<Value> row(leaves.size());
              for (size_t c = 0; c < leaves.size(); ++c) {
                if (r < column_values[c].size()) {
                  row[c] = column_values[c][r];
                } else {
                  row[c].set_null();
                }
              }
              table.rows_.push_back(row);
            }
          }
        }
        if (ok) {
          for (size_t c = 0; c < table.columns_.size(); ++c) {
            bool nullable = table.columns_[c].nullable_;
            for (size_t r = 0; r < table.rows_.size() && !nullable; ++r) {
              nullable = table.rows_[r][c].is_null();
            }
            table.columns_[c].nullable_ = nullable;
          }
          if (0 == table.rows_.size() && num_rows > 0) {
            ret = -1;
            err = "parquet file '" + path + "' declares rows but none could be decoded";
          }
        }
      }
    }
  }
  return ret;
}

}  // namespace filesql
}  // namespace sql
}  // namespace oceanbase
