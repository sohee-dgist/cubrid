/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * query_rewrite.h - Query rewrite utils - Do Not Include Except This Folder
 */

#ifndef _QUERY_REWRITER_UTIL_H_
#define _QUERY_REWRITER_UTIL_H_

#define QO_CHECK_AND_REDUCE_EQUALITY_TERMS(parser, node, where) \
	do                                                            \
	{                                                             \
		if (!node->flag.done_reduce_equality_terms)                 \
		{                                                           \
			node->flag.done_reduce_equality_terms = true;             \
			qo_reduce_equality_terms(parser, node, where);            \
		}                                                           \
	} while (0)


#define PROCESS_IF_EXISTS(parser, condition, func) \
	do                                                 \
	{                                                  \
		if (*(condition))                                \
		{                                                \
			func(parser, *condition);                      \
		}                                                \
	} while (0)

#define  IS_CONDITIONAL_JOIN(type) ((type) == PT_JOIN_INNER || \
                                (type) == PT_JOIN_LEFT_OUTER || \
                                (type) == PT_JOIN_RIGHT_OUTER)


#define PT_IS_EXPR_NODE_WITH_COMP_OP(n) \
        ( (PT_IS_EXPR_NODE (n)) && \
          ((n)->info.expr.op == PT_EQ || \
           (n)->info.expr.op == PT_GE || \
           (n)->info.expr.op == PT_GT || \
           (n)->info.expr.op == PT_LT || \
           (n)->info.expr.op == PT_LE || \
           (n)->info.expr.op == PT_GT_INF || \
           (n)->info.expr.op == PT_LT_INF || \
           (n)->info.expr.op == PT_RANGE ))


int qo_is_reduceable_const (PT_NODE * expr);
PT_NODE *qo_get_name_by_spec_id (PARSER_CONTEXT * parser, PT_NODE * node, void *arg, int *continue_walk);
PT_NODE *qo_check_nullable_expr_with_spec (PARSER_CONTEXT * parser, PT_NODE * node, void *arg, int *continue_walk);
bool qo_check_condition_yields_null (PARSER_CONTEXT * parser, PT_NODE * path_spec, PT_NODE * query_where);
int qo_is_oid_const (PT_NODE * node);
void qo_move_on_clause_of_explicit_join_to_where_clause (PARSER_CONTEXT * parser, PT_NODE ** fromp, PT_NODE ** wherep);
PT_NODE *qo_rewrite_hidden_col_as_derived (PARSER_CONTEXT * parser, PT_NODE * node, PT_NODE * parent_node);
void qo_rewrite_index_hints (PARSER_CONTEXT * parser, PT_NODE * statement);

#endif /* _QUERY_REWRITER_UTIL_H_ */
