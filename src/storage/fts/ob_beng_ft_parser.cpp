/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ob_beng_ft_parser.h"
#include "lib/allocator/page_arena.h"
#include "data_plane/fts/ob_fts_struct.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace storage
{

int ObBEngFTParser::get_next_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  ObDatum token;
  int64_t token_freq = 0;
  char *buf = nullptr;
  word = nullptr;
  word_len = 0;
  char_len = 0;
  word_freq = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(token_stream_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(token_stream_->get_next(token, token_freq))) {
    if (OB_ITER_END != ret) {
    }
  } else if (OB_ISNULL(token.ptr_) || OB_UNLIKELY(0 >= token.len_ || 0 >= token_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(buf = static_cast<char *>(token_alloc_->alloc(token.len_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, token.ptr_, token.len_);
    word = buf;
    word_len = token.len_;
    char_len = token.len_;
    word_freq = token_freq;
    LOG_DEBUG("succeed to add word", K(ObString(word_len, word)), K(word_freq));
  }
  return ret;
}

int ObBEngFTParser::init(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    doc_.set_string(param->fulltext_, param->ft_length_);
    analysis_ctx_.cs_ = param->cs_;
    analysis_ctx_.filter_stopword_ = false;
    analysis_ctx_.need_grouping_ = false;
    if (OB_FAIL(english_analyzer_.init(analysis_ctx_, allocator_))) {
    } else if (OB_FAIL(segment(doc_, token_stream_))) {
    } else if (OB_ISNULL(token_stream_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      is_inited_ = true;
    }
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

int ObBEngFTParser::prepare(ObFTParserParam *param, common::ObIAllocator *token_alloc)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    // Point token copies at the recyclable per-document arena.
    token_alloc_ = (OB_NOT_NULL(token_alloc) ? token_alloc : &allocator_);
    if (IS_INIT && OB_NOT_NULL(token_stream_) && analysis_ctx_.cs_ == param->cs_) {
      // Fast path: the analyzer pipeline is reusable; analyze() rebinds the
      // tail token stream to the new document.
      doc_.set_string(param->fulltext_, param->ft_length_);
      if (OB_FAIL(segment(doc_, token_stream_))) {
      }
    } else {
      reset();
      if (OB_FAIL(init(param))) {
      }
    }
  }
  return ret;
}

int ObBEngFTParser::segment(
    const common::ObDatum &doc,
    share::ObITokenStream *&token_stream)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(doc.ptr_) || OB_UNLIKELY(0 >= doc.len_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < doc.len_)) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(english_analyzer_.analyze(doc, token_stream))) {
  }
  return ret;
}

void ObBEngFTParser::reset()
{
  analysis_ctx_.reset();
  english_analyzer_.reset();
  doc_.reset();
  token_stream_ = nullptr;
  is_inited_ = false;
}

ObBasicEnglishFTParserDesc::ObBasicEnglishFTParserDesc() {}

int ObBasicEnglishFTParserDesc::segment(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  iter = nullptr;
  if (OB_ISNULL(param) || OB_ISNULL(param->fulltext_) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    // Reuse a thread-local parser: the analyzer pipeline is built once and
    // analyze() rebinds it per document. Token copies live in a per-document
    // arena recycled before each prepare().
    struct OBengTLSParser
    {
      ObArenaAllocator alloc;
      ObArenaAllocator doc_alloc;
      ObBEngFTParser *parser;
      OBengTLSParser()
          : alloc(lib::ObMemAttr("BEngFTParserTLS")),
            doc_alloc(lib::ObMemAttr("BEngDocTLS")),
            parser(nullptr)
      {}
      ~OBengTLSParser()
      {
        if (OB_NOT_NULL(parser)) {
          parser->~ObBEngFTParser();
          alloc.free(parser);
          parser = nullptr;
        }
      }
    };
    static thread_local OBengTLSParser tls;
    if (OB_ISNULL(tls.parser)) {
      if (OB_ISNULL(tls.parser = OB_NEWx(ObBEngFTParser, &tls.alloc, tls.alloc))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      }
    }
    if (OB_SUCC(ret)) {
      tls.doc_alloc.reset();
      if (OB_FAIL(tls.parser->prepare(param, &tls.doc_alloc))) {
      } else {
        iter = tls.parser;
      }
    }
  }

  if (OB_FAIL(ret) && OB_NOT_NULL(iter)) {
    free_token_iter(param, iter);
  }

  return ret;
}

void ObBasicEnglishFTParserDesc::free_token_iter(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  // The iterator is the thread-local parser kept for reuse; nothing to free.
  iter = nullptr;
  UNUSED(param);
}

int ObBasicEnglishFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_min_max_word();
  flag.set_stop_word();
  flag.set_groupby_word();
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
