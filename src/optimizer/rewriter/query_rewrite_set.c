#ident "$Id$"

#include <assert.h>
#include "parser.h"
#include "parser_message.h"
#include "parse_tree.h"
#include "optimizer.h"
#include "xasl_generation.h"
#include "virtual_object.h"
#include "system_parameter.h"
#include "semantic_check.h"
#include "execute_schema.h"
#include "view_transform.h"
#include "parser.h"
#include "object_primitive.h"
#include "object_representation.h"

#include "dbtype.h"
#include "query_rewrite.h"
#include "query_rewrite_util.h"

static bool qo_check_distinct_union (PARSER_CONTEXT * parser, PT_NODE * node);
static bool qo_check_hint_union (PARSER_CONTEXT * parser, PT_NODE * node, PT_HINT_ENUM hint);

static PT_NODE *qo_push_limit_to_union (PARSER_CONTEXT * parser, PT_NODE * node, PT_NODE * limit);
/*
 * qo_rewrite_union_with_limit_clause () - qo_rewrite_union_with_limit_clause
 *   return: PT_NODE *
 *   parser(in): parser environment
 *   node(in): possible query
 *   If LIMIT clause is specified without ORDER BY clause, we will rewrite the UNION query as derived.
 *   
 *   Example:
 *   (SELECT ...) UNION (SELECT ...) LIMIT 10 
 *   will be rewritten to ->
 *   SELECT * FROM ((SELECT ...) UNION (SELECT ...)) T WHERE INST_NUM() <= 10
 */

void
qo_rewrite_union_with_limit_clause (PARSER_CONTEXT * parser, PT_NODE * node, PT_NODE ** wherep)
{
  PT_NODE *limit_node, *derived, *limit;
  bool single_tuple_bak;

  limit = pt_limit_to_numbering_expr (parser, node->info.query.limit, PT_INST_NUM, false);
  if (limit == NULL)
    {
      return;
    }

  node->info.query.flag.rewrite_limit = 0;

  /* to move limit clause to derived */
  limit_node = node->info.query.limit;
  node->info.query.limit = NULL;

  /* to move single tuple to derived */
  single_tuple_bak = node->info.query.flag.single_tuple;
  node->info.query.flag.single_tuple = false;

  /* push limit to union */
  if (node->info.query.order_by == NULL && !qo_check_distinct_union (parser, node)
      && !qo_check_hint_union (parser, node, PT_HINT_NO_PUSH_PRED))
    {
      node = qo_push_limit_to_union (parser, node, limit_node);
    }

  derived = mq_rewrite_query_as_derived (parser, node);
  if (derived != NULL)
    {
      PT_NODE_MOVE_NUMBER_OUTERLINK (derived, node);

      assert (derived->info.query.q.select.where == NULL);

      derived->info.query.q.select.where = limit;
      wherep = &derived->info.query.q.select.where;
      node = derived;
    }
  node->info.query.flag.single_tuple = single_tuple_bak;
  node->info.query.limit = limit_node;
}

/*
 * qo_push_limit_to_union () - push limit on union query
 *   return: PT_NODE *
 *   parser(in): parser environment
 *   node(in): possible query
 *   arg(in):
 *   continue_walk(in):
 */
static PT_NODE *
qo_push_limit_to_union (PARSER_CONTEXT * parser, PT_NODE * node, PT_NODE * limit)
{
  PT_NODE *save_next, *add_limit;

  switch (node->node_type)
    {
    case PT_SELECT:
      if (!pt_has_inst_or_orderby_num_in_where (parser, node) && node->info.query.limit == NULL)
	{
	  /* case of limit 10,10 */
	  if (limit->next)
	    {
	      /* change 'limit 10,10' to 'limit 10 + 10' */
	      /* generate 'limit 10 + 10' */
	      if (!(add_limit = parser_new_node (parser, PT_EXPR)))
		{
		  PT_ERRORm (parser, add_limit, MSGCAT_SET_PARSER_SEMANTIC, MSGCAT_SEMANTIC_OUT_OF_MEMORY);
		  return NULL;
		}
	      /* cut off limit->next */
	      save_next = limit->next;
	      limit->next = NULL;

	      add_limit->type_enum = limit->type_enum;
	      add_limit->info.expr.op = PT_PLUS;
	      add_limit->info.expr.arg1 = parser_copy_tree (parser, save_next);
	      add_limit->info.expr.arg2 = parser_copy_tree (parser, limit);
	      limit->next = save_next;

	      node->info.query.limit = add_limit;
	    }
	  else
	    {
	      node->info.query.limit = parser_copy_tree (parser, limit);
	    }

	  node->info.query.flag.rewrite_limit = 1;
	  return node;
	}
      break;

    case PT_UNION:
      qo_push_limit_to_union (parser, node->info.query.q.union_.arg1, limit);
      qo_push_limit_to_union (parser, node->info.query.q.union_.arg2, limit);
      break;

    default:
      break;
    }
  return node;
}

/*
 * qo_check_distinct_union () - check having distinct in union clause
 *   return: PT_NODE *
 *   parser(in): parser environment
 *   node(in): possible query
 *   arg(in):
 *   continue_walk(in):
 */
static bool
qo_check_distinct_union (PARSER_CONTEXT * parser, PT_NODE * node)
{
  bool result = false;

  switch (node->node_type)
    {
    case PT_UNION:
      if (node->info.query.all_distinct == PT_DISTINCT)
	{
	  return true;
	}

      result |= qo_check_distinct_union (parser, node->info.query.q.union_.arg1);
      result |= qo_check_distinct_union (parser, node->info.query.q.union_.arg2);
      break;

    default:
      break;
    }
  return result;
}

/*
 * qo_check_hint_union () - check having distinct in union clause
 *   return: PT_NODE *
 *   parser(in): parser environment
 *   node(in): possible query
 *   arg(in):
 *   continue_walk(in):
 */
static bool
qo_check_hint_union (PARSER_CONTEXT * parser, PT_NODE * node, PT_HINT_ENUM hint)
{
  bool result = false;

  switch (node->node_type)
    {
    case PT_SELECT:
      if (node->info.query.q.select.hint & hint)
	{
	  return true;
	}
      break;
    case PT_UNION:
      result |= qo_check_hint_union (parser, node->info.query.q.union_.arg1, hint);
      result |= qo_check_hint_union (parser, node->info.query.q.union_.arg2, hint);
      break;

    default:
      break;
    }
  return result;
}
