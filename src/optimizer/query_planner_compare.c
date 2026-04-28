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

QO_PLAN *
qo_plan_order_by (QO_PLAN * plan, QO_EQCLASS * order)
{
  if (plan == NULL || order == QO_UNORDERED || plan->order == order)
    {
      return plan;
    }
  else if (BITSET_MEMBER ((plan->info)->eqclasses, QO_EQCLASS_IDX (order)))
    {
      return qo_sort_new (plan, order, SORT_TEMP);
    }
  else
    {
      return (QO_PLAN *) NULL;
    }
}

QO_PLAN_COMPARE_RESULT
qo_plan_cmp_prefer_covering_index (QO_PLAN * scan_plan_p, QO_PLAN * sort_plan_p)
{
  QO_PLAN *sort_subplan_p;

  assert (scan_plan_p->plan_type == QO_PLANTYPE_SCAN);
  assert (sort_plan_p->plan_type == QO_PLANTYPE_SORT);

  sort_subplan_p = sort_plan_p->plan_un.sort.subplan;

  if (!qo_is_interesting_order_scan (scan_plan_p) || !qo_is_interesting_order_scan (sort_subplan_p))
    {
      return PLAN_COMP_UNK;
    }

  if (qo_is_index_iss_scan (scan_plan_p) || qo_is_index_loose_scan (scan_plan_p))
    {
      return PLAN_COMP_UNK;
    }

  if (qo_is_index_iss_scan (sort_subplan_p) || qo_is_index_loose_scan (sort_subplan_p))
    {
      return PLAN_COMP_UNK;
    }

  if (qo_is_index_covering_scan (sort_subplan_p))
    {
      /* if the sort plan contains a index plan with segment covering, prefer it */
      if (qo_is_index_covering_scan (scan_plan_p))
	{
	  if (scan_plan_p->plan_un.scan.index->head == sort_subplan_p->plan_un.scan.index->head)
	    {
	      return PLAN_COMP_LT;
	    }
	}
      else
	{
	  if (!bitset_is_empty (&(sort_subplan_p->plan_un.scan.terms)))
	    {
	      /* prefer covering index scan with key-range */
	      return PLAN_COMP_GT;
	    }
	}
    }

  return PLAN_COMP_UNK;
}

QO_PLAN_COMPARE_RESULT
qo_plan_cmp (QO_PLAN * a, QO_PLAN * b)
{
#if 1				/* TODO - do not delete me */
#define QO_PLAN_CMP_CHECK_COST(a, b)
#else
#define QO_PLAN_CMP_CHECK_COST(a, b) assert ((a) < ((b)*10));
#endif

#ifdef OLD_CODE
  if (QO_PLAN_FIXED_COST (a) <= QO_PLAN_FIXED_COST (b))
    {
      return QO_PLAN_ACCESS_COST (a) <= QO_PLAN_ACCESS_COST (b) ? a : b;
    }
  else
    {
      return QO_PLAN_ACCESS_COST (b) <= QO_PLAN_ACCESS_COST (a) ? b : a;
    }
#else /* OLD_CODE */
  double af, aa, bf, ba, ta, tb;
  QO_NODE *a_node, *b_node;
  QO_PLAN_COMPARE_RESULT temp_res;

  af = a->fixed_cpu_cost + a->fixed_io_cost;
  aa = a->variable_cpu_cost + a->variable_io_cost;
  bf = b->fixed_cpu_cost + b->fixed_io_cost;
  ba = b->variable_cpu_cost + b->variable_io_cost;

  /* Check if RBO is needed */
  ta = af + aa;
  tb = bf + ba;
  if (ta > 0 && tb > 0 && QO_PLAN_HAS_LIMIT (a) && QO_PLAN_HAS_LIMIT (b))
    {
      if (ta * RBO_CHECK_LIMIT_RATIO <= tb)
	{
	  return PLAN_COMP_LT;
	}
      else if (ta > tb * RBO_CHECK_LIMIT_RATIO)
	{
	  return PLAN_COMP_GT;
	}
    }
  else
    {
      if ((ta + RBO_CHECK_COST <= tb) && (ta * RBO_CHECK_RATIO <= tb))
	{
	  return PLAN_COMP_LT;
	}
      else if ((ta > tb + RBO_CHECK_COST) && (ta > tb * RBO_CHECK_RATIO))
	{
	  return PLAN_COMP_GT;
	}
    }

  if (qo_is_sort_limit (a))
    {
      if (qo_is_sort_limit (b))
	{
	  /* compare subplans */
	  a = a->plan_un.sort.subplan;
	  b = b->plan_un.sort.subplan;
	}
      else if (a->plan_un.sort.subplan == b)
	{
	  /* a is a SORT-LIMIT plan over b */
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}
    }
  else if (qo_is_sort_limit (b) && a == b->plan_un.sort.subplan)
    {
      QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
      return PLAN_COMP_GT;
    }

  /* skip out top-level sort plan */
  if (a->top_rooted && b->top_rooted)
    {
      /* skip out the same sort plan */
      while (a->plan_type == QO_PLANTYPE_SORT && b->plan_type == QO_PLANTYPE_SORT
	     && a->plan_un.sort.sort_type == b->plan_un.sort.sort_type)
	{
	  a = a->plan_un.sort.subplan;
	  b = b->plan_un.sort.subplan;
	}
    }
  else
    {
      if (a->top_rooted)
	{
	  while (a->plan_type == QO_PLANTYPE_SORT)
	    {
	      if (a->plan_un.sort.sort_type == SORT_TEMP)
		{
		  break;	/* is not top-level plan */
		}
	      a = a->plan_un.sort.subplan;
	    }
	}
      if (b->top_rooted)
	{
	  while (b->plan_type == QO_PLANTYPE_SORT)
	    {
	      if (b->plan_un.sort.sort_type == SORT_TEMP)
		{
		  break;	/* is top-level plan */
		}
	      b = b->plan_un.sort.subplan;
	    }
	}
    }

  if (a == b)
    {
      QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
      QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
      return PLAN_COMP_EQ;
    }

  /* check for superset of index */
  if (a->plan_type == QO_PLANTYPE_JOIN && QO_IS_NL_JOIN (a) && qo_is_iscan (a->plan_un.join.inner) &&
      b->plan_type == QO_PLANTYPE_JOIN && QO_IS_NL_JOIN (b) && qo_is_iscan (b->plan_un.join.inner))
    {
      temp_res = qo_plan_iscan_terms_cmp (a->plan_un.join.inner, b->plan_un.join.inner);
      if (temp_res == PLAN_COMP_LT)
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}
      else if (temp_res == PLAN_COMP_GT)
	{
	  QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	  return PLAN_COMP_GT;
	}
    }

  if ((a->plan_type != QO_PLANTYPE_SCAN && a->plan_type != QO_PLANTYPE_SORT)
      || (b->plan_type != QO_PLANTYPE_SCAN && b->plan_type != QO_PLANTYPE_SORT))
    {
      /* there may be joins with multi range optimizations */
      temp_res = qo_multi_range_opt_plans_cmp (a, b);
      if (temp_res == PLAN_COMP_LT)
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}
      else if (temp_res == PLAN_COMP_GT)
	{
	  QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	  return PLAN_COMP_GT;
	}
    }

  /* a order by skip plan is always preferred to a sort plan */
  if (a->plan_type == QO_PLANTYPE_SCAN && b->plan_type == QO_PLANTYPE_SORT)
    {
      /* prefer scan if it is multi range opt */
      if (qo_is_index_mro_scan (a))
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}

      temp_res = qo_plan_cmp_prefer_covering_index (a, b);
      if (temp_res == PLAN_COMP_LT)
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}
      else if (temp_res == PLAN_COMP_GT)
	{
	  QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	  return PLAN_COMP_GT;
	}

      if (a->plan_un.scan.index && a->plan_un.scan.index->head->groupby_skip)
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}
      if (a->plan_un.scan.index && a->plan_un.scan.index->head->orderby_skip)
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}

      if (qo_plan_is_orderby_skip_candidate (b->plan_un.sort.subplan))
	{
	  QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	  return PLAN_COMP_GT;
	}
    }

  if (b->plan_type == QO_PLANTYPE_SCAN && a->plan_type == QO_PLANTYPE_SORT)
    {
      /* prefer scan if it is multi range opt */
      if (qo_is_index_mro_scan (b))
	{
	  QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	  return PLAN_COMP_GT;
	}

      temp_res = qo_plan_cmp_prefer_covering_index (b, a);

      /* Since we swapped its position, we have to negate the comp result */
      if (temp_res == PLAN_COMP_LT)
	{
	  QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	  return PLAN_COMP_GT;
	}
      else if (temp_res == PLAN_COMP_GT)
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}

      if (!qo_is_index_iss_scan (b) && !qo_is_index_loose_scan (b))
	{
	  if (b->plan_un.scan.index && b->plan_un.scan.index->head->groupby_skip)
	    {
	      QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	      return PLAN_COMP_GT;
	    }
	  if (b->plan_un.scan.index && b->plan_un.scan.index->head->orderby_skip)
	    {
	      QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	      return PLAN_COMP_GT;
	    }
	}

      if (qo_plan_is_orderby_skip_candidate (a->plan_un.sort.subplan))
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}
    }

  if (a->plan_type == QO_PLANTYPE_SCAN && b->plan_type == QO_PLANTYPE_SCAN)
    {
      /* check multi range optimization */
      if (qo_is_index_mro_scan (a) && !qo_is_index_mro_scan (b))
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}
      if (!qo_is_index_mro_scan (a) && qo_is_index_mro_scan (b))
	{
	  QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	  return PLAN_COMP_GT;
	}

      /* check covering index scan */
      if (qo_is_index_covering_scan (a) && qo_is_seq_scan (b))
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}
      if (qo_is_index_covering_scan (b) && qo_is_seq_scan (a))
	{
	  QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	  return PLAN_COMP_GT;
	}

      /* check index scan */
      if ((qo_is_iscan (a) || qo_is_iscan_from_orderby (a)) && qo_is_seq_scan (b))
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}
      if ((qo_is_iscan (b) || qo_is_iscan_from_orderby (b)) && qo_is_seq_scan (a))
	{
	  QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	  return PLAN_COMP_GT;
	}

      /* a plan does order by skip, the other does group by skip - prefer the group by skipping because it's done in
       * the final step
       */
      if (qo_is_interesting_order_scan (a) && qo_is_interesting_order_scan (b))
	{
	  if (!qo_is_index_iss_scan (a) && !qo_is_index_loose_scan (a) && !qo_is_index_iss_scan (b)
	      && !qo_is_index_loose_scan (b))
	    {
	      if (a->plan_un.scan.index->head->orderby_skip && b->plan_un.scan.index->head->groupby_skip)
		{
		  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
		  return PLAN_COMP_LT;
		}
	      else if (a->plan_un.scan.index->head->groupby_skip && b->plan_un.scan.index->head->orderby_skip)
		{
		  QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
		  return PLAN_COMP_GT;
		}
	    }
	}
    }

  /* prefer order by skip plan over sort plan */
  if (a->plan_type == QO_PLANTYPE_JOIN && b->plan_type == QO_PLANTYPE_SORT)
    {
      if (qo_plan_is_orderby_skip_candidate (a->plan_un.join.outer))
	{
	  QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	  return PLAN_COMP_LT;
	}
    }
  else if (b->plan_type == QO_PLANTYPE_JOIN && a->plan_type == QO_PLANTYPE_SORT)
    {
      if (qo_plan_is_orderby_skip_candidate (b->plan_un.join.outer))
	{
	  QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	  return PLAN_COMP_GT;
	}
    }

  if (a->plan_type != QO_PLANTYPE_SCAN || b->plan_type != QO_PLANTYPE_SCAN)
    {				/* impossible case */
      goto cost_cmp;		/* give up */
    }

  a_node = a->plan_un.scan.node;
  b_node = b->plan_un.scan.node;

  /* check for empty spec */
  if (QO_NODE_NCARD (a_node) == 0 && QO_NODE_TCARD (a_node) == 0)
    {
      if (QO_NODE_NCARD (b_node) == 0 && QO_NODE_TCARD (b_node) == 0)
	{
	  goto cost_cmp;	/* give up */
	}

      QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
      return PLAN_COMP_LT;
    }
  else if (QO_NODE_NCARD (b_node) == 0 && QO_NODE_TCARD (b_node) == 0)
    {
      QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
      return PLAN_COMP_GT;
    }

  if (QO_NODE_IDX (a_node) != QO_NODE_IDX (b_node))
    {
      goto cost_cmp;		/* give up */
    }

  /* check for both index scan of the same spec */
  if (!qo_is_interesting_order_scan (a) || !qo_is_interesting_order_scan (b))
    {
      goto cost_cmp;		/* give up */
    }

  /* check multi range optimization */
  temp_res = qo_multi_range_opt_plans_cmp (a, b);
  if (temp_res == PLAN_COMP_LT)
    {
      QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
      return PLAN_COMP_LT;
    }
  else if (temp_res == PLAN_COMP_GT)
    {
      QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
      return PLAN_COMP_GT;
    }

  /* check index coverage */
  temp_res = qo_index_covering_plans_cmp (a, b);
  if (temp_res == PLAN_COMP_LT)
    {
      QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
      return PLAN_COMP_LT;
    }
  else if (temp_res == PLAN_COMP_GT)
    {
      QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
      return PLAN_COMP_GT;
    }

  /* check if one of the plans skips the order by, and if so, prefer it */
  temp_res = qo_order_by_skip_plans_cmp (a, b);
  if (temp_res == PLAN_COMP_LT)
    {
      QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
      return PLAN_COMP_LT;
    }
  else if (temp_res == PLAN_COMP_GT)
    {
      QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
      return PLAN_COMP_GT;
    }

  /* check if one of the plans skips the group by, and if so, prefer it */
  temp_res = qo_group_by_skip_plans_cmp (a, b);
  if (temp_res == PLAN_COMP_LT)
    {
      QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
      return PLAN_COMP_LT;
    }
  else if (temp_res == PLAN_COMP_GT)
    {
      QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
      return PLAN_COMP_GT;
    }

  if (QO_PLAN_HAS_LIMIT (a) && QO_PLAN_HAS_LIMIT (b))
    {
      if ((ta + RBO_CHECK_COST <= tb) && (ta * RBO_CHECK_RATIO <= tb))
	{
	  return PLAN_COMP_LT;
	}
      else if ((ta > tb + RBO_CHECK_COST) && (ta > tb * RBO_CHECK_RATIO))
	{
	  return PLAN_COMP_GT;
	}
    }

  /* iscan vs iscan index rule comparison */

  {
    QO_NODE_INDEX_ENTRY *a_ni, *b_ni;
    QO_INDEX_ENTRY *a_ent, *b_ent;
    QO_ATTR_CUM_STATS *a_cum, *b_cum;
    int a_range, b_range;	/* num iscan range terms */
    int a_filter, b_filter;	/* num iscan filter terms */
    int a_last, b_last;		/* the last partial-key indicator */
    int a_keys, b_keys;		/* num keys */
    int a_pages, b_pages;	/* num access index pages */
    int a_leafs, b_leafs;	/* num access index leaf pages */
    int i;
    QO_TERM *term;

    /* index entry of spec 'a' */
    a_ni = a->plan_un.scan.index;
    a_ent = (a_ni)->head;
    a_cum = &(a_ni)->cum_stats;

    assert (a_cum->pkeys_size <= BTREE_STATS_PKEYS_NUM);
    for (i = 0; i < a_cum->pkeys_size; i++)
      {
	if (a_cum->pkeys[i] <= 0)
	  {
	    break;
	  }
      }
    a_last = i;

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

    assert (b_cum->pkeys_size <= BTREE_STATS_PKEYS_NUM);
    for (i = 0; i < b_cum->pkeys_size; i++)
      {
	if (b_cum->pkeys[i] <= 0)
	  {
	    break;
	  }
      }
    b_last = i;

    /* index range terms */
    b_range = bitset_cardinality (&(b->plan_un.scan.terms));
    if (b_range > 0 && !(b->plan_un.scan.index_equi))
      {
	b_range--;		/* set the last equal range term */
      }

    /* index filter terms */
    b_filter = bitset_cardinality (&(b->plan_un.scan.kf_terms));

    /* STEP 1: take the smaller search condition */

    /* check for same index pointer */
    if (a_ent == b_ent)
      {
	/* check for search condition */
	if (a_range == b_range && a_filter == b_filter)
	  {
	    ;			/* go ahead */
	  }
	else if (a_range >= b_range && a_filter >= b_filter)
	  {
	    QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	    return PLAN_COMP_LT;
	  }
	else if (a_range <= b_range && a_filter <= b_filter)
	  {
	    QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	    return PLAN_COMP_GT;
	  }
      }

    /* STEP 2: check by index terms */

    temp_res = qo_plan_iscan_terms_cmp (a, b);
    if (temp_res == PLAN_COMP_LT)
      {
	QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	return PLAN_COMP_LT;
      }
    else if (temp_res == PLAN_COMP_GT)
      {
	QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	return PLAN_COMP_GT;
      }

    /* STEP 3: take the smaller access pages */

    if (a->variable_io_cost != b->variable_io_cost)
      {
	goto cost_cmp;		/* give up */
      }

    /* btree partial-key stats */
    if (a_range == a_ent->col_num)
      {
	a_keys = a_cum->keys;
      }
    else if (a_range > 0 && a_range < a_last)
      {
	if (qo_is_index_iss_scan (a))
	  {
	    a_keys = a_cum->pkeys[a_range];
	  }
	else
	  {
	    a_keys = a_cum->pkeys[a_range - 1];
	  }
      }
    else
      {				/* a_range == 0 */
	a_keys = 1;		/* init as full range */
	if (a_last > 0)
	  {
	    if (bitset_cardinality (&(a->plan_un.scan.terms)) > 0)
	      {
		term = QO_ENV_TERM ((a->info)->env, bitset_first_member (&(a->plan_un.scan.terms)));
		a_keys = (int) ceil (1.0 / QO_TERM_SELECTIVITY (term));
	      }
	    else
	      {
		a_keys = (int) ceil (1.0 / DEFAULT_SELECTIVITY);
	      }

	    a_keys = MIN (a_cum->pkeys[0], a_keys);
	  }
      }

    if (a_cum->leafs <= a_keys)
      {
	a_leafs = 1;
      }
    else if (a_keys == 0)
      {
	a_leafs = 0;
      }
    else
      {
	a_leafs = (int) ceil ((double) a_cum->leafs / a_keys);
      }

    /* btree access pages */
    a_pages = a_leafs + a_cum->height - 1;

    /* btree partial-key stats  */
    if (b_range == b_ent->col_num)
      {
	b_keys = b_cum->keys;
      }
    else if (b_range > 0 && b_range < b_last)
      {
	if (qo_is_index_iss_scan (b))
	  {
	    b_keys = b_cum->pkeys[b_range];
	  }
	else
	  {
	    b_keys = b_cum->pkeys[b_range - 1];
	  }
      }
    else
      {				/* b_range == 0 */
	b_keys = 1;		/* init as full range */
	if (b_last > 0)
	  {
	    if (bitset_cardinality (&(b->plan_un.scan.terms)) > 0)
	      {
		term = QO_ENV_TERM ((b->info)->env, bitset_first_member (&(b->plan_un.scan.terms)));
		b_keys = (int) ceil (1.0 / QO_TERM_SELECTIVITY (term));
	      }
	    else
	      {
		b_keys = (int) ceil (1.0 / DEFAULT_SELECTIVITY);
	      }

	    b_keys = MIN (b_cum->pkeys[0], b_keys);
	  }
      }

    if (b_cum->leafs <= b_keys)
      {
	b_leafs = 1;
      }
    else if (b_keys == 0)
      {
	b_leafs = 0;
      }
    else
      {
	b_leafs = (int) ceil ((double) b_cum->leafs / b_keys);
      }

    /* btree access pages */
    b_pages = b_leafs + b_cum->height - 1;

    if (a_pages < b_pages)
      {
	QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	return PLAN_COMP_LT;
      }
    else if (a_pages > b_pages)
      {
	QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	return PLAN_COMP_GT;
      }

    /* STEP 4: take the smaller index */
    if (a_cum->pages > a_cum->height && b_cum->pages > b_cum->height)
      {
	/* each index is big enough */
	if (a_cum->pages < b_cum->pages)
	  {
	    QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	    return PLAN_COMP_LT;
	  }
	else if (a_cum->pages > b_cum->pages)
	  {
	    QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	    return PLAN_COMP_GT;
	  }
      }

    /* STEP 5: take the smaller key range */
    if (a_keys > b_keys)
      {
	QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
	return PLAN_COMP_LT;
      }
    else if (a_keys < b_keys)
      {
	QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
	return PLAN_COMP_GT;
      }

    if (af == bf && aa == ba)
      {
	if (a->plan_un.scan.index_equi == b->plan_un.scan.index_equi && qo_is_index_covering_scan (a)
	    && qo_is_index_covering_scan (b))
	  {
	    if (a_ent->col_num > b_ent->col_num)
	      {
		QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
		return PLAN_COMP_GT;
	      }
	    else if (a_ent->col_num < b_ent->col_num)
	      {
		QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
		return PLAN_COMP_LT;
	      }
	  }

	if (qo_is_index_mro_scan (a) && qo_is_index_mro_scan (b))
	  {
	    if (a_ent->col_num > b_ent->col_num)
	      {
		QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
		return PLAN_COMP_GT;
	      }
	    else if (a_ent->col_num < b_ent->col_num)
	      {
		QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
		return PLAN_COMP_LT;
	      }
	  }

	/* if both plans skip order by and same costs, take the larger one */
	if (!qo_is_index_iss_scan (a) && !qo_is_index_loose_scan (a) && !qo_is_index_iss_scan (b)
	    && !qo_is_index_loose_scan (b))
	  {
	    if (a_ent->orderby_skip && b_ent->orderby_skip)
	      {
		if (a_ent->col_num > b_ent->col_num)
		  {
		    QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
		    return PLAN_COMP_LT;
		  }
		else if (a_ent->col_num < b_ent->col_num)
		  {
		    /* if the new plan has more columns, prefer it */
		    QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
		    return PLAN_COMP_GT;
		  }
	      }
	  }

	/* if both plans skip group by and same costs, take the larger one */
	if (a_ent->groupby_skip && b_ent->groupby_skip)
	  {
	    if (a_ent->col_num > b_ent->col_num)
	      {
		QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
		return PLAN_COMP_LT;
	      }
	    else if (a_ent->col_num < b_ent->col_num)
	      {
		/* if the new plan has more columns, prefer it */
		QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
		return PLAN_COMP_GT;
	      }
	  }
      }
  }

cost_cmp:

  if (a == b || (af == bf && aa == ba))
    {
      return PLAN_COMP_EQ;
    }
  if (af + aa <= bf + ba)
    {
      QO_PLAN_CMP_CHECK_COST (af + aa, bf + ba);
      return PLAN_COMP_LT;
    }
  else
    {
      QO_PLAN_CMP_CHECK_COST (bf + ba, af + aa);
      return PLAN_COMP_GT;
    }

#endif /* OLD_CODE */
}

QO_PLAN_COMPARE_RESULT
qo_multi_range_opt_plans_cmp (QO_PLAN * a, QO_PLAN * b)
{
  QO_PLAN_COMPARE_RESULT temp_res;

  /* if no plan uses multi range optimization, nothing to do here */
  if (!qo_plan_multi_range_opt (a) && !qo_plan_multi_range_opt (b))
    {
      return PLAN_COMP_UNK;
    }

  /* check if only one plan uses multi range optimization */
  if (qo_plan_multi_range_opt (a) && !qo_plan_multi_range_opt (b))
    {
      return PLAN_COMP_LT;
    }
  if (!qo_plan_multi_range_opt (a) && qo_plan_multi_range_opt (b))
    {
      return PLAN_COMP_GT;
    }

  /* at here, both plans use multi range optimization */

  if (a->plan_type == QO_PLANTYPE_JOIN && b->plan_type == QO_PLANTYPE_JOIN)
    {
      /* choose the plan where the optimized index scan is "outer-most" */
      int a_mro_join_idx = -1, b_mro_join_idx = -1;

      if (qo_find_subplan_using_multi_range_opt (a, NULL, &a_mro_join_idx) != NO_ERROR
	  || qo_find_subplan_using_multi_range_opt (b, NULL, &b_mro_join_idx) != NO_ERROR)
	{
	  assert (0);
	  return PLAN_COMP_UNK;
	}
      if (a_mro_join_idx < b_mro_join_idx)
	{
	  return PLAN_COMP_LT;
	}
      else if (a_mro_join_idx > b_mro_join_idx)
	{
	  return PLAN_COMP_GT;
	}
      else
	{
	  return PLAN_COMP_EQ;
	}
    }

  if (a->plan_type == QO_PLANTYPE_JOIN || b->plan_type == QO_PLANTYPE_JOIN)
    {
      /* one plan is join, the other is not join */
      return PLAN_COMP_UNK;
    }

  /* both plans must be optimized index scans */
  assert (qo_is_index_mro_scan (a));
  assert (qo_is_index_mro_scan (b));

  assert (bitset_cardinality (&(a->plan_un.scan.terms)) > 0);
  assert (bitset_cardinality (&(b->plan_un.scan.terms)) > 0);

  /* choose the plan that also covers all segments */
  temp_res = qo_index_covering_plans_cmp (a, b);
  if (temp_res == PLAN_COMP_LT || temp_res == PLAN_COMP_GT)
    {
      return temp_res;
    }

  return qo_plan_iscan_terms_cmp (a, b);
}

QO_PLAN_COMPARE_RESULT
qo_index_covering_plans_cmp (QO_PLAN * a, QO_PLAN * b)
{
  int a_range, b_range;		/* num iscan range terms */

  if (!qo_is_interesting_order_scan (a) || !qo_is_interesting_order_scan (b))
    {
      return PLAN_COMP_UNK;
    }

  assert (a->plan_un.scan.index->head != NULL);
  assert (b->plan_un.scan.index->head != NULL);

  if (qo_is_index_iss_scan (a) || qo_is_index_loose_scan (a))
    {
      return PLAN_COMP_UNK;
    }

  if (qo_is_index_iss_scan (b) || qo_is_index_loose_scan (b))
    {
      return PLAN_COMP_UNK;
    }

  a_range = bitset_cardinality (&(a->plan_un.scan.terms));
  b_range = bitset_cardinality (&(b->plan_un.scan.terms));

  assert (a_range >= 0);
  assert (b_range >= 0);

  if (qo_is_index_covering_scan (a))
    {
      if (qo_is_index_covering_scan (b))
	{
	  return qo_plan_iscan_terms_cmp (a, b);
	}
      else
	{
	  if (a_range >= b_range)
	    {
	      /* prefer covering index scan with key-range */
	      return PLAN_COMP_LT;
	    }
	}
    }
  else if (qo_is_index_covering_scan (b))
    {
      if (b_range >= a_range)
	{
	  /* prefer covering index scan with key-range */
	  return PLAN_COMP_GT;
	}
    }

  return PLAN_COMP_EQ;
}

void
qo_init_planvec (QO_PLANVEC * planvec)
{
  int i;

  planvec->overflow = false;
  planvec->nplans = 0;

  for (i = 0; i < NPLANS; ++i)
    {
      planvec->plan[i] = (QO_PLAN *) NULL;
    }
}

void
qo_uninit_planvec (QO_PLANVEC * planvec)
{
  int i;

  for (i = 0; i < planvec->nplans; ++i)
    {
      qo_plan_del_ref (planvec->plan[i]);
    }

  planvec->overflow = false;
  planvec->nplans = 0;
}

void
qo_dump_planvec (QO_PLANVEC * planvec, FILE * f, int indent)
{
  int i;
  int positive_indent = indent < 0 ? -indent : indent;

  if (planvec->overflow)
    {
      fputs ("(overflowed) ", f);
    }

  for (i = 0; i < planvec->nplans; ++i)
    {
      qo_plan_fprint (planvec->plan[i], f, indent, NULL);
      fputs ("\n\n", f);
      indent = positive_indent;
    }
}

QO_PLAN_COMPARE_RESULT
qo_check_planvec (QO_PLANVEC * planvec, QO_PLAN * plan)
{
  /*
   * Check whether the new plan is definitely better than any of the
   * others.  Keep it if it is, or if it is incomparable and we still
   * have room in the planvec.  Return true if we keep the plan, false
   * if not.
   */
  int i;
  int already_retained;
  QO_PLAN_COMPARE_RESULT cmp;
  int num_eq;

  if (plan == NULL)
    {
      /* There is no new plan to be compared. */
      return PLAN_COMP_LT;
    }

  /* init */
  already_retained = 0;
  num_eq = 0;

  for (i = 0; i < planvec->nplans; i++)
    {
      cmp = qo_plan_cmp (planvec->plan[i], plan);

      /* cmp : PLAN_COMP_UNK, PLAN_COMP_LT, PLAN_COMP_EQ, PLAN_COMP_GT */
      if (cmp == PLAN_COMP_GT)
	{
	  /*
	   * The new plan is better than the previous one in the i'th
	   * slot.  Remove the old one, and if we haven't yet retained
	   * the new one, put it in the freshly-available slot.  If we
	   * have already retained the plan, pull the last element down
	   * from the end of the planvec and stuff it in this slot, and
	   * then check this slot all over again.  Don't forget to NULL
	   * out the old last element.
	   */
	  if (already_retained)
	    {
	      planvec->nplans--;
	      qo_plan_del_ref (planvec->plan[i]);
	      planvec->plan[i] = (i < planvec->nplans) ? planvec->plan[planvec->nplans] : NULL;
	      planvec->plan[planvec->nplans] = NULL;
	      /*
	       * Back up `i' so that we examine this slot again.
	       */
	      i--;
	    }
	  else
	    {
	      (void) qo_plan_add_ref (plan);
	      qo_plan_del_ref (planvec->plan[i]);
	      planvec->plan[i] = plan;
	    }
	  already_retained = 1;
	}
      else if (cmp == PLAN_COMP_EQ)
	{
	  /* found equal cost plan already found */
	  num_eq++;
	}
      else if (cmp == PLAN_COMP_LT)
	{
	  /*
	   * The new plan is worse than some plan that we already have.
	   * There is no point in checking any others; give up and get
	   * out.
	   */
	  return PLAN_COMP_LT;
	}
    }

  /*
   * Ok, we've looked at all of the current plans.  It's possible to
   * get here and still not have retained the new plan if it couldn't
   * be determined whether it was definitely better or worse than any
   * of the plans we're already holding (or if we're not holding any).
   * Try to add it to the vector of plans.
   */
  if (!already_retained && !num_eq)
    {				/* all is PLAN_COMP_UNK */

      if (i < NPLANS)
	{
	  planvec->nplans++;
	  (void) qo_plan_add_ref (plan);
	  qo_plan_del_ref (planvec->plan[i]);
	  planvec->plan[i] = plan;
	  already_retained = 1;
	}
      else
	{
	  int best_vc_pid, best_tc_pid;
	  int worst_vc_pid, worst_tc_pid;
	  double vc, tc, p_vc, p_tc;
	  QO_PLAN *p;

	  /*
	   * We would like to keep this plan, but we're out of slots in
	   * the planvec.  For now, we just throw out one plan with the
	   * highest access cost.
	   */

	  /* STEP 1: found best plan */
	  best_vc_pid = best_tc_pid = -1;	/* init */

	  vc = plan->variable_cpu_cost + plan->variable_io_cost;
	  tc = plan->fixed_cpu_cost + plan->fixed_io_cost + vc;

	  for (i = 0; i < planvec->nplans; i++)
	    {
	      p = planvec->plan[i];
	      p_vc = p->variable_cpu_cost + p->variable_io_cost;
	      p_tc = p->fixed_cpu_cost + p->fixed_io_cost + p_vc;

	      if (p_vc < vc)
		{		/* found best variable cost plan */
		  best_vc_pid = i;
		  vc = p_vc;	/* save best variable cost */
		}

	      if (p_tc < tc)
		{		/* found best total cost plan */
		  best_tc_pid = i;
		  tc = p_tc;	/* save best total cost */
		}
	    }

	  /* STEP 2: found worst plan */
	  worst_vc_pid = worst_tc_pid = -1;	/* init */

	  vc = plan->variable_cpu_cost + plan->variable_io_cost;
	  tc = plan->fixed_cpu_cost + plan->fixed_io_cost + vc;

	  for (i = 0; i < planvec->nplans; i++)
	    {
	      p = planvec->plan[i];
	      p_vc = p->variable_cpu_cost + p->variable_io_cost;
	      p_tc = p->fixed_cpu_cost + p->fixed_io_cost + p_vc;

	      if (i != best_vc_pid && i != best_tc_pid)
		{
		  if (p_vc > vc)
		    {		/* found worst variable cost plan */
		      worst_vc_pid = i;
		      vc = p_vc;	/* save worst variable cost */
		    }

		  if (p_tc > tc)
		    {		/* found worst total cost plan */
		      worst_tc_pid = i;
		      tc = p_tc;	/* save worst total cost */
		    }
		}
	    }

	  if (worst_tc_pid != -1)
	    {			/* release worst total cost plan */
	      (void) qo_plan_add_ref (plan);
	      qo_plan_del_ref (planvec->plan[worst_tc_pid]);
	      planvec->plan[worst_tc_pid] = plan;
	      already_retained = 1;
	    }
	  else if (worst_vc_pid != -1)
	    {			/* release worst variable cost plan */
	      (void) qo_plan_add_ref (plan);
	      qo_plan_del_ref (planvec->plan[worst_vc_pid]);
	      planvec->plan[worst_vc_pid] = plan;
	      already_retained = 1;
	    }
	  else
	    {
	      /*
	       * The new plan is worse than some plan that we already have.
	       * There is no point in checking any others; give up and get
	       * out.
	       */
	      return PLAN_COMP_LT;
	    }
	}
    }

  if (already_retained)
    {
      return PLAN_COMP_GT;
    }
  else if (num_eq)
    {
      return PLAN_COMP_EQ;
    }

  return cmp;
}

QO_PLAN_COMPARE_RESULT
qo_cmp_planvec (QO_PLANVEC * planvec, QO_PLAN * plan)
{
  int i;
  QO_PLAN_COMPARE_RESULT cmp;

  cmp = PLAN_COMP_UNK;		/* init */

  for (i = 0; i < planvec->nplans; i++)
    {
      cmp = qo_plan_cmp (planvec->plan[i], plan);
      if (cmp != PLAN_COMP_UNK)
	{
	  return cmp;
	}
    }

  /* at here, all is PLAN_COMP_UNK */

  return cmp;
}

QO_PLAN *
qo_find_best_plan_on_planvec (QO_PLANVEC * planvec, double n)
{
  int i;
  QO_PLAN *best_plan, *plan;
  double fixed, variable, best_cost, cost;

  /* While initializing the cost to QO_INFINITY and starting the loop at i = 0 might look equivalent to this, it
   * actually loses if all of the elements in the vector have cost QO_INFINITY, because the comparison never succeeds
   * and we never make plan non-NULL.  This is very bad for those callers above who believe that we're returning
   * something useful.
   */

  best_plan = planvec->plan[0];
  fixed = best_plan->fixed_cpu_cost + best_plan->fixed_io_cost;
  variable = best_plan->variable_cpu_cost + best_plan->variable_io_cost;
  best_cost = fixed + (n * variable);
  for (i = 1; i < planvec->nplans; i++)
    {
      plan = planvec->plan[i];
      fixed = plan->fixed_cpu_cost + plan->fixed_io_cost;
      variable = plan->variable_cpu_cost + plan->variable_io_cost;
      cost = fixed + (n * variable);

      if (cost < best_cost)
	{
	  best_plan = plan;
	  best_cost = cost;
	}
    }

  return best_plan;
}
