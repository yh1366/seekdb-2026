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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_content->is_null()) {
    // NULL input propagates as NULL instead of failing the whole query
    res.set_null();
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(
            ctx.exec_ctx_, temp_allocator, *arg_content, expr.args_[CONTENT_IDX]->datum_meta_,
            expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim) && !arg_dim->is_null()) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx,
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_batch_ai_embed;
  }
  return ret;
}

// rows sharing the same model and dimension are merged into one embedding request
struct ObAIEmbedBatchGroup
{
  ObString model_id_;
  int64_t dim_ = -1;
  ObArray<int64_t> row_ids_;
  ObArray<ObString> contents_;
};

int ObExprAIEmbed::eval_batch_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, const ObBitVector &skip, const int64_t size)
{
  INIT_SUCC(ret);
  ObDatumVector model_datums;
  ObDatumVector content_datums;
  ObDatumVector dim_datums;
  const bool has_dim = (expr.arg_cnt_ == 3);
  if (has_dim
      ? OB_FAIL(expr.eval_batch_param_value(ctx, skip, size, model_datums, content_datums, dim_datums))
      : OB_FAIL(expr.eval_batch_param_value(ctx, skip, size, model_datums, content_datums))) {
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObDatum *res_datum = expr.locate_batch_datums(ctx);
    ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);

    ObArray<ObAIEmbedBatchGroup> groups;
    for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
      if (skip.contain(i) || eval_flags.at(i)) {
        // row is skipped or already evaluated
      } else if (content_datums.at(i)->is_null()) {
        // NULL input propagates as NULL instead of failing the whole query
        res_datum[i].set_null();
        eval_flags.set(i);
      } else if (model_datums.at(i)->is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
        res_datum[i].set_null();
      } else {
        ObString model_id = model_datums.at(i)->get_string();
        ObString content;
        if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                ctx.exec_ctx_, temp_allocator, *content_datums.at(i), expr.args_[CONTENT_IDX]->datum_meta_,
                expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
        } else if (model_id.empty() || content.empty()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
          res_datum[i].set_null();
        } else {
          int64_t dim = -1;
          if (has_dim && !dim_datums.at(i)->is_null()) {
            dim = dim_datums.at(i)->get_int();
            if (dim <= 0) {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
              res_datum[i].set_null();
            }
          }
          if (OB_SUCC(ret)) {
            int64_t g = -1;
            for (int64_t k = 0; k < groups.count(); ++k) {
              if (groups.at(k).dim_ == dim && groups.at(k).model_id_ == model_id) {
                g = k;
                break;
              }
            }
            if (g < 0) {
              ObAIEmbedBatchGroup group;
              group.dim_ = dim;
              group.model_id_ = model_id;
              if (OB_FAIL(groups.push_back(group))) {
                LOG_WARN("ai_embed batch, failed to push group", K(ret));
              } else {
                g = groups.count() - 1;
              }
            }
            if (OB_SUCC(ret)) {
              if (OB_FAIL(groups.at(g).row_ids_.push_back(i))) {
                LOG_WARN("ai_embed batch, failed to push row id", K(ret));
              } else if (OB_FAIL(groups.at(g).contents_.push_back(content))) {
                LOG_WARN("ai_embed batch, failed to push content", K(ret));
              }
            }
          }
        }
      }
    }

    for (int64_t k = 0; OB_SUCC(ret) && k < groups.count(); ++k) {
      ObAIEmbedBatchGroup &group = groups.at(k);
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      ObJsonObject *config = nullptr;
      if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (group.dim_ > 0) {
        ObJsonInt *dim_json = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
          LOG_WARN("ai_embed batch, failed to create config object", K(ret));
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, group.dim_, dim_json))) {
          LOG_WARN("ai_embed batch, failed to create dimension json", K(ret));
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
          LOG_WARN("ai_embed batch, failed to add dimension", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, group.model_id_, info))) {
          LOG_WARN("ai_embed batch, failed to get model info", K(ret), K(group.model_id_));
        } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                       group.model_id_, temp_allocator, resolved_endpoint))) {
          LOG_WARN("ai_embed batch, failed to resolve endpoint", K(ret), K(group.model_id_));
        } else {
          ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
          ObArray<ObString> results;
          if (OB_FAIL(model.call_dense_embedding_vector_v2(group.contents_, config, results))) {
            LOG_WARN("ai_embed batch, failed to call dense embedding", K(ret), "rows", group.contents_.count());
          } else if (results.count() != group.row_ids_.count()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("ai_embed batch, result count mismatch", K(ret),
                     K(results.count()), "rows", group.row_ids_.count());
          } else {
            for (int64_t j = 0; OB_SUCC(ret) && j < results.count(); ++j) {
              const int64_t row = group.row_ids_.at(j);
              if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res_datum[row], results.at(j)))) {
                LOG_WARN("ai_embed batch, failed to set result", K(ret), K(row));
              } else {
                eval_flags.set(row);
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
