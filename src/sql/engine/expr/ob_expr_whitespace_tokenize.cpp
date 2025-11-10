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
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "lib/oblog/ob_log.h"
#include <cctype>

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

ObExprWhitespaceTokenize::ObExprWhitespaceTokenize(ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, T_FUN_SYS_WHITESPACE_TOKENIZE, N_WHITESPACE_TOKENIZE, 1, VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{
}

int ObExprWhitespaceTokenize::calc_result_typeN(ObExprResType &type,
                                                ObExprResType *types,
                                                int64_t param_num,
                                                common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;
  UNUSED(type_ctx);

  if (OB_ISNULL(types) || param_num != 1) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(param_num), KP(types));
  } else {
    // Set input parameter calculation type
    types[0].set_calc_type(ObVarcharType);
    types[0].set_calc_collation_type(types[0].get_collation_type());
    types[0].set_calc_collation_level(types[0].get_collation_level());

    // Always return VARCHAR type for stability and compatibility
    // Result format: [token1,token2,...]
    type.set_varchar();
    type.set_collation_type(types[0].get_collation_type());
    type.set_collation_level(types[0].get_collation_level());
    type.set_length(OB_MAX_VARCHAR_LENGTH);
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
  ObDatum *input_datum = NULL;

  if (OB_FAIL(expr.args_[0]->eval(ctx, input_datum))) {
    LOG_WARN("eval input argument failed", K(ret));
  } else if (OB_ISNULL(input_datum)) {
    // Null pointer check
    res.set_null();
  } else if (input_datum->is_null()) {
    // Null value check - covers all NULL sources
    res.set_null();
  } else {
    ObString input_str = input_datum->get_string();
    ObString real_input_str = input_str;

    // Get real string data if it's a lob (TEXT/CLOB support)
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(tmp_allocator,
                                                          *input_datum,
                                                          expr.args_[0]->datum_meta_,
                                                          expr.args_[0]->obj_meta_.has_lob_header(),
                                                          real_input_str))) {
      LOG_WARN("failed to get real string data", K(ret));
    } else {
      const char *str = real_input_str.ptr();
      int64_t len = real_input_str.length();
      
      // Safety check: validate string pointer and length
      if (OB_UNLIKELY(len < 0)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid string length", K(ret), K(len));
      } else if (OB_UNLIKELY(len > 0 && OB_ISNULL(str))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("null string pointer with non-zero length", K(ret), K(len));
      } else {
        ObExprStrResAlloc res_alloc(expr, ctx);
        
        // First pass: Calculate result length needed
        int64_t result_len = 2; // for "[]"
        int64_t pos = 0;
        int64_t token_count = 0;
        const int64_t MAX_RESULT_LEN = OB_MAX_VARCHAR_LENGTH;
        
        while (pos < len && OB_SUCC(ret)) {
          // Skip leading whitespace
          while (pos < len && isspace(static_cast<unsigned char>(str[pos]))) {
            pos++;
          }
          if (pos < len) {
            // Found token start
            int64_t token_start = pos;
            while (pos < len && !isspace(static_cast<unsigned char>(str[pos]))) {
              pos++;
            }
            int64_t token_len = pos - token_start;
            
            // Check for integer overflow before adding
            if (OB_UNLIKELY(result_len > MAX_RESULT_LEN - token_len - 1)) {
              ret = OB_SIZE_OVERFLOW;
              LOG_WARN("result buffer size overflow", K(ret), K(result_len), K(token_len));
              break;
            }
            
            result_len += token_len;
            if (token_count > 0) {
              result_len += 1; // comma separator
            }
            token_count++;
          }
        }

        // Allocate buffer for result
        if (OB_SUCC(ret)) {
          char *buf = static_cast<char *>(res_alloc.alloc(result_len));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
            LOG_WARN("fail to alloc result buffer", K(ret), K(result_len));
          } else {
            // Second pass: Build the string result with format [token1,token2,...]
            int64_t write_pos = 0;
            buf[write_pos++] = '[';
            
            pos = 0;
            int64_t token_idx = 0;
            while (pos < len && OB_SUCC(ret)) {
              // Skip leading whitespace
              while (pos < len && isspace(static_cast<unsigned char>(str[pos]))) {
                pos++;
              }
              if (pos < len) {
                // Add comma separator before non-first tokens
                if (token_idx > 0) {
                  if (OB_UNLIKELY(write_pos >= result_len)) {
                    ret = OB_SIZE_OVERFLOW;
                    LOG_WARN("buffer position overflow", K(ret), K(write_pos), K(result_len));
                    break;
                  }
                  buf[write_pos++] = ',';
                }
                
                // Extract and copy token
                int64_t token_start = pos;
                while (pos < len && !isspace(static_cast<unsigned char>(str[pos]))) {
                  pos++;
                }
                int64_t token_len = pos - token_start;
                
                // Safety check for buffer write
                if (OB_UNLIKELY(write_pos + token_len > result_len)) {
                  ret = OB_SIZE_OVERFLOW;
                  LOG_WARN("buffer size check failed", K(ret), K(write_pos), K(token_len), K(result_len));
                  break;
                }
                
                MEMCPY(buf + write_pos, str + token_start, token_len);
                write_pos += token_len;
                token_idx++;
              }
            }
            
            if (OB_SUCC(ret)) {
              if (OB_UNLIKELY(write_pos >= result_len)) {
                ret = OB_SIZE_OVERFLOW;
                LOG_WARN("final buffer position check failed", K(ret), K(write_pos), K(result_len));
              } else {
                buf[write_pos++] = ']';
                res.set_string(buf, write_pos);
              }
            }
          }
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
  
  if (OB_UNLIKELY(rt_expr.arg_cnt_ != 1)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(rt_expr.arg_cnt_));
  } else {
    rt_expr.eval_func_ = eval_whitespace_tokenize;
  }

  return ret;
}

} // namespace sql
} // namespace oceanbase

