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

static int infos_allocated = 0;
static int infos_deallocated = 0;

void
qo_info_nodes_init (QO_ENV * env)
{
  infos_allocated = 0;
  infos_deallocated = 0;
}

QO_INFO *
qo_alloc_info (QO_PLANNER * planner, BITSET * nodes, BITSET * terms, BITSET * eqclasses, double cardinality,
	       double total_rows)
{
  QO_INFO *info;
  int i;
  int EQ;

  info = (QO_INFO *) malloc (sizeof (QO_INFO));
  if (info == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, sizeof (QO_INFO));
      return NULL;
    }

  i = 0;
  EQ = planner->EQ;

  info->env = planner->env;
  info->planner = planner;

  bitset_init (&(info->nodes), planner->env);
  bitset_init (&(info->terms), planner->env);
  bitset_init (&(info->eqclasses), planner->env);
  bitset_init (&(info->projected_segs), planner->env);

  bitset_assign (&info->nodes, nodes);
  bitset_assign (&info->terms, terms);
  bitset_assign (&info->eqclasses, eqclasses);
  qo_compute_projected_segs (planner, nodes, terms, &info->projected_segs);
  info->projected_size = qo_compute_projected_size (planner, &info->projected_segs);
  info->cardinality = cardinality;
  info->scan_rows = cardinality;	/* after iscan_cost, sscan_cost. it'll be replaced accurately */
  info->total_rows = total_rows;
  info->group_rows = cardinality;	/* it is recalculated in qo_sort_new() */
  info->hit_prob = 1.0;

  qo_init_planvec (&info->best_no_order);

  /*
   * Set aside an array for ordered plans.  Each element of the array
   * holds a plan that is ordered according to the corresponding
   * equivalence class.
   *
   * If this malloc() fails, we'll lose the memory pointed to by
   * info.  I'll take the chance.
   */
  info->planvec = NULL;
  if (EQ > 0)
    {
      info->planvec = (QO_PLANVEC *) malloc (sizeof (QO_PLANVEC) * EQ);
      if (info->planvec == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, sizeof (QO_PLANVEC) * EQ);
	  free_and_init (info);
	  return NULL;
	}
    }

  for (i = 0; i < EQ; ++i)
    {
      qo_init_planvec (&info->planvec[i]);
    }

  info->join_unit = planner->join_unit;	/* init */

  info->detached = false;

  infos_allocated++;

  /* insert into the head of alloced info list */
  info->next = planner->info_list;
  planner->info_list = info;

  return info;
}

void
qo_free_info (QO_INFO * info)
{
  if (info == NULL)
    {
      return;
    }

  qo_detach_info (info);

  bitset_delset (&(info->nodes));
  bitset_delset (&(info->terms));
  bitset_delset (&(info->eqclasses));
  bitset_delset (&(info->projected_segs));

  free_and_init (info);

  infos_deallocated++;
}

void
qo_detach_info (QO_INFO * info)
{
  /*
   * If the node hasn't already been detached, detach it now and give
   * up references to all plans that are no longer needed.
   */
  if (!info->detached)
    {
      int i;
      int EQ = info->planner->EQ;

      for (i = 0; i < EQ; ++i)
	{
	  qo_uninit_planvec (&info->planvec[i]);
	}
      free_and_init (info->planvec);
      info->detached = true;

      qo_uninit_planvec (&info->best_no_order);
    }
}

bool
qo_check_new_best_plan_on_info (QO_INFO * info, QO_PLAN * plan)
{
  QO_PLAN_COMPARE_RESULT cmp;
  QO_EQCLASS *order;

  order = plan->order;
  if (order && bitset_is_empty (&(QO_EQCLASS_SEGS (order))))
    {
      /* Then this "equivalence class" is a phony fabricated especially for a complex merge term. skip out */
      cmp = PLAN_COMP_LT;
    }
  else
    {
      cmp = qo_check_planvec (&info->best_no_order, plan);

      if (cmp == PLAN_COMP_GT)
	{
	  if (plan->plan_type != QO_PLANTYPE_SORT || plan->plan_un.sort.sort_type != SORT_LIMIT)
	    {
	      int i, EQ;
	      QO_PLAN *new_plan, *sort_plan, *best_plan;
	      QO_ENV *env;
	      QO_PLAN_COMPARE_RESULT new_cmp;

	      env = info->env;

	      EQ = info->planner->EQ;
	      best_plan = qo_find_best_plan_on_planvec (&info->best_no_order, 1.0);
	      if (QO_ENV_USE_SORT_LIMIT (env) == QO_SL_USE && !best_plan->has_sort_limit
		  && bitset_is_equivalent (&QO_ENV_SORT_LIMIT_NODES (env), &info->nodes))
		{
		  /* generate a SORT_LIMIT plan over this plan */
		  sort_plan = qo_sort_new (best_plan, QO_UNORDERED, SORT_LIMIT);
		  if (sort_plan != NULL)
		    {
		      if (qo_check_plan_on_info (info, sort_plan) > 0)
			{
			  best_plan = sort_plan;
			}
		    }
		}
	      /*
	       * Check to see if any of the ordered solutions can be made
	       * cheaper by sorting this new plan.
	       */
	      for (i = 0; i < EQ; i++)
		{
		  order = &info->planner->eqclass[i];

		  new_plan = qo_plan_order_by (best_plan, order);
		  if (new_plan)
		    {
		      new_cmp = qo_check_planvec (&info->planvec[i], new_plan);
		      if (new_cmp == PLAN_COMP_LT || new_cmp == PLAN_COMP_EQ)
			{
			  qo_plan_release (new_plan);
			}
		    }
		}
	    }
	}
    }

  if (cmp == PLAN_COMP_LT || cmp == PLAN_COMP_EQ)
    {
      qo_plan_release (plan);
      return false;
    }

  return true;
}

int
qo_check_plan_on_info (QO_INFO * info, QO_PLAN * plan)
{
  QO_INFO *best_info;
  QO_EQCLASS *plan_order;
  QO_PLAN_COMPARE_RESULT cmp;
  bool found_new_best;

  if (info == NULL || plan == NULL)
    {
      return 0;
    }

  /* init */
  found_new_best = false;
  best_info = info->planner->best_info;
  plan_order = plan->order;

  /* if the plan is of type QO_SCANMETHOD_INDEX_ORDERBY_SCAN but it doesn't skip the orderby, we release the plan. */
  if (qo_is_iscan_from_orderby (plan)
      && !(plan->top_rooted ? plan->plan_un.scan.index->head->orderby_skip : qo_plan_is_orderby_skip_candidate (plan)))
    {
      qo_plan_release (plan);
      return 0;
    }

  /* if the plan is of type QO_SCANMETHOD_INDEX_GRUOPBY_SCAN but it doesn't skip the groupby, we release the plan. */
  if (qo_is_iscan_from_groupby (plan) && !plan->plan_un.scan.index->head->groupby_skip)
    {
      qo_plan_release (plan);
      return 0;
    }

  /*
   * If the cost of the new Plan already exceeds the cost of the best
   * known solution with the same order, there is no point in
   * remembering the new plan.
   */
  if (best_info)
    {
      cmp =
	qo_cmp_planvec (plan_order ==
			QO_UNORDERED ? &best_info->best_no_order : &best_info->planvec[QO_EQCLASS_IDX (plan_order)],
			plan);
      /* cmp : PLAN_COMP_UNK, PLAN_COMP_LT, PLAN_COMP_EQ, PLAN_COMP_GT */
      if (cmp == PLAN_COMP_LT || cmp == PLAN_COMP_EQ)
	{
	  qo_plan_release (plan);
	  return 0;
	}
    }

  /*
   * The only time we will keep an unordered plan is if it is cheaper
   * than any other plan we have seen so far (ordered or unordered).
   * Only ordered plans are kept in the _plan vector.
   */
  if (plan_order == QO_UNORDERED)
    {
      found_new_best = qo_check_new_best_plan_on_info (info, plan);
    }
  else
    {
      /*
       * If we get here, we are dealing with an ordered plan.  Check
       * whether we already have memo-ized a plan for this particular scan
       * order.  If so, see if this new plan is an improvement.
       */

      cmp = qo_check_planvec (&info->planvec[QO_EQCLASS_IDX (plan_order)], plan);
      if (cmp == PLAN_COMP_GT)
	{
	  (void) qo_check_new_best_plan_on_info (info, plan);
	  found_new_best = true;
	}
      else
	{
	  qo_plan_release (plan);
	  return 0;
	}
    }

  if (found_new_best != true)
    {
      return 0;
    }

  /* save the last join level; used for cache */
  info->join_unit = info->planner->join_unit;

  return 1;
}

QO_PLAN *
qo_find_best_nljoin_inner_plan_on_info (QO_PLAN * outer, QO_INFO * info, JOIN_TYPE join_type, int idx_join_plan_n)
{
  QO_PLANVEC *pv;
  QO_PLAN *temp, *best_plan, *inner;
  double temp_cost, best_cost;
  int i;

  /* init */
  best_plan = NULL;
  best_cost = 0;

  /* alloc temporary nl-join plan */
  temp = qo_plan_malloc (info->env);
  if (temp == NULL)
    {
      return NULL;
    }

  temp->info = info;
  temp->refcount = 0;
  temp->top_rooted = false;
  temp->well_rooted = false;
  temp->iscan_sort_list = NULL;
  temp->analytic_eval_list = NULL;
  temp->plan_type = QO_PLANTYPE_JOIN;
  temp->plan_un.join.join_type = join_type;	/* set nl-join type */
  temp->plan_un.join.outer = outer;	/* set outer */
  temp->plan_un.join.inner = NULL;

/* temporary join plan: initialize join-side bitsets explicitly */
  bitset_init (&(temp->plan_un.join.join_terms), info->env);
  bitset_init (&(temp->plan_un.join.hash_terms), info->env);
  bitset_init (&(temp->plan_un.join.during_join_terms), info->env);
  bitset_init (&(temp->plan_un.join.after_join_terms), info->env);



  temp->multi_range_opt_use = PLAN_MULTI_RANGE_OPT_NO;

  for (i = 0, pv = &info->best_no_order; i < pv->nplans; i++)
    {
      inner = pv->plan[i];	/* set inner */

      /* if already found idx-join, then exclude sequential inner */
      if (idx_join_plan_n > 0)
	{
	  if (qo_is_seq_scan (inner) || inner->has_sort_limit)
	    {
	      continue;
	    }
	}

      temp->plan_un.join.inner = inner;
      qo_nljoin_cost (temp);
      temp_cost = temp->fixed_cpu_cost + temp->fixed_io_cost + temp->variable_cpu_cost + temp->variable_io_cost;
      if (best_plan == NULL || temp_cost < best_cost)
	{
	  /* save new best inner */
	  best_cost = temp_cost;
	  best_plan = inner;
	}
    }

  /* free temp plan */
  bitset_delset (&(temp->plan_un.join.join_terms));
  bitset_delset (&(temp->plan_un.join.hash_terms));
  bitset_delset (&(temp->plan_un.join.during_join_terms));
  bitset_delset (&(temp->plan_un.join.after_join_terms));

  temp->plan_un.join.outer = NULL;
  temp->plan_un.join.inner = NULL;

  qo_plan_add_to_free_list (temp, NULL);

  return best_plan;
}

QO_PLAN *
qo_find_best_plan_on_info (QO_INFO * info, QO_EQCLASS * order, double n)
{
  QO_PLANVEC *pv;

  if (order == QO_UNORDERED)
    {
      pv = &info->best_no_order;
    }
  else
    {
      int order_idx = QO_EQCLASS_IDX (order);
      if (info->planvec[order_idx].nplans == 0)
	{
	  QO_PLAN *planp;

	  planp = qo_sort_new (qo_find_best_plan_on_planvec (&info->best_no_order, n), order, SORT_TEMP);

	  qo_check_planvec (&info->planvec[order_idx], planp);
	}
      pv = &info->planvec[order_idx];
    }

  return qo_find_best_plan_on_planvec (pv, n);
}

int
qo_examine_idx_join (QO_INFO * info, JOIN_TYPE join_type, QO_INFO * outer, QO_INFO * inner, BITSET * afj_terms,
		     BITSET * sarged_terms, BITSET * pinned_subqueries)
{
  int n = 0;
  QO_NODE *inner_node;

  /* check for right outer join; */
  if (join_type == JOIN_RIGHT)
    {
      if (bitset_cardinality (&(outer->nodes)) != 1)
	{			/* not single class spec */
	  /* inner of correlated index join should be plain class access */
	  goto exit;
	}

      inner_node = QO_ENV_NODE (outer->env, bitset_first_member (&(outer->nodes)));
      if (QO_NODE_HINT (inner_node) & PT_HINT_ORDERED)
	{
	  /* join hint: force join left-to-right; skip idx-join because, these are only support left outer join */
	  goto exit;
	}
    }
  else
    {
      inner_node = QO_ENV_NODE (inner->env, bitset_first_member (&(inner->nodes)));
    }

  /* inner is single class spec */
  if (QO_NODE_HINT (inner_node) & (PT_HINT_USE_IDX | PT_HINT_USE_NL))
    {
      /* join hint: force idx-join */
    }
  else if (QO_NODE_HINT (inner_node) & PT_HINT_USE_MERGE)
    {
      /* join hint: force merge-join; skip idx-join */
      goto exit;
    }
  else if (!(QO_NODE_HINT (inner_node) & PT_HINT_NO_USE_HASH) && (QO_NODE_HINT (inner_node) & PT_HINT_USE_HASH))
    {
      /* join hint: force hash-join; skip idx-join */
      goto exit;
    }
  else
    {
      /* fall through */
    }

  /* check whether we can build a nested loop join with a correlated index scan. That is, is the inner term a scan of a
   * single node, and can this join term be used as an index with respect to that node? If so, we can build a special
   * kind of plan to exploit that.
   */
  if (join_type == JOIN_RIGHT)
    {
      /* if right outer join, select outer plan from the inner node and inner plan from the outer node, and do left
       * outer join
       */
      n = qo_examine_correlated_index (info, JOIN_LEFT, inner, outer, afj_terms, sarged_terms, pinned_subqueries);
    }
  else
    {
      n = qo_examine_correlated_index (info, join_type, outer, inner, afj_terms, sarged_terms, pinned_subqueries);
    }

exit:

  return n;
}

int
qo_examine_nl_join (QO_INFO * info, JOIN_TYPE join_type, QO_INFO * outer, QO_INFO * inner, BITSET * nl_join_terms,
		    BITSET * duj_terms, BITSET * afj_terms, BITSET * sarged_terms, BITSET * pinned_subqueries,
		    int idx_join_plan_n, BITSET * hash_terms)
{
  int n = 0;
  QO_PLAN *outer_plan, *inner_plan;
  QO_NODE *inner_node;

  if (join_type == JOIN_RIGHT)
    {
      /* converse outer join type */
      join_type = JOIN_LEFT;

      if (bitset_intersects (sarged_terms, &(info->env->fake_terms)))
	{
	  goto exit;
	}

      {
	int t;
	QO_TERM *term;
	BITSET_ITERATOR iter;

	for (t = bitset_iterate (nl_join_terms, &iter); t != -1; t = bitset_next_member (&iter))
	  {
	    term = QO_ENV_TERM (info->env, t);
	    if (QO_TERM_CLASS (term) == QO_TC_DEP_LINK)
	      {
		goto exit;
	      }
	  }			/* for (t = ...) */
      }

      if (bitset_cardinality (&(outer->nodes)) == 1)
	{			/* single class spec */
	  inner_node = QO_ENV_NODE (outer->env, bitset_first_member (&(outer->nodes)));
	  if (QO_NODE_HINT (inner_node) & PT_HINT_ORDERED)
	    {
	      /* join hint: force join left-to-right; skip idx-join because, these are only support left outer join */
	      goto exit;
	    }

	  if (QO_NODE_HINT (inner_node) & PT_HINT_USE_NL)
	    {
	      /* join hint: force nl-join */
	    }
	  else if (QO_NODE_HINT (inner_node) & (PT_HINT_USE_IDX | PT_HINT_USE_MERGE))
	    {
	      /* join hint: force idx-join, merge-join; skip nl-join */
	      goto exit;
	    }
	  else if (!(QO_NODE_HINT (inner_node) & PT_HINT_NO_USE_HASH) && (QO_NODE_HINT (inner_node) & PT_HINT_USE_HASH))
	    {
	      /* join hint: force hash-join; skip nl-join */
	      goto exit;
	    }
	  else
	    {
	      /* fall through */
	    }
	}

      outer_plan = qo_find_best_plan_on_info (inner, QO_UNORDERED, 1.0);
      if (outer_plan == NULL)
	{
	  goto exit;
	}
      inner_plan = qo_find_best_nljoin_inner_plan_on_info (outer_plan, outer, join_type, idx_join_plan_n);
      if (inner_plan == NULL)
	{
	  goto exit;
	}
    }
  else
    {
      /* At here, inner is single class spec */
      inner_node = QO_ENV_NODE (inner->env, bitset_first_member (&(inner->nodes)));
      if (QO_NODE_HINT (inner_node) & PT_HINT_USE_NL)
	{
	  /* join hint: force nl-join */
	}
      else if (QO_NODE_HINT (inner_node) & (PT_HINT_USE_IDX | PT_HINT_USE_MERGE))
	{
	  /* join hint: force idx-join, merge-join; skip nl-join */
	  goto exit;
	}
      else if (!(QO_NODE_HINT (inner_node) & PT_HINT_NO_USE_HASH) && (QO_NODE_HINT (inner_node) & PT_HINT_USE_HASH))
	{
	  /* join hint: force hash-join; skip nl-join */
	  goto exit;
	}
      else
	{
	  /* fall through */
	}

      outer_plan = qo_find_best_plan_on_info (outer, QO_UNORDERED, 1.0);
      if (outer_plan == NULL)
	{
	  goto exit;
	}
      inner_plan = qo_find_best_nljoin_inner_plan_on_info (outer_plan, inner, join_type, idx_join_plan_n);
      if (inner_plan == NULL)
	{
	  goto exit;
	}
    }

#if 0				/* CHAINS_ONLY */
  /* If CHAINS_ONLY is defined, we want the optimizer constrained to produce only left-linear trees of joins, i.e., no
   * inner term can itself be a join or a follow.
   */

  if (inner_plan->plan_type != QO_PLANTYPE_SCAN)
    {
      if (inner_plan->plan_type == QO_PLANTYPE_SORT && inner_plan->order == QO_UNORDERED)
	{
	  /* inner has temporary list file plan; it's ok */
	  ;
	}
      else
	{
	  goto exit;
	}
    }
#endif /* CHAINS_ONLY */

#if 0				/* JOIN_FOLLOW_RESTRICTION */
  /* Under this restriction, we are not permitted to produce plans that have follow nodes sandwiched between joins.
   * Don't ask why.
   */

  if (outer_plan->plan_type == QO_PLANTYPE_FOLLOW && QO_PLAN_SUBJOINS (outer_plan))
    {
      goto exit;
    }
  if (inner_plan->plan_type == QO_PLANTYPE_FOLLOW && QO_PLAN_SUBJOINS (inner_plan))
    {
      goto exit;
    }
#endif /* JOIN_FOLLOW_RESTRICTION */

  /* look for the best nested loop solution we can find.  Since the subnodes are already keeping track of the
   * lowest-cost plan they have seen, we needn't do any search here to find the cheapest nested loop join we can
   * produce for this combination.
   */
  n =
    qo_check_plan_on_info (info,
			   qo_join_new (info, join_type, QO_JOINMETHOD_NL_JOIN, outer_plan, inner_plan, nl_join_terms,
					duj_terms, afj_terms, sarged_terms, pinned_subqueries, hash_terms));

exit:

  return n;
}

int
qo_examine_merge_join (QO_INFO * info, JOIN_TYPE join_type, QO_INFO * outer, QO_INFO * inner, BITSET * sm_join_terms,
		       BITSET * duj_terms, BITSET * afj_terms, BITSET * sarged_terms, BITSET * pinned_subqueries)
{
  int n = 0;
  QO_PLAN *outer_plan, *inner_plan;
  QO_NODE *inner_node;
  QO_EQCLASS *order = QO_UNORDERED;
  int t;
  BITSET_ITERATOR iter;
  QO_TERM *term;
  BITSET empty_terms;
  bitset_init (&empty_terms, info->env);

  /* If any of the sarged terms are fake terms, we can't implement this join as a merge join, because the timing
   * assumptions required by the fake terms won't be satisfied.  Nested loops are the only joins that will work.
   */
  if (bitset_intersects (sarged_terms, &(info->env->fake_terms)))
    {
      goto exit;
    }

  /* examine ways of producing ordered results.  For each ordering, check whether the inner and outer subresults can be
   * produced in that order.  If so, check a merge join plan on that order.
   */
  for (t = bitset_iterate (sm_join_terms, &iter); t != -1; t = bitset_next_member (&iter))
    {
      term = QO_ENV_TERM (info->env, t);
      order = QO_TERM_EQCLASS (term);
      if (order != QO_UNORDERED)
	{
	  break;
	}
    }

  if (order == QO_UNORDERED)
    {
      goto exit;
    }

#ifdef OUTER_MERGE_JOIN_RESTRICTION
  if (IS_OUTER_JOIN_TYPE (join_type))
    {
      int node_idx;

      term = QO_ENV_TERM (info->env, bitset_first_member (sm_join_terms));
      node_idx = (join_type == JOIN_LEFT) ? QO_NODE_IDX (QO_TERM_HEAD (term)) : QO_NODE_IDX (QO_TERM_TAIL (term));
      for (t = bitset_iterate (duj_terms, &iter); t != -1; t = bitset_next_member (&iter))
	{
	  term = QO_ENV_TERM (info->env, t);
	  if (!BITSET_MEMBER (QO_TERM_NODES (term), node_idx))
	    {
	      goto exit;
	    }
	}
    }
#endif /* OUTER_MERGE_JOIN_RESTRICTION */

  /* At here, inner is single class spec */
  inner_node = QO_ENV_NODE (inner->env, bitset_first_member (&(inner->nodes)));

  if (QO_NODE_HINT (inner_node) & PT_HINT_USE_MERGE)
    {
      /* join hint: force m-join */
    }
  else if (QO_NODE_HINT (inner_node) & (PT_HINT_USE_NL | PT_HINT_USE_IDX))
    {
      /* join hint: force nl-join, idx-join; skip m-join */
      goto exit;
    }
  else if (!(QO_NODE_HINT (inner_node) & PT_HINT_NO_USE_HASH) && (QO_NODE_HINT (inner_node) & PT_HINT_USE_HASH))
    {
      /* join hint: force hash-join; skip m-join */
      goto exit;
    }
  else if (!prm_get_bool_value (PRM_ID_OPTIMIZER_ENABLE_MERGE_JOIN))
    {
      /* optimizer prm: keep out m-join; */
      goto exit;
    }
  else
    {
      /* fall through */
    }

  outer_plan = qo_find_best_plan_on_info (outer, order, 1.0);
  if (outer_plan == NULL)
    {
      goto exit;
    }

  inner_plan = qo_find_best_plan_on_info (inner, order, 1.0);
  if (inner_plan == NULL)
    {
      goto exit;
    }

#ifdef CHAINS_ONLY
  /* If CHAINS_ONLY is defined, we want the optimizer constrained to produce only left-linear trees of joins, i.e., no
   * inner term can itself be a join or a follow.
   */

  if (inner_plan->plan_type != QO_PLANTYPE_SCAN)
    {
      if (inner_plan->plan_type == QO_PLANTYPE_SORT && inner_plan->order == QO_UNORDERED)
	{
	  /* inner has temporary list file plan; it's ok */
	  ;
	}
      else
	{
	  goto exit;
	}
    }
#endif /* CHAINS_ONLY */

#if 0				/* JOIN_FOLLOW_RESTRICTION */
  /* Under this restriction, we are not permitted to produce plans that have follow nodes sandwiched between joins.
   * Don't ask why.
   */

  if (outer_plan->plan_type == QO_PLANTYPE_FOLLOW && QO_PLAN_SUBJOINS (outer_plan))
    {
      goto exit;
    }
  if (inner_plan->plan_type == QO_PLANTYPE_FOLLOW && QO_PLAN_SUBJOINS (inner_plan))
    {
      goto exit;
    }
#endif /* JOIN_FOLLOW_RESTRICTION */

  n =
    qo_check_plan_on_info (info,
			   qo_join_new (info, join_type, QO_JOINMETHOD_MERGE_JOIN, outer_plan, inner_plan,
					sm_join_terms, duj_terms, afj_terms, sarged_terms, pinned_subqueries,
					&empty_terms));

exit:

  return n;
}

int
qo_examine_hash_join (QO_INFO * info, JOIN_TYPE join_type, QO_INFO * outer, QO_INFO * inner, BITSET * hash_join_terms,
		      BITSET * duj_terms, BITSET * afj_terms, BITSET * sarged_terms, BITSET * pinned_subqueries)
{
  QO_PLAN *outer_plan, *inner_plan;
  QO_NODE *outer_node, *inner_node;
  QO_NODE_INDEX *node_index;
  QO_TERM *term;
  BITSET_ITERATOR bitset_iter;
  int bitset_index;
  int i, n = 0;

  UINT64 mem_limit = prm_get_bigint_value (PRM_ID_MAX_HASH_LIST_SCAN_SIZE);
  if (mem_limit <= 0)
    {
      goto exit;		/* give up */
    }

  /* If any of the sarged terms are fake terms, we can't implement this join as a merge join, because the timing
   * assumptions required by the fake terms won't be satisfied.  Nested loops are the only joins that will work.
   */
  if (bitset_intersects (sarged_terms, &info->env->fake_terms))
    {
      goto exit;
    }

  /* A query using a predicate for an oid is rewritten as a path join query.  The path join query is executed
   * as a left outer join, so if the path join query is not executed with a follow plan, results with NULL values
   * are retrieved even if the join predicate is not satisfied.
   * 
   *   e.g. drop table if exists t;
   *        create table t (c int) dont_reuse_oid;
   *        insert into t values (1);
   *        select dual into :dummy_oid from dual limit 1;
   * 
   *        select * from t where t = :dummy_oid;
   *
   *        -- rewritten query
   *        select dt_1.da_2.t.c from table({:dummy_oid}) dt_1 (da_2)
   * 
   *        -- Query plan: follow
   *        There are no results.
   *        0 row selected.
   *
   *        -- Query plan: hash-join (left outer join)
   *        c1: NULL
   *        1 row selected.
   * 
   * This code prevents the path join query from being executed as a hash join plan rather than as a follow plan.
   */
  for (bitset_index = bitset_iterate (hash_join_terms, &bitset_iter); bitset_index != -1;
       bitset_index = bitset_next_member (&bitset_iter))
    {
      term = QO_ENV_TERM (info->env, bitset_index);
      if (QO_IS_PATH_TERM (term) && QO_TERM_JOIN_TYPE (term) != JOIN_INNER)
	{
	  goto exit;		/* give up */
	}
    }

  /* At here, inner is single class spec */
  inner_node = QO_ENV_NODE (inner->env, bitset_first_member (&(inner->nodes)));

  if (QO_NODE_HINT (inner_node) & PT_HINT_NO_USE_HASH)
    {
      /* join hint: disable hash-join */
      goto exit;
    }
  else if (QO_NODE_HINT (inner_node) & PT_HINT_USE_HASH)
    {
      /* join hint: force hash-join */
    }
  else if (QO_NODE_HINT (inner_node) & (PT_HINT_USE_NL | PT_HINT_USE_IDX | PT_HINT_USE_MERGE))
    {
      /* join hint: force nl-join, idx-join, m-join; skip hash-join */
      goto exit;
    }
  else
    {
      /* default: disable hash-join */
#if TEST_HASH_JOIN_ENABLE
      /* fall through */
#else /* TEST_HASH_JOIN_ENABLE */
      goto exit;
#endif /* TEST_HASH_JOIN_ENABLE */
    }

  /* Check if a click counter is set. */
  if (QO_ENV_PT_TREE (info->env)->flag.is_click_counter)
    {
      goto exit;		/* give up */
    }

  /* Check if a key limit is set. */
  node_index = QO_NODE_INDEXES (inner_node);
  if (node_index != NULL)
    {
      for (i = 0; i < QO_NI_N (node_index); i++)
	{
	  if (QO_NI_ENTRY (node_index, i)->head->key_limit != NULL)
	    {
	      goto exit;	/* give up */
	    }
	}
    }

  for (bitset_index = bitset_iterate (&outer->nodes, &bitset_iter); bitset_index != -1;
       bitset_index = bitset_next_member (&bitset_iter))
    {
      outer_node = QO_ENV_NODE (outer->env, bitset_index);

      node_index = QO_NODE_INDEXES (outer_node);
      if (node_index != NULL)
	{
	  for (i = 0; i < QO_NI_N (node_index); i++)
	    {
	      if (QO_NI_ENTRY (node_index, i)->head->key_limit != NULL)
		{
		  goto exit;	/* give up */
		}
	    }
	}
    }

  outer_plan = qo_find_best_plan_on_info (outer, QO_UNORDERED, 1.0);
  if (outer_plan == NULL)
    {
      goto exit;
    }

  inner_plan = qo_find_best_plan_on_info (inner, QO_UNORDERED, 1.0);
  if (inner_plan == NULL)
    {
      goto exit;
    }

  n =
    qo_check_plan_on_info (info,
			   qo_join_new (info, join_type, QO_JOINMETHOD_HASH_JOIN, outer_plan, inner_plan,
					hash_join_terms, duj_terms, afj_terms, sarged_terms, pinned_subqueries,
					hash_join_terms));

exit:
  return n;
}

int
qo_examine_correlated_index (QO_INFO * info, JOIN_TYPE join_type, QO_INFO * outer, QO_INFO * inner, BITSET * afj_terms,
			     BITSET * sarged_terms, BITSET * pinned_subqueries)
{
  QO_NODE *nodep;
  QO_NODE_INDEX *node_indexp;
  QO_NODE_INDEX_ENTRY *ni_entryp;
  QO_INDEX_ENTRY *index_entryp;
  QO_PLAN *outer_plan;
  int i, n = 0;
  BITSET_ITERATOR iter;
  int t;
  QO_TERM *termp;
  int num_only_args;
  BITSET indexable_terms;

  /* outer plan */
  outer_plan = qo_find_best_plan_on_info (outer, QO_UNORDERED, 1.0);
  if (outer_plan == NULL)
    {
      return 0;
    }

#if 0				/* JOIN_FOLLOW_RESTRICTION */
  /* Under this restriction, we are not permitted to produce plans that have follow nodes sandwiched between joins.
   * Don't ask why.
   */

  if (outer_plan->plan_type == QO_PLANTYPE_FOLLOW && QO_PLAN_SUBJOINS (outer_plan))
    {
      return 0;
    }
#endif /* JOIN_FOLLOW_RESTRICTION */

  /* inner node and its indexes */
  nodep = &info->planner->node[bitset_first_member (&(inner->nodes))];
  node_indexp = QO_NODE_INDEXES (nodep);
  if (node_indexp == NULL)
    {
      /* inner does not have any usable index */
      return 0;
    }

  bitset_init (&indexable_terms, info->env);

  /* We're interested in all of the terms so combine 'join_term' and 'sarged_terms' together. */
  if (IS_OUTER_JOIN_TYPE (join_type))
    {
      for (t = bitset_iterate (sarged_terms, &iter); t != -1; t = bitset_next_member (&iter))
	{

	  termp = QO_ENV_TERM (QO_NODE_ENV (nodep), t);

	  if (QO_TERM_CLASS (termp) == QO_TC_AFTER_JOIN)
	    {
	      /* exclude after-join term in 'sarged_terms' */
	      continue;
	    }

	  bitset_add (&indexable_terms, t);
	}
    }
  else
    {
      bitset_union (&indexable_terms, sarged_terms);
    }

  /* finally, combine inner plan's 'sarg term' together */
  bitset_union (&indexable_terms, &(QO_NODE_SARGS (nodep)));

  num_only_args = 0;		/* init */

  /* Iterate through the indexes attached to this node and look for ones which are a subset of the terms that we're
   * interested in. For each applicable index, register a plans and compute the cost.
   */
  for (i = 0; i < QO_NI_N (node_indexp); i++)
    {
      /* pointer to QO_NODE_INDEX_ENTRY structure */
      ni_entryp = QO_NI_ENTRY (node_indexp, i);
      /* pointer to QO_INDEX_ENTRY structure */
      index_entryp = (ni_entryp)->head;
      if (index_entryp->force < 0)
	{
	  continue;		/* is disabled index; skip and go ahead */
	}

      /* the index has terms which are a subset of the terms that we're interested in */
      if (bitset_intersects (&indexable_terms, &(index_entryp->terms)))
	{

	  if (!bitset_intersects (sarged_terms, &(index_entryp->terms)))
	    {
	      /* there is not join-edge, only inner sargs */
	      num_only_args++;
	      continue;
	    }

	  /* generate join index scan using 'ni_entryp' */
	  n +=
	    qo_generate_join_index_scan (info, join_type, outer_plan, inner, nodep, ni_entryp, &indexable_terms,
					 afj_terms, sarged_terms, pinned_subqueries);
	}
    }

  if (QO_NODE_HINT (nodep) & PT_HINT_USE_IDX)
    {
      /* join hint: force idx-join */
      if (n == 0 && num_only_args)
	{			/* not found 'idx-join' plan */
	  /* Re-Iterate */
	  for (i = 0; i < QO_NI_N (node_indexp); i++)
	    {
	      /* pointer to QO_NODE_INDEX_ENTRY structure */
	      ni_entryp = QO_NI_ENTRY (node_indexp, i);
	      /* pointer to QO_INDEX_ENTRY structure */
	      index_entryp = (ni_entryp)->head;
	      if (index_entryp->force < 0)
		{
		  continue;	/* is disabled index; skip and go ahead */
		}

	      /* the index has terms which are a subset of the terms that we're intersted in */
	      if (bitset_intersects (&indexable_terms, &(index_entryp->terms)))
		{
		  if (bitset_intersects (sarged_terms, &(index_entryp->terms)))
		    {
		      /* there is join-edge; already examined */
		      continue;
		    }

		  /* generate join index scan using 'ni_entryp' */
		  n +=
		    qo_generate_join_index_scan (info, join_type, outer_plan, inner, nodep, ni_entryp, &indexable_terms,
						 afj_terms, sarged_terms, pinned_subqueries);
		}
	    }
	}
    }

  bitset_delset (&indexable_terms);

  return n;
}

int
qo_examine_follow (QO_INFO * info, QO_TERM * path_term, QO_INFO * head_info, BITSET * sarged_terms,
		   BITSET * pinned_subqueries)
{
  PT_NODE *entity_spec;
  /*
   * Examine the feasibility of a follow plan implementation for this
   * edge.  Don't build follow plans if the tail of the path is an rdb
   * proxy; these things *have* to be implemented via joins.
   */
  entity_spec = path_term->tail->entity_spec;
  if (entity_spec->info.spec.flat_entity_list == NULL)
    {
      return 0;
    }

  return qo_check_plan_on_info (info,
				qo_follow_new (info, qo_find_best_plan_on_info (head_info, QO_UNORDERED, 1.0),
					       path_term, sarged_terms, pinned_subqueries));

}

void
qo_compute_projected_segs (QO_PLANNER * planner, BITSET * nodes, BITSET * terms, BITSET * projected)
{
  /*
   * Figure out which of the attributes of the nodes joined by the
   * terms in 'terms' need to be projected out of the join in order to
   * satisfy the needs of higher-level plans.  An attribute will need
   * to preserved if it is to be produced as part of the final result
   * or if it is needed to compute some term that isn't included in
   * 'terms'.
   */

  BITSET required;
  int i;
  QO_TERM *term;

  BITSET_CLEAR (*projected);
  bitset_init (&required, planner->env);
  bitset_assign (&required, &(planner->final_segs));

  for (i = 0; i < (signed) planner->T; i++)
    {
      if (!BITSET_MEMBER (*terms, i))
	{
	  term = &planner->term[i];
	  bitset_union (&required, &(QO_TERM_SEGS (term)));
	}
    }

  for (i = 0; i < (signed) planner->N; ++i)
    {
      if (BITSET_MEMBER (*nodes, i))
	bitset_union (projected, &(QO_NODE_SEGS (&planner->node[i])));
    }

  bitset_intersect (projected, &required);
  bitset_delset (&required);
}

int
qo_compute_projected_size (QO_PLANNER * planner, BITSET * segset)
{
  BITSET_ITERATOR si;
  int i;
  int size;

  /*
   * 8 bytes overhead per record.
   */
  size = 8;

  for (i = bitset_iterate (segset, &si); i != -1; i = bitset_next_member (&si))
    {
      /*
       * Four bytes overhead for each field.
       */
      size += qo_seg_width (QO_ENV_SEG (planner->env, i)) + 4;
    }

  return size;
}

void
qo_dump_info (QO_INFO * info, FILE * f)
{
  /*
   * Dump the contents of this node for debugging scrutiny.
   */

  int i;

  fputs ("  projected segments: ", f);
  bitset_print (&(info->projected_segs), f);
  fputs ("\n", f);

  fputs ("  best: ", f);
  if (info->best_no_order.nplans > 0)
    {
      qo_dump_planvec (&info->best_no_order, f, -8);
    }
  else
    {
      fputs ("(empty)\n\n", f);
    }

  if (info->planvec)
    {
      for (i = 0; i < (signed) info->planner->EQ; ++i)
	{
	  if (info->planvec[i].nplans > 0)
	    {
	      char buf[20];

	      sprintf (buf, "[%d]: ", i);
	      fprintf (f, "%8s", buf);
	      qo_dump_planvec (&info->planvec[i], f, -8);
	    }
	}
    }
}

void
qo_info_stats (FILE * f)
{
  fprintf (f, "%d/%d info nodes allocated/deallocated\n", infos_allocated, infos_deallocated);
}

QO_PLANNER *
qo_alloc_planner (QO_ENV * env)
{
  int i;
  QO_PLANNER *planner;

  planner = (QO_PLANNER *) malloc (sizeof (QO_PLANNER));
  if (planner == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, sizeof (QO_PLANNER));
      return NULL;
    }

  env->planner = planner;

  planner->env = env;
  planner->node = env->nodes;
  planner->N = env->nnodes;
  planner->E = env->nedges;
  planner->M = 0;		/* later, set in qo_search_planner() */
  if (planner->N < 32)
    {
      planner->node_mask = (unsigned long) ((unsigned int) (1 << planner->N) - 1);
    }
  else
    {
      planner->node_mask = (unsigned long) DB_UINT32_MAX;
    }
  planner->join_unit = 0;
  planner->term = env->terms;
  planner->T = env->nterms;
  planner->segment = env->segs;
  planner->S = env->nsegs;
  planner->eqclass = env->eqclasses;
  planner->EQ = env->neqclasses;
  planner->subqueries = env->subqueries;
  planner->Q = env->nsubqueries;
  planner->P = env->npartitions;
  planner->partition = env->partitions;

  bitset_init (&(planner->final_segs), env);
  bitset_assign (&(planner->final_segs), &(env->final_segs));
  bitset_init (&(planner->all_subqueries), env);
  for (i = 0; i < (signed) planner->Q; ++i)
    bitset_add (&(planner->all_subqueries), i);

  planner->node_info = NULL;
  planner->join_info = NULL;
  planner->best_info = NULL;
  planner->cp_info = NULL;

  planner->info_list = NULL;

  planner->cleanup_needed = true;
  planner->can_apply_limit_card = qo_can_apply_limit_card (env);

  return planner;
}

void
qo_planner_free (QO_PLANNER * planner)
{
  if (planner->cleanup_needed)
    qo_clean_planner (planner);

  qo_plan_del_ref (planner->worst_plan);

  if (planner->info_list)
    {
      QO_INFO *info, *next_info;

      for (info = planner->info_list; info; info = next_info)
	{
	  next_info = info->next;	/* save next link */
	  qo_free_info (info);
	}
    }

  if (planner->node_info)
    {
      free_and_init (planner->node_info);
    }

  if (planner->join_info)
    {
      free_and_init (planner->join_info);
    }

  if (planner->cp_info)
    {
      free_and_init (planner->cp_info);
    }

  free_and_init (planner);
}

void
qo_dump_planner_info (QO_PLANNER * planner, QO_PARTITION * partition, FILE * f)
{
  int i, M;
  QO_INFO *info;
  int t;
  BITSET_ITERATOR iter;
  const char *prefix;

  fputs ("\nNode info maps:\n", f);
  for (i = 0; i < (signed) planner->N; i++)
    {
      if (BITSET_MEMBER (QO_PARTITION_NODES (partition), i))
	{
	  info = planner->node_info[i];
	  if (info && !info->detached)
	    {
	      fprintf (f, "node_info[%d]:\n", i);
	      qo_dump_info (info, f);
	    }
	}
    }

  if (!bitset_is_empty (&(QO_PARTITION_EDGES (partition))))
    {
      fputs ("\nJoin info maps:\n", f);
      /* in current implementation, join_info[0..2] does not used */
      i = QO_PARTITION_M_OFFSET (partition);
      M = i + QO_JOIN_INFO_SIZE (partition);
      for (i = i + 3; i < M; i++)
	{
	  info = planner->join_info[i];
	  if (info && !info->detached)
	    {
	      fputs ("join_info[", f);
	      prefix = "";	/* init */
	      for (t = bitset_iterate (&(info->nodes), &iter); t != -1; t = bitset_next_member (&iter))
		{
		  fprintf (f, "%s%d", prefix, QO_NODE_IDX (QO_ENV_NODE (planner->env, t)));
		  prefix = ",";
		}
	      fputs ("]:\n", f);
	      qo_dump_info (info, f);
	    }
	}
    }
}

void
qo_get_term_hit_prob (QO_TERM * term, QO_INFO * head_info, QO_INFO * tail_info, QO_ENV * env,
		      double *out_head_factor, double *out_tail_factor)
{
  const BITSET *term_segs = (const BITSET *) &(term->segments);
  BITSET_ITERATOR seg_iter;
  int seg_idx;
  QO_SEGMENT *head_seg = NULL, *tail_seg = NULL;
  INT64 head_ndv = 1, tail_ndv = 1;

  *out_head_factor = 1.0;
  *out_tail_factor = 1.0;
  if (bitset_cardinality (term_segs) != 2)
    {
      return;
    }

  for (seg_idx = bitset_iterate (term_segs, &seg_iter); seg_idx != -1; seg_idx = bitset_next_member (&seg_iter))
    {
      QO_SEGMENT *seg = QO_ENV_SEG (env, seg_idx);
      QO_NODE *node = QO_SEG_HEAD (seg);
      int node_idx = QO_NODE_IDX (node);

      if (BITSET_MEMBER (head_info->nodes, node_idx))
	{
	  head_seg = seg;
	}
      else if (BITSET_MEMBER (tail_info->nodes, node_idx))
	{
	  tail_seg = seg;
	}
    }
  if (head_seg == NULL || tail_seg == NULL)
    {
      return;
    }

  if (QO_SEG_INFO (head_seg) != NULL && QO_SEG_INFO (head_seg)->ndv > 0)
    {
      head_ndv = QO_SEG_INFO (head_seg)->ndv;
    }
  else
    {
      return;
    }

  if (QO_SEG_INFO (tail_seg) != NULL && QO_SEG_INFO (tail_seg)->ndv > 0)
    {
      tail_ndv = QO_SEG_INFO (tail_seg)->ndv;
    }
  else
    {
      return;
    }

  *out_head_factor = MIN (1.0, (double) tail_ndv / (double) head_ndv);
  *out_tail_factor = MIN (1.0, (double) head_ndv / (double) tail_ndv);
}

void
planner_visit_node (QO_PLANNER * planner, QO_PARTITION * partition, PT_HINT_ENUM hint, QO_NODE * head_node,
		    QO_NODE * tail_node, BITSET * visited_nodes, BITSET * visited_rel_nodes, BITSET * visited_terms,
		    BITSET * nested_path_nodes, BITSET * remaining_nodes, BITSET * remaining_terms,
		    BITSET * remaining_subqueries, int num_path_inner)
{
  JOIN_TYPE join_type = NO_JOIN;
  QO_TERM *follow_term = NULL;
  int idx_join_cnt = 0;		/* number of idx-join edges */
  QO_TERM *term;
  QO_NODE *node;
  QO_INFO *head_info = (QO_INFO *) NULL;
  QO_INFO *tail_info = (QO_INFO *) NULL;
  QO_INFO *new_info = (QO_INFO *) NULL;
  QO_PLAN *new_plan, *best_plan;
  int i, j;
  bool check_afj_terms = false;
  bool is_dummy_term = false;
  BITSET_ITERATOR bi, bj;
  BITSET nl_join_terms;		/* nested-loop join terms */
  BITSET sm_join_terms;		/* sort merge join terms */
  BITSET duj_terms;		/* during join terms */
  BITSET afj_terms;		/* after join terms */
  BITSET sarged_terms;
  BITSET info_terms;
  BITSET pinned_subqueries;
  BITSET visited_segs;

  bitset_init (&nl_join_terms, planner->env);
  bitset_init (&sm_join_terms, planner->env);
  bitset_init (&duj_terms, planner->env);
  bitset_init (&afj_terms, planner->env);
  bitset_init (&sarged_terms, planner->env);
  bitset_init (&info_terms, planner->env);
  bitset_init (&pinned_subqueries, planner->env);
  bitset_init (&visited_segs, planner->env);

  if (head_node == NULL)
    {
      goto wrapup;		/* unknown error */
    }

  if (num_path_inner)
    {				/* check for path connected nodes */
      if (bitset_is_empty (nested_path_nodes))
	{			/* not yet assign path connected nodes */
	  int found_num, found_idx;

	  for (i = bitset_iterate (remaining_terms, &bi); i != -1; i = bitset_next_member (&bi))
	    {
	      term = QO_ENV_TERM (planner->env, i);
	      if (QO_TERM_CLASS (term) == QO_TC_PATH && QO_NODE_IDX (QO_TERM_HEAD (term)) == QO_NODE_IDX (head_node)
		  && QO_NODE_IDX (QO_TERM_TAIL (term)) == QO_NODE_IDX (tail_node))
		{
		  bitset_add (nested_path_nodes, QO_NODE_IDX (QO_TERM_TAIL (term)));
		  /* Traverse tail link */
		  do
		    {
		      found_num = 0;	/* init */
		      for (j = bitset_iterate (remaining_terms, &bj); j != -1; j = bitset_next_member (&bj))
			{
			  term = QO_ENV_TERM (planner->env, j);
			  if (QO_TERM_CLASS (term) == QO_TC_PATH
			      && BITSET_MEMBER (*nested_path_nodes, QO_NODE_IDX (QO_TERM_HEAD (term))))
			    {
			      found_idx = QO_NODE_IDX (QO_TERM_TAIL (term));
			      /* found nested path term */
			      if (!BITSET_MEMBER (*nested_path_nodes, found_idx))
				{
				  bitset_add (nested_path_nodes, found_idx);
				  found_num++;
				}
			    }
			}
		    }
		  while (found_num);

		  /* Traverse head link reversely */
		  do
		    {
		      found_num = 0;	/* init */
		      for (j = bitset_iterate (remaining_terms, &bj); j != -1; j = bitset_next_member (&bj))
			{
			  term = QO_ENV_TERM (planner->env, j);
			  if (QO_TERM_CLASS (term) == QO_TC_PATH
			      && BITSET_MEMBER (*nested_path_nodes, QO_NODE_IDX (QO_TERM_TAIL (term))))
			    {
			      found_idx = QO_NODE_IDX (QO_TERM_HEAD (term));
			      /* found nested path term */
			      if (!BITSET_MEMBER (*nested_path_nodes, found_idx))
				{
				  bitset_add (nested_path_nodes, found_idx);
				  found_num++;
				}
			    }
			}
		    }
		  while (found_num);

		  /* exclude already joined nodes */
		  bitset_difference (nested_path_nodes, visited_nodes);
		  /* remove tail_node from path connected nodes */
		  bitset_remove (nested_path_nodes, QO_NODE_IDX (tail_node));

		  break;	/* exit for-loop */
		}
	    }
	}
      else
	{			/* already assign path connected nodes */
	  if (BITSET_MEMBER (*nested_path_nodes, QO_NODE_IDX (tail_node)))
	    {
	      /* remove tail_node from path connected nodes */
	      bitset_remove (nested_path_nodes, QO_NODE_IDX (tail_node));
	    }
	  else
	    {
	      goto wrapup;
	    }
	}
    }

  /*
   * STEP 1: set head_info, tail_info, visited_nodes, visited_rel_nodes
   */

  /* head_info points to the current prefix */
  if (bitset_cardinality (visited_nodes) == 1)
    {
      /* current prefix has only one node */
      head_info = planner->node_info[QO_NODE_IDX (head_node)];
    }
  else
    {
      /* current prefix has two or more nodes */
      head_info = planner->join_info[QO_INFO_INDEX (QO_PARTITION_M_OFFSET (partition), *visited_rel_nodes)];
      /* currently, do not permit cross join plan. for future work, NEED MORE CONSIDERAION */
      if (head_info == NULL)
	{
	  goto wrapup;
	}
    }

  /* tail_info points to the node for the single class being added to the prefix */
  tail_info = planner->node_info[QO_NODE_IDX (tail_node)];

  /* connect tail_node to the prefix */
  bitset_add (visited_nodes, QO_NODE_IDX (tail_node));
  bitset_add (visited_rel_nodes, QO_NODE_REL_IDX (tail_node));
  bitset_remove (remaining_nodes, QO_NODE_IDX (tail_node));

  new_info = planner->join_info[QO_INFO_INDEX (QO_PARTITION_M_OFFSET (partition), *visited_rel_nodes)];

  /* check for already examined join_info */
  if (new_info && new_info->join_unit < planner->join_unit)
    {
      /* at here, not yet visited at this join level; use cache */

      if (new_info->best_no_order.nplans == 0)
	{
	  goto wrapup;		/* give up */
	}

      /* STEP 2: set terms for join_info */
      /* set info terms */
      bitset_assign (&info_terms, &(new_info->terms));
      bitset_difference (&info_terms, visited_terms);

      /* extract visited info terms */
      bitset_union (visited_terms, &info_terms);
      bitset_difference (remaining_terms, &info_terms);

      /* STEP 3: set pinned_subqueries */
      {
	QO_SUBQUERY *subq;

	for (i = bitset_iterate (remaining_subqueries, &bi); i != -1; i = bitset_next_member (&bi))
	  {
	    subq = &planner->subqueries[i];
	    if (bitset_subset (visited_nodes, &(subq->nodes)) && bitset_subset (visited_terms, &(subq->terms)))
	      {
		bitset_add (&pinned_subqueries, i);
	      }
	  }

	/* extract pinned subqueries */
	bitset_difference (remaining_subqueries, &pinned_subqueries);
      }

      goto go_ahead_subvisit;
    }

  /* extract terms of the tail_info subplan. this is necessary to ensure that we are aware of any terms that have been
   * sarged by the subplans */
  bitset_union (&info_terms, &(tail_info->terms));

  /* extract visited info terms */
  bitset_union (visited_terms, &info_terms);
  bitset_difference (remaining_terms, &info_terms);

  /* STEP 2: set specific terms for follow and join */

  /* in given partition, collect terms connected to tail_info */
  {
    int retry_cnt, edge_cnt, path_cnt;
    bool found_edge, skip_term;

    /* set visited segs for removing join terms already logically evaluated. */
    for (i = bitset_iterate (visited_terms, &bi); i != -1; i = bitset_next_member (&bi))
      {
	term = QO_ENV_TERM (planner->env, i);

	if (QO_TERM_NOMINAL_SEG (term))
	  {
	    bitset_union (&visited_segs, &(QO_TERM_SEGS (term)));
	  }
      }

    retry_cnt = 0;		/* init */

  retry_join_edge:

    edge_cnt = path_cnt = 0;	/* init */

    for (i = bitset_iterate (remaining_terms, &bi); i != -1; i = bitset_next_member (&bi))
      {

	term = QO_ENV_TERM (planner->env, i);

	/* check term nodes */
	if (!bitset_subset (visited_nodes, &(QO_TERM_NODES (term))))
	  {
	    continue;
	  }

	/* check location for outer join */
	if (QO_TERM_CLASS (term) == QO_TC_DURING_JOIN)
	  {
	    QO_ASSERT (planner->env, QO_ON_COND_TERM (term));

	    for (j = bitset_iterate (visited_nodes, &bj); j != -1; j = bitset_next_member (&bj))
	      {
		node = QO_ENV_NODE (planner->env, j);
		if (QO_NODE_LOCATION (node) == QO_TERM_LOCATION (term))
		  {
		    break;
		  }
	      }

	    if (j == -1)
	      {			/* out of location */
		continue;
	      }
	  }

	found_edge = false;	/* init */
	skip_term = false;

	if (BITSET_MEMBER (QO_TERM_NODES (term), QO_NODE_IDX (tail_node)))
	  {
	    if (QO_TERM_CLASS (term) == QO_TC_PATH)
	      {
		if (retry_cnt == 0)
		  {		/* is the first stage */
		    /* need to check the direction; head -> tail */
		    if (QO_NODE_IDX (QO_TERM_TAIL (term)) == QO_NODE_IDX (tail_node))
		      {
			found_edge = true;
		      }
		    else
		      {
			/* save path for the retry stage */
			path_cnt++;
		      }
		  }
		else
		  {
		    /* at retry stage; there is only path edge so, need not to check the direction */
		    found_edge = true;
		  }
	      }
	    else if (QO_IS_EDGE_TERM (term))
	      {
		found_edge = true;
	      }
	  }

	if (found_edge == true)
	  {
	    /* found edge */
	    edge_cnt++;

	    /* set join type */
	    if (join_type == NO_JOIN || is_dummy_term)
	      {
		/* the first time except dummy term */
		join_type = QO_TERM_JOIN_TYPE (term);
		is_dummy_term = QO_TERM_CLASS (term) == QO_TC_DUMMY_JOIN ? true : false;
	      }
	    else if (QO_TERM_CLASS (term) == QO_TC_DUMMY_JOIN)
	      {
		/* The dummy join term is excluded from the outer join check. */
	      }
	    else
	      {			/* already assigned */
		if (IS_OUTER_JOIN_TYPE (join_type))
		  {
		    /* outer join type must be the same */
		    if (IS_OUTER_JOIN_TYPE (QO_TERM_JOIN_TYPE (term)))
		      {
			QO_ASSERT (planner->env, join_type == QO_TERM_JOIN_TYPE (term));
		      }
		  }
		else
		  {
		    if (IS_OUTER_JOIN_TYPE (QO_TERM_JOIN_TYPE (term)))
		      {
			/* replace to the outer join type */
			join_type = QO_TERM_JOIN_TYPE (term);
		      }
		  }
	      }

	    switch (QO_TERM_CLASS (term))
	      {
	      case QO_TC_DUMMY_JOIN:	/* is always true dummy join term */
		/* check for idx-join */
		if (QO_TERM_CAN_USE_INDEX (term))
		  {
		    idx_join_cnt++;
		  }
		break;

	      case QO_TC_PATH:
		if (follow_term == NULL)
		  {		/* get the first PATH term idx */
		    follow_term = term;
		    /* for path-term, if join type is not outer join, we can use idx-join, nl-join */
		    if (QO_TERM_JOIN_TYPE (follow_term) == JOIN_INNER)
		      {
			/* check for idx-join */
			if (QO_TERM_CAN_USE_INDEX (term))
			  {
			    idx_join_cnt++;
			  }
			bitset_add (&nl_join_terms, i);
		      }
		    /* check for m-join */
		    if (QO_TERM_IS_FLAGED (term, QO_TERM_MERGEABLE_EDGE))
		      {
			bitset_add (&sm_join_terms, i);
		      }
		  }
		else
		  {		/* found another PATH term */
		    /* unknown error */
		    QO_ASSERT (planner->env, UNEXPECTED_CASE);
		  }
		break;

	      case QO_TC_JOIN:
		/* check for term which is already logically evaluated. */
		if (QO_TERM_NOMINAL_SEG (term))
		  {
		    if (qo_check_skip_term (planner->env, visited_segs, term, visited_terms, &info_terms))
		      {
			skip_term = true;
		      }
		    else
		      {
			bitset_union (&visited_segs, &(QO_TERM_SEGS (term)));
		      }
		  }

		if (!skip_term)
		  {
		    /* check for idx-join */
		    if (QO_TERM_CAN_USE_INDEX (term))
		      {
			idx_join_cnt++;
		      }
		    bitset_add (&nl_join_terms, i);
		    /* check for m-join */
		    if (QO_TERM_IS_FLAGED (term, QO_TERM_MERGEABLE_EDGE))
		      {
			bitset_add (&sm_join_terms, i);
		      }
		    else
		      {		/* non-eq edge */
			if (IS_OUTER_JOIN_TYPE (join_type) && QO_ON_COND_TERM (term))
			  {	/* ON clause */
			    bitset_add (&duj_terms, i);	/* need for m-join */
			  }
		      }
		  }
		break;

	      case QO_TC_DEP_LINK:
	      case QO_TC_DEP_JOIN:
		bitset_add (&nl_join_terms, i);
		break;

	      default:
		QO_ASSERT (planner->env, UNEXPECTED_CASE);
		break;
	      }
	  }
	else
	  {
	    /* does not edge */

	    if (QO_TERM_CLASS (term) == QO_TC_DURING_JOIN)
	      {
		bitset_add (&duj_terms, i);
	      }
	    else if (QO_TERM_CLASS (term) == QO_TC_AFTER_JOIN)
	      {
		check_afj_terms = true;

		/* If visited_nodes is the same as partition's nodes, then we have successfully generated one of the
		 * graph permutations(i.e., we have considered every one of the nodes). only include after-join term
		 * for this plan.
		 */
		if (!bitset_is_equivalent (visited_nodes, &(QO_PARTITION_NODES (partition))))
		  {
		    continue;
		  }
		bitset_add (&afj_terms, i);
	      }
	    else if (QO_TERM_CLASS (term) == QO_TC_OTHER)
	      {
		if (IS_OUTER_JOIN_TYPE (join_type) && QO_ON_COND_TERM (term))
		  {		/* ON clause */
		    bitset_add (&duj_terms, i);
		  }
	      }
	  }

	bitset_add (&info_terms, i);	/* add to info term */

	/* skip always true dummy join term and do not evaluate */
	if (!skip_term && QO_TERM_CLASS (term) != QO_TC_DUMMY_JOIN)
	  {
	    bitset_add (&sarged_terms, i);	/* add to sarged term */
	  }
      }

    /* currently, do not permit cross join plan. for future work, NEED MORE CONSIDERAION */
    if (edge_cnt == 0)
      {
	if (retry_cnt == 0)
	  {			/* is the first stage */
	    if (path_cnt > 0)
	      {
		/* there is only path edge and the direction is reversed */
		retry_cnt++;
		goto retry_join_edge;
	      }
	  }
	goto wrapup;
      }
  }

#if 1				/* TO NOT DELETE ME - very special case for Object fetch plan */
  /* re-check for after join term; is depence to Object fetch plan */
  if (check_afj_terms && bitset_is_empty (&afj_terms))
    {
      BITSET path_nodes;

      bitset_init (&path_nodes, planner->env);

      for (i = bitset_iterate (remaining_terms, &bi); i != -1; i = bitset_next_member (&bi))
	{
	  term = QO_ENV_TERM (planner->env, i);

	  if (QO_TERM_CLASS (term) == QO_TC_PATH)
	    {
	      bitset_add (&path_nodes, QO_NODE_IDX (QO_TERM_TAIL (term)));
	    }
	}

      /* there is only path joined nodes. So, should apply after join terms at here. */
      if (bitset_subset (&path_nodes, remaining_nodes))
	{
	  for (i = bitset_iterate (remaining_terms, &bi); i != -1; i = bitset_next_member (&bi))
	    {
	      term = QO_ENV_TERM (planner->env, i);

	      if (QO_TERM_CLASS (term) == QO_TC_AFTER_JOIN)
		{
		  bitset_add (&afj_terms, i);
		  bitset_add (&info_terms, i);	/* add to info term */
		  bitset_add (&sarged_terms, i);	/* add to sarged term */
		}
	    }
	}

      bitset_delset (&path_nodes);
    }
#endif

  /* extract visited info terms */
  bitset_union (visited_terms, &info_terms);
  bitset_difference (remaining_terms, &info_terms);

  /* STEP 3: set pinned_subqueries */

  /* Find out if we can pin any of the remaining subqueries.  A subquery is eligible to be pinned here if all of the
   * nodes on which it depends are covered here.  However, it mustn't be pinned here if it is part of a term that
   * hasn't been pinned yet.  Doing so risks improperly pushing a subquery plan down through a merge join during XASL
   * generation, which results in an incorrect plan (the subquery has to be evaluated during the merge, rather than
   * during the scan that feeds the merge).
   */
  {
    QO_SUBQUERY *subq;

    for (i = bitset_iterate (remaining_subqueries, &bi); i != -1; i = bitset_next_member (&bi))
      {
	subq = &planner->subqueries[i];
	if (bitset_subset (visited_nodes, &(subq->nodes)) && bitset_subset (visited_terms, &(subq->terms)))
	  {
	    bitset_add (&pinned_subqueries, i);
	  }
      }

    /* extract pinned subqueries */
    bitset_difference (remaining_subqueries, &pinned_subqueries);
  }

  /* STEP 4: set joined info */

  if (new_info == NULL)
    {

      double selectivity, cardinality, total_rows, head_hit_prob, tail_hit_prob;
      BITSET eqclasses;

      bitset_init (&eqclasses, planner->env);


      selectivity = 1.0;	/* init */

      cardinality = head_info->cardinality * tail_info->cardinality;
      total_rows = head_info->total_rows * tail_info->total_rows;
      head_hit_prob = 1.0;
      tail_hit_prob = 1.0;
      if (IS_OUTER_JOIN_TYPE (join_type))
	{
	  /* set lower bound of outer join result */
	  if (join_type == JOIN_RIGHT)
	    {
	      cardinality = MAX (cardinality, tail_info->cardinality);
	    }
	  else
	    {
	      cardinality = MAX (cardinality, head_info->cardinality);
	    }
	}

      if (cardinality != 0)
	{			/* not empty */
	  cardinality = MAX (1.0, cardinality);
	  for (i = bitset_iterate (&sarged_terms, &bi); i != -1; i = bitset_next_member (&bi))
	    {
	      term = &planner->term[i];
	      if (QO_IS_PATH_TERM (term) && QO_TERM_JOIN_TYPE (term) != JOIN_INNER)
		{
		  /* single-fetch */
		  cardinality = head_info->cardinality;
		  if (cardinality != 0)
		    {		/* not empty */
		      cardinality = MAX (1.0, cardinality);
		    }
		}
	      else
		{
		  double term_sel = QO_TERM_SELECTIVITY (term);

		  term_sel = qo_apply_mcv_hotkey_join_guard (term, head_info, tail_info, cardinality, term_sel);

		  selectivity *= term_sel;
		  selectivity = MAX (1.0 / MAX (cardinality, 1.0), selectivity);

		  double head_factor, tail_factor;
		  qo_get_term_hit_prob (term, head_info, tail_info, planner->env, &head_factor, &tail_factor);
		  head_hit_prob *= head_factor;
		  tail_hit_prob *= tail_factor;
		}
	    }
	  cardinality *= selectivity;
	  cardinality = MAX (1.0, cardinality);
	  total_rows *= selectivity;
	  total_rows = MAX (1.0, total_rows);

	  if (IS_OUTER_JOIN_TYPE (join_type) && bitset_is_empty (&afj_terms))
	    {
	      /* set lower bound of outer join result */
	      if (join_type == JOIN_RIGHT)
		{
		  cardinality = MAX (cardinality, tail_info->cardinality);
		}
	      else
		{
		  cardinality = MAX (cardinality, head_info->cardinality);
		}
	    }
	}

      bitset_assign (&eqclasses, &(head_info->eqclasses));
      bitset_union (&eqclasses, &(tail_info->eqclasses));

      head_info->hit_prob = head_hit_prob;
      tail_info->hit_prob = tail_hit_prob;
      if (IS_OUTER_JOIN_TYPE (join_type))
	{
	  /* set lower bound of outer join result */
	  if (join_type == JOIN_RIGHT)
	    {
	      tail_info->hit_prob = 1.0;
	    }
	  else
	    {
	      head_info->hit_prob = 1.0;
	    }
	}

      new_info = planner->join_info[QO_INFO_INDEX (QO_PARTITION_M_OFFSET (partition), *visited_rel_nodes)] =
	qo_alloc_info (planner, visited_nodes, visited_terms, &eqclasses, cardinality, total_rows);

      bitset_delset (&eqclasses);
    }

  /* STEP 5: do EXAMINE follow, join */

  {
    int kept = 0;
    int idx_join_plan_n = 0;

    /* for path-term, if join order is correct, we can use follow. */
    if (follow_term && (QO_NODE_IDX (QO_TERM_TAIL (follow_term)) == QO_NODE_IDX (tail_node)))
      {
	/* STEP 5-1: examine follow */
	kept += qo_examine_follow (new_info, follow_term, head_info, &sarged_terms, &pinned_subqueries);
      }

    if (follow_term && join_type != JOIN_INNER && QO_NODE_IDX (QO_TERM_TAIL (follow_term)) != QO_NODE_IDX (tail_node))
      {
	/* if there is a path-term whose outer join order is not correct, we can not use idx-join, nl-join, m-join */
	;
      }
    else
      {
#if 1				/* CORRELATED_INDEX */
	/* STEP 5-2: examine idx-join */
	if (idx_join_cnt)
	  {
	    idx_join_plan_n =
	      qo_examine_idx_join (new_info, join_type, head_info, tail_info, &afj_terms, &sarged_terms,
				   &pinned_subqueries);
	    kept += idx_join_plan_n;
	  }
#endif /* CORRELATED_INDEX */

	/* STEP 5-3: examine nl-join */
	/* sm_join_terms is a mergeable term for SM join. In hash list scan, mergeable term is used as hash term. */
	/* The mergeable term and the hash term have the same characteristics. */
	/* If the characteristics for mergeable terms are changed, the logic for hash terms should be separated. */
	/* mergeable term : equi-term, symmetrical term, e.g. TBL1.a = TBL2.a, function(TAB1.a) = function(TAB2.a) */
	kept +=
	  qo_examine_nl_join (new_info, join_type, head_info, tail_info, &nl_join_terms, &duj_terms, &afj_terms,
			      &sarged_terms, &pinned_subqueries, idx_join_plan_n, &sm_join_terms);

#if 1				/* MERGE_JOINS */
	/* STEP 5-4: examine merge-join */
	if (!bitset_is_empty (&sm_join_terms))
	  {
	    kept +=
	      qo_examine_merge_join (new_info, join_type, head_info, tail_info, &sm_join_terms, &duj_terms, &afj_terms,
				     &sarged_terms, &pinned_subqueries);
	  }
#endif /* MERGE_JOINS */

#if 1				/* HASH_JOINS */
	/* STEP 5-5: examine hash-join */
	if (!bitset_is_empty (&sm_join_terms))
	  {
	    /**
	     * sm_join_terms is a mergeable term for SM join. In hash join, mergeable term is used as hash join term.
	     * The mergeable term and the hash join term have the same characteristics. If the characteristics
	     * for mergeable terms are changed, the logic for hash join terms should be separated.
	     *
	     * mergeable term: equi-term, symmetrical term, e.g. TBL1.a = TBL2.a, function(TAB1.a) = function(TAB2.a)
	     */
	    kept +=
	      qo_examine_hash_join (new_info, join_type, head_info, tail_info, &sm_join_terms, &duj_terms, &afj_terms,
				    &sarged_terms, &pinned_subqueries);
	  }
#endif /* HASH_JOINS */
      }

    /* At this point, kept indicates the number of worthwhile plans generated by examine_joins (i.e., plans that where
     * cheaper than some previous equivalent plan).  If that number is 0, then there is no point in continuing this
     * particular branch of permutations: we've already generated all of the suffixes once before, and with a better
     * prefix to boot.  There is no possibility of finding a better plan with this prefix.
     */
    if (!kept)
      {
	goto wrapup;
      }
  }

  /* STEP 7: go on sub permutations */

go_ahead_subvisit:

  /* If visited_nodes' cardinality is the same as join_unit, then we have successfully generated one of the graph
   * permutations (i.e., we have considered every one of the nodes). If not, we need to try to recursively generate
   * suffixes.
   */
  if (bitset_cardinality (visited_nodes) >= planner->join_unit)
    {
      /* If this is the info node that corresponds to the final plan (i.e., every node in the partition is covered by
       * the plans at this node), *AND* we have something to put in it, then record that fact in the planner.  This
       * permits more aggressive pruning, since we can immediately discard any plan (or subplan) that is no better than
       * the best known plan for the entire partition.
       */
      if (!planner->best_info)
	{
	  planner->best_info = new_info;
	  goto wrapup;
	}

      new_plan = qo_find_best_plan_on_info (new_info, QO_UNORDERED, 1.0);
      best_plan = qo_find_best_plan_on_info (planner->best_info, QO_UNORDERED, 1.0);
      if (best_plan == NULL || new_plan == NULL)
	{			/* unknown error */
	  goto wrapup;		/* give up */
	}
      QO_PLAN_COMPARE_RESULT cmp = qo_plan_cmp (best_plan, new_plan);

      if (cmp == PLAN_COMP_GT)
	{
	  planner->best_info = new_info;
	}
    }
  else
    {
      for (i = bitset_iterate (remaining_nodes, &bi); i != -1; i = bitset_next_member (&bi))
	{
	  node = QO_ENV_NODE (planner->env, i);

	  /* node dependency check; */
	  if (!bitset_subset (visited_nodes, &(QO_NODE_DEP_SET (node))))
	    {
	      /* node represents dependent tables, so there is no way this combination can work in isolation.  Give up
	       * so we can try some other combinations.
	       */
	      continue;
	    }
	  if (!bitset_subset (visited_nodes, &(QO_NODE_OUTER_DEP_SET (node))))
	    {
	      /* All previous nodes participating in outer join spec should be joined before. QO_NODE_OUTER_DEP_SET()
	       * represents all previous nodes which are dependents on the node.
	       */
	      continue;
	    }

	  /* now, set node as next tail node, do recursion */
	  (void) planner_visit_node (planner, partition, hint, tail_node,	/* next head node */
				     node,	/* next tail node */
				     visited_nodes, visited_rel_nodes, visited_terms, nested_path_nodes,
				     remaining_nodes, remaining_terms, remaining_subqueries, num_path_inner);

	  /* join hint: force join left-to-right */
	  if (hint & PT_HINT_ORDERED)
	    {
	      break;
	    }
	}
    }

wrapup:

  /* recover to original */

  bitset_remove (visited_nodes, QO_NODE_IDX (tail_node));
  bitset_remove (visited_rel_nodes, QO_NODE_REL_IDX (tail_node));
  bitset_add (remaining_nodes, QO_NODE_IDX (tail_node));

  bitset_difference (visited_terms, &info_terms);
  bitset_union (remaining_terms, &info_terms);

  bitset_union (remaining_subqueries, &pinned_subqueries);

  /* free alloced */
  bitset_delset (&nl_join_terms);
  bitset_delset (&sm_join_terms);
  bitset_delset (&duj_terms);
  bitset_delset (&afj_terms);
  bitset_delset (&sarged_terms);
  bitset_delset (&info_terms);
  bitset_delset (&pinned_subqueries);
  bitset_delset (&visited_segs);
}

double
planner_nodeset_join_cost (QO_PLANNER * planner, BITSET * nodeset)
{
  int i;
  BITSET_ITERATOR bi;
  QO_NODE *node;
  QO_INFO *info;
  QO_PLAN *plan, *subplan;
  double total_cost, objects, result_size, pages;

  total_cost = 0.0;		/* init */

  for (i = bitset_iterate (nodeset, &bi); i != -1; i = bitset_next_member (&bi))
    {

      node = QO_ENV_NODE (planner->env, i);
      info = planner->node_info[QO_NODE_IDX (node)];

      plan = qo_find_best_plan_on_info (info, QO_UNORDERED, 1.0);

      if (plan == NULL)
	{			/* something wrong */
	  continue;		/* give up */
	}

      objects = (plan->info)->cardinality;
      result_size = objects * (double) (plan->info)->projected_size;
      pages = result_size / (double) IO_PAGESIZE;
      pages = MAX (1.0, pages);

      /* apply join cost; add to the total cost */
      total_cost += pages;

      /* TODO: Consider the priority of hints */
      if (QO_NODE_HINT (node) & (PT_HINT_USE_IDX | PT_HINT_USE_NL))
	{
	  /* join hint: force idx-join, nl-join */
	}
      else if (!(QO_NODE_HINT (node) & PT_HINT_NO_USE_HASH) && (QO_NODE_HINT (node) & PT_HINT_USE_HASH))
	{
	  /* join hint: force hash-join */
	}
      else if (QO_NODE_HINT (node) & PT_HINT_USE_MERGE)
	{
	  /* join hint: force m-join */
	  if (plan->plan_type == QO_PLANTYPE_SORT)
	    {
	      subplan = plan->plan_un.sort.subplan;
	    }
	  else
	    {
	      subplan = plan;
	    }

	  objects = (subplan->info)->cardinality;
	  result_size = objects * (double) (subplan->info)->projected_size;
	  pages = result_size / (double) IO_PAGESIZE;
	  pages = MAX (1.0, pages);

	  /* apply merge cost; add to the total cost */
	  if (plan->plan_type == QO_PLANTYPE_SORT)
	    {
	      /* already apply inner cost: apply only outer cost */
	      total_cost += pages;
	    }
	  else
	    {
	      /* do guessing: apply outer, inner cost */
	      total_cost += pages * 2.0;
	    }
	}
      else
	{
	  /* fall through */
	}
    }

  return total_cost;
}

void
planner_permutate (QO_PLANNER * planner, QO_PARTITION * partition, PT_HINT_ENUM hint, QO_NODE * prev_head_node,
		   BITSET * visited_nodes, BITSET * visited_rel_nodes, BITSET * visited_terms,
		   BITSET * nested_path_nodes, BITSET * remaining_nodes, BITSET * remaining_terms,
		   BITSET * remaining_subqueries, int num_path_inner, int *node_idxp)
{
  int i, j;
  BITSET_ITERATOR bi, bj;
  QO_INFO *head_info, *best_info;
  QO_NODE *head_node, *tail_node;
  QO_PLAN *best_plan;
  double best_cost, prev_best_cost;
  BITSET rest_nodes;

  bitset_init (&rest_nodes, planner->env);

  planner->best_info = NULL;	/* init */

  prev_best_cost = -1.0;	/* init */

  /* Now perform the actual search.  Entries in join_info will gradually be filled and refined within the calls to
   * examine_xxx_join(). When we finish, planner->best_info will hold information about the best ways discovered to
   * perform the entire join.
   */
  for (i = bitset_iterate (remaining_nodes, &bi); i != -1; i = bitset_next_member (&bi))
    {

      head_node = QO_ENV_NODE (planner->env, i);

      /* head node dependency check; */
      if (!bitset_subset (visited_nodes, &(QO_NODE_DEP_SET (head_node))))
	{
	  /* head node represents dependent tables, so there is no way this combination can work in isolation.  Give up
	   * so we can try some other combinations.
	   */
	  continue;
	}
      if (!bitset_subset (visited_nodes, &(QO_NODE_OUTER_DEP_SET (head_node))))
	{
	  /* All previous nodes participating in outer join spec should be joined before. QO_NODE_OUTER_DEP_SET()
	   * represents all previous nodes which are dependents on the node.
	   */
	  continue;
	}

      if (bitset_is_empty (visited_nodes))
	{			/* not found outermost nodes */

	  head_info = planner->node_info[QO_NODE_IDX (head_node)];

	  /* init */
	  bitset_add (visited_nodes, QO_NODE_IDX (head_node));
	  bitset_add (visited_rel_nodes, QO_NODE_REL_IDX (head_node));
	  bitset_remove (remaining_nodes, QO_NODE_IDX (head_node));

	  bitset_union (visited_terms, &(head_info->terms));
	  bitset_difference (remaining_terms, &(head_info->terms));

	  for (j = bitset_iterate (remaining_nodes, &bj); j != -1; j = bitset_next_member (&bj))
	    {

	      tail_node = QO_ENV_NODE (planner->env, j);

	      /* tail node dependency check; */
	      if (!bitset_subset (visited_nodes, &(QO_NODE_DEP_SET (tail_node))))
		{
		  continue;
		}
	      if (!bitset_subset (visited_nodes, &(QO_NODE_OUTER_DEP_SET (tail_node))))
		{
		  continue;
		}

	      BITSET_CLEAR (*nested_path_nodes);

	      (void) planner_visit_node (planner, partition, hint, head_node, tail_node, visited_nodes,
					 visited_rel_nodes, visited_terms, nested_path_nodes, remaining_nodes,
					 remaining_terms, remaining_subqueries, num_path_inner);

	      /* join hint: force join left-to-right */
	      if (hint & PT_HINT_ORDERED)
		{
		  break;
		}
	    }

	  /* recover to original */
	  BITSET_CLEAR (*visited_nodes);
	  BITSET_CLEAR (*visited_rel_nodes);
	  bitset_add (remaining_nodes, QO_NODE_IDX (head_node));

	  bitset_difference (visited_terms, &(head_info->terms));
	  bitset_union (remaining_terms, &(head_info->terms));

	}
      else
	{			/* found some outermost nodes */

	  BITSET_CLEAR (*nested_path_nodes);

	  (void) planner_visit_node (planner, partition, hint, prev_head_node, head_node,	/* next tail node */
				     visited_nodes, visited_rel_nodes, visited_terms, nested_path_nodes,
				     remaining_nodes, remaining_terms, remaining_subqueries, num_path_inner);
	}

      if (node_idxp)
	{			/* is partial node visit */
	  best_info = planner->best_info;
	  if (best_info == NULL)
	    {			/* not found best plan */
	      continue;		/* skip and go ahead */
	    }

	  best_plan = qo_find_best_plan_on_info (best_info, QO_UNORDERED, 1.0);

	  if (best_plan == NULL)
	    {			/* unknown error */
	      break;		/* give up */
	    }

	  /* set best plan's cost */
	  best_cost =
	    best_plan->fixed_cpu_cost + best_plan->fixed_io_cost + best_plan->variable_cpu_cost +
	    best_plan->variable_io_cost;

	  /* apply rest nodes's cost */
	  bitset_assign (&rest_nodes, remaining_nodes);
	  bitset_difference (&rest_nodes, &(best_info->nodes));
	  best_cost += planner_nodeset_join_cost (planner, &rest_nodes);

	  if (prev_best_cost == -1.0	/* the first time */
	      || best_cost < prev_best_cost)
	    {
	      *node_idxp = QO_NODE_IDX (head_node);
	      prev_best_cost = best_cost;	/* found new best */
	    }

	  planner->best_info = NULL;	/* clear */
	}

      /* join hint: force join left-to-right */
      if (hint & PT_HINT_ORDERED)
	{
	  break;
	}
    }

  if (node_idxp)
    {				/* is partial node visit */
      planner->best_info = NULL;	/* clear */
    }

  bitset_delset (&rest_nodes);

  return;
}

QO_PLAN *
qo_planner_search (QO_ENV * env)
{
  QO_PLANNER *planner;
  QO_PLAN *plan;

  planner = NULL;
  plan = NULL;

  planner = qo_alloc_planner (env);
  if (planner == NULL)
    {
      return NULL;
    }

  qo_info_nodes_init (env);
  qo_plans_init (env);
  plan = qo_search_planner (planner);
  qo_clean_planner (planner);

  return plan;
}

int
qo_generate_join_index_scan (QO_INFO * infop, JOIN_TYPE join_type, QO_PLAN * outer_plan, QO_INFO * inner,
			     QO_NODE * nodep, QO_NODE_INDEX_ENTRY * ni_entryp, BITSET * indexable_terms,
			     BITSET * afj_terms, BITSET * sarged_terms, BITSET * pinned_subqueries)
{
  QO_ENV *env;
  QO_INDEX_ENTRY *index_entryp;
  BITSET_ITERATOR iter;
  QO_TERM *termp;
  QO_PLAN *inner_plan;
  int i, t, last_t, j, n, seg, rangelist_term_idx;
  bool found_rangelist;
  BITSET range_terms;
  BITSET empty_terms;
  BITSET remaining_terms;

  if (nodep != NULL && QO_NODE_IS_CLASS_HIERARCHY (nodep))
    {
      /* Class hierarchies are split into scan blocks which cannot be used for index joins. However, if the class
       * hierarchy is a partitioning hierarchy, we can use an index join for inner joins
       */
      if (!QO_NODE_IS_CLASS_PARTITIONED (nodep))
	{
	  return 0;
	}
      else if (join_type != JOIN_INNER)
	{
	  return 0;
	}
    }
  env = infop->env;

  bitset_init (&range_terms, env);
  bitset_init (&empty_terms, env);
  bitset_init (&remaining_terms, env);

  bitset_assign (&remaining_terms, sarged_terms);

  /* pointer to QO_INDEX_ENTRY structure */
  index_entryp = (ni_entryp)->head;
  if (index_entryp->force < 0)
    {
      assert (false);
      return 0;
    }

  found_rangelist = false;
  rangelist_term_idx = -1;
  for (i = 0; i < index_entryp->nsegs; i++)
    {
      seg = index_entryp->seg_idxs[i];
      if (seg == -1)
	{
	  break;
	}
      n = 0;
      last_t = -1;
      for (t = bitset_iterate (indexable_terms, &iter); t != -1; t = bitset_next_member (&iter))
	{
	  termp = QO_ENV_TERM (env, t);

	  /* check for always true dummy join term */
	  if (QO_TERM_CLASS (termp) == QO_TC_DUMMY_JOIN)
	    {
	      /* skip out from all terms */
	      bitset_remove (&remaining_terms, t);
	      continue;		/* do not add to range_terms */
	    }

	  if (QO_TERM_IS_FLAGED (termp, QO_TERM_MULTI_COLL_PRED))
	    {
	      /* case of multi column term ex) (a,b) in ... */
	      if (found_rangelist == true && QO_TERM_IDX (termp) != rangelist_term_idx)
		{
		  break;	/* already found. give up */
		}
	      for (j = 0; j < termp->multi_col_cnt; j++)
		{
		  if (QO_TERM_IS_FLAGED (termp, QO_TERM_RANGELIST))
		    {
		      found_rangelist = true;
		      rangelist_term_idx = QO_TERM_IDX (termp);
		    }
		  /* found term */
		  if (termp->multi_col_segs[j] == seg && BITSET_MEMBER (index_entryp->seg_equal_terms[i], t))
		    /* multi col term is only indexable when term's class is TC_SARG. so can use seg_equal_terms */
		    {
		      /* save last found term */
		      last_t = t;
		      /* found EQ term */
		      if (QO_TERM_IS_FLAGED (termp, QO_TERM_EQUAL_OP))
			{
			  bitset_add (&range_terms, t);
			  bitset_add (&(index_entryp->multi_col_range_segs), seg);
			  n++;
			}
		    }
		}
	    }
	  else
	    {
	      for (j = 0; j < termp->can_use_index; j++)
		{
		  /* found term */
		  if (QO_SEG_IDX (termp->index_seg[j]) == seg)
		    {
		      /* save last found term */
		      last_t = t;

		      /* found EQ term */
		      if (QO_TERM_IS_FLAGED (termp, QO_TERM_EQUAL_OP))
			{
			  if (QO_TERM_IS_FLAGED (termp, QO_TERM_RANGELIST))
			    {
			      if (found_rangelist == true)
				{
				  break;	/* already found. give up */
				}

			      /* is the first time */
			      found_rangelist = true;
			      rangelist_term_idx = QO_TERM_IDX (termp);
			    }

			  bitset_add (&range_terms, t);
			  n++;
			}

		      break;
		    }
		}
	    }

	  /* found EQ term. exit term-iteration loop */
	  if (n)
	    {
	      break;
	    }
	}

      /* not found EQ term. exit seg-iteration loop */
      if (n == 0)
	{
	  /* found term. add last non-EQ term */
	  if (last_t != -1)
	    {
	      if (found_rangelist == true)
		{
		  termp = QO_ENV_TERM (env, last_t);
		  if (QO_TERM_IS_FLAGED (termp, QO_TERM_RANGELIST))
		    {
		      break;	/* give up */
		    }
		}
	      bitset_add (&range_terms, last_t);
	    }
	  break;
	}
    }

  n = 0;
  if (!bitset_is_empty (&range_terms))
    {
      inner_plan = qo_index_scan_new (inner, nodep, ni_entryp, QO_SCANMETHOD_INDEX_SCAN, &range_terms, indexable_terms);

      if (inner_plan)
	{
	  /* now, key-filter is assigned; exclude key-range, key-filter terms from remaining terms */
	  bitset_difference (&remaining_terms, &range_terms);
	  bitset_difference (&remaining_terms, &(inner_plan->plan_un.scan.kf_terms));

	  n =
	    qo_check_plan_on_info (infop,
				   qo_join_new (infop, join_type, QO_JOINMETHOD_IDX_JOIN, outer_plan, inner_plan,
						&empty_terms, &empty_terms, afj_terms, &remaining_terms,
						pinned_subqueries, &empty_terms));
	}
    }

  bitset_delset (&remaining_terms);
  bitset_delset (&empty_terms);
  bitset_delset (&range_terms);

  return n;
}

void
qo_generate_seq_scan (QO_INFO * infop, QO_NODE * nodep)
{
  int n;
  QO_PLAN *planp;
  bool plan_created = false;

  planp = qo_seq_scan_new (infop, nodep);

  n = qo_check_plan_on_info (infop, planp);
  if (n)
    {
      plan_created = true;
    }
}

int
qo_generate_index_scan (QO_INFO * infop, QO_NODE * nodep, QO_NODE_INDEX_ENTRY * ni_entryp, int nsegs)
{
  QO_INDEX_ENTRY *index_entryp;
  BITSET_ITERATOR iter;
  int i, t, n, normal_index_plan_n = 0;
  QO_PLAN *planp;
  BITSET range_terms;
  BITSET seg_other_terms;
  int start_column = 0;

  bitset_init (&range_terms, infop->env);
  bitset_init (&seg_other_terms, infop->env);

  /* pointer to QO_INDEX_ENTRY structure */
  index_entryp = (ni_entryp)->head;
  if (index_entryp->force < 0)
    {
      assert (false);
      return 0;
    }

  if (QO_ENTRY_MULTI_COL (index_entryp))
    {
      assert (nsegs >= 1);
      ;				/* nop */
    }
  else
    {
      assert (nsegs == 1);
      assert (index_entryp->is_iss_candidate == 0);
      assert (!(index_entryp->ils_prefix_len > 0));
    }

  start_column = index_entryp->is_iss_candidate ? 1 : 0;

  for (i = start_column; i < nsegs - 1; i++)
    {
      t = bitset_first_member (&(index_entryp->seg_equal_terms[i]));
      bitset_add (&range_terms, t);

      /* add multi_col_range_segs */
      if (QO_TERM_IS_FLAGED (QO_ENV_TERM (infop->env, t), QO_TERM_MULTI_COLL_PRED))
	{
	  bitset_add (&(index_entryp->multi_col_range_segs), index_entryp->seg_idxs[i]);
	}
    }

  /* for each terms associated with the last segment */
  t = bitset_iterate (&(index_entryp->seg_equal_terms[nsegs - 1]), &iter);
  for (; t != -1; t = bitset_next_member (&iter))
    {
      bitset_add (&range_terms, t);
      /* add multi_col_range_segs */
      if (QO_TERM_IS_FLAGED (QO_ENV_TERM (infop->env, t), QO_TERM_MULTI_COLL_PRED))
	{
	  bitset_add (&(index_entryp->multi_col_range_segs), index_entryp->seg_idxs[nsegs - 1]);
	}

      /* generate index scan plan */
      planp = qo_index_scan_new (infop, nodep, ni_entryp, QO_SCANMETHOD_INDEX_SCAN, &range_terms, NULL);

      n = qo_check_plan_on_info (infop, planp);
      if (n)
	{
	  normal_index_plan_n++;	/* include index skip scan */
	}

      /* is it safe to ignore the result of qo_check_plan_on_info()? */
      bitset_remove (&range_terms, t);
      if (QO_TERM_IS_FLAGED (QO_ENV_TERM (infop->env, t), QO_TERM_MULTI_COLL_PRED))
	{
	  bitset_remove (&(index_entryp->multi_col_range_segs), index_entryp->seg_idxs[nsegs - 1]);
	}
    }

  bitset_assign (&seg_other_terms, &(index_entryp->seg_other_terms[nsegs - 1]));
  for (t = bitset_iterate (&seg_other_terms, &iter); t != -1; t = bitset_next_member (&iter))
    {
      bitset_add (&range_terms, t);

      /* generate index scan plan */
      planp = qo_index_scan_new (infop, nodep, ni_entryp, QO_SCANMETHOD_INDEX_SCAN, &range_terms, NULL);

      n = qo_check_plan_on_info (infop, planp);
      if (n)
	{
	  normal_index_plan_n++;	/* include index skip scan */
	}

      /* is it safe to ignore the result of qo_check_plan_on_info()? */
      bitset_remove (&range_terms, t);
    }

  bitset_delset (&seg_other_terms);
  bitset_delset (&range_terms);

  return normal_index_plan_n;
}

int
qo_generate_loose_index_scan (QO_INFO * infop, QO_NODE * nodep, QO_NODE_INDEX_ENTRY * ni_entryp)
{
  QO_INDEX_ENTRY *index_entryp;
  int n = 0;
  QO_PLAN *planp;
  BITSET range_terms;

  bitset_init (&range_terms, infop->env);

  /* pointer to QO_INDEX_ENTRY structure */
  index_entryp = (ni_entryp)->head;
  if (index_entryp->force < 0)
    {
      assert (false);
      return 0;
    }

  assert (bitset_is_empty (&(index_entryp->seg_equal_terms[0])));
  assert (index_entryp->ils_prefix_len > 0);
  assert (QO_ENTRY_MULTI_COL (index_entryp));
  assert (index_entryp->cover_segments == true);
  assert (index_entryp->is_iss_candidate == false);

  assert (bitset_is_empty (&range_terms));

  planp = qo_index_scan_new (infop, nodep, ni_entryp, QO_SCANMETHOD_INDEX_SCAN, &range_terms, NULL);

  n = qo_check_plan_on_info (infop, planp);

  bitset_delset (&range_terms);

  return n;
}

int
qo_generate_sort_limit_plan (QO_ENV * env, QO_INFO * infop, QO_PLAN * subplan)
{
  int n;
  QO_PLAN *plan;

  if (subplan->order != QO_UNORDERED)
    {
      /* Do not put a SORT_LIMIT plan over an ordered plan because we have to keep the ordered principle. At best, we
       * can place a SORT_LIMIT plan directly under an ordered one.
       */
      return 0;
    }

  plan = qo_sort_new (subplan, QO_UNORDERED, SORT_LIMIT);
  if (plan == NULL)
    {
      return 0;
    }
  n = qo_check_plan_on_info (infop, plan);
  return n;
}

int
qo_has_is_not_null_term (QO_NODE * node)
{
  QO_ENV *env;
  QO_TERM *term;
  PT_NODE *expr;
  int i;
  bool found;

  assert (node != NULL && node->env != NULL);
  if (node == NULL || node->env == NULL)
    {
      return 0;
    }

  env = QO_NODE_ENV (node);
  for (i = 0; i < env->nterms; i++)
    {
      term = QO_ENV_TERM (env, i);

      /* term should belong to the given node */
      if (!bitset_intersects (&(QO_TERM_SEGS (term)), &(QO_NODE_SEGS (node))))
	{
	  continue;
	}

      expr = QO_TERM_PT_EXPR (term);
      if (!PT_IS_EXPR_NODE (expr))
	{
	  continue;
	}

      found = false;
      while (expr)
	{
	  if (expr->info.expr.op == PT_IS_NOT_NULL)
	    {
	      found = true;
	    }
	  else if (expr->info.expr.op == PT_IS_NULL)
	    {
	      found = false;
	      break;
	    }

	  expr = expr->or_next;
	}

      /* return if one of sarg term has not null operation */
      if (found)
	{
	  return 1;
	}
    }
  return 0;
}

QO_PLAN *
qo_search_planner (QO_PLANNER * planner)
{
  int i, j, nsegs;
  bool broken;
  QO_PLAN *plan;
  QO_NODE *node;
  QO_INFO *info;
  BITSET_ITERATOR si;
  int subq_idx;
  QO_SUBQUERY *subq;
  QO_NODE_INDEX *node_index;
  QO_NODE_INDEX_ENTRY *ni_entry;
  QO_INDEX_ENTRY *index_entry;
  BITSET seg_terms;
  BITSET nodes, subqueries, remaining_subqueries;
  int join_info_bytes;
  int n;
  int start_column = 0;
  PT_NODE *tree = NULL;
  bool special_index_scan = false;

  bitset_init (&nodes, planner->env);
  bitset_init (&subqueries, planner->env);
  bitset_init (&remaining_subqueries, planner->env);

  planner->worst_plan = qo_worst_new (planner->env);
  if (planner->worst_plan == NULL)
    {
      plan = NULL;
      goto end;
    }

  planner->worst_info = qo_alloc_info (planner, &nodes, &nodes, &nodes, QO_INFINITY, QO_INFINITY);
  (planner->worst_plan)->info = planner->worst_info;
  (void) qo_plan_add_ref (planner->worst_plan);

  /*
   * At this point, N (and node), S (and seg), E (and edge), and
   * EQ (and eqclass) have been initialized; we now need to set up the
   * various info vectors.
   *
   * For the time being, we assume that N is never "too large", and we
   * go ahead and allocate the full join_info vector of M elements.
   */
  if (planner->N > 1)
    {
      planner->M =
	QO_PARTITION_M_OFFSET (&planner->partition[planner->P - 1]) +
	QO_JOIN_INFO_SIZE (&planner->partition[planner->P - 1]);

      join_info_bytes = planner->M * sizeof (QO_INFO *);
      if (join_info_bytes > 0)
	{
	  planner->join_info = (QO_INFO **) malloc (join_info_bytes);
	  if (planner->join_info == NULL)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, (size_t) join_info_bytes);
	      plan = NULL;
	      goto end;
	    }
	}
      else
	{
	  plan = NULL;
	  goto end;
	}

      memset (planner->join_info, 0, join_info_bytes);
    }

  bitset_assign (&remaining_subqueries, &(planner->all_subqueries));

  /*
   * Add appropriate scan plans for each node.
   */
  planner->node_info = NULL;
  if (planner->N > 0)
    {
      size_t size = sizeof (QO_INFO *) * planner->N;

      planner->node_info = (QO_INFO **) malloc (size);
      if (planner->node_info == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, size);
	  plan = NULL;
	  goto end;
	}
    }

  for (i = 0; i < (signed) planner->N; ++i)
    {
      node = &planner->node[i];
      BITSET_CLEAR (nodes);
      bitset_add (&nodes, i);
      planner->node_info[i] =
	qo_alloc_info (planner, &nodes, &QO_NODE_SARGS (node), &QO_NODE_EQCLASSES (node),
		       QO_NODE_SELECTIVITY (node) * (double) QO_NODE_NCARD (node), (double) QO_NODE_NCARD (node));

      if (planner->node_info[i] == NULL)
	{
	  plan = NULL;
	  goto end;
	}

      BITSET_CLEAR (subqueries);
      for (subq_idx = bitset_iterate (&remaining_subqueries, &si); subq_idx != -1; subq_idx = bitset_next_member (&si))
	{
	  subq = &planner->subqueries[subq_idx];
	  if (bitset_is_empty (&subq->nodes)	/* uncorrelated */
	      || (bitset_subset (&nodes, &(subq->nodes))	/* correlated */
		  && bitset_subset (&(QO_NODE_SARGS (node)), &(subq->terms))))
	    {
	      bitset_add (&subqueries, subq_idx);
	      bitset_remove (&remaining_subqueries, subq_idx);
	    }
	}
      bitset_assign (&(QO_NODE_SUBQUERIES (node)), &subqueries);
    }

  /*
   * Check all of the terms to determine which are eligible to serve as
   * index scans.
   */
  for (i = 0; i < (signed) planner->N; i++)
    {
      node = &planner->node[i];
      info = planner->node_info[QO_NODE_IDX (node)];

      node_index = QO_NODE_INDEXES (node);

      /* Set special_index_scan to true if spec if flagged as: 1. Scan for b-tree key info. 2. Scan for b-tree node
       * info. These are special cases which need index scan forced.
       */
      special_index_scan = PT_SPEC_SPECIAL_INDEX_SCAN (QO_NODE_ENTITY_SPEC (node));
      if (special_index_scan)
	{
	  /* Make sure there is only one index entry */
	  assert (node_index != NULL && QO_NI_N (node_index) == 1);
	  ni_entry = QO_NI_ENTRY (node_index, 0);
	  n =
	    qo_check_plan_on_info (info,
				   qo_index_scan_new (info, node, ni_entry, QO_SCANMETHOD_INDEX_SCAN_INSPECT, NULL,
						      NULL));
	  assert (n == 1);
	  continue;
	}

      /*
       *  It is possible that this node will not have indexes.  This would
       *  happen (for instance) if the node represented a derived table.
       *  There is no purpose looking for index scans for a node without
       *  indexes so skip the search in this case.
       */
      if (node_index != NULL)
	{
	  bitset_init (&seg_terms, planner->env);

	  for (j = 0; j < QO_NI_N (node_index); j++)
	    {
	      ni_entry = QO_NI_ENTRY (node_index, j);
	      index_entry = (ni_entry)->head;
	      if (index_entry->force < 0)
		{
		  continue;	/* is disabled index; skip and go ahead */
		}

	      /* If the index is a candidate for index skip scan, then it will not have any terms for seg_equal or
	       * seg_other[0], so we should skip that first column from initial checks. Set the start column to 1.
	       */
	      start_column = index_entry->is_iss_candidate ? 1 : 0;

	      /* seg_terms will contain all the indexable terms that refer segments from this node; stops at the first
	       * one that has no equals or other terms
	       */
	      BITSET_CLEAR (seg_terms);
	      for (nsegs = start_column; nsegs < index_entry->nsegs; nsegs++)
		{
		  bitset_union (&seg_terms, &(index_entry->seg_equal_terms[nsegs]));
		  bitset_union (&seg_terms, &(index_entry->seg_other_terms[nsegs]));

		  if (bitset_is_empty (&(index_entry->seg_equal_terms[nsegs])))
		    {
		      if (!bitset_is_empty (&(index_entry->seg_other_terms[nsegs])))
			{
			  nsegs++;	/* include this term */
			}
		      break;
		    }
		}

	      bitset_intersect (&seg_terms, &(QO_NODE_SARGS (node)));

	      n = 0;		/* init */

	      if (!bitset_is_empty (&seg_terms))
		{
		  assert (nsegs > 0);

		  n = qo_generate_index_scan (info, node, ni_entry, nsegs);
		}
	      else if (index_entry->constraints->filter_predicate && index_entry->force > 0)
		{
		  assert (bitset_is_empty (&seg_terms));

		  /* Currently, CUBRID does not allow null values in index. The filter index expression must contain at
		   * least one term different than "is null". Otherwise, the index will be empty. Having at least one
		   * term different than "is null" in a filter index expression, the user knows from beginning that
		   * null values can't appear when scan filter index.
		   */

		  n =
		    qo_check_plan_on_info (info,
					   qo_index_scan_new (info, node, ni_entry, QO_SCANMETHOD_INDEX_SCAN,
							      &seg_terms, NULL));
		}
	      else if (index_entry->ils_prefix_len > 0)
		{
		  assert (bitset_is_empty (&seg_terms));

		  n = qo_generate_loose_index_scan (info, node, ni_entry);
		}
	      else
		{
		  assert (bitset_is_empty (&seg_terms));

		  /* if the index didn't normally skipped the order by, we try the new plan, maybe this will be better.
		   * DO NOT generate a order by index if there is no order by! Skip generating index from order by if
		   * multi_range_opt is true (multi range optimized plan is already better)
		   */
		  tree = QO_ENV_PT_TREE (info->env);
		  if (tree == NULL)
		    {
		      assert (false);	/* is invalid case */
		      continue;	/* nop */
		    }

		  if (tree->info.query.q.select.connect_by != NULL || qo_is_prefix_index (index_entry))
		    {
		      continue;	/* nop; go ahead */
		    }

		  /* if the index didn't normally skipped the group/order by, we try the new plan, maybe this will be
		   * better. DO NOT generate if there is no group/order by!
		   */
		  if (!n && !index_entry->groupby_skip && tree->info.query.q.select.group_by
		      && qo_validate_index_for_groupby (info->env, ni_entry))
		    {
		      n =
			qo_check_plan_on_info (info,
					       qo_index_scan_new (info, node, ni_entry,
								  QO_SCANMETHOD_INDEX_GROUPBY_SCAN, &seg_terms, NULL));
		    }

		  if (!n && !index_entry->orderby_skip && !tree->info.query.q.select.group_by
		      && tree->info.query.order_by && qo_validate_index_for_orderby (info->env, ni_entry))
		    {
		      n =
			qo_check_plan_on_info (info,
					       qo_index_scan_new (info, node, ni_entry,
								  QO_SCANMETHOD_INDEX_ORDERBY_SCAN, &seg_terms, NULL));
		    }
		}
	    }

	  bitset_delset (&seg_terms);
	}

      /* Create a sequential scan plan for each node. */
      qo_generate_seq_scan (info, node);

      if (QO_ENV_USE_SORT_LIMIT (planner->env) && QO_NODE_SORT_LIMIT_CANDIDATE (node))
	{
	  /* generate a stop plan over the current best plan of the */
	  QO_PLAN *best_plan;
	  best_plan = qo_find_best_plan_on_info (info, QO_UNORDERED, 1.0);
	  if (best_plan->plan_type == QO_PLANTYPE_SCAN && !qo_plan_multi_range_opt (best_plan)
	      && !qo_is_iscan_from_groupby (best_plan))
	    {
	      qo_generate_sort_limit_plan (planner->env, info, best_plan);
	    }
	}
    }

  /*
   * Now remaining_subqueries should contain only entries that depend
   * on more than one class.
   */

  if (planner->P > 1)
    {
      size_t size = sizeof (QO_INFO *) * planner->P;

      planner->cp_info = (QO_INFO **) malloc (size);
      if (planner->cp_info == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, size);
	  plan = NULL;
	  goto end;
	}

      for (i = 0; i < (signed) planner->P; i++)
	{
	  planner->cp_info[i] = NULL;
	}
    }

  broken = false;
  for (i = 0; i < (signed) planner->P; ++i)
    {
      /*
       * If any partition fails, give up.  We'll have to build an
       * unoptimized plan elsewhere.
       */
      if (qo_search_partition (planner, &planner->partition[i], QO_UNORDERED, &remaining_subqueries) == NULL)
	{
	  for (j = 0; j < i; ++j)
	    {
	      qo_plan_del_ref (planner->partition[j].plan);
	    }
	  broken = true;
	  break;
	}
    }
  plan = broken ? NULL : qo_combine_partitions (planner, &remaining_subqueries);

  /* if we have use_desc_idx hint and order by or group by, do some checking */

  if (plan == NULL)
    {
      goto end;
    }

  if (plan->use_iscan_descending == true && qo_plan_multi_range_opt (plan) == false)
    {
      qo_set_use_desc (plan);
    }

  tree = QO_ENV_PT_TREE (planner->env);
  assert (tree != NULL);

  if (tree->info.query.q.select.hint & PT_HINT_USE_IDX_DESC)
    {
      /* check direction of the first order by column. */
      if (tree->info.query.order_by != NULL && tree->info.query.q.select.connect_by == NULL)
	{
	  /* if we have order by and the hint, we allow the hint only if we have order by descending on first column.
	   * Otherwise we clear it */
	  if (tree->info.query.order_by->info.sort_spec.asc_or_desc == PT_ASC)
	    {
	      tree->info.query.q.select.hint &= ~PT_HINT_USE_IDX_DESC;
	    }
	}

      /* check direction of the first order by column. */
      if (tree->info.query.q.select.group_by != NULL && tree->info.query.q.select.group_by->flag.with_rollup == false)
	{
	  /* if we have group by and the hint, we allow the hint only if we have group by descending on first column.
	   * Otherwise we clear it */
	  if (tree->info.query.q.select.group_by->info.sort_spec.asc_or_desc == PT_ASC)
	    {
	      tree->info.query.q.select.hint &= ~PT_HINT_USE_IDX_DESC;
	    }
	}
    }

  /* some indexes may be marked with multi range optimization (as candidates) However, if the chosen top plan is not
   * marked as using multi range optimization it means that the optimization has been invalidated, or maybe another
   * plan was chosen. Make sure to un-mark indexes in this case
   */
  if (!qo_plan_multi_range_opt (plan))
    {
      qo_walk_plan_tree (plan, qo_unset_multi_range_optimization, NULL);
      if (plan->info->env != NULL && plan->info->env->multi_range_opt_candidate)
	{
	  plan->multi_range_opt_use = PLAN_MULTI_RANGE_OPT_CAN_USE;
	}
    }

  qo_walk_plan_tree (plan, qo_unset_hint_use_desc_idx, NULL);

end:

  bitset_delset (&nodes);
  bitset_delset (&subqueries);
  bitset_delset (&remaining_subqueries);

  return plan;
}

void
qo_clean_planner (QO_PLANNER * planner)
{
  /*
   * This cleans up everything that isn't needed for the surviving
   * plan.  In particular, it will give back all excess QO_PLAN
   * structures that we have allocated during the search.  All
   * detachable QO_INFO should already have been detached, so we don't
   * worry about them here.
   */
  planner->cleanup_needed = false;
  bitset_delset (&(planner->all_subqueries));
  bitset_delset (&(planner->final_segs));
  qo_plans_teardown (planner->env);
}

QO_INFO *
qo_search_partition_join (QO_PLANNER * planner, QO_PARTITION * partition, BITSET * remaining_subqueries)
{
  QO_ENV *env;
  int i, nodes_cnt, node_idx;
  PT_NODE *tree;
  PT_HINT_ENUM hint;
  QO_TERM *term;
  QO_NODE *node;
  int num_path_inner;
  QO_INFO *visited_info;
  BITSET visited_nodes;
  BITSET visited_rel_nodes;
  BITSET visited_terms;
  BITSET nested_path_nodes;
  BITSET remaining_nodes;
  BITSET remaining_terms;

  env = planner->env;
  bitset_init (&visited_nodes, env);
  bitset_init (&visited_rel_nodes, env);
  bitset_init (&visited_terms, env);
  bitset_init (&nested_path_nodes, env);
  bitset_init (&remaining_nodes, env);
  bitset_init (&remaining_terms, env);

  /* include useful nodes */
  bitset_assign (&remaining_nodes, &(QO_PARTITION_NODES (partition)));
  nodes_cnt = bitset_cardinality (&remaining_nodes);

  num_path_inner = 0;		/* init */

  /* include useful terms */
  for (i = 0; i < (signed) planner->T; i++)
    {
      term = &planner->term[i];
      if (QO_TERM_CLASS (term) == QO_TC_TOTALLY_AFTER_JOIN)
	{
	  continue;		/* skip and go ahead */
	}

      if (bitset_subset (&remaining_nodes, &(QO_TERM_NODES (term))))
	{
	  bitset_add (&remaining_terms, i);
	  if (QO_TERM_CLASS (term) == QO_TC_PATH)
	    {
	      num_path_inner++;	/* path-expr sargs */
	    }
	}

    }				/* for (i = ...) */

  /* set hint info */
  tree = QO_ENV_PT_TREE (env);
  hint = tree->info.query.q.select.hint;

  /* set #tables consider at a time */
  if (num_path_inner || (hint & PT_HINT_ORDERED))
    {
      /* inner join type path term exist; WHERE x.y.z = ? or there is a SQL hint ORDERED */
      planner->join_unit = nodes_cnt;	/* give up */
    }
  else
    {
      planner->join_unit = (nodes_cnt <= 25) ? MIN (8, nodes_cnt) : (nodes_cnt <= 37) ? 3 : 2;
    }

  /* STEP 1: do join search with visited nodes */

  node = NULL;			/* init */

  while (1)
    {
      node_idx = -1;		/* init */
      (void) planner_permutate (planner, partition, hint, node,	/* previous head node */
				&visited_nodes, &visited_rel_nodes, &visited_terms, &nested_path_nodes,
				&remaining_nodes, &remaining_terms, remaining_subqueries, num_path_inner,
				(planner->join_unit < nodes_cnt) ? &node_idx
				/* partial join search */
				: NULL /* total join search */ );
      if (planner->best_info)
	{			/* OK */
	  break;		/* found best total join plan */
	}

      if (planner->join_unit >= nodes_cnt)
	{
	  /* something wrong for total join search */
	  break;		/* give up */
	}
      else
	{
	  if (node_idx == -1)
	    {
	      /* something wrong for partial join search; rollback and retry total join search */
	      bitset_union (&remaining_nodes, &visited_nodes);
	      bitset_union (&remaining_terms, &visited_terms);

	      BITSET_CLEAR (nested_path_nodes);
	      BITSET_CLEAR (visited_nodes);
	      BITSET_CLEAR (visited_rel_nodes);
	      BITSET_CLEAR (visited_terms);

	      /* set #tables consider at a time */
	      planner->join_unit = nodes_cnt;

	      /* STEP 2: do total join search without visited nodes */

	      continue;
	    }
	}

      /* at here, still do partial join search */

      /* extract the outermost nodes at this join level */
      node = QO_ENV_NODE (env, node_idx);
      bitset_add (&visited_nodes, node_idx);
      bitset_add (&visited_rel_nodes, QO_NODE_REL_IDX (node));
      bitset_remove (&remaining_nodes, node_idx);

      /* extract already used terms at this join level */
      if (bitset_cardinality (&visited_nodes) == 1)
	{
	  /* current prefix has only one node */
	  visited_info = planner->node_info[node_idx];
	}
      else
	{
	  /* current prefix has two or more nodes */
	  visited_info = planner->join_info[QO_INFO_INDEX (QO_PARTITION_M_OFFSET (partition), visited_rel_nodes)];
	}

      if (visited_info == NULL)
	{			/* something wrong */
	  break;		/* give up */
	}

      bitset_assign (&visited_terms, &(visited_info->terms));
      bitset_difference (&remaining_terms, &(visited_info->terms));

      planner->join_unit++;	/* increase join unit level */

    }

  bitset_delset (&visited_rel_nodes);
  bitset_delset (&visited_nodes);
  bitset_delset (&visited_terms);
  bitset_delset (&nested_path_nodes);
  bitset_delset (&remaining_nodes);
  bitset_delset (&remaining_terms);

  return planner->best_info;
}

QO_PLAN *
qo_search_partition (QO_PLANNER * planner, QO_PARTITION * partition, QO_EQCLASS * order, BITSET * remaining_subqueries)
{
  int i, nodes_cnt;

  nodes_cnt = bitset_cardinality (&(QO_PARTITION_NODES (partition)));

  /* nodes are multi if there is a join to be done. If not, this is just a degenerate search to determine which of the
   * indexes (if available) to use for the (single) class involved in the query.
   */
  if (nodes_cnt > 1)
    {
      planner->best_info = qo_search_partition_join (planner, partition, remaining_subqueries);
    }
  else
    {
      QO_NODE *node;

      i = bitset_first_member (&(QO_PARTITION_NODES (partition)));
      node = QO_ENV_NODE (planner->env, i);
      planner->best_info = planner->node_info[QO_NODE_IDX (node)];
    }

  if (planner->env->dump_enable)
    {
      qo_dump_planner_info (planner, partition, stdout);
    }

  if (planner->best_info)
    {
      QO_PARTITION_PLAN (partition) = qo_plan_finalize (qo_find_best_plan_on_info (planner->best_info, order, 1.0));
    }
  else
    {
      QO_PARTITION_PLAN (partition) = NULL;
    }

  /* Now clean up after ourselves.  Free all of the plans that aren't part of the winner for this partition, but retain
   * the nodes: they contain information that the winning plan requires.
   */

  if (nodes_cnt > 1)
    {
      QO_INFO *info;

      for (info = planner->info_list; info; info = info->next)
	{
	  if (bitset_subset (&(QO_PARTITION_NODES (partition)), &(info->nodes)))
	    {
	      qo_detach_info (info);
	    }
	}
    }
  else
    {				/* single class */
      for (i = 0; i < (signed) planner->N; i++)
	{
	  if (BITSET_MEMBER (QO_PARTITION_NODES (partition), i))
	    {
	      qo_detach_info (planner->node_info[i]);
	    }
	}
    }

  return QO_PARTITION_PLAN (partition);
}

void
sort_partitions (QO_PLANNER * planner)
{
  int i, j;
  QO_PARTITION *i_part, *j_part;
  QO_PARTITION tmp;

  for (i = 1; i < (signed) planner->P; ++i)
    {
      i_part = &planner->partition[i];
      for (j = 0; j < i; ++j)
	{
	  j_part = &planner->partition[j];
	  /*
	   * If the higher partition (i_part) supplies something that
	   * the lower partition (j_part) needs, swap them.
	   */
	  if (bitset_intersects (&(QO_PARTITION_NODES (i_part)), &(QO_PARTITION_DEPENDENCIES (j_part))))
	    {
	      tmp = *i_part;
	      *i_part = *j_part;
	      *j_part = tmp;
	    }
	}
    }
}

QO_PLAN *
qo_combine_partitions (QO_PLANNER * planner, BITSET * reamining_subqueries)
{
  QO_PARTITION *partition = planner->partition;
  QO_PLAN *plan, *t_plan;
  BITSET nodes;
  BITSET terms;
  BITSET eqclasses;
  BITSET sarged_terms;
  BITSET subqueries;
  BITSET_ITERATOR bi;
  int i, t, s;
  double cardinality, total_rows;
  QO_PLAN *next_plan;

  bitset_init (&nodes, planner->env);
  bitset_init (&terms, planner->env);
  bitset_init (&eqclasses, planner->env);
  bitset_init (&sarged_terms, planner->env);
  bitset_init (&subqueries, planner->env);

  /*
   * Order the partitions by dependency information.  We could probably
   * undertake a more sophisticated search here that takes the
   * remaining sargable terms into account, but this code is probably
   * hardly ever exercised anyway, and this query is already known to
   * be a loser, so don't worry about it.
   */
  sort_partitions (planner);

  for (i = 0; i < (signed) planner->P; ++i)
    {
      (QO_PARTITION_PLAN (&planner->partition[i]))->refcount--;
    }

  /*
   * DON'T initialize these until after the sorting is done.
   */
  plan = QO_PARTITION_PLAN (partition);
  cardinality = (plan->info)->cardinality;
  total_rows = (plan->info)->total_rows;

  bitset_assign (&nodes, &((plan->info)->nodes));
  bitset_assign (&terms, &((plan->info)->terms));
  bitset_assign (&eqclasses, &((plan->info)->eqclasses));

  for (++partition, i = 1; i < (signed) planner->P; ++partition, ++i)
    {
      next_plan = QO_PARTITION_PLAN (partition);

      bitset_union (&nodes, &((next_plan->info)->nodes));
      bitset_union (&terms, &((next_plan->info)->terms));
      bitset_union (&eqclasses, &((next_plan->info)->eqclasses));
      cardinality *= (next_plan->info)->cardinality;
      total_rows *= (next_plan->info)->total_rows;

      planner->cp_info[i] = qo_alloc_info (planner, &nodes, &terms, &eqclasses, cardinality, total_rows);

      for (t = planner->E; t < (signed) planner->T; ++t)
	{
	  if (!bitset_is_empty (&(QO_TERM_NODES (&planner->term[t]))) && !BITSET_MEMBER (terms, t)
	      && bitset_subset (&nodes, &(QO_TERM_NODES (&planner->term[t])))
	      && (QO_TERM_CLASS (&planner->term[t]) != QO_TC_TOTALLY_AFTER_JOIN))
	    {
	      bitset_add (&sarged_terms, t);
	    }
	}

      BITSET_CLEAR (subqueries);
      for (s = bitset_iterate (reamining_subqueries, &bi); s != -1; s = bitset_next_member (&bi))
	{
	  QO_SUBQUERY *subq = &planner->subqueries[s];
	  if (bitset_subset (&nodes, &(subq->nodes)) && bitset_subset (&sarged_terms, &(subq->terms)))
	    {
	      bitset_add (&subqueries, s);
	      bitset_remove (reamining_subqueries, s);
	    }
	}

      plan = qo_cp_new (planner->cp_info[i], plan, next_plan, &sarged_terms, &subqueries);
      qo_detach_info (planner->cp_info[i]);
      BITSET_CLEAR (sarged_terms);
    }

  /*
   * Now finalize the topmost node of the tree.
   */
  if (plan != NULL)
    {
      qo_plan_finalize (plan);
    }

  for (i = planner->E; i < (signed) planner->T; ++i)
    {
      if (bitset_is_empty (&(QO_TERM_NODES (&planner->term[i]))))
	{
	  bitset_add (&sarged_terms, i);
	}
    }

  /* skip empty sort plan */
  for (t_plan = plan; t_plan && t_plan->plan_type == QO_PLANTYPE_SORT; t_plan = t_plan->plan_un.sort.subplan)
    {
      if (!bitset_is_empty (&(t_plan->sarged_terms)))
	{
	  break;
	}
    }

  if (t_plan)
    {
      bitset_union (&(t_plan->sarged_terms), &sarged_terms);
    }
  else if (plan != NULL)
    {
      /* invalid plan structure. occur error */
      qo_plan_discard (plan);
      plan = NULL;
    }

  bitset_delset (&nodes);
  bitset_delset (&terms);
  bitset_delset (&eqclasses);
  bitset_delset (&sarged_terms);
  bitset_delset (&subqueries);

  return plan;
}
