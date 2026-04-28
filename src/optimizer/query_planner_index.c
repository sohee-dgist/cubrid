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

#ident "$Id$"

#include "config.h"
#include "query_planner_internal.h"

bool
qo_is_seq_scan (QO_PLAN * plan)
{
  if (plan && plan->plan_type == QO_PLANTYPE_SCAN && plan->plan_un.scan.scan_method == QO_SCANMETHOD_SEQ_SCAN)
    {
      return true;
    }

  return false;
}

bool
qo_is_iscan (QO_PLAN * plan)
{
  if (plan && plan->plan_type == QO_PLANTYPE_SCAN
      && (plan->plan_un.scan.scan_method == QO_SCANMETHOD_INDEX_SCAN
	  || plan->plan_un.scan.scan_method == QO_SCANMETHOD_INDEX_SCAN_INSPECT))
    {
      return true;
    }

  return false;
}

int
qo_index_cardinality (QO_ENV * env, PT_NODE * attr)
{
  PT_NODE *dummy;
  QO_NODE *nodep;
  QO_SEGMENT *segp;
  QO_ATTR_INFO *info;

  if (attr->node_type == PT_DOT_)
    {
      attr = attr->info.dot.arg2;
    }

  QO_ASSERT (env, (attr->node_type == PT_NAME || pt_is_function_index_expression (attr)));

  nodep = lookup_node (attr, env, &dummy);
  if (nodep == NULL)
    {
      return 0;
    }

  segp = lookup_seg (nodep, attr, env);
  if (segp == NULL)
    {
      return 0;
    }

  if (attr->info.name.meta_class == PT_RESERVED)
    {
      return 0;
    }

  info = QO_SEG_INFO (segp);
  if (info == NULL)
    {
      return 0;
    }

  if (info->ndv > 0)
    {
      int ndv = (info->ndv > INT_MAX) ? INT_MAX : info->ndv;	/* need to change type to INT64 */

      if (info->cum_stats.is_indexed == true && info->cum_stats.pkeys[0] > 0)
	{
	  /* Choose the better NDV of the two. */
	  return MIN (ndv, info->cum_stats.pkeys[0]);
	}
      return ndv;
    }

  if (info->cum_stats.is_indexed != true)
    {
      return 0;
    }

  QO_ASSERT (env, info->cum_stats.pkeys_size > 0);
  QO_ASSERT (env, info->cum_stats.pkeys_size <= BTREE_STATS_PKEYS_NUM);
  QO_ASSERT (env, info->cum_stats.pkeys != NULL);

  /* return number of the first partial-key of the index on the attribute shown in the expression */
  return info->cum_stats.pkeys[0];
}

int
qo_index_cardinality_with_dedup (QO_ENV * env, PT_NODE * attr, BITSET * seg_bitset)
{
  PT_NODE *dummy;
  QO_NODE *nodep;
  QO_SEGMENT *segp;
  QO_ATTR_INFO *info;

  if (attr->node_type == PT_DOT_)
    {
      attr = attr->info.dot.arg2;
    }

  QO_ASSERT (env, (attr->node_type == PT_NAME || pt_is_function_index_expression (attr)));

  nodep = lookup_node (attr, env, &dummy);
  if (nodep == NULL)
    {
      return 0;
    }

  segp = lookup_seg (nodep, attr, env);
  if (segp == NULL)
    {
      return 0;
    }

  /* check if there are duplicate columns */
  if (seg_bitset)
    {
      if (BITSET_MEMBER (*seg_bitset, QO_SEG_IDX (segp)))
	{
	  return 0;
	}
      else
	{
	  bitset_add (seg_bitset, QO_SEG_IDX (segp));
	}
    }

  if (attr->info.name.meta_class == PT_RESERVED)
    {
      return 0;
    }

  info = QO_SEG_INFO (segp);
  if (info == NULL)
    {
      return 0;
    }

  if (info->ndv > 0)
    {
      int ndv = (info->ndv > INT_MAX) ? INT_MAX : info->ndv;	/* need to change type to INT64 */

      if (info->cum_stats.is_indexed == true && info->cum_stats.pkeys[0] > 0)
	{
	  /* Choose the better NDV of the two. */
	  return MIN (ndv, info->cum_stats.pkeys[0]);
	}
      return ndv;
    }

  if (info->cum_stats.is_indexed != true)
    {
      return 0;
    }

  QO_ASSERT (env, info->cum_stats.pkeys_size > 0);
  QO_ASSERT (env, info->cum_stats.pkeys_size <= BTREE_STATS_PKEYS_NUM);
  QO_ASSERT (env, info->cum_stats.pkeys != NULL);

  /* return number of the first partial-key of the index on the attribute shown in the expression */
  return info->cum_stats.pkeys[0];
}

bool
qo_is_all_unique_index_columns_are_equi_terms (QO_PLAN * plan)
{
  if (qo_is_iscan (plan) && plan->plan_un.scan.index && plan->plan_un.scan.index->head
      && (plan->plan_un.scan.index->head->all_unique_index_columns_are_equi_terms))
    {
      return true;
    }
  return false;
}

bool
qo_is_iscan_from_orderby (QO_PLAN * plan)
{
  if (plan && plan->plan_type == QO_PLANTYPE_SCAN && plan->plan_un.scan.scan_method == QO_SCANMETHOD_INDEX_ORDERBY_SCAN)
    {
      return true;
    }

  return false;
}

bool
qo_validate_index_term_notnull (QO_ENV * env, QO_INDEX_ENTRY * index_entryp)
{
  bool term_notnull = false;	/* init */
  PT_NODE *node;
  const char *node_name;
  QO_CLASS_INFO_ENTRY *index_class;
  int t;
  QO_TERM *termp;
  int iseg;
  QO_SEGMENT *segp;

  assert (env != NULL);
  assert (index_entryp != NULL);
  assert (index_entryp->class_ != NULL);

  index_class = index_entryp->class_;

  /* do a check on the first column - it should be present in the where clause check if exists a simple expression
   * with PT_IS_NOT_NULL on the first key this should not contain OR operator and the PT_IS_NOT_NULL should contain the
   * column directly as parameter (PT_NAME)
   */
  for (t = 0; t < env->nterms && !term_notnull; t++)
    {
      /* get the pointer to QO_TERM structure */
      termp = QO_ENV_TERM (env, t);
      assert (termp != NULL);
      if (QO_ON_COND_TERM (termp))
	{
	  continue;
	}

      node = QO_TERM_PT_EXPR (termp);
      if (node == NULL)
	{
	  continue;
	}

      if (node && node->or_next)
	{
	  continue;
	}

      if (node->node_type == PT_EXPR && node->info.expr.op == PT_IS_NOT_NULL
	  && node->info.expr.arg1->node_type == PT_NAME)
	{
	  iseg = index_entryp->seg_idxs[0];
	  if (iseg != -1 && BITSET_MEMBER (QO_TERM_SEGS (termp), iseg))
	    {
	      /* check it's the same column as the first in the index */
	      node_name = pt_get_name (node->info.expr.arg1);
	      segp = QO_ENV_SEG (env, iseg);
	      assert (segp != NULL);
	      if (!intl_identifier_casecmp (node_name, QO_SEG_NAME (segp)))
		{
		  /* we have found a term with no OR and with IS_NOT_NULL on our key. The plan is ready for group by
		   * skip!
		   */
		  term_notnull = true;
		  break;
		}
	    }
	}
    }

  return term_notnull;
}

bool
qo_validate_index_attr_notnull (QO_ENV * env, QO_INDEX_ENTRY * index_entryp, PT_NODE * col)
{
  bool attr_notnull = false;	/* init */
  QO_NODE *node;
  PT_NODE *dummy;
  int i;
  QO_CLASS_INFO_ENTRY *index_class;
  QO_SEGMENT *segp = NULL;
  SM_ATTRIBUTE *attr;
  void *env_seg[2];

  /* key_term_status is -1 if no term with key, 0 if isnull or is not null terms with key and 1 if other term with key */
  int old_bail_out, key_term_status;

  assert (env != NULL);
  assert (index_entryp != NULL);
  assert (index_entryp->class_ != NULL);
  assert (index_entryp->class_->smclass != NULL);
  assert (col != NULL);

  index_class = index_entryp->class_;

  if (col->node_type != PT_NAME)
    {
      return false;		/* give up */
    }

  node = lookup_node (col, env, &dummy);
  if (node == NULL)
    {
      return false;
    }

  segp = lookup_seg (node, col, env);
  if (segp == NULL)
    {				/* is invalid case */
      assert (false);
      return false;
    }

  for (i = 0; i < index_entryp->col_num; i++)
    {
      if (index_entryp->seg_idxs[i] == QO_SEG_IDX (segp))
	{
	  break;		/* found */
	}
    }
  if (i >= index_entryp->col_num)
    {
      /* col is not included in this index */
      return false;
    }

  assert (segp != NULL);
#if !defined(NDEBUG)
  {
    const char *col_name = pt_get_name (col);

    assert (!intl_identifier_casecmp (QO_SEG_NAME (segp), col_name));
  }
#endif

  /* we now search in the class columns for the index key */
  for (i = 0; i < index_class->smclass->att_count; i++)
    {
      attr = &index_class->smclass->attributes[i];
      if (attr && !intl_identifier_casecmp (QO_SEG_NAME (segp), attr->header.name))
	{
	  if (attr->flags & SM_ATTFLAG_NON_NULL)
	    {
	      attr_notnull = true;
	    }
	  else
	    {
	      attr_notnull = false;
	    }

	  break;
	}
    }
  if (i >= index_class->smclass->att_count)
    {
      /* column wasn't found - this should not happen! */
      assert (false);
      return false;
    }

  /* now search for not terms with the key */
  if (attr_notnull != true)
    {
      /* save old value of bail_out */
      old_bail_out = env->bail_out;
      env->bail_out = -1;	/* no term found value */

      /* check for isnull terms with the key */
      env_seg[0] = (void *) env;
      env_seg[1] = (void *) segp;
      parser_walk_tree (env->parser, QO_ENV_PT_TREE (env)->info.query.q.select.where, qo_search_isnull_key_expr,
			env_seg, NULL, NULL);

      /* restore old value and keep walk_tree result in key_term_status */
      key_term_status = env->bail_out;
      env->bail_out = old_bail_out;

      /* if there is no isnull on the key, check that the key appears in some term and if so, make sure that that term
       * doesn't have a OR
       */
      if (key_term_status == 1)
	{
	  BITSET expr_segments, key_segment;
	  QO_TERM *termp;
	  PT_NODE *pt_expr;

	  bitset_init (&expr_segments, env);
	  bitset_init (&key_segment, env);

	  /* key segment bitset */
	  bitset_add (&key_segment, QO_SEG_IDX (segp));

	  /* key found in a term */
	  for (i = 0; i < env->nterms; i++)
	    {
	      termp = QO_ENV_TERM (env, i);
	      assert (termp != NULL);

	      pt_expr = QO_TERM_PT_EXPR (termp);
	      if (pt_expr == NULL)
		{
		  continue;
		}

	      if (pt_expr->or_next)
		{
		  BITSET_CLEAR (expr_segments);

		  qo_expr_segs (env, pt_expr, &expr_segments);
		  if (bitset_intersects (&expr_segments, &key_segment))
		    {
		      break;	/* give up */
		    }
		}
	    }

	  if (i >= env->nterms)
	    {
	      attr_notnull = true;	/* OK */
	    }

	  bitset_delset (&key_segment);
	  bitset_delset (&expr_segments);
	}
    }

  return attr_notnull;
}

int
qo_validate_index_for_orderby (QO_ENV * env, QO_NODE_INDEX_ENTRY * ni_entryp)
{
  bool key_notnull = false;	/* init */
  QO_INDEX_ENTRY *index_entryp;
  QO_CLASS_INFO_ENTRY *index_class;

  int pos;
  PT_NODE *node = NULL;

  assert (ni_entryp != NULL);
  assert (ni_entryp->head != NULL);
  assert (ni_entryp->head->class_ != NULL);

  index_entryp = ni_entryp->head;
  index_class = index_entryp->class_;

  if (!QO_ENV_PT_TREE (env) || !QO_ENV_PT_TREE (env)->info.query.order_by)
    {
      goto end;
    }

  key_notnull = qo_validate_index_term_notnull (env, index_entryp);
  if (key_notnull)
    {
      goto final_;
    }

  pos = QO_ENV_PT_TREE (env)->info.query.order_by->info.sort_spec.pos_descr.pos_no;
  node = QO_ENV_PT_TREE (env)->info.query.q.select.list;

  while (pos > 1 && node)
    {
      node = node->next;
      pos--;
    }
  if (!node)
    {
      goto end;
    }

  if (node->node_type == PT_EXPR && node->info.expr.op == PT_CAST)
    {
      node = node->info.expr.arg1;
      if (!node)
	{
	  goto end;
	}
    }

  node = pt_get_end_path_node (node);

  assert (key_notnull == false);

  key_notnull = qo_validate_index_attr_notnull (env, index_entryp, node);
  if (key_notnull)
    {
      goto final_;
    }

  /* Now we have the information we need: if the key column can be null and if there is a PT_IS_NULL or PT_IS_NOT_NULL
   * expression with this key column involved and also if we have other terms with the key. We must decide if there can
   * be NULLs in the results and if so, drop this index. 1. If the key cannot have null values, we have a winner. 2.
   * Otherwise, if we found a term isnull/isnotnull(key) we drop it (because we cannot evaluate if this yields true or
   * false so we skip all, for safety) 3. If we have a term with other operator except isnull/isnotnull and does not
   * have an OR following we have a winner again! (because we cannot have a null value).
   */
final_:
  if (key_notnull)
    {
      return 1;
    }
end:
  return 0;
}

PT_NODE *
qo_search_isnull_key_expr (PARSER_CONTEXT * parser, PT_NODE * tree, void *arg, int *continue_walk)
{
  BITSET expr_segments, key_segment;
  QO_ENV *env;
  QO_SEGMENT *segm;
  void **env_seg = (void **) arg;

  env = (QO_ENV *) env_seg[0];
  segm = (QO_SEGMENT *) env_seg[1];

  *continue_walk = PT_CONTINUE_WALK;

  /* key segment bitset */
  bitset_init (&key_segment, env);
  bitset_add (&key_segment, QO_SEG_IDX (segm));

  bitset_init (&expr_segments, env);
  if (tree->node_type == PT_EXPR)
    {
      /* get all segments in this expression */
      qo_expr_segs (env, tree, &expr_segments);

      /* now check if the key segment is in there */
      if (bitset_intersects (&expr_segments, &key_segment))
	{
	  int nullable_terms = 0;
	  qo_check_nullable_expr (parser, tree, &nullable_terms, NULL);
	  /* this expr contains the key segment */
	  if (nullable_terms >= 1)
	    {
	      /* 0 all the way, suppress other terms found */
	      env->bail_out = 0;
	      *continue_walk = PT_STOP_WALK;

	      return tree;
	    }
	  else if (env->bail_out == -1)
	    {
	      /* set as 1 only if we haven't found any isnull terms */
	      env->bail_out = 1;
	    }
	}
    }

  return tree;
}

PT_NODE *
qo_get_col_product_ndv (PARSER_CONTEXT * parser, PT_NODE * tree, void *arg, int *continue_walk)
{
  NDV_INFO *ndv_info = (NDV_INFO *) arg;
  int ndv;

  *continue_walk = PT_CONTINUE_WALK;

  if (PT_IS_QUERY_NODE_TYPE (tree->node_type))
    {
      *continue_walk = PT_LIST_WALK;
      return tree;
    }
  else if (pt_is_attr (tree))
    {
      ndv = qo_index_cardinality_with_dedup (ndv_info->env, pt_get_end_path_node (tree), &ndv_info->seg_bitset);
      ndv_info->total_ndv *= (ndv == 0) ? 1 : ndv;
    }

  return tree;
}

QO_PLAN_COMPARE_RESULT
qo_plan_iscan_terms_cmp (QO_PLAN * a, QO_PLAN * b)
{
  QO_NODE_INDEX_ENTRY *a_ni, *b_ni;
  QO_INDEX_ENTRY *a_ent, *b_ent;
  QO_ATTR_CUM_STATS *a_cum, *b_cum;
  int a_range, b_range;		/* num iscan range terms */
  int a_filter, b_filter;	/* num iscan filter terms */

  if (QO_NODE_IDX (a->plan_un.scan.node) != QO_NODE_IDX (b->plan_un.scan.node))
    {
      return PLAN_COMP_UNK;
    }

  if (!qo_is_interesting_order_scan (a) || !qo_is_interesting_order_scan (b))
    {
      assert_release (qo_is_interesting_order_scan (a));
      assert_release (qo_is_interesting_order_scan (b));

      return PLAN_COMP_UNK;
    }

  /* index entry of spec 'a' */
  a_ni = a->plan_un.scan.index;
  a_ent = (a_ni)->head;
  a_cum = &(a_ni)->cum_stats;
  assert (a_cum != NULL);

  /* index range terms */
  a_range = bitset_cardinality (&(a->plan_un.scan.terms));
  if (a_range > 0 && !(a->plan_un.scan.index_equi))
    {
      a_range--;		/* set the last equal range term */
    }

  /* index filter terms */
  a_filter = bitset_cardinality (&(a->plan_un.scan.kf_terms));

  /* index entry of spec 'b' */
  b_ni = b->plan_un.scan.index;
  b_ent = (b_ni)->head;
  b_cum = &(b_ni)->cum_stats;
  assert (b_cum != NULL);

  /* index range terms */
  b_range = bitset_cardinality (&(b->plan_un.scan.terms));
  if (b_range > 0 && !(b->plan_un.scan.index_equi))
    {
      b_range--;		/* set the last equal range term */
    }

  /* index filter terms */
  b_filter = bitset_cardinality (&(b->plan_un.scan.kf_terms));

  assert (a_range >= 0);
  assert (b_range >= 0);

  /* STEP 1: check by terms containment */

  if (bitset_is_equivalent (&(a->plan_un.scan.terms), &(b->plan_un.scan.terms)))
    {
      /* both plans have the same range terms we will check now the key filter terms */
      if (a_filter > b_filter)
	{
	  return PLAN_COMP_LT;
	}
      else if (a_filter < b_filter)
	{
	  return PLAN_COMP_GT;
	}

      /* prefer covering scan */
      if (qo_is_index_covering_scan (a) && !qo_is_index_covering_scan (b))
	{
	  return PLAN_COMP_LT;
	}
      else if (!qo_is_index_covering_scan (a) && qo_is_index_covering_scan (b))
	{
	  return PLAN_COMP_GT;
	}

      /* both have the same range terms and same number of filters */
      if (a_cum && b_cum)
	{
	  /* take the smaller index pages */
	  if (a_cum->pages < b_cum->pages)
	    {
	      return PLAN_COMP_LT;
	    }
	  else if (a_cum->pages > b_cum->pages)
	    {
	      return PLAN_COMP_GT;
	    }

	  assert (a_cum->pkeys_size <= BTREE_STATS_PKEYS_NUM);
	  assert (b_cum->pkeys_size <= BTREE_STATS_PKEYS_NUM);

	  /* take the smaller index pkeys_size */
	  if (a_cum->pkeys_size < b_cum->pkeys_size)
	    {
	      return PLAN_COMP_LT;
	    }
	  else if (a_cum->pkeys_size > b_cum->pkeys_size)
	    {
	      return PLAN_COMP_GT;
	    }
	}

      /* both have the same number of index pages and pkeys_size */
      return PLAN_COMP_EQ;
    }
  else if (a_range > 0 && bitset_subset (&(a->plan_un.scan.terms), &(b->plan_un.scan.terms)))
    {
      return PLAN_COMP_LT;
    }
  else if (b_range > 0 && bitset_subset (&(b->plan_un.scan.terms), &(a->plan_un.scan.terms)))
    {
      return PLAN_COMP_GT;
    }

  /* check if it is an unique index and all columns are equi */
  if (qo_is_all_unique_index_columns_are_equi_terms (a) && !qo_is_all_unique_index_columns_are_equi_terms (b))
    {
      return PLAN_COMP_LT;
    }
  else if (!qo_is_all_unique_index_columns_are_equi_terms (a) && qo_is_all_unique_index_columns_are_equi_terms (b))
    {
      return PLAN_COMP_GT;
    }

  /* STEP 2: check by term cardinality */

  if (a->plan_un.scan.index_equi == b->plan_un.scan.index_equi)
    {
      if (a_range > b_range)
	{
	  return PLAN_COMP_LT;
	}
      else if (a_range < b_range)
	{
	  return PLAN_COMP_GT;
	}

      assert (a_range == b_range);

      /* both plans have the same number of range terms we will check now the key filter terms */
      if (a_filter > b_filter)
	{
	  return PLAN_COMP_LT;
	}
      else if (a_filter < b_filter)
	{
	  return PLAN_COMP_GT;
	}

      /* both have the same number of range terms and same number of filters */
      return PLAN_COMP_EQ;
    }

  return PLAN_COMP_EQ;		/* is equal with terms not cost */
}

QO_PLAN_COMPARE_RESULT
qo_group_by_skip_plans_cmp (QO_PLAN * a, QO_PLAN * b)
{
  QO_INDEX_ENTRY *a_ent, *b_ent;

  if (!qo_is_interesting_order_scan (a) || !qo_is_interesting_order_scan (b))
    {
      return PLAN_COMP_UNK;
    }

  a_ent = a->plan_un.scan.index->head;
  b_ent = b->plan_un.scan.index->head;

  if (a_ent == NULL || b_ent == NULL)
    {
      assert (false);
      return PLAN_COMP_UNK;
    }

  if (qo_is_index_iss_scan (a) || qo_is_index_loose_scan (a))
    {
      return PLAN_COMP_UNK;
    }

  if (qo_is_index_iss_scan (b) || qo_is_index_loose_scan (b))
    {
      return PLAN_COMP_UNK;
    }

  if (a_ent->groupby_skip)
    {
      if (b_ent->groupby_skip)
	{
	  return qo_plan_iscan_terms_cmp (a, b);
	}
      else
	{
	  return PLAN_COMP_LT;
	}
    }
  else
    {
      if (b_ent->groupby_skip)
	{
	  return PLAN_COMP_GT;
	}
    }

  return PLAN_COMP_EQ;
}

QO_PLAN_COMPARE_RESULT
qo_order_by_skip_plans_cmp (QO_PLAN * a, QO_PLAN * b)
{
  QO_INDEX_ENTRY *a_ent, *b_ent;

  if (!qo_is_interesting_order_scan (a) || !qo_is_interesting_order_scan (b))
    {
      return PLAN_COMP_UNK;
    }

  a_ent = a->plan_un.scan.index->head;
  b_ent = b->plan_un.scan.index->head;

  if (a_ent == NULL || b_ent == NULL)
    {
      assert (false);
      return PLAN_COMP_UNK;
    }

  if (qo_is_index_iss_scan (a) || qo_is_index_loose_scan (a))
    {
      return PLAN_COMP_UNK;
    }

  if (qo_is_index_iss_scan (b) || qo_is_index_loose_scan (b))
    {
      return PLAN_COMP_UNK;
    }

  if (a_ent->orderby_skip)
    {
      if (b_ent->orderby_skip)
	{
	  return qo_plan_iscan_terms_cmp (a, b);
	}
      else
	{
	  return PLAN_COMP_LT;
	}
    }
  else
    {
      if (b_ent->orderby_skip)
	{
	  return PLAN_COMP_GT;
	}
    }

  return PLAN_COMP_EQ;
}

bool
qo_check_orderby_skip_descending (QO_PLAN * plan)
{
  bool orderby_skip = false;
  QO_ENV *env;
  PT_NODE *tree, *trav, *order_by;

  env = NULL;
  tree = order_by = NULL;

  if (plan == NULL)
    {
      return false;
    }
  if (plan->info)
    {
      env = plan->info->env;
    }

  if (env == NULL)
    {
      return false;
    }

  tree = QO_ENV_PT_TREE (env);

  if (tree == NULL)
    {
      return false;
    }

  if (tree->info.query.q.select.hint & PT_HINT_NO_IDX_DESC)
    {
      return false;
    }

  order_by = tree->info.query.order_by;

  for (trav = plan->iscan_sort_list; trav; trav = trav->next)
    {
      /* change PT_ASC to PT_DESC and vice-versa */
      trav->info.sort_spec.asc_or_desc = (PT_MISC_TYPE) (PT_ASC + PT_DESC - trav->info.sort_spec.asc_or_desc);
    }

  /* test again the order by skip */
  orderby_skip = pt_sort_spec_cover (plan->iscan_sort_list, order_by);

  /* change back directions */
  for (trav = plan->iscan_sort_list; trav; trav = trav->next)
    {
      /* change PT_ASC to PT_DESC and vice-versa */
      trav->info.sort_spec.asc_or_desc = (PT_MISC_TYPE) (PT_ASC + PT_DESC - trav->info.sort_spec.asc_or_desc);
    }

  return orderby_skip;
}

bool
qo_check_skip_term (QO_ENV * env, BITSET visited_segs, QO_TERM * term, BITSET * visited_terms,
		    BITSET * cur_visited_terms)
{
  BITSET remaining_terms, connected_segs, all_visited_terms, eq_visited_segs;
  BITSET_ITERATOR bi;
  QO_TERM *tmp_term;
  int i, prev_card;
  bool result;

  /* check unvisited segments */
  if (!bitset_subset (&visited_segs, &(QO_TERM_SEGS (term))))
    {
      return false;
    }
  bitset_init (&remaining_terms, env);
  bitset_init (&connected_segs, env);
  bitset_init (&all_visited_terms, env);
  bitset_init (&eq_visited_segs, env);

  /* gather terms having same eqclass */
  bitset_union (&all_visited_terms, visited_terms);
  bitset_union (&all_visited_terms, cur_visited_terms);

  for (i = bitset_iterate (&all_visited_terms, &bi); i != -1; i = bitset_next_member (&bi))
    {
      tmp_term = QO_ENV_TERM (env, i);

      if (QO_TERM_EQCLASS (tmp_term) == QO_TERM_EQCLASS (term))
	{
	  bitset_add (&remaining_terms, i);
	  bitset_union (&eq_visited_segs, &(QO_TERM_SEGS (tmp_term)));
	}
    }

  /* check number of remaining terms. at least n-1 terms can be fully connected. */
  if (bitset_cardinality (&remaining_terms) < bitset_cardinality (&eq_visited_segs) - 1)
    {
      result = false;
      goto end;
    }

  /* check whether segments of eqclass are fully connected */
  prev_card = bitset_cardinality (&remaining_terms);
  while (!bitset_is_empty (&remaining_terms))
    {
      for (i = bitset_iterate (&remaining_terms, &bi); i != -1; i = bitset_next_member (&bi))
	{
	  tmp_term = QO_ENV_TERM (env, i);

	  if (bitset_is_empty (&connected_segs) || bitset_intersects (&connected_segs, &(QO_TERM_SEGS (tmp_term))))
	    {
	      /* first time or connected segs */
	      bitset_union (&connected_segs, &(QO_TERM_SEGS (tmp_term)));
	      bitset_remove (&remaining_terms, i);
	    }
	}

      if (prev_card == bitset_cardinality (&remaining_terms))
	{
	  /* There are no more connected terms. */
	  break;
	}
      prev_card = bitset_cardinality (&remaining_terms);
    }

  if (bitset_subset (&connected_segs, &(QO_TERM_SEGS (term))))
    {
      /* already evaluated */
      result = true;
    }
  else
    {
      result = false;
    }

end:
  bitset_delset (&remaining_terms);
  bitset_delset (&connected_segs);
  bitset_delset (&all_visited_terms);
  bitset_delset (&eq_visited_segs);

  return result;
}

bool
qo_is_iscan_from_groupby (QO_PLAN * plan)
{
  if (plan && plan->plan_type == QO_PLANTYPE_SCAN && plan->plan_un.scan.scan_method == QO_SCANMETHOD_INDEX_GROUPBY_SCAN)
    {
      return true;
    }

  return false;
}

int
qo_validate_index_for_groupby (QO_ENV * env, QO_NODE_INDEX_ENTRY * ni_entryp)
{
  bool key_notnull = false;	/* init */
  QO_INDEX_ENTRY *index_entryp;
  QO_CLASS_INFO_ENTRY *index_class;

  PT_NODE *groupby_expr = NULL;

  assert (ni_entryp != NULL);
  assert (ni_entryp->head != NULL);
  assert (ni_entryp->head->class_ != NULL);

  index_entryp = ni_entryp->head;
  index_class = index_entryp->class_;

  if (!QO_ENV_PT_TREE (env) || !QO_ENV_PT_TREE (env)->info.query.q.select.group_by)
    {
      goto end;
    }

  key_notnull = qo_validate_index_term_notnull (env, index_entryp);
  if (key_notnull)
    {
      goto final;
    }

  /* get the name of the first column in the group by list */
  groupby_expr = QO_ENV_PT_TREE (env)->info.query.q.select.group_by->info.sort_spec.expr;

  assert (key_notnull == false);

  key_notnull = qo_validate_index_attr_notnull (env, index_entryp, groupby_expr);
  if (key_notnull)
    {
      goto final;
    }

  /* Now we have the information we need: if the key column can be null and if there is a PT_IS_NULL or PT_IS_NOT_NULL
   * expression with this key column involved and also if we have other terms with the key. We must decide if there can
   * be NULLs in the results and if so, drop this index. 1. If the key cannot have null values, we have a winner. 2.
   * Otherwise, if we found a term isnull/isnotnull(key) we drop it (because we cannot evaluate if this yields true or
   * false so we skip all, for safety) 3. If we have a term with other operator except isnull/isnotnull and does not
   * have an OR following we have a winner again! (because we cannot have a null value).
   */
final:
  if (key_notnull)
    {
      return 1;
    }
end:
  return 0;
}

bool
qo_check_groupby_skip_descending (QO_PLAN * plan, PT_NODE * list)
{
  bool groupby_skip = false;
  QO_ENV *env;
  PT_NODE *tree, *trav, *group_by;

  env = NULL;
  tree = group_by = NULL;

  if (plan == NULL)
    {
      return false;
    }

  if (plan->info)
    {
      env = plan->info->env;
    }

  if (env == NULL)
    {
      return false;
    }

  tree = QO_ENV_PT_TREE (env);

  if (tree == NULL)
    {
      return false;
    }

  if (tree->info.query.q.select.hint & PT_HINT_NO_IDX_DESC)
    {
      return false;
    }

  group_by = tree->info.query.q.select.group_by;

  for (trav = list; trav; trav = trav->next)
    {
      /* change PT_ASC to PT_DESC and vice-versa */
      trav->info.sort_spec.asc_or_desc = (PT_MISC_TYPE) (PT_ASC + PT_DESC - trav->info.sort_spec.asc_or_desc);
    }

  /* test again the group by skip */
  groupby_skip = pt_sort_spec_cover_groupby (env->parser, list, group_by, tree);

  /* change back directions */
  for (trav = list; trav; trav = trav->next)
    {
      /* change PT_ASC to PT_DESC and vice-versa */
      trav->info.sort_spec.asc_or_desc = (PT_MISC_TYPE) (PT_ASC + PT_DESC - trav->info.sort_spec.asc_or_desc);
    }

  return groupby_skip;
}

PT_NODE *
qo_plan_compute_iscan_sort_list (QO_PLAN * root, PT_NODE * group_by, bool * is_index_w_prefix,
				 bool for_min_max_optimize)
{
  QO_PLAN *plan;
  QO_ENV *env;
  PARSER_CONTEXT *parser;
  PT_NODE *tree, *sort_list, *sort, *col, *node, *expr;
  QO_NODE_INDEX_ENTRY *ni_entryp;
  QO_INDEX_ENTRY *index_entryp;
  int nterms, equi_nterms, seg_idx, i, j;
  QO_SEGMENT *seg;
  PT_MISC_TYPE asc_or_desc;
  QFILE_TUPLE_VALUE_POSITION pos_descr;
  TP_DOMAIN *key_type, *col_type;
  BITSET *terms;
  BITSET_ITERATOR bi;
  bool is_const_eq_term;

  sort_list = NULL;		/* init */
  col = NULL;
  *is_index_w_prefix = false;

  /* find sortable plan */
  plan = root;
  while (plan && plan->plan_type != QO_PLANTYPE_SCAN)
    {
      switch (plan->plan_type)
	{
	case QO_PLANTYPE_FOLLOW:
	  plan = plan->plan_un.follow.head;
	  break;

	case QO_PLANTYPE_JOIN:
	  plan = plan->plan_un.join.outer;
	  break;

	case QO_PLANTYPE_SORT:
	  plan = plan->plan_un.sort.subplan;
	  break;

	default:
	  plan = NULL;
	  break;
	}
    }

  /* check for plan type */
  if (plan == NULL || plan->plan_type != QO_PLANTYPE_SCAN)
    {
      goto exit_on_end;		/* nop */
    }
  else if (QO_NODE_INFO (plan->plan_un.scan.node) == NULL)
    {
      /* if there's no class information or the class is not normal class */
      goto exit_on_end;		/* nop */
    }
  else if (QO_NODE_IS_CLASS_HIERARCHY (plan->plan_un.scan.node))
    {
      /* exclude class hierarchy scan */
      goto exit_on_end;		/* nop */
    }

  /* check for index scan plan */
  if (!qo_is_interesting_order_scan (plan) || (env = (plan->info)->env) == NULL
      || (parser = QO_ENV_PARSER (env)) == NULL || (tree = QO_ENV_PT_TREE (env)) == NULL)
    {
      goto exit_on_end;		/* nop */
    }

  /* pointer to QO_NODE_INDEX_ENTRY structure in QO_PLAN */
  ni_entryp = plan->plan_un.scan.index;
  /* pointer to linked list of index node, 'head' field(QO_INDEX_ENTRY strucutre) of QO_NODE_INDEX_ENTRY */
  index_entryp = (ni_entryp)->head;

  nterms = bitset_cardinality (&(plan->plan_un.scan.terms));
  if (nterms > 0)
    {
      equi_nterms = plan->plan_un.scan.index_equi ? nterms : nterms - 1;
    }
  else
    {
      equi_nterms = 0;
    }
  assert (equi_nterms >= 0);

  if (index_entryp->rangelist_seg_idx != -1)
    {
      equi_nterms = MIN (equi_nterms, index_entryp->rangelist_seg_idx);
    }

  /* we must have the first index column appear as the first sort column, so we pretend the number of index_equi
   * columns is zero, to force it to match the sort list and the index columns one-for-one.
   */
  if (qo_is_index_iss_scan (plan))
    {
      equi_nterms = 0;
    }
  assert (equi_nterms >= 0);
  assert (equi_nterms <= index_entryp->nsegs);

  if (equi_nterms >= index_entryp->nsegs)
    {
      /* is all constant col's order node */
      goto exit_on_end;		/* nop */
    }

  /* check if this is an index with prefix */
  *is_index_w_prefix = qo_is_prefix_index (index_entryp);

  asc_or_desc = (SM_IS_CONSTRAINT_REVERSE_INDEX_FAMILY (index_entryp->constraints->type) ? PT_DESC : PT_ASC);

  key_type = index_entryp->key_type;
  if (key_type == NULL)
    {				/* is invalid case */
      assert (false);
      goto exit_on_end;		/* nop */
    }

  if (asc_or_desc == PT_DESC)
    {
      col_type = NULL;		/* nop; do not care asc_or_desc anymore */
    }
  else
    {
      if (TP_DOMAIN_TYPE (key_type) == DB_TYPE_MIDXKEY)
	{
	  assert (QO_ENTRY_MULTI_COL (index_entryp));

	  col_type = key_type->setdomain;
	  assert (col_type != NULL);

	  /* get the first non-equal range key domain */
	  for (j = 0; j < equi_nterms && col_type; j++)
	    {
	      col_type = col_type->next;
	    }
	}
      else
	{
	  col_type = key_type;

	  assert (col_type != NULL);
	  assert (col_type->next == NULL);
	  assert (equi_nterms <= 1);

	  /* get the first non-equal range key domain */
	  if (equi_nterms > 0)
	    {
	      col_type = NULL;	/* give up */
	    }
	}

      assert (col_type != NULL || equi_nterms > 0);
    }

  for (i = equi_nterms; i < index_entryp->nsegs; i++)
    {
      if (index_entryp->ils_prefix_len > 0 && i >= index_entryp->ils_prefix_len)
	{
	  /* sort list should contain only prefix when using loose index scan */
	  break;
	}

      seg_idx = (index_entryp->seg_idxs[i]);
      if (seg_idx == -1)
	{			/* not exist in query */
	  break;		/* give up */
	}

      seg = QO_ENV_SEG (env, seg_idx);
      node = QO_SEG_PT_NODE (seg);
      if (node->node_type == PT_DOT_)
	{
	  /* FIXME :: we do not handle path-expr here */
	  break;		/* give up */
	}


      if (QO_SEG_FUNC_INDEX (seg) == true)
	{
	  asc_or_desc = index_entryp->constraints->func_index_info->fi_domain->is_desc ? PT_DESC : PT_ASC;
	  if (col_type)
	    {
	      col_type = col_type->next;
	    }
	}
      else
	{
	  if (col_type)
	    {
	      asc_or_desc = (col_type->is_desc) ? PT_DESC : PT_ASC;
	      col_type = col_type->next;
	    }

	  /* skip segment of const eq term */
	  terms = &(QO_SEG_INDEX_TERMS (seg));
	  is_const_eq_term = false;
	  for (j = bitset_iterate (terms, &bi); j != -1; j = bitset_next_member (&bi))
	    {
	      expr = QO_TERM_PT_EXPR (QO_ENV_TERM (env, j));
	      if (PT_IS_EXPR_NODE_WITH_OPERATOR (expr, PT_EQ)
		  && (PT_IS_CONST (expr->info.expr.arg1) || PT_IS_CONST (expr->info.expr.arg2)))
		{
		  is_const_eq_term = true;
		}
	    }
	  if (is_const_eq_term)
	    {
	      continue;
	    }
	}

      /* is for order_by skip */

      /* check for constant col's order node */
      pt_to_pos_descr (parser, &pos_descr, node, tree, NULL, for_min_max_optimize);
      if (pos_descr.pos_no > 0)
	{
	  col = tree->info.query.q.select.list;
	  for (j = 1; j < pos_descr.pos_no && col; j++)
	    {
	      col = col->next;
	    }

	  if (col)
	    {
	      col = pt_get_end_path_node (col);
	      if (col && col->node_type == PT_NAME && PT_NAME_INFO_IS_FLAGED (col, PT_NAME_INFO_CONSTANT))
		{
		  continue;	/* skip out constant order */
		}
	    }
	}

      /* is for group_by skip */
      if (group_by != NULL)
	{
	  assert (!group_by->flag.with_rollup);

	  /* check for constant col's group node */
	  pt_to_pos_descr_groupby (parser, &pos_descr, node, tree);
	  if (pos_descr.pos_no > 0)
	    {
	      assert (group_by == tree->info.query.q.select.group_by);
	      for (col = group_by, j = 1; j < pos_descr.pos_no && col; j++)
		{
		  col = col->next;
		}

	      while (col && col->node_type == PT_SORT_SPEC)
		{
		  col = col->info.sort_spec.expr;
		}

	      if (col)
		{
		  col = pt_get_end_path_node (col);
		  if (col && col->node_type == PT_NAME && PT_NAME_INFO_IS_FLAGED (col, PT_NAME_INFO_CONSTANT))
		    {
		      continue;	/* skip out constant order */
		    }
		}
	    }
	}

      if (pos_descr.pos_no <= 0 || col == NULL)
	{			/* not found i-th key element */
	  break;		/* give up */
	}

      /* set sort info */
      sort = parser_new_node (parser, PT_SORT_SPEC);
      if (sort == NULL)
	{
	  PT_ERRORm (parser, node, MSGCAT_SET_PARSER_SEMANTIC, MSGCAT_SEMANTIC_OUT_OF_MEMORY);
	  break;		/* give up */
	}

      sort->info.sort_spec.expr = pt_point (parser, col);
      sort->info.sort_spec.pos_descr = pos_descr;
      sort->info.sort_spec.asc_or_desc = asc_or_desc;

      sort_list = parser_append_node (sort, sort_list);
    }

exit_on_end:

  return sort_list;
}

bool
qo_is_interesting_order_scan (QO_PLAN * plan)
{
  if (qo_is_iscan (plan) || qo_is_iscan_from_groupby (plan) || qo_is_iscan_from_orderby (plan))
    {
      return true;
    }

  return false;
}

bool
qo_plan_is_orderby_skip_candidate (QO_PLAN * plan)
{
  PARSER_CONTEXT *parser;
  PT_NODE *order_by, *statement, *entity;
  QO_ENV *env;
  bool is_prefix = false, is_orderby_skip = false;
  bool need_cleanup = false;

  if (plan == NULL || plan->info == NULL)
    {
      assert (false);
      return false;
    }

  env = plan->info->env;

  switch (plan->skip_orderby_opt)
    {
    case QO_PLAN_SKIP_ORDERBY_USE:
    case QO_PLAN_SKIP_ORDERBY_CAN_USE:
      return true;

    case QO_PLAN_SKIP_ORDERBY_CANNOT_USE:
      return false;

    case QO_PLAN_SKIP_ORDERBY_NO:
      /* need check */
      break;

    default:
      /* impossible case */
      assert (false);

      /* need check */
      break;
    }

  parser = QO_ENV_PARSER (env);
  statement = QO_ENV_PT_TREE (env);
  order_by = statement->info.query.order_by;

  if (plan->iscan_sort_list == NULL)
    {
      plan->iscan_sort_list = qo_plan_compute_iscan_sort_list (plan, NULL, &is_prefix, false);
      need_cleanup = true;
    }

  if (plan->iscan_sort_list == NULL || is_prefix)
    {
      is_orderby_skip = false;
      goto cleanup;
    }

  is_orderby_skip = pt_sort_spec_cover (plan->iscan_sort_list, order_by);
  if (!is_orderby_skip)
    {
      /* verify descending */
      is_orderby_skip = qo_check_orderby_skip_descending (plan);
    }

  /*
   * In RIGHT OUTER JOIN, all leading tables are used as null-supplying,
   * so skip ORDER BY cannot be applied.
   * In LEFT OUTER JOIN, trailing tables may also be null-supplying,
   * but not always, so skip ORDER BY can be applied.
   * Since trailing tables in LEFT OUTER JOIN usually have join conditions in the ON clause,
   * a general Index Scan plan is more likely than an Index Scan plan for skip ORDER BY.
   * Here, we only check RIGHT OUTER JOIN.
   */
  if (qo_is_iscan_from_orderby (plan))
    {
      entity = QO_NODE_ENTITY_SPEC (plan->plan_un.scan.node)->next;
      for (; entity != NULL; entity = entity->next)
	{
	  if (entity->info.spec.join_type == PT_JOIN_RIGHT_OUTER)
	    {
	      is_orderby_skip = false;
	    }
	}
    }

cleanup:
  if (need_cleanup)
    {
      if (!is_orderby_skip && plan->iscan_sort_list != NULL)
	{
	  parser_free_tree (parser, plan->iscan_sort_list);
	  plan->iscan_sort_list = NULL;
	}
    }

  if (is_orderby_skip)
    {
      plan->skip_orderby_opt = QO_PLAN_SKIP_ORDERBY_CAN_USE;
    }
  else
    {
      plan->skip_orderby_opt = QO_PLAN_SKIP_ORDERBY_CANNOT_USE;
    }

  return is_orderby_skip;
}

bool
qo_is_sort_limit (QO_PLAN * plan)
{
  return (plan != NULL && plan->plan_type == QO_PLANTYPE_SORT && plan->plan_un.sort.sort_type == SORT_LIMIT);
}

bool
qo_has_sort_limit_subplan (QO_PLAN * plan)
{
  if (plan == NULL)
    {
      return false;
    }

  switch (plan->plan_type)
    {
    case QO_PLANTYPE_SCAN:
      return false;

    case QO_PLANTYPE_SORT:
      if (plan->plan_un.sort.sort_type == SORT_LIMIT)
	{
	  return true;
	}
      return qo_has_sort_limit_subplan (plan->plan_un.sort.subplan);

    case QO_PLANTYPE_JOIN:
      return (qo_has_sort_limit_subplan (plan->plan_un.join.outer)
	      || qo_has_sort_limit_subplan (plan->plan_un.join.inner));

    case QO_PLANTYPE_FOLLOW:
    case QO_PLANTYPE_WORST:
      return false;
    }

  return false;
}

int
qo_check_like_recompile_candidate (QO_PLAN * plan, void *arg)
{
  BITSET terms_set, temp_segs_set;
  int term_idx, seg_idx;
  BITSET_ITERATOR terms_iter;
  QO_ENV *env;
  QO_TERM *termp;
  PT_NODE *expr;
  QO_SEGMENT *seg;

  bool *result = (bool *) arg;

  env = (plan->info)->env;
  bitset_init (&terms_set, env);

  bitset_assign (&terms_set, &(plan->sarged_terms));
  bitset_union (&terms_set, &(plan->plan_un.scan.terms));
  bitset_union (&terms_set, &(plan->plan_un.scan.kf_terms));

  for (term_idx = bitset_iterate (&terms_set, &terms_iter); term_idx != -1; term_idx = bitset_next_member (&terms_iter))
    {
      termp = QO_ENV_TERM (env, term_idx);
      expr = QO_TERM_PT_EXPR (termp);
      if (expr == NULL)
	{
	  continue;
	}

      bitset_init (&temp_segs_set, env);

      if (expr->info.expr.op != PT_LIKE)
	{
	  continue;
	}

      qo_expr_segs (env, pt_left_part (expr), &temp_segs_set);
      seg_idx = bitset_first_member (&temp_segs_set);
      if (seg_idx == -1)
	{
	  continue;
	}

      seg = QO_ENV_SEG (env, seg_idx);
      if (seg->is_not_null)
	{
	  *result = true;
	  return NO_ERROR;
	}
    }

  return NO_ERROR;
}

int
qo_has_like_recompile_candidate (QO_PLAN * plan, void *arg)
{
  return qo_walk_plan_tree (plan, qo_check_like_recompile_candidate, arg);
}
