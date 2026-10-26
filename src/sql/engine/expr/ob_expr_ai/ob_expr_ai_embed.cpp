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
    // Keep the content's own type/collation: forcing a cast to VARCHAR would
    // hide SQL NULLs behind an empty string in the batch cast path and would
    // also lose the LOB header of TEXT columns. The real string data is read
    // during evaluation.
    if (!ob_is_string_type(types_stack[CONTENT_IDX].get_type())) {
      types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
      types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
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
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
    res.set_null();
  } else if (arg_content->is_null()) {
    // SQL NULL propagates: no request is sent for this row.
    res.set_null();
  } else if (arg_model_id->get_string().empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is empty");
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
            ctx.exec_ctx_, temp_allocator, *arg_content,
            expr.args_[CONTENT_IDX]->datum_meta_,
            expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_NOT_NULL(arg_dim)) {
      if (arg_dim->is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter is null");
        res.set_null();
      } else {
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

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t size)
{
  int ret = OB_SUCCESS;
  if (size <= 0) {
    return ret;
  }
  ObDatumVector res_datums = expr.locate_expr_datumvector(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
  query::ObIAiEndpointResolver *endpoint_resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  const bool has_dim = (expr.arg_cnt_ == 3);
  bool has_null_result = false;

  for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; ++i) {
    if (OB_FAIL(expr.args_[i]->eval_batch(ctx, skip, size))) {
    }
  }

  struct EmbedGroup {
    ObString model_id_;
    int64_t dim_;
    ObString *contents_;
    int64_t *rows_;
    int64_t count_;
    EmbedGroup() : model_id_(), dim_(0), contents_(nullptr), rows_(nullptr), count_(0) {}
  };

  ObDatumVector model_datums;
  ObDatumVector content_datums;
  ObDatumVector dim_datums;
  ObArray<EmbedGroup *> groups;
  if (OB_SUCC(ret)) {
    model_datums = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
    content_datums = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
    if (has_dim) {
      dim_datums = expr.args_[DIM_IDX]->locate_expr_datumvector(ctx);
    }
  }

  // Collect the rows that really need a remote call. Rows skipped by the
  // operator (filtered out) or already evaluated are not sent at all.
  for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
    ObDatum *model_datum = nullptr;
    ObDatum *content_datum = nullptr;
    int64_t dim = 0;
    if (skip.at(i) || eval_flags.at(i)) {
      continue;
    } else if (OB_ISNULL(model_datum = model_datums.at(i)) ||
               OB_ISNULL(content_datum = content_datums.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      break;
    } else if (model_datum->is_null() || content_datum->is_null()) {
      // NULL in, NULL out; this row is not sent to the service.
      res_datums.at(i)->set_null();
      eval_flags.set(i);
      has_null_result = true;
      continue;
    } else if (model_datum->get_string().empty()) {
      res_datums.at(i)->set_null();
      eval_flags.set(i);
      has_null_result = true;
      continue;
    } else if (has_dim) {
      ObDatum *dim_datum = dim_datums.at(i);
      if (OB_ISNULL(dim_datum) || dim_datum->is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter is null");
        break;
      }
      dim = dim_datum->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        break;
      }
    }
    ObString content;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(
            ctx.exec_ctx_, temp_allocator, *content_datum,
            expr.args_[CONTENT_IDX]->datum_meta_,
            expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
      break;
    }
    ObString model_id = model_datum->get_string();
    EmbedGroup *group = nullptr;
    for (int64_t g = 0; OB_ISNULL(group) && g < groups.count(); ++g) {
      if (groups.at(g)->dim_ == dim && groups.at(g)->model_id_ == model_id) {
        group = groups.at(g);
      }
    }
    if (OB_ISNULL(group)) {
      if (OB_ISNULL(group = OB_NEWx(EmbedGroup, &temp_allocator))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        group->model_id_ = model_id;
        group->dim_ = dim;
        group->count_ = 0;
        if (OB_ISNULL(group->contents_ = static_cast<ObString *>(
                          temp_allocator.alloc(sizeof(ObString) * size))) ||
            OB_ISNULL(group->rows_ = static_cast<int64_t *>(
                          temp_allocator.alloc(sizeof(int64_t) * size)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(groups.push_back(group))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      // At most one entry per row in the batch, so "size" is a safe capacity.
      group->contents_[group->count_] = content;
      group->rows_[group->count_] = i;
      ++group->count_;
    }
  }

  // One request per group/chunk: resolve the model and endpoint once, then
  // send all the inputs of the chunk in a single HTTP request.
  const int64_t EMBED_REQUEST_BATCH_SIZE = 20;
  for (int64_t g = 0; OB_SUCC(ret) && g < groups.count(); ++g) {
    EmbedGroup *group = groups.at(g);
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, group->model_id_, info))) {
    } else if (OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!ObAIFuncUtils::is_dense_embedding_type(info)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, info type is not dense embedding");
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   group->model_id_, temp_allocator, resolved_endpoint))) {
    } else {
      ObJsonObject *config = nullptr;
      if (group->dim_ > 0) {
        ObJsonInt *dim_json = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, group->dim_, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
      for (int64_t start = 0; OB_SUCC(ret) && start < group->count_;
           start += EMBED_REQUEST_BATCH_SIZE) {
        int64_t end = start + EMBED_REQUEST_BATCH_SIZE;
        if (end > group->count_) {
          end = group->count_;
        }
        ObArray<ObString> chunk_contents;
        ObArray<ObString> chunk_results;
        for (int64_t i = start; OB_SUCC(ret) && i < end; ++i) {
          if (OB_FAIL(chunk_contents.push_back(group->contents_[i]))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else {
          ObAIFuncModel model(temp_allocator, *info, resolved_endpoint);
          if (OB_FAIL(model.call_dense_embedding_vector_v2(chunk_contents, config, chunk_results))) {
          } else if (chunk_results.count() != chunk_contents.count()) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            for (int64_t k = 0; OB_SUCC(ret) && k < chunk_results.count(); ++k) {
              const ObString &vec = chunk_results.at(k);
              const int64_t row = group->rows_[start + k];
              char *buf = static_cast<char *>(ctx.get_expr_res_alloc().alloc(vec.length()));
              if (OB_ISNULL(buf)) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              } else {
                if (vec.length() > 0) {
                  MEMCPY(buf, vec.ptr(), vec.length());
                }
                res_datums.at(row)->set_string(ObString(static_cast<int32_t>(vec.length()), buf));
                eval_flags.set(row);
              }
            }
          }
        }
      }
    }
  }

  if (OB_SUCC(ret) && has_null_result) {
    expr.get_eval_info(ctx).notnull_ = false;
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
    // The batch entry point is what turns "N rows" into one HTTP request with
    // N inputs. It is only used when the expression is evaluated in batch
    // (i.e. it depends on a column, see ObRawExpr::is_vectorize_result()).
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
