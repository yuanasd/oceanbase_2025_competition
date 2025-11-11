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
#include "lib/charset/ob_charset.h"
#include "lib/oblog/ob_log.h"
#include <cctype>

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

ObExprWhitespaceTokenize::ObExprWhitespaceTokenize(ObIAllocator &alloc)
    : ObStringExprOperator(alloc, T_FUN_SYS_WHITESPACE_TOKENIZE, "whitespace_tokenize", 1, VALID_FOR_GENERATED_COL)
{
}

int ObExprWhitespaceTokenize::calc_result_type1(ObExprResType &type,
                                                ObExprResType &type1,
                                                ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  

  // 验证输入参数类型
  if (OB_UNLIKELY(!ob_is_varchar_char_type(type1.get_type(), type1.get_collation_type()) &&
                  !ob_is_text(type1.get_type(), type1.get_collation_type()) &&
                  !ob_is_null(type1.get_type()))) {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
    LOG_USER_ERROR(OB_ERR_INVALID_TYPE_FOR_OP, "VARCHAR", ob_obj_type_str(type1.get_type()));
  } else {
    // 设置输入参数计算类型为 VARCHAR
    ObCollationType calc_cs_type = type1.get_collation_type();
    if (!ob_is_string_tc(type1.get_type()) || CS_TYPE_INVALID == calc_cs_type) {
      calc_cs_type = ObCharset::get_default_collation(ObCharset::get_default_charset());
    }
    type1.set_calc_type(ObVarcharType);
    type1.set_calc_collation_type(calc_cs_type);
    type1.set_calc_collation_level(CS_LEVEL_COERCIBLE);
    

    // 设置返回类型为 VARCHAR，格式为 [token1,token2,...]
    type.set_varchar();
    type.set_collation_type(get_default_collation_type(type.get_type(), type_ctx));
    type.set_collation_level(CS_LEVEL_COERCIBLE);
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
    res.set_null();
  } else if (input_datum->is_null()) {
    res.set_null();
  } else {
    ObString input_str = input_datum->get_string();
    ObString real_input_str = input_str;

    bool is_likely_null_string = expr.args_[0]->obj_meta_.has_lob_header()
                                 && nullptr == input_str.ptr()
                                 && 0 == input_str.length();
    if (is_likely_null_string) {
      res.set_null();
    } else {

      // 获取真实字符串数据 (支持 LOB/CLOB)
      if (OB_FAIL(ObTextStringHelper::read_real_string_data(tmp_allocator,
                                                          *input_datum,
                                                          expr.args_[0]->datum_meta_,
                                                          expr.args_[0]->obj_meta_.has_lob_header(),
                                                          real_input_str))) {
        LOG_WARN("failed to get real string data", K(ret));
      } else {
        const char *str = real_input_str.ptr();
        int64_t len = real_input_str.length();

        // 安全检查：验证字符串指针和长度
        if (OB_UNLIKELY(len < 0)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("invalid string length", K(ret), K(len));
        } else if (OB_UNLIKELY(len > 0 && OB_ISNULL(str))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("null string pointer with non-zero length", K(ret), K(len));
        } else {
          ObExprStrResAlloc res_alloc(expr, ctx);

          // 第一遍：计算结果长度
          int64_t result_len = 2; // "[]"
          int64_t pos = 0;
          int64_t token_count = 0;
          const int64_t MAX_RESULT_LEN = OB_MAX_VARCHAR_LENGTH;

          while (pos < len && OB_SUCC(ret)) {
            // 跳过前导空格
            while (pos < len && isspace(static_cast<unsigned char>(str[pos]))) {
              pos++;
            }
            if (pos < len) {
              // 找到 token 的起点
              int64_t token_start = pos;
              while (pos < len && !isspace(static_cast<unsigned char>(str[pos]))) {
                pos++;
              }
              int64_t token_len = pos - token_start;

              // 检查整数溢出
              if (OB_UNLIKELY(result_len > MAX_RESULT_LEN - token_len - 1)) {
                ret = OB_SIZE_OVERFLOW;
                LOG_WARN("result buffer size overflow", K(ret), K(result_len), K(token_len));
                break;
              }
              result_len += token_len + 1; // token + ","
              token_count++;
            }
          }

          if (OB_SUCC(ret)) {
            // 分配空间
            char *buf = static_cast<char *>(res_alloc.alloc(result_len));
            if (OB_ISNULL(buf)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
              LOG_WARN("failed to allocate buffer", K(ret), K(result_len));
            } else {
              // 第二遍：填充结果
              int64_t write_pos = 0;
              pos = 0;
              buf[write_pos++] = '[';
              int64_t token_idx = 0;

              while (pos < len && OB_SUCC(ret)) {
                // 跳过前导空格
                while (pos < len && isspace(static_cast<unsigned char>(str[pos]))) {
                  pos++;
                }
                if (pos < len) {
                  // 找到 token 的起点
                  int64_t token_start = pos;
                  while (pos < len && !isspace(static_cast<unsigned char>(str[pos]))) {
                    pos++;
                  }
                  int64_t token_len = pos - token_start;

                  // 添加分隔符
                  if (token_idx > 0) {
                    if (OB_UNLIKELY(write_pos >= result_len)) {
                      ret = OB_SIZE_OVERFLOW;
                      LOG_WARN("buffer position check failed", K(ret), K(write_pos), K(result_len));
                      break;
                    }
                    buf[write_pos++] = ',';
                  }

                  if (OB_FAIL(ret)) {
                  } else if (OB_UNLIKELY(write_pos + token_len > result_len)) {
                    ret = OB_SIZE_OVERFLOW;
                    LOG_WARN("buffer size check failed", K(ret), K(write_pos), K(token_len), K(result_len));
                  } else {
                    MEMCPY(buf + write_pos, str + token_start, token_len);
                    write_pos += token_len;
                    token_idx++;
                  }
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
  }

  return ret;
}

int ObExprWhitespaceTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                                     const ObRawExpr &raw_expr,
                                     ObExpr &rt_expr) const
{
  UNUSED(op_cg_ctx);
  UNUSED(raw_expr);
  rt_expr.eval_func_ = eval_whitespace_tokenize;
  return OB_SUCCESS;
}

} // namespace sql
} // namespace oceanbase
