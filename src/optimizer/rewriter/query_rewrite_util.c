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
 * query_rewrite_util.c - Query rewrite utils - Do Not Include Except This Folder
 */

#ident "$Id$"

#include <assert.h>
#include "query_rewrite.h"
#include "query_rewrite_util.h"


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

/*
 * qo_is_reduceable_const () -
 *   return:
 *   expr(in):
 */
int
qo_is_reduceable_const (PT_NODE * expr)
{
  while (expr && expr->node_type == PT_EXPR)
    {
      if (expr->info.expr.op == PT_CAST || expr->info.expr.op == PT_TO_ENUMERATION_VALUE)
	{
	  expr = expr->info.expr.arg1;
	}
      else
	{
	  return false;		/* give up */
	}
    }

  return PT_IS_CONST_INPUT_HOSTVAR (expr);
}


/* 
 * qo_get_name_by_spec_id () - looks for a name with a matching id
 *   return: PT_NODE *
 *   parser(in): parser environment
 *   node(in): (name) node to compare id's with
 *   arg(in): info of spec and result
 *   continue_walk(in):
 */
PT_NODE *
qo_get_name_by_spec_id (PARSER_CONTEXT * parser, PT_NODE * node, void *arg, int *continue_walk)
{
  SPEC_ID_INFO *info = (SPEC_ID_INFO *) arg;

  if (node->node_type == PT_NAME && node->info.name.spec_id == info->id)
    {
      *continue_walk = PT_STOP_WALK;
      info->appears = true;
    }

  return node;
}

/*
 * qo_check_nullable_expr () -
 *   return:
 *   parser(in):
 *   node(in):
 *   arg(in):
 *   continue_walk(in):
 */
PT_NODE *
qo_check_nullable_expr (PARSER_CONTEXT * parser, PT_NODE * node, void *arg, int *continue_walk)
{
  int *nullable_cntp = (int *) arg;

  if (node->node_type == PT_EXPR)
    {
      /* check for nullable term: expr(..., NULL, ...) can be non-NULL */
      switch (node->info.expr.op)
	{
	case PT_IS_NULL:
	case PT_CASE:
	case PT_COALESCE:
	case PT_NVL:
	case PT_NVL2:
	case PT_DECODE:
	case PT_IF:
	case PT_IFNULL:
	case PT_ISNULL:
	case PT_CONCAT_WS:
	case PT_NULLSAFE_EQ:
	  /* NEED FUTURE OPTIMIZATION */
	  (*nullable_cntp)++;
	  break;
	default:
	  break;
	}
    }

  return node;
}

/*
 * qo_check_nullable_expr_with_spec () -
 *   return:
 *   parser(in):
 *   node(in):
 *   arg(in):
 *   continue_walk(in):
 */
PT_NODE *
qo_check_nullable_expr_with_spec (PARSER_CONTEXT * parser, PT_NODE * node, void *arg, int *continue_walk)
{
  SPEC_ID_INFO *info = (SPEC_ID_INFO *) arg;

  if (node->node_type == PT_EXPR)
    {
      /* check for nullable term: expr(..., NULL, ...) can be non-NULL */
      switch (node->info.expr.op)
	{
	case PT_IS_NULL:
	case PT_CASE:
	case PT_COALESCE:
	case PT_NVL:
	case PT_NVL2:
	case PT_DECODE:
	case PT_IF:
	case PT_IFNULL:
	case PT_ISNULL:
	case PT_CONCAT_WS:
	  info->appears = false;
	  parser_walk_tree (parser, node, qo_get_name_by_spec_id, info, NULL, NULL);
	  if (info->appears)
	    {
	      info->nullable = true;
	      *continue_walk = PT_STOP_WALK;
	    }
	  break;
	default:
	  break;
	}
    }

  return node;
}


/*
 * qo_replace_spec_name_with_null () - replace spec names with PT_TYPE_NULL pt_values
 *   return: PT_NODE *
 *   parser(in): parser environment
 *   node(in): (name) node to compare id's with
 *   arg(in): spec
 *   continue_walk(in):
 */
static PT_NODE *
qo_replace_spec_name_with_null (PARSER_CONTEXT * parser, PT_NODE * node, void *arg, int *continue_walk)
{
  PT_NODE *spec = (PT_NODE *) arg;
  PT_NODE *name;

  if (node->node_type == PT_NAME && node->info.name.spec_id == spec->info.spec.id)
    {
      node->node_type = PT_VALUE;
      node->type_enum = PT_TYPE_NULL;
    }

  if (node->node_type == PT_DOT_ && (name = node->info.dot.arg2) && name->info.name.spec_id == spec->info.spec.id)
    {
      parser_free_tree (parser, name);
      parser_free_tree (parser, node->info.expr.arg1);
      node->node_type = PT_VALUE;
      node->type_enum = PT_TYPE_NULL;
      /* By changing this node, we need to null the value container so that we protect parts of the code that ignore
       * type_enum set to PT_TYPE_NULL.  This is particularly problematic on PCs since they have different alignment
       * requirements. */
      node->info.value.data_value.set = NULL;
    }

  return node;
}


/*
 * qo_check_condition_yields_null () -
 *   return:
 *   parser(in): parser environment
 *   path_spec(in): to test attributes as NULL
 *   query_where(in): clause to evaluate
 */
bool
qo_check_condition_yields_null (PARSER_CONTEXT * parser, PT_NODE * path_spec, PT_NODE * query_where)
{
  PT_NODE *where;
  bool result = false;
  SEMANTIC_CHK_INFO sc_info = { NULL, NULL, 0, 0, 0, false, false };

  if (query_where == NULL)
    {
      return result;
    }

  where = parser_copy_tree_list (parser, query_where);
  where = parser_walk_tree (parser, where, qo_replace_spec_name_with_null, path_spec, NULL, NULL);

  sc_info.top_node = where;
  sc_info.donot_fold = false;
  where = pt_semantic_type (parser, where, &sc_info);
  result = pt_false_search_condition (parser, where);
  parser_free_tree (parser, where);

  /*
   * Ignore any error returned from semantic type check.
   * Just wanted to evaluate where clause with nulled spec names.
   */
  if (pt_has_error (parser))
    {
      parser_free_tree (parser, parser->error_msgs);
      parser->error_msgs = NULL;
    }

  return result;
}


/*
 * qo_is_oid_const () -
 *   return: Returns true iff the argument looks like a constant for
 *	     the purposes f the oid equality rewrite optimization
 *   node(in):
 */
int
qo_is_oid_const (PT_NODE * node)
{
  if (node == NULL)
    {
      return 0;
    }

  switch (node->node_type)
    {
    case PT_VALUE:
    case PT_HOST_VAR:
      return 1;

    case PT_NAME:
      /*
       * This *could* look to see if the name is correlated to the same
       * level as the caller, but that's going to require more context
       * to come in...
       */
      return node->info.name.meta_class == PT_PARAMETER;

    case PT_FUNCTION:
      if (node->info.function.function_type != F_SET && node->info.function.function_type != F_MULTISET
	  && node->info.function.function_type != F_SEQUENCE)
	{
	  return 0;
	}
      else
	{
	  /*
	   * The is the case for an expression like
	   *
	   *  {:a, :b, :c}
	   *
	   * Here the the expression '{:a, :b, :c}' comes in as a
	   * sequence function call, with PT_NAMEs 'a', 'b', and 'c' as
	   * its arglist.
	   */
	  PT_NODE *p;

	  for (p = node->info.function.arg_list; p; p = p->next)
	    {
	      if (!qo_is_oid_const (p))
		return 0;
	    }
	  return 1;
	}

    case PT_SELECT:
    case PT_UNION:
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
      return node->info.query.correlation_level != 1;

    default:
      return 0;
    }
}




/*
 * qo_move_on_clause_of_explicit_join_to_where_clause () - move on clause of explicit join to where clause
 *   return: void
 *   parser(in): parser environment
 *   fromp(in/out): &from of SELECT, &spec of UPDATE/DELETE
 *   wherep(in/out): &where of SELECT/UPDATE/DELETE
 *
 * NOTE: It moves on clause of explicit join for SELECT/UPDATE/DELETE to where clase for temporary purpose.
 *       qo_optimize_queries_post will restore them after several optimizations, for instance, range merge/intersection,
 *       auto-parameterization.
 *
 */
void
qo_move_on_clause_of_explicit_join_to_where_clause (PARSER_CONTEXT * parser, PT_NODE ** fromp, PT_NODE ** wherep)
{
  PT_NODE *t_node, *spec;

  t_node = *wherep;
  while (t_node != NULL && t_node->next != NULL)
    {
      t_node = t_node->next;
    }

  for (spec = *fromp; spec != NULL; spec = spec->next)
    {
      if (spec->node_type == PT_SPEC && spec->info.spec.on_cond != NULL)
	{
	  if (t_node == NULL)
	    {
	      t_node = *wherep = spec->info.spec.on_cond;
	    }
	  else
	    {
	      t_node->next = spec->info.spec.on_cond;
	    }

	  spec->info.spec.on_cond = NULL;

	  while (t_node->next != NULL)
	    {
	      t_node = t_node->next;
	    }
	}
    }
}

/*
 * qo_is_partition_attr () -
 *   return:
 *   node(in):
 */
static int
qo_is_partition_attr (PT_NODE * node)
{
  if (node == NULL)
    {
      return 0;
    }

  node = pt_get_end_path_node (node);

  if (node->node_type == PT_NAME && node->info.name.meta_class == PT_NORMAL && node->info.name.spec_id)
    {
      if (node->info.name.partition)
	{
	  return 1;
	}
    }

  return 0;
}


/*
 * qo_rewrite_hidden_col_as_derived () - Rewrite subquery with ORDER BY
 *				      hidden column as derived one
 *   return: PT_NODE *
 *   parser(in):
 *   node(in): QUERY node
 *   parent_node(in):
 *
 * Note: Keep out hidden column from derived select list
 */
PT_NODE *
qo_rewrite_hidden_col_as_derived (PARSER_CONTEXT * parser, PT_NODE * node, PT_NODE * parent_node)
{
  PT_NODE *t_node, *next, *derived;

  switch (node->node_type)
    {
    case PT_SELECT:
      if (node->info.query.order_by)
	{
	  bool remove_order_by = true;	/* guessing */

	  /* check parent context */
	  if (parent_node)
	    {
	      switch (parent_node->node_type)
		{
		case PT_FUNCTION:
		  switch (parent_node->info.function.function_type)
		    {
		    case F_TABLE_SEQUENCE:
		      remove_order_by = false;
		      break;
		    default:
		      break;
		    }
		  break;
		default:
		  break;
		}
	    }
	  else
	    {
	      remove_order_by = false;
	    }

	  /* check node context */
	  if (remove_order_by == true)
	    {
	      if (node->info.query.orderby_for)
		{
		  remove_order_by = false;
		}
	    }

	  if (remove_order_by == true)
	    {
	      for (t_node = node->info.query.q.select.list; t_node; t_node = t_node->next)
		{
		  if (t_node->node_type == PT_EXPR && t_node->info.expr.op == PT_ORDERBY_NUM)
		    {
		      remove_order_by = false;
		      break;
		    }
		}
	    }

	  /* remove unnecessary ORDER BY clause */
	  if (remove_order_by == true && !node->info.query.q.select.connect_by)
	    {
	      parser_free_tree (parser, node->info.query.order_by);
	      node->info.query.order_by = NULL;

	      for (t_node = node->info.query.q.select.list; t_node && t_node->next; t_node = next)
		{
		  next = t_node->next;
		  if (next->flag.is_hidden_column)
		    {
		      parser_free_tree (parser, next);
		      t_node->next = NULL;
		      break;
		    }
		}
	    }
	  else
	    {
	      /* Check whether we can rewrite query as derived. */
	      bool skip_query_rewrite_as_derived = false;
	      if (node->info.query.is_subquery == PT_IS_SUBQUERY && node->info.query.order_by != NULL)
		{
		  /* If all nodes in select list are hidden columns, we do not rewrite the query as derived
		   * since we want to avoid null select list. This will avoid the crash for queries like:
		   * set @a = 1; SELECT  (SELECT @a := @a + 1 FROM db_root ORDER BY @a + 1)
		   */
		  skip_query_rewrite_as_derived = true;
		  for (t_node = node->info.query.q.select.list; t_node; t_node = t_node->next)
		    {
		      if (!t_node->flag.is_hidden_column)
			{
			  skip_query_rewrite_as_derived = false;
			}
		    }
		}

	      if (!skip_query_rewrite_as_derived)
		{
		  for (t_node = node->info.query.q.select.list; t_node; t_node = t_node->next)
		    {
		      if (t_node->flag.is_hidden_column)
			{
			  /* make derived query */
			  derived = mq_rewrite_query_as_derived (parser, node);
			  if (derived == NULL)
			    {
			      break;
			    }

			  PT_NODE_MOVE_NUMBER_OUTERLINK (derived, node);
			  derived->info.query.q.select.flavor = node->info.query.q.select.flavor;
			  derived->info.query.is_subquery = node->info.query.is_subquery;
			  derived->type_enum = node->type_enum;

			  /* free old composite query */
			  parser_free_tree (parser, node);
			  node = derived;
			  break;
			}
		    }
		}
	    }			/* else */
	}
      break;

    case PT_UNION:
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
      node->info.query.q.union_.arg1 = qo_rewrite_hidden_col_as_derived (parser, node->info.query.q.union_.arg1, NULL);
      node->info.query.q.union_.arg2 = qo_rewrite_hidden_col_as_derived (parser, node->info.query.q.union_.arg2, NULL);
      break;
    default:
      return node;
    }

  return node;
}

/*
 * qo_rewrite_index_hints () - Rewrite index hint list, removing useless hints
 *   return: PT_NODE *
 *   parser(in):
 *   node(in): QUERY node
 *   parent_node(in):
 */
void
qo_rewrite_index_hints (PARSER_CONTEXT * parser, PT_NODE * statement)
{
  PT_NODE *using_index = NULL, *hint_node, *prev_node, *next_node;

  bool is_sorted, is_idx_reversed, is_idx_match_nokl, is_hint_masked;

  PT_NODE *hint_none, *root_node;
  PT_NODE dummy_hint_local, *dummy_hint;

  switch (statement->node_type)
    {
    case PT_SELECT:
      using_index = statement->info.query.q.select.using_index;
      break;
    case PT_UPDATE:
      using_index = statement->info.update.using_index;
      break;
    case PT_DELETE:
      using_index = statement->info.delete_.using_index;
      break;
    default:
      /* USING index clauses are not allowed for other query types */
      assert (false);
      return;
    }

  if (using_index == NULL)
    {
      /* no index hints, nothing to do here */
      return;
    }

  /* Main logic - we can safely assume that pt_check_using_index() has already checked for possible semantic errors or
   * incompatible index hints. */

  /* basic rewrite, for USING INDEX NONE */
  hint_node = using_index;
  prev_node = NULL;
  hint_none = NULL;
  while (hint_node != NULL)
    {
      if (hint_node->etc == (void *) PT_IDX_HINT_NONE)
	{
	  hint_none = hint_node;
	  break;
	}
      prev_node = (prev_node == NULL) ? hint_node : prev_node->next;
      hint_node = hint_node->next;
    }

  if (hint_none != NULL)
    {
      /* keep only the using_index_none hint stored in hint_none */
      /* update links and discard the first part of the hint list */
      if (prev_node != NULL)
	{
	  prev_node->next = NULL;
	  parser_free_tree (parser, using_index);
	  using_index = NULL;
	}
      /* update links and discard the last part of the hint list */
      hint_node = hint_none->next;
      if (hint_node != NULL)
	{
	  parser_free_tree (parser, hint_node);
	  hint_node = NULL;
	}
      /* update links and keep only the USING INDEX NONE node */
      hint_none->next = NULL;
      using_index = hint_none;
      goto exit;
    }

  if (using_index->etc == (void *) PT_IDX_HINT_ALL_EXCEPT)
    {
      /* find all t.none index hints and mark them for later removal */
      /* the first node, when USING INDEX ALL EXCEPT, is a '*', so use this node as a constant list root */
      hint_node = using_index;
      while (hint_node != NULL && (next_node = hint_node->next) != NULL)
	{
	  if (next_node->info.name.original == NULL && next_node->info.name.resolved != NULL
	      && strcmp (next_node->info.name.resolved, "*") != 0)
	    {
	      /* found a t.none identifier; remove it from the list */
	      hint_node->next = next_node->next;
	      next_node->next = NULL;
	      parser_free_node (parser, next_node);
	    }
	  else
	    {
	      hint_node = hint_node->next;
	    }
	}

      /* if only the '*' marker node is left in the list, it means that USING INDEX ALL EXCEPT contains only
       * t.none-like hints, so it is actually an empty hint list */
      if (using_index->next == NULL)
	{
	  parser_free_node (parser, using_index);
	  using_index = NULL;
	  goto exit;
	}

      root_node = prev_node = using_index;
      hint_node = using_index->next;
    }
  else
    {
      /* there is no USING INDEX {NONE|ALL EXCEPT ...} in the query; the dummy node is necessary for faster operation;
       * use local variable dummy_hint */
      dummy_hint = &dummy_hint_local;
      dummy_hint->next = using_index;
      /* just need something else than PT_IDX_HINT_ALL AEXCEPT, so that this node won't be kept later */
      dummy_hint->etc = (void *) PT_IDX_HINT_USE;
      root_node = prev_node = dummy_hint;
      hint_node = using_index;
    }

  /* remove duplicate index hints and sort them; keep the same order for the hints of the same type with keylimit */
  /* order: class_none, ignored, forced, used */
  is_sorted = false;
  while (!is_sorted)
    {
      prev_node = root_node;
      hint_node = prev_node->next;
      is_sorted = true;
      while ((next_node = hint_node->next) != NULL)
	{
	  is_idx_reversed = false;
	  is_idx_match_nokl = false;
	  if (PT_IDX_HINT_ORDER (hint_node) > PT_IDX_HINT_ORDER (next_node))
	    {
	      is_idx_reversed = true;
	    }
	  else if (hint_node->etc == next_node->etc)
	    {
	      /* if hints have the same type, check if they need to be swapped or are identical and one of them needs
	       * to be removed */
	      int res_cmp_tbl_names = -1;
	      /* unless USING INDEX NONE, which is rewritten above, all indexes should have table names already
	       * resolved */
	      assert (hint_node->info.name.resolved != NULL && next_node->info.name.resolved != NULL);

	      /*
	       * Case of comparing names after dot(.).
	       * 1. When comparing owner_name.class_name and class_name.
	       *    class_name used in index_name must be in from. So, only class_name should be compared,
	       *    not owner_name.
	       *    e.g. select c1 from t1 force index (i1) where c1 >= 0 using index i1(+);
	       *      - resolved_name of "force index (i1)"  : "t1"
	       *      - resolved_name of "using index i1(+)" : "dba.t1"
	       */

	      /* compare the tables on which the indexes are defined */
	      res_cmp_tbl_names =
		pt_user_specified_name_compare (hint_node->info.name.resolved, next_node->info.name.resolved);

	      if (res_cmp_tbl_names == 0)
		{
		  /* also compare index names */
		  if (hint_node->info.name.original != NULL && next_node->info.name.original != NULL)
		    {
		      /* index names can be null if t.none */
		      int res_cmp_idx_names;

		      res_cmp_idx_names =
			intl_identifier_casecmp (hint_node->info.name.original, next_node->info.name.original);
		      if (res_cmp_idx_names == 0)
			{
			  is_idx_match_nokl = true;
			}
		      else
			{
			  is_idx_reversed = (res_cmp_idx_names > 0);
			}
		    }
		  else
		    {
		      /* hints are of the same type, name.original is either NULL or not NULL for both hints */
		      assert (hint_node->info.name.original == NULL && next_node->info.name.original == NULL);
		      /* both hints are "same-table.none"; identical */
		      is_idx_match_nokl = true;
		    }
		}
	      else
		{
		  is_idx_reversed = (res_cmp_tbl_names > 0);
		}

	      if (is_idx_match_nokl)
		{
		  /* The same index is used in both hints; examine the keylimit clauses; if search_node does not have
		   * keylimit, the IF below will skip, and search_node will be deleted */
		  if (next_node->info.name.indx_key_limit != NULL)
		    {
		      /* search_node has keylimit */
		      if (hint_node->info.name.indx_key_limit != NULL)
			{
			  /* hint_node has keylimit; no action is performed; we want to preserve the order of index
			   * hints for the same index, with keylimit */
			  is_idx_reversed = false;
			  is_idx_match_nokl = false;
			}
		      else
			{
			  /* special case; need to delete hint_node and keep search_node, because this one has
			   * keylimit; */
			  assert (!is_idx_reversed);
			  is_idx_reversed = true;
			  /* reverse the two nodes so the code below can be reused for this situation */
			}
		    }		/* endif (search_node) */
		}		/* endif (is_idx_match_nokl) */
	    }

	  if (is_idx_reversed)
	    {
	      /* Interchange the two hints */
	      hint_node->next = next_node->next;
	      next_node->next = hint_node;
	      prev_node->next = next_node;
	      is_sorted = false;
	      /* update hint_node and search_node, for possible delete */
	      hint_node = prev_node->next;
	      next_node = hint_node->next;
	    }

	  if (is_idx_match_nokl)
	    {
	      /* remove search_node */
	      hint_node->next = next_node->next;
	      next_node->next = NULL;
	      parser_free_node (parser, next_node);
	      /* node removed, use prev_node and hint_node in next loop */
	      continue;
	    }
	  prev_node = prev_node->next;
	  hint_node = prev_node->next;
	}
    }

  /* Find index hints to remove later. At this point, the only index hints that can be found in using_index are
   * {USE|FORCE|IGNORE} INDEX and USING INDEX {idx|idx(-)|idx(+)|t.none}... Need to ignore duplicate hints, and hints
   * that are masked by applying the hint operation rules. */
  hint_node = root_node->next;
  while (hint_node != NULL)
    {
      next_node = hint_node->next;
      prev_node = hint_node;
      while (next_node != NULL)
	{
	  if (next_node->etc == hint_node->etc)
	    {
	      /* same hint type; duplicates were already removed, skip hint */
	      prev_node = next_node;
	      next_node = next_node->next;
	      continue;
	    }

	  /* Main logic for removing redundant/masked index hints */
	  /* The hint list is now sorted, first by index type, then by table and index name, so the next_node type is
	   * the same as hint_node or lower in importance (class.none > ignore > force > use), so it is not necessary
	   * to check next_index hint type */
	  is_hint_masked = false;

	  if ((hint_node->etc == (void *) PT_IDX_HINT_CLASS_NONE
	       || ((hint_node->etc == (void *) PT_IDX_HINT_IGNORE || hint_node->etc == (void *) PT_IDX_HINT_FORCE)
		   && (intl_identifier_casecmp (hint_node->info.name.original, next_node->info.name.original) == 0)))
	      && (pt_user_specified_name_compare (hint_node->info.name.resolved, next_node->info.name.resolved) == 0))
	    {
	      is_hint_masked = true;
	    }

	  if (is_hint_masked)
	    {
	      /* hint search_node is masked; remove it from the hint list */
	      prev_node->next = next_node->next;
	      next_node->next = NULL;
	      parser_free_node (parser, next_node);
	      next_node = prev_node;
	    }
	  prev_node = next_node;
	  next_node = next_node->next;
	}
      hint_node = hint_node->next;
    }

  /* remove the dummy first node, if any */
  if (root_node->etc != (void *) PT_IDX_HINT_ALL_EXCEPT)
    {
      using_index = root_node->next;
      root_node->next = NULL;
    }
  else
    {
      using_index = root_node;
    }

exit:
  /* Save changes to query node */
  switch (statement->node_type)
    {
    case PT_SELECT:
      statement->info.query.q.select.using_index = using_index;
      break;
    case PT_UPDATE:
      statement->info.update.using_index = using_index;
      break;
    case PT_DELETE:
      statement->info.delete_.using_index = using_index;
      break;
    default:
      break;
    }
}
