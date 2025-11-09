/**
 * Copyright (c) 2025 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/expr/ob_expr_whitespace_tokenize.h"
#include "lib/udt/ob_collection_type.h"
#include "lib/udt/ob_array_type.h"
#include "sql/engine/expr/ob_array_expr_utils.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_result_type_util.h"
#include "lib/charset/ob_ctype.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

ObExprWhitespaceTokenize::ObExprWhitespaceTokenize(ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, T_FUN_SYS_WHITESPACE_TOKENIZE, N_WHITESPACE_TOKENIZE, ONE, VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{
}

int ObExprWhitespaceTokenize::calc_result_typeN(ObExprResType &type,
                                                ObExprResType *types,
                                                int64_t param_num,
                                                common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;
  uint16_t subschema_id;
  ObDataType res_data_type;
  ObSQLSessionInfo *session = const_cast<ObSQLSessionInfo *>(type_ctx.get_session());
  ObExecContext *exec_ctx = OB_ISNULL(session) ? NULL : session->get_cur_exec_ctx();

  if (OB_ISNULL(types) || param_num != 1) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(param_num), KP(types));
  } else if (OB_ISNULL(exec_ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("exec ctx is null", K(ret));
  } else if (ob_is_text(types[0].get_type(), types[0].get_collation_type())) {
    types[0].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
  } else if (ob_is_varchar_char_type(types[0].get_type(), types[0].get_collation_type())) {
    types[0].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
  } else if (!ob_is_null(types[0].get_type())) {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
    LOG_USER_ERROR(OB_ERR_INVALID_TYPE_FOR_OP, "VARCHAR", ob_obj_type_str(types[0].get_type()));
  }

  if (OB_SUCC(ret)) {
    ObObjMeta meta;
    meta.set_varchar();
    res_data_type.set_meta_type(meta);
    res_data_type.set_length(OB_MAX_VARCHAR_LENGTH / 4);
    if (OB_FAIL(exec_ctx->get_subschema_id_by_collection_elem_type(ObNestedType::OB_ARRAY_TYPE, res_data_type, subschema_id))) {
      LOG_WARN("failed to get collection subschema id", K(ret));
    } else {
      type.set_collection(subschema_id);
    }
  }

  return ret;
}

int ObExprWhitespaceTokenize::eval_whitespace_tokenize(const ObExpr &expr,
                                                       ObEvalCtx &ctx,
                                                       ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &tmp_allocator = tmp_alloc_g.get_allocator();
  uint16_t subschema_id = expr.obj_meta_.get_subschema_id();
  ObDatum *input_datum = NULL;

  if (OB_FAIL(expr.args_[0]->eval(ctx, input_datum))) {
    LOG_WARN("eval input argument failed", K(ret));
  } else if (OB_ISNULL(input_datum)) {
    res.set_null();
  } else if (input_datum->is_null()) {
    res.set_null();
  } else {
    ObCollationType cs_type = expr.args_[0]->datum_meta_.cs_type_;
    ObString input_str = input_datum->get_string();
    ObIArrayType *arr_obj = NULL;
    ObArrayBinary *binary_array = NULL;

    // Get real string data if it's a lob
    ObString real_input_str = input_str;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(tmp_allocator,
                                                          *input_datum,
                                                          expr.args_[0]->datum_meta_,
                                                          expr.args_[0]->obj_meta_.has_lob_header(),
                                                          real_input_str))) {
      LOG_WARN("failed to get real string data", K(ret));
    } else if (OB_FAIL(ObArrayExprUtils::construct_array_obj(tmp_allocator, ctx, subschema_id, arr_obj, false))) {
      LOG_WARN("construct array obj failed", K(ret), K(subschema_id));
    } else if (OB_ISNULL(binary_array = static_cast<ObArrayBinary *>(arr_obj))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("binary array is null", K(ret), K(subschema_id));
    } else {
      // Tokenize the input string based on whitespace
      const char *str = real_input_str.ptr();
      int64_t len = real_input_str.length();
      int64_t pos = 0;
      
      // Skip leading whitespace
      while (pos < len && isspace(str[pos])) {
        pos++;
      }

      while (pos < len && OB_SUCC(ret)) {
        int64_t token_start = pos;
        
        // Find the end of the token (until whitespace)
        while (pos < len && !isspace(str[pos])) {
          pos++;
        }

        // Add the token to the array
        if (token_start < pos) {
          ObString token(pos - token_start, str + token_start);
          if (OB_FAIL(binary_array->push_back(ObString(token)))) {
            LOG_WARN("failed to push token to array", K(ret), K(token));
          }
        }

        // Skip trailing whitespace
        while (pos < len && isspace(str[pos])) {
          pos++;
        }
      }

      if (OB_SUCC(ret)) {
        ObString res_str;
        if (OB_FAIL(ObArrayExprUtils::set_array_res(arr_obj, arr_obj->get_raw_binary_len(), expr, ctx, res_str))) {
          LOG_WARN("get array binary string failed", K(ret));
        } else {
          res.set_string(res_str);
        }
      }
    }
  }

  return ret;
}

int ObExprWhitespaceTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                                     const ObRawExpr &raw_expr,
                                     ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  UNUSED(op_cg_ctx);
  UNUSED(raw_expr);
  
  if (OB_UNLIKELY(rt_expr.arg_cnt_ != 1) || OB_ISNULL(rt_expr.args_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(rt_expr.arg_cnt_), KP(rt_expr.args_));
  } else {
    rt_expr.eval_func_ = eval_whitespace_tokenize;
  }

  return ret;
}

} // namespace sql
} // namespace oceanbase

