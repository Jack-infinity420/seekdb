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
#include "ob_expr_ai_rerank.h"
#include <algorithm>
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

namespace
{

struct RerankResultItem
{
  int64_t index_;
  double score_;
  ObJsonObject *obj_;
};

bool rerank_result_less(const RerankResultItem &lhs, const RerankResultItem &rhs)
{
  if (lhs.score_ != rhs.score_) {
    return lhs.score_ > rhs.score_;
  }
  // Stable tie-break: preserve the original document order for equal scores.
  return lhs.index_ < rhs.index_;
}

int get_rerank_score(ObJsonNode *node, double &score)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_INVALID_DATA;
  } else if (node->json_type() == ObJsonNodeType::J_DOUBLE) {
    score = static_cast<ObJsonDouble *>(node)->value();
  } else if (node->json_type() == ObJsonNodeType::J_INT) {
    score = static_cast<double>(static_cast<ObJsonInt *>(node)->value());
  } else {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

// Validate the per-batch result and remember the global (original) index of
// every returned document. The service returns batch-local indexes, so the
// offset has to be added before anything is sorted or returned.
int collect_rerank_result(ObIAllocator &allocator, ObJsonArray *batch_result,
                          const int64_t start_idx, const int64_t batch_count,
                          ObArray<RerankResultItem> &items)
{
  int ret = OB_SUCCESS;
  bool *seen = nullptr;
  if (OB_ISNULL(batch_result)) {
    ret = OB_INVALID_DATA;
  } else if (batch_count > 0 &&
             OB_ISNULL(seen = static_cast<bool *>(allocator.alloc(sizeof(bool) * batch_count)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    if (batch_count > 0) {
      MEMSET(seen, 0, sizeof(bool) * batch_count);
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(batch_result->element_count()); ++i) {
      ObJsonNode *node = batch_result->get_value(static_cast<uint64_t>(i));
      ObJsonObject *obj = nullptr;
      ObJsonNode *index_node = nullptr;
      ObJsonNode *score_node = nullptr;
      int64_t local_index = 0;
      double score = 0.0;
      if (OB_ISNULL(node) || node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
      } else if (FALSE_IT(obj = static_cast<ObJsonObject *>(node))) {
      } else if (OB_ISNULL(index_node = obj->get_value("index")) ||
                 index_node->json_type() != ObJsonNodeType::J_INT) {
        ret = OB_INVALID_DATA;
      } else if (OB_ISNULL(score_node = obj->get_value("relevance_score"))) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(get_rerank_score(score_node, score))) {
      } else if (FALSE_IT(local_index = static_cast<ObJsonInt *>(index_node)->value())) {
      } else if (local_index < 0 || local_index >= batch_count || seen[local_index]) {
        // Index out of range or duplicated: the response is unusable.
        ret = OB_INVALID_DATA;
      } else {
        seen[local_index] = true;
        // Rewrite the index field to the global document position.
        static_cast<ObJsonInt *>(index_node)->set_value(local_index + start_idx);
        RerankResultItem item;
        item.index_ = local_index + start_idx;
        item.score_ = score;
        item.obj_ = obj;
        if (OB_FAIL(items.push_back(item))) {
        }
      }
    }
  }
  return ret;
}

} // anonymous namespace

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc,
                    T_FUN_SYS_AI_RERANK,
                    N_AI_RERANK,
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL,
                    NOT_ROW_DIMENSION)
{
}

ObExprAIRerank::~ObExprAIRerank()
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator,
                                          const ObAIFuncExprInfo &info,
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &header_array,
                                          ObString &query,
                                          ObJsonArray *document_array,
                                          ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (OB_ISNULL(response) || response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else if (arg_model_id->get_string().empty() || arg_query->get_string().empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query is empty");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);

    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    const bool has_doc_key = OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null();
    ObString doc_key;
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    bool is_null_result = false;
    const int64_t batch_size = 20; // max documents per rerank request
    if (has_doc_key) {
      doc_key = arg_doc_key->get_string();
      if (doc_key.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document key is empty");
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
                   j_base, is_null_result))) {
    } else if (is_null_result || OB_ISNULL(j_base)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, documents is null");
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(document_array->element_count()); i++) {
        ObJsonNode *doc_node = document_array->get_value(static_cast<uint64_t>(i));
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (has_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (!has_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        }
      }
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, *endpoint_info, header_array))) {
    } else {
      // Build the text-only document array sent to the model. With doc_key we
      // extract that field from every object; otherwise documents are strings.
      ObJsonArray *text_array = nullptr;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, text_array))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(document_array->element_count()); ++i) {
        ObJsonNode *doc_node = document_array->get_value(static_cast<uint64_t>(i));
        if (!has_doc_key) {
          if (OB_FAIL(text_array->append(doc_node))) {
          }
        } else {
          ObJsonNode *value_node = static_cast<ObJsonObject *>(doc_node)->get_value(doc_key);
          if (OB_ISNULL(value_node) || value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(text_array->append(value_node))) {
          }
        }
      }

      ObArray<RerankResultItem> items;
      for (int64_t start = 0; OB_SUCC(ret) && start < static_cast<int64_t>(text_array->element_count());
           start += batch_size) {
        int64_t end = start + batch_size;
        if (end > static_cast<int64_t>(text_array->element_count())) {
          end = static_cast<int64_t>(text_array->element_count());
        }
        ObJsonArray *batch_documents = nullptr;
        ObJsonArray *batch_result = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, batch_documents))) {
        }
        for (int64_t i = start; OB_SUCC(ret) && i < end; ++i) {
          if (OB_FAIL(batch_documents->append(text_array->get_value(static_cast<uint64_t>(i))))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(inner_eval_ai_rerank(temp_allocator, *info, *endpoint_info,
                       header_array, query, batch_documents, batch_result))) {
        } else if (OB_FAIL(collect_rerank_result(temp_allocator, batch_result, start, end - start, items))) {
        }
      }

      // Always order by score ourselves: the service may return the documents
      // in an arbitrary (or per-batch only) order.
      if (OB_SUCC(ret)) {
        std::stable_sort(items.begin(), items.end(), rerank_result_less);
      }

      ObJsonArray *result_array = nullptr;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, result_array))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
        const RerankResultItem &item = items.at(i);
        if (!has_doc_key) {
          if (OB_FAIL(result_array->append(item.obj_))) {
          }
        } else {
          ObJsonNode *origin_node = document_array->get_value(static_cast<uint64_t>(item.index_));
          ObJsonObject *new_obj = nullptr;
          ObJsonDouble *score_node = nullptr;
          if (OB_ISNULL(origin_node) || origin_node->json_type() != ObJsonNodeType::J_OBJECT) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_ISNULL(new_obj = static_cast<ObJsonObject *>(origin_node->clone(&temp_allocator, true)))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_ISNULL(score_node = OB_NEWx(ObJsonDouble, &temp_allocator, item.score_))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(new_obj->add(MODEL_SCORE_KEY, score_node))) {
          } else if (OB_FAIL(result_array->append(new_obj))) {
          }
        }
      }

      if (OB_SUCC(ret)) {
        ObString raw_str;
        if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx,
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  return ret;
}

} // namespace sql
} // namespace oceanbase
