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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{

namespace
{

// Every tokenize() evaluation used to re-parse the parser name, re-parse and
// re-serialize the parser property JSON, probe the dictionary loader and
// create a word hash map. All of that only depends on the two parser
// arguments, while a fulltext index build evaluates tokenize() once per column
// per row, so the result of the last evaluation is kept per thread.
class ObTokenizePlanCache
{
public:
  static constexpr int64_t MAX_RAW_NAME_LEN = storage::OB_FT_PARSER_NAME_LENGTH;
  static constexpr int64_t MAX_RAW_PROPS_LEN = 4096;
  static constexpr int64_t MAX_TABLE_NAME_LEN = 128;

  ObTokenizePlanCache()
      : raw_name_(),
        raw_props_(),
        raw_name_len_(0),
        raw_props_len_(0),
        parser_(),
        property_(),
        desc_(nullptr),
        stopword_table_len_(0),
        dict_table_len_(0),
        quantifier_table_len_(0),
        valid_(false),
        word_map_created_(false)
  {
  }
  ~ObTokenizePlanCache() = default;

  bool match(const common::ObString &raw_name, const common::ObString &raw_props) const
  {
    bool is_match = false;
    if (valid_ && raw_name_len_ == raw_name.length() && raw_props_len_ == raw_props.length()) {
      is_match = (0 == raw_name_len_ || 0 == MEMCMP(raw_name_, raw_name.ptr(), raw_name_len_))
                 && (0 == raw_props_len_ || 0 == MEMCMP(raw_props_, raw_props.ptr(), raw_props_len_));
    }
    return is_match;
  }

  int save(const common::ObString &raw_name,
           const common::ObString &raw_props,
           const storage::ObFTParser &parser,
           const storage::ObFTParserProperty &property,
           const storage::ObIFTParserDesc *desc)
  {
    int ret = OB_SUCCESS;
    valid_ = false;
    if (OB_ISNULL(desc)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (raw_name.length() >= MAX_RAW_NAME_LEN || raw_props.length() >= MAX_RAW_PROPS_LEN
               || !copy_table_name(property.stopword_table_,
                                   stopword_table_, stopword_table_len_)
               || !copy_table_name(property.dict_table_, dict_table_, dict_table_len_)
               || !copy_table_name(property.quantifier_table_,
                                   quantifier_table_, quantifier_table_len_)) {
      // Arguments that do not fit the cache simply re-parse every time.
    } else {
      if (raw_name.length() > 0) {
        MEMCPY(raw_name_, raw_name.ptr(), raw_name.length());
      }
      raw_name_len_ = raw_name.length();
      if (raw_props.length() > 0) {
        MEMCPY(raw_props_, raw_props.ptr(), raw_props.length());
      }
      raw_props_len_ = raw_props.length();
      parser_ = parser;
      property_ = property;
      // The parsed table names may point into the transient property buffer,
      // re-point them at the copies owned by this cache.
      property_.stopword_table_.assign_ptr(stopword_table_, static_cast<int32_t>(stopword_table_len_));
      property_.dict_table_.assign_ptr(dict_table_, static_cast<int32_t>(dict_table_len_));
      property_.quantifier_table_.assign_ptr(quantifier_table_, static_cast<int32_t>(quantifier_table_len_));
      desc_ = desc;
      valid_ = true;
    }
    return ret;
  }

  int get_word_map(const int64_t bucket_cnt, storage::ObFTWordMap *&word_map)
  {
    int ret = OB_SUCCESS;
    if (!word_map_created_) {
      if (OB_FAIL(word_map_.create(bucket_cnt, common::ObMemAttr("FTWordMap")))) {
      } else {
        word_map_created_ = true;
      }
    }
    if (OB_SUCC(ret)) {
      word_map = &word_map_;
    }
    return ret;
  }

  const storage::ObFTParser &parser() const { return parser_; }
  const storage::ObFTParserProperty &parser_property() const { return property_; }
  const storage::ObIFTParserDesc *parser_desc() const { return desc_; }

private:
  bool copy_table_name(const common::ObString &src, char *dst, int64_t &dst_len)
  {
    bool is_ok = true;
    if (src.length() >= MAX_TABLE_NAME_LEN) {
      is_ok = false;
    } else {
      if (src.length() > 0 && OB_NOT_NULL(src.ptr())) {
        MEMCPY(dst, src.ptr(), src.length());
      }
      dst_len = src.length();
    }
    return is_ok;
  }

private:
  char raw_name_[MAX_RAW_NAME_LEN];
  char raw_props_[MAX_RAW_PROPS_LEN];
  int64_t raw_name_len_;
  int64_t raw_props_len_;
  storage::ObFTParser parser_;
  storage::ObFTParserProperty property_;
  const storage::ObIFTParserDesc *desc_;
  char stopword_table_[MAX_TABLE_NAME_LEN];
  char dict_table_[MAX_TABLE_NAME_LEN];
  char quantifier_table_[MAX_TABLE_NAME_LEN];
  int64_t stopword_table_len_;
  int64_t dict_table_len_;
  int64_t quantifier_table_len_;
  bool valid_;
  storage::ObFTWordMap word_map_;
  bool word_map_created_;
};

ObTokenizePlanCache &get_tokenize_plan_cache()
{
  static thread_local ObTokenizePlanCache cache;
  return cache;
}

} // namespace

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
  } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                     ctx,
                                                     temp_allocator,
                                                     json_result,
                                                     expr_datum))) {
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  ObTokenizePlanCache &plan_cache = get_tokenize_plan_cache();
  storage::ObFTWordMap *token_map = nullptr;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(plan_cache.get_word_map(ft_word_bkt_cnt, token_map))) {
  } else if (plan_cache.match(param.raw_parser_name_, param.raw_properties_)) {
    if (OB_FAIL(tokenize_helper.init_prepared(&allocator,
                                              plan_cache.parser(),
                                              plan_cache.parser_property(),
                                              plan_cache.parser_desc()))) {
      LOG_WARN("failed to init tokenize helper from cached plan", K(ret));
    }
  } else {
    // Slow path: build the parse plan once, then reuse it for every following
    // evaluation with the same parser arguments.
    storage::ObFTParser parser;
    storage::ObFTParserProperty property;
    const storage::ObIFTParserDesc *parser_desc = nullptr;
    if (OB_FAIL(construct_ft_parser_inner_name(param.raw_parser_name_, param))) {
    } else if (OB_FAIL(param.reform_parser_properties(param.raw_properties_))) {
    } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
    } else if (OB_FAIL(tokenize_helper.init_and_export(&allocator,
                                                       param.parser_name_,
                                                       param.properties_,
                                                       parser,
                                                       property,
                                                       parser_desc))) {
    } else if (OB_FAIL(plan_cache.save(param.raw_parser_name_,
                                       param.raw_properties_,
                                       parser,
                                       property,
                                       parser_desc))) {
    }
  }

  if (OB_FAIL(ret)) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     *token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(*token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(*token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    raw_parser_name_(),
    raw_properties_(),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        raw_properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  char *parser_name_buf = nullptr;
  if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;

  // Only record the raw parser name here, the (re)parsing into the inner
  // name is done once per distinct argument by tokenize_fulltext.
  if (expr.arg_cnt_ < 2) {
    param.raw_parser_name_ = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      param.raw_parser_name_ = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      param.raw_parser_name_ = parser_datum->get_string().trim();
    }
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;

  if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
