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
#include "query_planner_constants.h"

void qo_plan_fprint (QO_PLAN * plan, FILE * f, int howfar, const char *title);
void qo_plan_lite_print (QO_PLAN * plan, FILE * f, int howfar);
PT_NODE *qo_get_col_product_ndv (PARSER_CONTEXT * parser, PT_NODE * tree, void *arg, int *continue_walk);
bool qo_plan_is_orderby_skip_candidate (QO_PLAN * plan);

double
log3 (double n)
{
  // C++11 and later: Thread-safe initialization for local static variables is guaranteed by the compiler, called "Magic Statics"
  static double ln3_value = log (3.0);
  return log (n) / ln3_value;
}

void
qo_estimate_ngroups (QO_PLAN * plan, SORT_TYPE sort_type)
{
  int group_ndv, estimate_ndv;
  double expected_nrows = plan->info->cardinality;
  double total_nrows = plan->info->total_rows;

  /* get NDV of GROUP BY */
  group_ndv = MIN (qo_get_group_ndv (plan, sort_type), expected_nrows);
  if (group_ndv == -1)
    {
      plan->info->group_rows = expected_nrows;
      return;
    }

  if (expected_nrows == total_nrows)
    {
      estimate_ndv = group_ndv;
    }
  else
    {
      /* estimate number of groups */
      estimate_ndv = MAX (qo_estimate_ndv (total_nrows, expected_nrows, group_ndv), 1);
    }

  estimate_ndv = MIN (expected_nrows, estimate_ndv);

  if (plan->info->group_rows > estimate_ndv)
    {
      plan->info->group_rows = estimate_ndv;
    }
}

double
qo_estimate_ndv (double N, double p, double n)
{
  if (N <= 0.0 || n <= 0.0)
    {
      return 0.0;
    }

  double ratio = (N - p) / N;
  double exponent = N / n;

  return n * (1.0 - pow (ratio, exponent));
}

int
qo_get_group_ndv (QO_PLAN * plan, SORT_TYPE sort_type)
{
  PT_NODE *nodes;
  QO_ENV *env = NULL;
  PARSER_CONTEXT *parser = NULL;
  NDV_INFO ndv_info;


  env = (plan->info)->env;
  parser = QO_ENV_PARSER (env);

  if ((QO_ENV_PT_TREE (env))->node_type == PT_SELECT)
    {
      if (sort_type == SORT_GROUPBY)
	{
	  nodes = (QO_ENV_PT_TREE (env))->info.query.q.select.group_by;
	}
      else
	{
	  nodes = (QO_ENV_PT_TREE (env))->info.query.q.select.list;
	}
    }
  else
    {
      return -1;
    }

  ndv_info.env = env;
  ndv_info.total_ndv = 1;
  bitset_init (&ndv_info.seg_bitset, env);
  /* The NDV is simply extracted from column without considering the function, etc. and product of NDV of each column */
  parser_walk_tree (parser, nodes, qo_get_col_product_ndv, &ndv_info, NULL, NULL);
  if (ndv_info.total_ndv == 1)
    {
      ndv_info.total_ndv = -1;
    }
  bitset_delset (&ndv_info.seg_bitset);

  return ndv_info.total_ndv;
}

void
qo_plan_compute_cost (QO_PLAN * plan)
{
  QO_ENV *env;
  QO_SUBQUERY *subq;
  PT_NODE *query;
  double temp_cpu_cost, temp_io_cost;
  double subq_cpu_cost, subq_io_cost;
  int i;
  BITSET_ITERATOR iter;

  /* When computing the cost for a WORST_PLAN, we'll get in here without a backing info node; just work around it. */
  env = plan->info ? (plan->info)->env : NULL;
  subq_cpu_cost = subq_io_cost = 0.0;

  /* Compute the costs for all of the subqueries. Each of the pinned subqueries is intended to be evaluated once for
   * each row produced by this plan; the cost of each such evaluation in the fixed cost of the subquery plus one trip
   * through the result, i.e.,
   *
   * QO_PLAN_FIXED_COST(subplan) + QO_PLAN_ACCESS_COST(subplan)
   *
   * The cost info for the subplan has (probably) been squirreled away in a QO_SUMMARY structure reachable from the
   * original select node.
   */

  for (i = bitset_iterate (&(plan->subqueries), &iter); i != -1; i = bitset_next_member (&iter))
    {
      subq = env ? &env->subqueries[i] : NULL;
      query = subq ? subq->node : NULL;
      qo_plan_compute_subquery_cost (query, &temp_cpu_cost, &temp_io_cost);
      subq_cpu_cost += temp_cpu_cost;
      subq_io_cost += temp_io_cost;
    }

  /* This computes the specific cost characteristics for each plan. */
  (*(plan->vtbl)->cost_fn) (plan);

  /* Now add in the subquery costs; this cost is incurred for each row produced by this plan, so multiply it by the
   * estimated scan_rows and add it to the access cost.
   */
  if (plan->info)
    {
      plan->variable_cpu_cost += (plan->info)->scan_rows * subq_cpu_cost;
      plan->variable_io_cost += (plan->info)->scan_rows * subq_io_cost;
    }
}

void
qo_plan_compute_subquery_cost (PT_NODE * subquery, double *subq_cpu_cost, double *subq_io_cost)
{
  QO_SUMMARY *summary;
  double arg1_cpu_cost, arg1_io_cost, arg2_cpu_cost, arg2_io_cost;

  *subq_cpu_cost = *subq_io_cost = 0.0;	/* init */

  if (subquery == NULL)
    {
      /* This case is selected when a subquery wasn't optimized for some reason.
       * just take a guess. ---> NEED MORE CONSIDERATION
       */
      *subq_cpu_cost = 5.0;
      *subq_io_cost = 5.0;
      return;
    }

  switch (subquery->node_type)
    {
    case PT_SELECT:
      summary = (QO_SUMMARY *) subquery->info.query.q.select.qo_summary;
      if (summary)
	{
	  *subq_cpu_cost += summary->fixed_cpu_cost + summary->variable_cpu_cost;
	  *subq_io_cost += summary->fixed_io_cost + summary->variable_io_cost;
	}
      else
	{
	  /* it may be unknown error. just take a guess. ---> NEED MORE CONSIDERATION */
	  *subq_cpu_cost = 5.0;
	  *subq_io_cost = 5.0;
	}

      /* Here, GROUP BY and ORDER BY cost must be considered. ---> NEED MORE CONSIDERATION */
      /* ---> under construction <--- */
      break;

    case PT_UNION:
    case PT_INTERSECTION:
    case PT_DIFFERENCE:
      qo_plan_compute_subquery_cost (subquery->info.query.q.union_.arg1, &arg1_cpu_cost, &arg1_io_cost);
      qo_plan_compute_subquery_cost (subquery->info.query.q.union_.arg2, &arg2_cpu_cost, &arg2_io_cost);

      *subq_cpu_cost = arg1_cpu_cost + arg2_cpu_cost;
      *subq_io_cost = arg1_io_cost + arg2_io_cost;

      /* later, sort cost and result-set scan cost must be considered. ---> NEED MORE CONSIDERATION */
      /* ---> under construction <--- */
      break;

    default:
      /* it is unknown case. just take a guess. ---> NEED MORE CONSIDERATION */
      *subq_cpu_cost = 5.0;
      *subq_io_cost = 5.0;
      break;
    }

  return;
}

void
qo_sscan_cost (QO_PLAN * planp)
{
  QO_NODE *nodep;
  double extra_weight = 0.0;
  double scan_rows;

  nodep = planp->plan_un.scan.node;
  planp->fixed_cpu_cost = 0.0;
  planp->fixed_io_cost = 0.0;

  scan_rows = MAX (1, QO_NODE_NCARD (nodep));
  planp->info->scan_rows = scan_rows;

  if (QO_NODE_NCARD (nodep) == 0)
    {
      planp->variable_cpu_cost = 1.0 * (double) QO_CPU_WEIGHT;
    }
  else
    {
      planp->variable_cpu_cost = (double) QO_NODE_NCARD (nodep) * (double) QO_CPU_WEIGHT;
    }

  extra_weight = qo_sum_bitset_term_cost_weights (planp->info->env, &(QO_NODE_SARGS (nodep)));

  planp->variable_cpu_cost += scan_rows * QO_CPU_WEIGHT * extra_weight;
  planp->variable_io_cost = (double) QO_NODE_TCARD (nodep);

#if TEST_DUMP_PLAN_SCAN_COST
  fprintf (stdout, "\nSequential Scan Cost: \n");
  fprintf (stdout, "  -    Fixed CPU Cost: %lf\n", planp->fixed_cpu_cost);
  fprintf (stdout, "  -    Fixed I/O Cost: %lf\n", planp->fixed_io_cost);
  fprintf (stdout, "  - Variable CPU Cost: %lf\n", planp->variable_cpu_cost);
  fprintf (stdout, "  - Variable I/O Cost: %lf\n", planp->variable_io_cost);
  fprintf (stdout, "  -    Total     Cost: %lf\n",
	   planp->fixed_cpu_cost + planp->fixed_io_cost + planp->variable_cpu_cost + planp->variable_io_cost);
  if (planp->vtbl != NULL)
    {
      // qo_plan_lite_print (planp, stdout, 0);
      qo_plan_fprint (planp, stdout, 0, NULL);
      fprintf (stdout, "\n");
    }
  fprintf (stdout, "\n");
#endif /* TEST_DUMP_PLAN_SCAN_COST */
}

void
qo_iscan_cost (QO_PLAN * planp)
{
  QO_NODE *nodep;
  QO_NODE_INDEX_ENTRY *ni_entryp;
  QO_ATTR_CUM_STATS *cum_statsp;
  QO_INDEX_ENTRY *index_entryp;
  double sel, sel_limit, height, leaves, opages, filter_sel, leaf_access, heap_access;
  double object_IO, index_IO;
  QO_TERM *termp;
  BITSET_ITERATOR iter;
  int i, t, n, pkeys_num, index;

  nodep = planp->plan_un.scan.node;
  ni_entryp = planp->plan_un.scan.index;
  index_entryp = (ni_entryp)->head;
  cum_statsp = &(ni_entryp)->cum_stats;

  if (index_entryp->force < 0)
    {
      assert (false);
      qo_worst_cost (planp);
      return;
    }
  else if (index_entryp->force > 0)
    {
      qo_zero_cost (planp);
      return;
    }

  n = index_entryp->col_num;
  if (SM_IS_CONSTRAINT_UNIQUE_FAMILY (index_entryp->constraints->type) && n == index_entryp->nsegs)
    {
      assert (n > 0);

      for (i = 0; i < n; i++)
	{
	  if (bitset_is_empty (&index_entryp->seg_equal_terms[i]))
	    {
	      break;
	    }
	}

      if (i == n)
	{
	  /* When the index is a unique family and all of index columns are specified in the equal conditions, the
	   * cardinality of the scan will 0 or 1. In this case we will make the scan cost to zero, thus to force the
	   * optimizer to select this scan.
	   */
	  index_entryp->all_unique_index_columns_are_equi_terms = true;
	}
    }

  /* selectivity of the index terms */
  sel = 1.0;

  pkeys_num = MIN (n, cum_statsp->pkeys_size);
  assert (pkeys_num <= BTREE_STATS_PKEYS_NUM);

  if (bitset_is_empty (&(planp->plan_un.scan.terms)))
    {
      assert (!qo_is_index_iss_scan (planp));
    }

  for (i = 0, t = bitset_iterate (&(planp->plan_un.scan.terms), &iter); t != -1; t = bitset_next_member (&iter))
    {
      termp = QO_ENV_TERM (QO_NODE_ENV (nodep), t);
      sel *= QO_TERM_SELECTIVITY (termp);

      /* each term can have multi index column. e.g.) (a,b) in .. */
      for (int j = 0; j < index_entryp->col_num; j++)
	{
	  if (BITSET_MEMBER (QO_TERM_SEGS (termp), index_entryp->seg_idxs[j]))
	    {
	      i++;
	    }
	}
    }

  /* check upper bound */
  sel = MIN (sel, 1.0);

  sel_limit = 0.0;		/* init */

  /* set selectivity limit */
  if (qo_is_index_iss_scan (planp))
    {
      index = i;
    }
  else
    {
      index = (i == 0) ? 0 : i - 1;
    }

  if (i <= pkeys_num && cum_statsp->pkeys[index] >= 1)
    {
      sel_limit = 1.0 / (double) cum_statsp->pkeys[index];
    }
  else
    {				/* can not use btree partial-key statistics */
      if (cum_statsp->keys >= 1)
	{
	  sel_limit = 1.0 / (double) cum_statsp->keys;
	}
      else
	{
	  if (QO_NODE_NCARD (nodep) == 0)
	    {			/* empty class */
	      sel_limit = sel = 0.0;
	    }
	  else if (QO_NODE_NCARD (nodep) >= 1)
	    {
	      sel_limit = 1.0 / (double) QO_NODE_NCARD (nodep);
	    }
	}
    }
  assert (sel_limit <= 1.0);

  /* check lower bound */
  sel = MAX (sel, sel_limit);

  /* selectivity of the index key filter terms */
  filter_sel = 1.0;
  for (t = bitset_iterate (&(planp->plan_un.scan.kf_terms), &iter); t != -1; t = bitset_next_member (&iter))
    {
      termp = QO_ENV_TERM (QO_NODE_ENV (nodep), t);
      filter_sel *= QO_TERM_SELECTIVITY (termp);
    }

  /* number of leaf to be selected */
  leaf_access = sel * (double) QO_NODE_NCARD (nodep);
  /* height of the B+tree */
  height = (double) cum_statsp->height - 1;
  if (height < 0)
    {
      height = 0;
    }
  /* number of leaf pages to be accessed */
  leaves = ceil (sel * (double) cum_statsp->leafs);
  /* total number of pages occupied by objects */
  opages = (double) QO_NODE_TCARD (nodep);
  /* I/O cost to access B+tree index */
  index_IO = ((ni_entryp)->n * height) + leaves;

  /* Index Skip Scan adds to the index IO cost the K extra BTREE searches it does to fetch the next value for the
   * following BTRangeScan
   */
  if (qo_is_index_iss_scan (planp))
    {
      if (pkeys_num > 0)
	{
	  assert (cum_statsp->pkeys != NULL);
	  assert (cum_statsp->pkeys_size != 0);

	  /* K leaves are additionally read */
	  index_IO += cum_statsp->pkeys[0];
	}
    }

  /* IO cost to fetch objects */
  if (qo_is_index_covering_scan (planp))
    {
      object_IO = 1.0;
      heap_access = 0;
    }
  else
    {
      object_IO = opages * sel * filter_sel;
      heap_access = (double) QO_NODE_NCARD (nodep) * sel * filter_sel * (double) ISCAN_OID_ACCESS_OVERHEAD;
    }
  object_IO = MAX (1.0, object_IO);

  /* index scan requires more CPU cost than sequential scan */
  planp->fixed_cpu_cost = 0.0;
  planp->fixed_io_cost = index_IO;
  planp->variable_cpu_cost = (leaf_access + heap_access) * (double) QO_CPU_WEIGHT;
  planp->variable_io_cost = object_IO;
  planp->info->scan_rows = MAX (1, (double) QO_NODE_NCARD (nodep) * sel * filter_sel);
  qo_apply_scan_term_cpu_overhead (planp);

#if TEST_DUMP_PLAN_SCAN_COST
  fprintf (stdout, "\nIndex Scan Cost: \n");
  fprintf (stdout, "  -    Fixed CPU Cost: %lf\n", planp->fixed_cpu_cost);
  fprintf (stdout, "  -    Fixed I/O Cost: %lf\n", planp->fixed_io_cost);
  fprintf (stdout, "  - Variable CPU Cost: %lf\n", planp->variable_cpu_cost);
  fprintf (stdout, "  - Variable I/O Cost: %lf\n", planp->variable_io_cost);
  fprintf (stdout, "  -    Total     Cost: %lf\n",
	   planp->fixed_cpu_cost + planp->fixed_io_cost + planp->variable_cpu_cost + planp->variable_io_cost);
  if (planp->vtbl != NULL)
    {
      // qo_plan_lite_print (planp, stdout, 0);
      qo_plan_fprint (planp, stdout, 0, NULL);
      fprintf (stdout, "\n");
    }
  fprintf (stdout, "\n");
#endif /* TEST_DUMP_PLAN_SCAN_COST */
}

void
qo_sort_cost (QO_PLAN * planp)
{
  QO_PLAN *subplanp;

  subplanp = planp->plan_un.sort.subplan;

  /* for worst cost */
  if (subplanp->fixed_cpu_cost == QO_INFINITY || subplanp->fixed_io_cost == QO_INFINITY
      || subplanp->variable_cpu_cost == QO_INFINITY || subplanp->variable_io_cost == QO_INFINITY)
    {
      qo_worst_cost (planp);
      return;
    }

  if (subplanp->plan_type == QO_PLANTYPE_SORT && planp->plan_un.sort.sort_type == SORT_TEMP)
    {
      /* This plan won't actually incur any runtime cost because it won't actually exist (its sort spec will supersede
       * the sort spec of the subplan).  We can't just clobber the sort spec on the lower plan because it might be
       * shared by others.
       */
      planp->fixed_cpu_cost = subplanp->fixed_cpu_cost;
      planp->fixed_io_cost = subplanp->fixed_io_cost;
      planp->variable_cpu_cost = subplanp->variable_cpu_cost;
      planp->variable_io_cost = subplanp->variable_io_cost;
    }
  else if (planp->plan_un.sort.sort_type == SORT_LIMIT)
    {
      if (subplanp->plan_type == QO_PLANTYPE_SORT)
	{
	  /* No sense in having a STOP plan above a SORT plan */
	  qo_worst_cost (planp);
	}

      if (qo_is_iscan_from_orderby (subplanp))
	{
	  double save_ncard = QO_NODE_NCARD (subplanp->plan_un.scan.node);
	  QO_NODE_NCARD (subplanp->plan_un.scan.node) = (double) db_get_bigint (&QO_ENV_LIMIT_VALUE (planp->info->env));
	  (*(subplanp->vtbl)->cost_fn) (subplanp);
	  QO_NODE_NCARD (subplanp->plan_un.scan.node) = save_ncard;
	}

      /* SORT-LIMIT plan has the same cost as the subplan (since actually sorting items in memory is not a big
       * drawback. Costs improvements will be applied when we consider joining this plan with other plans
       */
      planp->fixed_cpu_cost = subplanp->fixed_cpu_cost;
      planp->fixed_io_cost = subplanp->fixed_io_cost;
      planp->variable_cpu_cost = subplanp->variable_cpu_cost;
      planp->variable_io_cost = subplanp->variable_io_cost;
    }
  else
    {
      QO_EQCLASS *order;
      double objects, pages, result_size;

      order = planp->order;
      objects = (subplanp->info)->cardinality;
      result_size = objects * (double) (subplanp->info)->projected_size;
      pages = result_size / (double) IO_PAGESIZE;
      if (pages < 1.0)
	{
	  pages = 1.0;
	}

      /* The cost (in io's) of just setting up a list file.  This is mostly to discourage the optimizer from choosing
       * merge join for joins of little classes.
       */
      planp->fixed_cpu_cost = subplanp->fixed_cpu_cost + subplanp->variable_cpu_cost + TEMP_SETUP_COST;
      planp->fixed_io_cost = subplanp->fixed_io_cost + subplanp->variable_io_cost;
      planp->variable_cpu_cost = objects * (double) QO_CPU_WEIGHT;
      planp->variable_io_cost = pages;

      if (order != QO_UNORDERED && order != subplanp->order)
	{
	  double sort_io, tcard;

	  sort_io = 0.0;	/* init */

	  if (objects > 1.0)
	    {
	      if (pages < (double) prm_get_integer_value (PRM_ID_SR_NBUFFERS))
		{
		  /* We can sort the result in memory without any additional io costs. Assume cpu costs are n*log(n) in
		   * number of recors.
		   */
		  sort_io = (double) QO_CPU_WEIGHT *objects * log2 (objects);
		}
	      else
		{
		  /* There are too many records to permit an in-memory sort, so io costs will be increased.  Assume
		   * that the io costs increase by the number of pages required to hold the intermediate result.  CPU
		   * costs increase as above. Model courtesy of Ender.
		   */
		  sort_io = pages * log3 (pages / 4.0);

		  /* guess: apply IO caching for big size sort list. Disk IO cost cannot be greater than the 10% number
		   * of the requested IO pages
		   */
		  if (subplanp->plan_type == QO_PLANTYPE_SCAN)
		    {
		      tcard = (double) QO_NODE_TCARD (subplanp->plan_un.scan.node);
		      tcard *= 0.1;
		      if (pages >= tcard)
			{	/* big size sort list */
			  sort_io *= 0.1;
			}
		    }
		}
	    }

	  planp->fixed_io_cost += sort_io;
	}
    }

#if TEST_DUMP_PLAN_SORT_COST
  fprintf (stdout, "\nSort Cost: \n");
  fprintf (stdout, "  -    Fixed CPU Cost: %lf\n", planp->fixed_cpu_cost);
  fprintf (stdout, "  -    Fixed I/O Cost: %lf\n", planp->fixed_io_cost);
  fprintf (stdout, "  - Variable CPU Cost: %lf\n", planp->variable_cpu_cost);
  fprintf (stdout, "  - Variable I/O Cost: %lf\n", planp->variable_io_cost);
  fprintf (stdout, "  -    Total     Cost: %lf\n",
	   planp->fixed_cpu_cost + planp->fixed_io_cost + planp->variable_cpu_cost + planp->variable_io_cost);
  if (planp->vtbl != NULL)
    {
      // qo_plan_lite_print (planp, stdout, 0);
      qo_plan_fprint (planp, stdout, 0, NULL);
      fprintf (stdout, "\n");
    }
  fprintf (stdout, "\n");
#endif /* TEST_DUMP_PLAN_SORT_COST */
}

bool
qo_can_apply_limit_card (QO_ENV * env)
{
  PARSER_CONTEXT *parser;
  PT_NODE *tree;

  if (env == NULL || (tree = QO_ENV_PT_TREE (env)) == NULL)
    {
      return false;
    }

  parser = QO_ENV_PARSER (env);
  if (parser == NULL)
    {
      return false;
    }

  /* Only PT_SELECT has group_by, connect_by in q.select */
  if (tree->node_type != PT_SELECT)
    {
      return false;
    }

  /* 1. ORDER BY */
  if (tree->info.query.order_by != NULL)
    {
      return false;
    }

  /* 2. Analytic (Window functions) */
  if (pt_has_analytic (parser, tree))
    {
      return false;
    }

  /* 3. DISTINCT */
  if (tree->info.query.all_distinct == PT_DISTINCT)
    {
      return false;
    }

  /* 4. GROUP BY */
  if (tree->info.query.q.select.group_by != NULL)
    {
      return false;
    }

  /* 5. Aggregate functions */
  if (pt_has_aggregate (parser, tree))
    {
      return false;
    }

  /* 6. Hierarchical Query (CONNECT BY) */
  if (tree->info.query.q.select.connect_by != NULL)
    {
      return false;
    }

  /* 7. Recursive CTE */
  if (tree->info.query.with != NULL && tree->info.query.with->info.with_clause.recursive != 0)
    {
      return false;
    }

  return true;
}

void
qo_nljoin_cost (QO_PLAN * planp)
{
  QO_PLAN *inner, *outer;
  double inner_io_cost, inner_cpu_cost, outer_io_cost, outer_cpu_cost;
  double guessed_result_cardinality, limit_val, outer_card;

  inner = planp->plan_un.join.inner;

  /* for worst cost */
  if (inner->fixed_cpu_cost == QO_INFINITY || inner->fixed_io_cost == QO_INFINITY
      || inner->variable_cpu_cost == QO_INFINITY || inner->variable_io_cost == QO_INFINITY)
    {
      qo_worst_cost (planp);
      return;
    }

  outer = planp->plan_un.join.outer;

  /* for worst cost */
  if (outer->fixed_cpu_cost == QO_INFINITY || outer->fixed_io_cost == QO_INFINITY
      || outer->variable_cpu_cost == QO_INFINITY || outer->variable_io_cost == QO_INFINITY)
    {
      qo_worst_cost (planp);
      return;
    }

  /* CPU and IO costs which are fixed againt join */
  planp->fixed_cpu_cost = outer->fixed_cpu_cost + inner->fixed_cpu_cost;
  planp->fixed_io_cost = outer->fixed_io_cost + inner->fixed_io_cost;

  /* inner side CPU cost of nested-loop block join */
  if (outer->plan_type == QO_PLANTYPE_SORT && outer->plan_un.sort.sort_type == SORT_LIMIT)
    {
      /* cardinality of a SORT_LIMIT plan is given by the value of the query limit */
      guessed_result_cardinality = (double) db_get_bigint (&QO_ENV_LIMIT_VALUE (outer->info->env));
    }
  else if (QO_PLAN_HAS_LIMIT (planp)
	   && (planp->info->planner->can_apply_limit_card || qo_plan_is_orderby_skip_candidate (planp)))
    {
      limit_val = QO_PLAN_HAS_CONSTANT_LIMIT (planp)
	? (double) db_get_bigint (&QO_ENV_LIMIT_VALUE (planp->info->env)) : GUESSED_BIND_LIMIT_CARD;

      if (outer->plan_type == QO_PLANTYPE_SCAN)
	{
	  planp->limit_nljoin_guessed_card = MAX (limit_val / (outer->info)->hit_prob, 1.0);
	  guessed_result_cardinality = MIN (planp->limit_nljoin_guessed_card, (outer->info)->cardinality);
	}
      else if (outer->plan_type == QO_PLANTYPE_JOIN)
	{
	  guessed_result_cardinality = outer->limit_nljoin_guessed_card;
	  outer_card = ((outer->info)->cardinality == 0) ? 1 : (outer->info)->cardinality;
	  /* result = outer_guessed * (inner_card * selectivity) = outer_guessed * (plan_card/outer_card). */
	  planp->limit_nljoin_guessed_card =
	    MAX (1.0, guessed_result_cardinality * ((planp->info)->cardinality / outer_card));
	}
      else
	{
	  /* won't come here */
	  guessed_result_cardinality = (outer->info)->cardinality;
	}
      guessed_result_cardinality = MAX (1.0, guessed_result_cardinality);
    }
  else
    {
      guessed_result_cardinality = (outer->info)->cardinality;
    }

  inner_cpu_cost = guessed_result_cardinality * inner->variable_cpu_cost;

  if (qo_is_iscan (inner))
    {
      inner_io_cost = guessed_result_cardinality * inner->variable_io_cost * (1 - ISCAN_IO_HIT_RATIO);
    }
  else
    {
      inner_io_cost = (guessed_result_cardinality + SSCAN_DEFAULT_CARD) * inner->variable_io_cost;
    }

  {
    double delayed_sarg_penalty;

    delayed_sarg_penalty = qo_get_delayed_sarg_lookup_penalty (planp, guessed_result_cardinality);

    if (delayed_sarg_penalty > 1.0)
      {
	inner_cpu_cost *= delayed_sarg_penalty;
	inner_io_cost *= delayed_sarg_penalty;
      }
  }


  /* inner side IO cost of nested-loop block join */
  if (qo_is_iscan (inner))
    {
      inner_io_cost = guessed_result_cardinality * inner->variable_io_cost * (1 - ISCAN_IO_HIT_RATIO);
    }
  else
    {
      /* if inner is seq scan, it is calculated by default card. */
      /* This prevents the worst plan if the cardinality is calculated to be less than the actual value. */
      inner_io_cost = (guessed_result_cardinality + SSCAN_DEFAULT_CARD) * inner->variable_io_cost;
    }

  /* outer side CPU cost of nested-loop block join */
  outer_cpu_cost = outer->variable_cpu_cost;
  /* outer side IO cost of nested-loop block join */
  outer_io_cost = outer->variable_io_cost;

  /* CPU and IO costs which are variable according to the join plan */
  planp->variable_cpu_cost = inner_cpu_cost + outer_cpu_cost;
  planp->variable_io_cost = inner_io_cost + outer_io_cost;

  {
    QO_ENV *env;
    int i;
    QO_SUBQUERY *subq;
    PT_NODE *query;
    double temp_cpu_cost, temp_io_cost;
    double subq_cpu_cost, subq_io_cost;
    BITSET_ITERATOR iter;

    /* Compute the costs for all of the subqueries. Each of the pinned subqueries is intended to be evaluated once for
     * each row produced by this plan; the cost of each such evaluation in the fixed cost of the subquery plus one trip
     * through the result, i.e.,
     *
     * QO_PLAN_FIXED_COST(subplan) + QO_PLAN_ACCESS_COST(subplan)
     *
     * The cost info for the subplan has (probably) been squirreled away in a QO_SUMMARY structure reachable from the
     * original select node.
     */

    /* When computing the cost for a WORST_PLAN, we'll get in here without a backing info node; just work around it. */
    env = inner->info ? (inner->info)->env : NULL;
    subq_cpu_cost = subq_io_cost = 0.0;	/* init */

    for (i = bitset_iterate (&(inner->subqueries), &iter); i != -1; i = bitset_next_member (&iter))
      {
	subq = env ? &env->subqueries[i] : NULL;
	query = subq ? subq->node : NULL;
	qo_plan_compute_subquery_cost (query, &temp_cpu_cost, &temp_io_cost);
	subq_cpu_cost += temp_cpu_cost;
	subq_io_cost += temp_io_cost;
      }

    /* subq cost is already included in the inner. so add it for the cardinality excluded due to ISCAN_IO_HIT_RATIO. */
    planp->variable_cpu_cost += guessed_result_cardinality * ISCAN_IO_HIT_RATIO * subq_cpu_cost;
    planp->variable_io_cost += guessed_result_cardinality * ISCAN_IO_HIT_RATIO * subq_io_cost;	/* assume IO as # blocks */

    planp->variable_cpu_cost += qo_get_nljoin_term_cpu_overhead (planp, guessed_result_cardinality);
  }

#if TEST_DUMP_PLAN_JOIN_COST
  fprintf (stdout, "\nNested Loop Cost: \n");
  fprintf (stdout, "  -    Fixed CPU Cost: %lf\n", planp->fixed_cpu_cost);
  fprintf (stdout, "  -    Fixed I/O Cost: %lf\n", planp->fixed_io_cost);
  fprintf (stdout, "  - Variable CPU Cost: %lf\n", planp->variable_cpu_cost);
  fprintf (stdout, "  - Variable I/O Cost: %lf\n", planp->variable_io_cost);
  fprintf (stdout, "  -    Total     Cost: %lf\n",
	   planp->fixed_cpu_cost + planp->fixed_io_cost + planp->variable_cpu_cost + planp->variable_io_cost);
  if (planp->vtbl != NULL)
    {
      // qo_plan_lite_print (planp, stdout, 0);
      qo_plan_fprint (planp, stdout, 0, NULL);
      fprintf (stdout, "\n");
    }
  fprintf (stdout, "\n");
#endif /* TEST_DUMP_PLAN_JOIN_COST */
}

void
qo_mjoin_cost (QO_PLAN * planp)
{
  QO_PLAN *inner;
  QO_PLAN *outer;
  QO_ENV *env;
  double outer_cardinality = 0.0, inner_cardinality = 0.0;

  inner = planp->plan_un.join.inner;

  /* for worst cost */
  if (inner->fixed_cpu_cost == QO_INFINITY || inner->fixed_io_cost == QO_INFINITY
      || inner->variable_cpu_cost == QO_INFINITY || inner->variable_io_cost == QO_INFINITY)
    {
      qo_worst_cost (planp);
      return;
    }

  outer = planp->plan_un.join.outer;

  /* for worst cost */
  if (outer->fixed_cpu_cost == QO_INFINITY || outer->fixed_io_cost == QO_INFINITY
      || outer->variable_cpu_cost == QO_INFINITY || outer->variable_io_cost == QO_INFINITY)
    {
      qo_worst_cost (planp);
      return;
    }

  env = outer->info->env;
  if (outer->has_sort_limit)
    {
      outer_cardinality = (double) db_get_bigint (&QO_ENV_LIMIT_VALUE (env));
    }
  else
    {
      outer_cardinality = outer->info->cardinality;
    }

  if (inner->has_sort_limit)
    {
      inner_cardinality = (double) db_get_bigint (&QO_ENV_LIMIT_VALUE (env));
    }
  else
    {
      inner_cardinality = inner->info->cardinality;
    }

  /* CPU and IO costs which are fixed against join */
  planp->fixed_cpu_cost = outer->fixed_cpu_cost + inner->fixed_cpu_cost;
  planp->fixed_io_cost = outer->fixed_io_cost + inner->fixed_io_cost;
  /* CPU and IO costs which are variable according to the join plan */
  planp->variable_cpu_cost = outer->variable_cpu_cost + inner->variable_cpu_cost;
  planp->variable_cpu_cost += (outer_cardinality + inner_cardinality) * QO_CPU_WEIGHT * MJ_CPU_OVERHEAD_FACTOR;
  /* merge cost */
  planp->variable_io_cost = outer->variable_io_cost + inner->variable_io_cost;

#if TEST_DUMP_PLAN_JOIN_COST
  fprintf (stdout, "\nSort Merge Cost: \n");
  fprintf (stdout, "  -    Fixed CPU Cost: %lf\n", planp->fixed_cpu_cost);
  fprintf (stdout, "  -    Fixed I/O Cost: %lf\n", planp->fixed_io_cost);
  fprintf (stdout, "  - Variable CPU Cost: %lf\n", planp->variable_cpu_cost);
  fprintf (stdout, "  - Variable I/O Cost: %lf\n", planp->variable_io_cost);
  fprintf (stdout, "  -    Total     Cost: %lf\n",
	   planp->fixed_cpu_cost + planp->fixed_io_cost + planp->variable_cpu_cost + planp->variable_io_cost);
  if (planp->vtbl != NULL)
    {
      // qo_plan_lite_print (planp, stdout, 0);
      qo_plan_fprint (planp, stdout, 0, NULL);
      fprintf (stdout, "\n");
    }
  fprintf (stdout, "\n");
#endif /* TEST_DUMP_PLAN_JOIN_COST */
}

void
qo_hjoin_cost (QO_PLAN * plan_p)
{
  QO_PLAN *inner_plan_p, *outer_plan_p;
  double inner_cardinality, outer_cardinality;
  double inner_build_cpu_cost, outer_build_cpu_cost;
  double inner_build_io_cost, outer_build_io_cost;

  inner_plan_p = plan_p->plan_un.join.inner;

  /* for worst cost */
  if ((inner_plan_p->fixed_cpu_cost == QO_INFINITY) || (inner_plan_p->fixed_io_cost == QO_INFINITY)
      || (inner_plan_p->variable_cpu_cost == QO_INFINITY) || (inner_plan_p->variable_io_cost == QO_INFINITY))
    {
      qo_worst_cost (plan_p);
      return;
    }

  outer_plan_p = plan_p->plan_un.join.outer;

  /* for worst cost */
  if ((outer_plan_p->fixed_cpu_cost == QO_INFINITY) || (outer_plan_p->fixed_io_cost == QO_INFINITY)
      || (outer_plan_p->variable_cpu_cost == QO_INFINITY) || (outer_plan_p->variable_io_cost == QO_INFINITY))
    {
      qo_worst_cost (plan_p);
      return;
    }

  inner_cardinality = inner_plan_p->info->cardinality;
  outer_cardinality = outer_plan_p->info->cardinality;

  /**
   * STEP 1: Sum up the fixed and variable costs from both the outer and inner.
   */
  plan_p->fixed_cpu_cost = outer_plan_p->fixed_cpu_cost + inner_plan_p->fixed_cpu_cost;
  plan_p->fixed_io_cost = outer_plan_p->fixed_io_cost + inner_plan_p->fixed_io_cost;

  plan_p->variable_cpu_cost = outer_plan_p->variable_cpu_cost + inner_plan_p->variable_cpu_cost;
  plan_p->variable_io_cost = outer_plan_p->variable_io_cost + inner_plan_p->variable_io_cost;

  /**
   * STEP 2: Calculate the cost when inner is used as build input.
   */
  inner_build_cpu_cost = (inner_cardinality * QO_CPU_WEIGHT * HJ_BUILD_CPU_OVERHEAD_FACTOR);
  inner_build_cpu_cost += (outer_cardinality * QO_CPU_WEIGHT * HJ_PROBE_CPU_OVERHEAD_FACTOR);
  inner_build_io_cost = 0.0;

  /**
   * STEP 3: Calculate the cost when outer is used as build input.
   */
  outer_build_cpu_cost = (inner_cardinality * QO_CPU_WEIGHT * HJ_PROBE_CPU_OVERHEAD_FACTOR);
  outer_build_cpu_cost += (outer_cardinality * QO_CPU_WEIGHT * HJ_BUILD_CPU_OVERHEAD_FACTOR);
  outer_build_io_cost = 0.0;

#if 0
  /* No need to increase weight since partitioned hash join is used even when mem_limit is exceeded. */

  UINT64 mem_limit = prm_get_bigint_value (PRM_ID_MAX_HASH_LIST_SCAN_SIZE);

  if ((inner_cardinality * (sizeof (HENTRY_HLS) + 16 /* sizeof (QFILE_TUPLE_SIMPLE_POS) */ )) > mem_limit)
    {
      inner_build_io_cost += (inner_cardinality * HJ_FILE_IO_WEIGHT);
      inner_build_io_cost += (outer_cardinality * HJ_FILE_IO_WEIGHT);
    }

  if ((outer_cardinality * (sizeof (HENTRY_HLS) + 16 /* sizeof (QFILE_TUPLE_SIMPLE_POS) */ )) > mem_limit)
    {
      outer_build_io_cost += (inner_cardinality * HJ_FILE_IO_WEIGHT);
      outer_build_io_cost += (outer_cardinality * HJ_FILE_IO_WEIGHT);
    }
#endif

  /**
   * STEP 4: Choose the lowest cost.
   */
  switch (plan_p->plan_un.join.join_type)
    {
    case JOIN_LEFT:
      plan_p->variable_cpu_cost += inner_build_cpu_cost;
      plan_p->variable_io_cost += inner_build_io_cost;
      break;

    case JOIN_RIGHT:
      plan_p->variable_cpu_cost += outer_build_cpu_cost;
      plan_p->variable_io_cost += outer_build_io_cost;
      break;

    case JOIN_INNER:
      if ((inner_build_cpu_cost + inner_build_io_cost) <= (outer_build_cpu_cost + outer_build_io_cost))
	{
	  plan_p->variable_cpu_cost += inner_build_cpu_cost;
	  plan_p->variable_io_cost += inner_build_io_cost;
	}
      else
	{
	  plan_p->variable_cpu_cost += outer_build_cpu_cost;
	  plan_p->variable_io_cost += outer_build_io_cost;
	}
      break;

    default:
      qo_worst_cost (plan_p);
      assert (false);
    }

#if TEST_DUMP_PLAN_JOIN_COST
  fprintf (stdout, "\nHash Join Cost: \n");
  fprintf (stdout, "  -    Fixed CPU Cost: %lf\n", plan_p->fixed_cpu_cost);
  fprintf (stdout, "  -    Fixed I/O Cost: %lf\n", plan_p->fixed_io_cost);
  fprintf (stdout, "  - Variable CPU Cost: %lf\n", plan_p->variable_cpu_cost);
  fprintf (stdout, "  - Variable I/O Cost: %lf\n", plan_p->variable_io_cost);
  fprintf (stdout, "  -    Total     Cost: %lf\n",
	   plan_p->fixed_cpu_cost + plan_p->fixed_io_cost + plan_p->variable_cpu_cost + plan_p->variable_io_cost);
  if (plan_p->vtbl != NULL)
    {
      // qo_plan_lite_print (plan_p, stdout, 0);
      qo_plan_fprint (plan_p, stdout, 0, NULL);
      fprintf (stdout, "\n");
    }
  fprintf (stdout, "\n");
#endif /* TEST_DUMP_PLAN_JOIN_COST */
}

void
qo_follow_cost (QO_PLAN * planp)
{
  QO_PLAN *head;
  QO_NODE *tail;
  double cardinality, target_pages, fetch_ios;

  head = planp->plan_un.follow.head;

  /* for worst cost */
  if (head->fixed_cpu_cost == QO_INFINITY || head->fixed_io_cost == QO_INFINITY
      || head->variable_cpu_cost == QO_INFINITY || head->variable_io_cost == QO_INFINITY)
    {
      qo_worst_cost (planp);
      return;
    }

  cardinality = (planp->info)->cardinality;
  tail = QO_TERM_TAIL (planp->plan_un.follow.path);
  target_pages = (double) QO_NODE_TCARD (tail);

  if (cardinality < target_pages)
    {
      /* If we expect to fetch fewer objects than there are pages in the target class, just assume that each fetch will
       * touch a new page.
       */
      fetch_ios = cardinality;
    }
  else if (prm_get_integer_value (PRM_ID_PB_NBUFFERS) >= target_pages)
    {
      /* We have more pointers to follow than pages in the target, but fewer target pages than buffer pages.  Assume
       * that the page buffering will limit the number of of page fetches to the number of target pages.
       */
      fetch_ios = target_pages;
    }
  else
    {
      fetch_ios = cardinality * (1.0 - ((double) prm_get_integer_value (PRM_ID_PB_NBUFFERS)) / target_pages);
    }

  planp->fixed_cpu_cost = head->fixed_cpu_cost;
  planp->fixed_io_cost = head->fixed_io_cost;
  planp->variable_cpu_cost = head->variable_cpu_cost + (cardinality * (double) QO_CPU_WEIGHT);
  planp->variable_io_cost = head->variable_io_cost + fetch_ios;

#if TEST_DUMP_PLAN_FOLLOW_COST
  fprintf (stdout, "\nFollow Cost: \n");
  fprintf (stdout, "  -    Fixed CPU Cost: %lf\n", planp->fixed_cpu_cost);
  fprintf (stdout, "  -    Fixed I/O Cost: %lf\n", planp->fixed_io_cost);
  fprintf (stdout, "  - Variable CPU Cost: %lf\n", planp->variable_cpu_cost);
  fprintf (stdout, "  - Variable I/O Cost: %lf\n", planp->variable_io_cost);
  fprintf (stdout, "  -    Total     Cost: %lf\n",
	   planp->fixed_cpu_cost + planp->fixed_io_cost + planp->variable_cpu_cost + planp->variable_io_cost);
  if (planp->vtbl != NULL)
    {
      qo_plan_lite_print (planp, stdout, 0);
      fprintf (stdout, "\n");
    }
  fprintf (stdout, "\n");
#endif /* TEST_DUMP_PLAN_FOLLOW_COST */
}

void
qo_worst_cost (QO_PLAN * planp)
{
  planp->fixed_cpu_cost = QO_INFINITY;
  planp->fixed_io_cost = QO_INFINITY;
  planp->variable_cpu_cost = QO_INFINITY;
  planp->variable_io_cost = QO_INFINITY;
  planp->use_iscan_descending = false;
}

void
qo_zero_cost (QO_PLAN * planp)
{
  planp->fixed_cpu_cost = 0.0;
  planp->fixed_io_cost = 0.0;
  planp->variable_cpu_cost = 0.0;
  planp->variable_io_cost = 0.0;
}

double
qo_sum_bitset_term_cost_weights (QO_ENV * env, BITSET * terms)
{
  BITSET_ITERATOR iter;
  int t;
  double sum = 0.0;

  for (t = bitset_iterate (terms, &iter); t != -1; t = bitset_next_member (&iter))
    {
      QO_TERM *term = QO_ENV_TERM (env, t);
      sum += qo_get_term_cost_weight (term);
    }

  return sum;
}

void
qo_apply_scan_term_cpu_overhead (QO_PLAN * planp)
{
  double scan_rows = MAX (1.0, planp->info->scan_rows);
  double range_weight = 0.0;
  double key_filter_weight = 0.0;
  double data_filter_weight = 0.0;

  range_weight = qo_sum_bitset_term_cost_weights (planp->info->env, &(planp->plan_un.scan.terms));

  key_filter_weight = qo_sum_bitset_term_cost_weights (planp->info->env, &(planp->plan_un.scan.kf_terms));

  data_filter_weight = qo_sum_bitset_term_cost_weights (planp->info->env, &(planp->sarged_terms));

  planp->variable_cpu_cost +=
    scan_rows * QO_CPU_WEIGHT * (1.2 * range_weight + 1.0 * key_filter_weight + 0.8 * data_filter_weight);
}

double
qo_get_join_term_cost_weight (QO_TERM * term)
{
  PT_NODE *expr;

  if (term == NULL)
    {
      return QO_COST_WEIGHT_JOIN_DEFAULT;
    }

  expr = QO_TERM_PT_EXPR (term);
  if (expr == NULL || expr->node_type != PT_EXPR)
    {
      return QO_COST_WEIGHT_JOIN_DEFAULT;
    }

  switch (expr->info.expr.op)
    {
    case PT_EQ:
    case PT_NULLSAFE_EQ:
      {
	PT_NODE *lhs = expr->info.expr.arg1;

	if (lhs != NULL && (lhs->type_enum == PT_TYPE_CHAR || lhs->type_enum == PT_TYPE_VARCHAR))
	  {
	    return QO_COST_WEIGHT_JOIN_STRING_EQUAL;
	  }
	return QO_COST_WEIGHT_JOIN_DEFAULT;
      }

    case PT_LT:
    case PT_LE:
    case PT_GT:
    case PT_GE:
    case PT_BETWEEN:
    case PT_RANGE:
      {
	PT_NODE *lhs = expr->info.expr.arg1;

	if (lhs != NULL && (lhs->type_enum == PT_TYPE_CHAR || lhs->type_enum == PT_TYPE_VARCHAR))
	  {
	    return QO_COST_WEIGHT_JOIN_STRING_RANGE;
	  }
	return QO_COST_WEIGHT_JOIN_DEFAULT;
      }

    default:
      return QO_COST_WEIGHT_JOIN_DEFAULT;
    }
}

double
qo_sum_join_term_cost_weights (QO_ENV * env, BITSET * terms)
{
  BITSET_ITERATOR iter;
  int t;
  double sum = 0.0;

  if (env == NULL || terms == NULL)
    {
      return 0.0;
    }

  for (t = bitset_iterate (terms, &iter); t != -1; t = bitset_next_member (&iter))
    {
      QO_TERM *term = QO_ENV_TERM (env, t);
      sum += qo_get_join_term_cost_weight (term);
    }

  return sum;
}

double
qo_get_nljoin_term_cpu_overhead (QO_PLAN * planp, double guessed_result_cardinality)
{
  double join_term_weight_sum = 0.0;

  if (planp == NULL || planp->plan_type != QO_PLANTYPE_JOIN || planp->info == NULL || planp->info->env == NULL)
    {
      return 0.0;
    }

  if (guessed_result_cardinality <= 0.0)
    {
      return 0.0;
    }

  /*
   * For temporary nl-join plans used only for inner-plan search,
   * join-term bitsets may be empty even after explicit init.
   * That is fine; the summed overhead will become 0.
   */

  join_term_weight_sum += qo_sum_join_term_cost_weights (planp->info->env, &(planp->plan_un.join.join_terms));

  if (IS_OUTER_JOIN_TYPE (planp->plan_un.join.join_type))
    {
      join_term_weight_sum +=
	qo_sum_join_term_cost_weights (planp->info->env, &(planp->plan_un.join.during_join_terms));
    }

  if (join_term_weight_sum <= 0.0)
    {
      return 0.0;
    }

  return guessed_result_cardinality * QO_CPU_WEIGHT * 0.5 * join_term_weight_sum;
}

double
qo_get_term_cost_weight (QO_TERM * term)
{
  PT_NODE *expr;

  if (term == NULL)
    {
      return QO_COST_WEIGHT_PRED_DEFAULT;
    }

  expr = QO_TERM_PT_EXPR (term);
  if (expr == NULL || expr->node_type != PT_EXPR)
    {
      return QO_COST_WEIGHT_PRED_DEFAULT;
    }

  switch (expr->info.expr.op)
    {
    case PT_EQ:
    case PT_NE:
      {
	PT_NODE *lhs = expr->info.expr.arg1;
	PT_NODE *rhs = expr->info.expr.arg2;

	if (lhs != NULL && lhs->type_enum == PT_TYPE_CHAR)
	  {
	    return QO_COST_WEIGHT_STRING_EQUAL;
	  }
	if (lhs != NULL && lhs->type_enum == PT_TYPE_VARCHAR)
	  {
	    return QO_COST_WEIGHT_STRING_EQUAL;
	  }
	return QO_COST_WEIGHT_NUMERIC_COMPARE;
      }

    case PT_LT:
    case PT_LE:
    case PT_GT:
    case PT_GE:
      {
	PT_NODE *lhs = expr->info.expr.arg1;
	if (lhs != NULL && (lhs->type_enum == PT_TYPE_CHAR || lhs->type_enum == PT_TYPE_VARCHAR))
	  {
	    return QO_COST_WEIGHT_STRING_RANGE;
	  }
	return QO_COST_WEIGHT_NUMERIC_COMPARE;
      }

    case PT_LIKE:
      {
	PT_NODE *rhs = expr->info.expr.arg2;

	if (rhs != NULL && rhs->node_type == PT_VALUE)
	  {
	    const char *pat = db_get_string (&rhs->info.value.db_value);
	    if (pat != NULL)
	      {
		const char *pct = strchr (pat, '%');
		const char *und = strchr (pat, '_');

		if (pct != NULL && pct[1] == '\0' && und == NULL && pct != pat)
		  {
		    return QO_COST_WEIGHT_LIKE_PREFIX;
		  }
		if (pat[0] == '%' || und != NULL)
		  {
		    return QO_COST_WEIGHT_LIKE_CONTAINS;
		  }
	      }
	  }
	return QO_COST_WEIGHT_LIKE_COMPLEX;
      }

    default:
      return QO_COST_WEIGHT_PRED_DEFAULT;
    }
}

bool
qo_info_is_small_filtered_side (QO_INFO * info)
{
  if (info == NULL)
    {
      return false;
    }

  if (info->cardinality <= QO_MCV_GUARD_SMALL_CARD_ABS)
    {
      return true;
    }

  if (info->total_rows > 0.0 && info->cardinality / info->total_rows <= QO_MCV_GUARD_SMALL_CARD_RATIO)
    {
      return true;
    }

  return false;
}

double
qo_apply_mcv_hotkey_join_guard (QO_TERM * term, QO_INFO * head_info, QO_INFO * tail_info,
				double base_cardinality, double term_sel)
{
  double effective_mcv_max_frequency;
  double head_card, tail_card, small_card, large_card;
  bool head_small, tail_small;
  double risk_fanout, risk_card, risk_sel;

  if (term == NULL || head_info == NULL || tail_info == NULL)
    {
      return term_sel;
    }

  if (!QO_TERM_IS_FLAGED (term, QO_TERM_EQUAL_OP))
    {
      return term_sel;
    }

  /*
   * Broad join terms are not the hot-key problem this guard is meant to fix.
   * Penalizing them can hide good plans that start from filtered dimension tables.
   */
  if (term_sel >= QO_MCV_GUARD_MAX_BASE_SELECTIVITY)
    {
      return term_sel;
    }

  head_card = MAX (1.0, head_info->cardinality);
  tail_card = MAX (1.0, tail_info->cardinality);

  head_small = qo_info_is_small_filtered_side (head_info);
  tail_small = qo_info_is_small_filtered_side (tail_info);

  /*
   * Apply only when exactly one side is small.
   * This protects against broad joins and symmetric small-small joins.
   */
  if (head_small == tail_small)
    {
      return term_sel;
    }

  small_card = head_small ? head_card : tail_card;
  large_card = head_small ? tail_card : head_card;
  effective_mcv_max_frequency =
    head_small ? QO_TERM_TAIL_MCV_MAX_FREQUENCY (term) : QO_TERM_HEAD_MCV_MAX_FREQUENCY (term);

  if (effective_mcv_max_frequency < QO_MCV_GUARD_MIN_FREQUENCY)
    {
      return term_sel;
    }

  base_cardinality = MAX (1.0, base_cardinality);

  /*
   * If the small side joins through one hot key, the largest single MCV
   * frequency on the large side is the direct upper-risk fanout signal.
   */
  risk_fanout = large_card * effective_mcv_max_frequency;
  risk_card = small_card * risk_fanout;

  if (risk_card <= 1.0)
    {
      return term_sel;
    }

  risk_sel = risk_card / base_cardinality;
  risk_sel = MIN (risk_sel, term_sel * QO_MCV_GUARD_MAX_SELECTIVITY_MULTIPLIER);

  if (risk_sel > term_sel)
    {
      term_sel = risk_sel;
    }

  term_sel = MIN (term_sel, 1.0);

  return term_sel;
}

double
qo_get_delayed_sarg_lookup_penalty (QO_PLAN * planp, double guessed_outer_cardinality)
{
  QO_PLAN *inner;
  double data_filter_weight;
  double penalty;

  if (planp == NULL || planp->plan_type != QO_PLANTYPE_JOIN)
    {
      return 1.0;
    }

  if (!QO_IS_NL_JOIN (planp))
    {
      return 1.0;
    }

  inner = planp->plan_un.join.inner;
  if (inner == NULL || inner->info == NULL || inner->info->env == NULL)
    {
      return 1.0;
    }

  /*
   * Only repeated index lookups are targeted.
   * This is intended for cases like:
   *   outer has many rows
   *   inner is PK/index lookup
   *   inner still has data filters, e.g. n.name LIKE 'B%'
   */
  if (!qo_is_iscan (inner))
    {
      return 1.0;
    }

  if (guessed_outer_cardinality < QO_DELAYED_SARG_OUTER_CARD_THRESHOLD)
    {
      return 1.0;
    }

  if (bitset_is_empty (&inner->sarged_terms))
    {
      return 1.0;
    }

  data_filter_weight = qo_sum_bitset_term_cost_weights (inner->info->env, &inner->sarged_terms);
  if (data_filter_weight <= 0.0)
    {
      return 1.0;
    }

  /*
   * Penalize repeated lookup with late data filters.
   *
   * Examples:
   * - n.id = ci.person_id via PK lookup
   * - n.name LIKE 'B%' remains as a sarg/data filter
   * - outer cardinality is large
   */
  penalty = 1.0
    + MIN (QO_DELAYED_SARG_PENALTY_MAX,
	   data_filter_weight * log10 (MAX (10.0, guessed_outer_cardinality)) * QO_DELAYED_SARG_PENALTY_FACTOR);

  return penalty;
}
