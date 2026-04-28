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

static int qo_plans_allocated;
static int qo_plans_deallocated;
static int qo_plans_malloced;
static int qo_plans_demalloced;
static int qo_accumulating_plans;
static int qo_next_tmpfile;

static QO_PLAN *qo_plan_free_list;

static QO_PLAN_VTBL qo_seq_scan_plan_vtbl = {
  "sscan",
  qo_scan_fprint,
  qo_scan_walk,
  qo_scan_free,
  qo_sscan_cost,
  qo_sscan_cost,
  qo_scan_info,
  "Sequential scan"
};

static QO_PLAN_VTBL qo_index_scan_plan_vtbl = {
  "iscan",
  qo_scan_fprint,
  qo_scan_walk,
  qo_scan_free,
  qo_iscan_cost,
  qo_iscan_cost,
  qo_scan_info,
  "Index scan"
};

static QO_PLAN_VTBL qo_sort_plan_vtbl = {
  "temp",
  qo_sort_fprint,
  qo_sort_walk,
  qo_sort_free,
  qo_sort_cost,
  qo_sort_cost,
  qo_sort_info,
  "Sort"
};

static QO_PLAN_VTBL qo_nl_join_plan_vtbl = {
  "nl-join",
  qo_join_fprint,
  qo_join_walk,
  qo_join_free,
  qo_nljoin_cost,
  qo_nljoin_cost,
  qo_join_info,
  "Nested-loop join"
};

static QO_PLAN_VTBL qo_idx_join_plan_vtbl = {
  "idx-join",
  qo_join_fprint,
  qo_join_walk,
  qo_join_free,
  qo_nljoin_cost,
  qo_nljoin_cost,
  qo_join_info,
  "Correlated-index join"
};

static QO_PLAN_VTBL qo_merge_join_plan_vtbl = {
  "m-join",
  qo_join_fprint,
  qo_join_walk,
  qo_join_free,
  qo_mjoin_cost,
  qo_mjoin_cost,
  qo_join_info,
  "Merge join"
};

static QO_PLAN_VTBL qo_hash_join_plan_vtbl = {
  "hash-join",
  qo_hjoin_fprint,
  qo_join_walk,
  qo_join_free,
#if TEST_HASH_JOIN_FORCE_ENABLE
  qo_zero_cost,
  qo_zero_cost,
#else /* TEST_HASH_JOIN_FORCE_ENABLE */
  qo_hjoin_cost,
  qo_hjoin_cost,
#endif /* TEST_HASH_JOIN_FORCE_ENABLE */
  qo_join_info,
  "Hash join"
};

static QO_PLAN_VTBL qo_follow_plan_vtbl = {
  "follow",
  qo_follow_fprint,
  qo_follow_walk,
  qo_follow_free,
  qo_follow_cost,
  qo_follow_cost,
  qo_follow_info,
  "Object fetch"
};

static QO_PLAN_VTBL qo_set_follow_plan_vtbl = {
  "set_follow",
  qo_follow_fprint,
  qo_follow_walk,
  qo_follow_free,
  qo_follow_cost,
  qo_follow_cost,
  qo_follow_info,
  "Set fetch"
};

static QO_PLAN_VTBL qo_worst_plan_vtbl = {
  "worst",
  qo_worst_fprint,
  qo_worst_walk,
  qo_worst_free,
  qo_worst_cost,
  qo_worst_cost,
  qo_worst_info,
  "Bogus"
};

QO_PLAN_VTBL *all_vtbls[] = {
  &qo_seq_scan_plan_vtbl,
  &qo_index_scan_plan_vtbl,
  &qo_sort_plan_vtbl,
  &qo_nl_join_plan_vtbl,
  &qo_idx_join_plan_vtbl,
  &qo_merge_join_plan_vtbl,
  &qo_hash_join_plan_vtbl,
  &qo_follow_plan_vtbl,
  &qo_set_follow_plan_vtbl,
  &qo_worst_plan_vtbl
};

QO_PLAN *
qo_plan_malloc (QO_ENV * env)
{
  QO_PLAN *plan;

  ++qo_plans_allocated;
  if (qo_plan_free_list)
    {
      plan = qo_plan_free_list;
      qo_plan_free_list = plan->plan_un.free.link;
    }
  else
    {
      ++qo_plans_malloced;
      plan = (QO_PLAN *) malloc (sizeof (QO_PLAN));
      if (plan == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, sizeof (QO_PLAN));
	  return NULL;
	}

#if !defined(NDEBUG)
      /* Prevent faults when qo_plan_fprint or qo_plan_lite_print is called. */
      plan->vtbl = NULL;
#endif
    }

  bitset_init (&(plan->sarged_terms), env);
  bitset_init (&(plan->subqueries), env);

  plan->parallel_opt_use = PLAN_PARALLEL_OPT_NO;
  plan->skip_orderby_opt = QO_PLAN_SKIP_ORDERBY_NO;

  plan->has_sort_limit = false;
  plan->use_iscan_descending = false;
  plan->need_final_sort = false;
  plan->limit_nljoin_guessed_card = 0.0;

  return plan;
}

const char *
qo_term_string (QO_TERM * term, char *buf)
{
  char *p;
  BITSET_ITERATOR bi;
  int i;
  QO_ENV *env;
  const char *separator;
  PT_NODE *conj, *saved_next = NULL;

  env = QO_TERM_ENV (term);

  conj = QO_TERM_PT_EXPR (term);
  if (conj)
    {
      saved_next = conj->next;
      conj->next = NULL;
    }

  switch (QO_TERM_CLASS (term))
    {
    case QO_TC_DEP_LINK:
      sprintf (buf, "table(");
      p = buf + strlen (buf);
      separator = "";
      for (i = bitset_iterate (&(QO_NODE_DEP_SET (QO_TERM_TAIL (term))), &bi); i != -1; i = bitset_next_member (&bi))
	{
	  sprintf (p, "%s%s", separator, QO_NODE_NAME (QO_ENV_NODE (env, i)));
	  p = buf + strlen (buf);
	  separator = ",";
	}
      sprintf (p, ") -> %s", QO_NODE_NAME (QO_TERM_TAIL (term)));
      p = buf;
      break;

    case QO_TC_DEP_JOIN:
      p = buf;
      sprintf (p, "dep-join(%s,%s)", QO_NODE_NAME (QO_TERM_HEAD (term)), QO_NODE_NAME (QO_TERM_TAIL (term)));
      break;

    case QO_TC_DUMMY_JOIN:
      p = buf;
      sprintf (p, "dummy(%s,%s)", QO_NODE_NAME (QO_TERM_HEAD (term)), QO_NODE_NAME (QO_TERM_TAIL (term)));
      break;

    default:
      assert_release (conj != NULL);
      if (conj)
	{
	  PARSER_CONTEXT *parser = QO_ENV_PARSER (QO_TERM_ENV (term));
	  PT_PRINT_VALUE_FUNC saved_func = parser->print_db_value;
	  unsigned int save_custom = parser->custom_print;

	  parser->custom_print |= PT_CONVERT_RANGE;
	  parser->print_db_value = NULL;

	  p = parser_print_tree (parser, conj);

	  parser->custom_print = save_custom;
	  parser->print_db_value = saved_func;
	}
      else
	{
	  p = buf;
	  buf[0] = '\0';
	}
    }

  /* restore link */
  if (conj)
    {
      conj->next = saved_next;
    }

  return p;
}



int
qo_walk_plan_tree (QO_PLAN * plan, QO_WALK_FUNCTION f, void *arg)
{
  int ret = NO_ERROR;

  if (!plan)
    {
      return NO_ERROR;
    }

  switch (plan->plan_type)
    {
    case QO_PLANTYPE_SCAN:
      return f (plan, arg);

    case QO_PLANTYPE_FOLLOW:
      return qo_walk_plan_tree (plan->plan_un.follow.head, f, arg);

    case QO_PLANTYPE_SORT:
      return qo_walk_plan_tree (plan->plan_un.sort.subplan, f, arg);

    case QO_PLANTYPE_JOIN:
      ret = qo_walk_plan_tree (plan->plan_un.join.outer, f, arg);
      if (ret != NO_ERROR)
	{
	  return ret;
	}

      return qo_walk_plan_tree (plan->plan_un.join.inner, f, arg);

    default:
      return ER_FAILED;
    }
}

void
qo_set_use_desc (QO_PLAN * plan)
{
  switch (plan->plan_type)
    {
    case QO_PLANTYPE_SCAN:
      if (((qo_is_iscan (plan) || qo_is_iscan_from_groupby (plan))
	   && plan->plan_un.scan.index->head->groupby_skip == true)
	  || ((qo_is_iscan (plan) || qo_is_iscan_from_orderby (plan))
	      && plan->plan_un.scan.index->head->orderby_skip == true))
	{
	  plan->plan_un.scan.index->head->use_descending = true;
	}
      break;

    case QO_PLANTYPE_SORT:
      qo_set_use_desc (plan->plan_un.sort.subplan);
      break;

    case QO_PLANTYPE_JOIN:
      qo_set_use_desc (plan->plan_un.join.outer);
      break;

    case QO_PLANTYPE_FOLLOW:
      qo_set_use_desc (plan->plan_un.follow.head);
      break;

    default:
      break;
    }
}

int
qo_unset_multi_range_optimization (QO_PLAN * plan, void *arg)
{
  if (plan->multi_range_opt_use == PLAN_MULTI_RANGE_OPT_NO)
    {
      /* nothing to do */
      return NO_ERROR;
    }

  /* set multi_range_opt to false */
  plan->multi_range_opt_use = PLAN_MULTI_RANGE_OPT_NO;

  if (qo_is_index_mro_scan (plan))
    {
      QO_INDEX_ENTRY *index_entryp;

      index_entryp = plan->plan_un.scan.index->head;

      /* multi_range_opt may have set the descending order on index */
      if (index_entryp->use_descending)
	{
	  /* if descending order is hinted or if skip order by / skip group by are true, leave the index as descending */
	  if (((plan->info->env->pt_tree->info.query.q.select.hint & PT_HINT_USE_IDX_DESC) == 0)
	      && !index_entryp->groupby_skip && !index_entryp->orderby_skip)
	    {
	      /* set use_descending to false */
	      index_entryp->use_descending = false;
	    }
	}
    }

  return NO_ERROR;
}

int
qo_set_orderby_skip (QO_PLAN * plan, void *arg)
{
  if (qo_is_iscan (plan) || qo_is_iscan_from_orderby (plan))
    {
      bool yn = *((bool *) arg);
      plan->plan_un.scan.index->head->orderby_skip = yn;
      plan->skip_orderby_opt = (yn) ? QO_PLAN_SKIP_ORDERBY_USE : plan->skip_orderby_opt;
    }

  return NO_ERROR;
}

int
qo_unset_hint_use_desc_idx (QO_PLAN * plan, void *arg)
{
  if (qo_is_interesting_order_scan (plan))
    {
      if (plan->plan_un.scan.index && plan->plan_un.scan.index->head)
	{
	  if (plan->plan_un.scan.index->head->use_descending)
	    {
	      /* We no longer need to set the USE_DESC_IDX hint if the planner wants a descending index, because the
	       * requirement is copied to each scan_ptr's index info at XASL generation.
	       * plan->info->env->pt_tree->info.query.q.select.hint |= PT_HINT_USE_IDX_DESC;
	       */
	    }
	  else if (plan->plan_un.scan.index->head->orderby_skip || qo_is_index_mro_scan (plan))
	    {
	      if (plan->info->env != NULL)
		{
		  plan->info->env->pt_tree->info.query.q.select.hint &= ~PT_HINT_USE_IDX_DESC;
		}
	    }
	}
    }

  return NO_ERROR;
}

int
qo_validate_indexes_for_orderby (QO_PLAN * plan, void *arg)
{
  if (qo_is_iscan_from_orderby (plan))
    {
      if (!qo_validate_index_for_orderby (plan->info->env, plan->plan_un.scan.index))
	{
	  return ER_FAILED;
	}
    }

  return NO_ERROR;
}

QO_PLAN *
qo_top_plan_new (QO_PLAN * plan)
{
  QO_ENV *env;
  PT_NODE *tree, *group_by, *order_by, *orderby_for;
  bool ordbynum_flag = false;
  PT_MISC_TYPE all_distinct;
  PARSER_CONTEXT *parser;

  if (plan == NULL || plan->top_rooted)
    {
      return plan;		/* is already top-level plan - OK */
    }

  if (plan->info == NULL	/* worst plan */
      || (env = (plan->info)->env) == NULL || bitset_cardinality (&((plan->info)->nodes)) < env->Nnodes
      || /* sub-plan */ (tree = QO_ENV_PT_TREE (env)) == NULL
      || (parser = QO_ENV_PARSER (env)) == NULL)
    {
      return plan;		/* do nothing */
    }

  QO_ASSERT (env, tree->node_type == PT_SELECT);

  plan->top_rooted = true;	/* mark as top-level plan */

  if (pt_is_single_tuple (QO_ENV_PARSER (env), tree))
    {				/* one tuple plan */
      return plan;		/* do nothing */
    }

  if (qo_plan_multi_range_opt (plan))
    {
      /* already found out that multi range optimization can be applied on current plan, skip any other checks */
      return plan;
    }

  all_distinct = tree->info.query.all_distinct;
  group_by = tree->info.query.q.select.group_by;
  order_by = tree->info.query.order_by;
  orderby_for = tree->info.query.orderby_for;

  if (order_by)
    {
      (void) parser_walk_leaves (parser, tree, pt_check_orderbynum_pre, NULL, pt_check_orderbynum_post, &ordbynum_flag);
    }

  if (group_by || (all_distinct == PT_DISTINCT || order_by))
    {

      bool groupby_skip, orderby_skip, is_index_w_prefix;
      bool found_instnum;
      int t;

      groupby_skip = orderby_skip = false;	/* init */

      found_instnum = false;
      for (t = 0; t < (signed) plan->info->planner->T; t++)
	{
	  if (QO_TERM_CLASS (&plan->info->planner->term[t]) == QO_TC_TOTALLY_AFTER_JOIN)
	    {
	      found_instnum = true;
	      break;
	    }
	}

      plan->iscan_sort_list = qo_plan_compute_iscan_sort_list (plan, NULL, &is_index_w_prefix, false);

      /* GROUP BY */
      /* if we have rollup, we do not skip the group by */
      if (group_by && !group_by->flag.with_rollup)
	{
	  PT_NODE *group_sort_list = NULL;

	  group_sort_list = qo_plan_compute_iscan_sort_list (plan, group_by, &is_index_w_prefix, false);

	  if (group_sort_list)
	    {
	      if (found_instnum /* && found_grpynum */ )
		{
		  ;		/* give up */
		}
	      else
		{
		  groupby_skip = pt_sort_spec_cover_groupby (parser, group_sort_list, group_by, tree);

		  /* if index plan and can't skip group by, we search that maybe a descending scan can be used. */
		  if (qo_is_interesting_order_scan (plan) && !groupby_skip)
		    {
		      groupby_skip = qo_check_groupby_skip_descending (plan, group_sort_list);

		      if (groupby_skip)
			{
			  plan->use_iscan_descending = true;
			}
		    }
		}

	      parser_free_node (parser, group_sort_list);
	    }

	  if (groupby_skip)
	    {
	      /* if the plan is index_groupby, we validate the plan */
	      if (qo_is_iscan_from_groupby (plan) || qo_is_iscan_from_orderby (plan))
		{
		  if (!qo_validate_index_for_groupby (plan->info->env, plan->plan_un.scan.index))
		    {
		      /* drop the plan if it wasn't validated */
		      qo_worst_cost (plan);
		      return plan;
		    }
		}
	      /* if all goes well, we have an indexed plan with group by skip! */
	      if (plan->plan_type == QO_PLANTYPE_SCAN && plan->plan_un.scan.index)
		{
		  plan->plan_un.scan.index->head->groupby_skip = true;
		}
	    }
	  else
	    {
	      if (plan->iscan_sort_list)
		{
		  parser_free_tree (parser, plan->iscan_sort_list);
		  plan->iscan_sort_list = NULL;
		}

	      /* if the order by is not skipped we drop the plan because it didn't helped us */
	      if (qo_is_iscan_from_groupby (plan) || qo_is_iscan_from_orderby (plan))
		{
		  qo_worst_cost (plan);
		  return plan;
		}

	      plan = qo_sort_new (plan, QO_UNORDERED, SORT_GROUPBY);
	      assert (plan->iscan_sort_list == NULL);
	    }
	}

      /* DISTINCT, ORDER BY */
      if (all_distinct == PT_DISTINCT || order_by)
	{
	  if (plan->iscan_sort_list)
	    {			/* need to check */
	      if (all_distinct == PT_DISTINCT)
		{
		  ;		/* give up */
		}
	      else
		{		/* non distinct */
		  if (group_by)
		    {
		      /* we already removed covered ORDER BY in reduce_order_by(). so is not covered ordering */
		      ;		/* give up; DO NOT DELETE ME - need future work */
		    }
		  else
		    {		/* non group_by */
		      if (found_instnum && (orderby_for || ordbynum_flag))
			{
			  /* at here, we can not merge orderby_num pred with inst_num pred */
			  ;	/* give up; DO NOT DELETE ME - need future work */
			}
		      else if (!is_index_w_prefix && !tree->info.query.q.select.connect_by
			       && !pt_has_analytic (parser, tree))
			{
			  orderby_skip = pt_sort_spec_cover (plan->iscan_sort_list, order_by);

			  /* try using a reverse scan */
			  if (!orderby_skip)
			    {
			      orderby_skip = qo_check_orderby_skip_descending (plan);

			      if (orderby_skip)
				{
				  plan->use_iscan_descending = true;
				}
			    }
			}
		    }
		}
	    }

	  if (orderby_skip)
	    {
	      if (qo_is_iscan_from_groupby (plan))
		{
		  /* group by skipping plan and we have order by skip -> drop */
		  qo_worst_cost (plan);
		  return plan;
		}

	      if (orderby_for)
		{		/* apply inst_num filter */
		  ;		/* DO NOT DELETE ME - need future work */
		}

	      /* validate the index orderby plan or subplans */
	      if (qo_walk_plan_tree (plan, qo_validate_indexes_for_orderby, NULL) != NO_ERROR)
		{
		  /* drop the plan if it wasn't validated */
		  qo_worst_cost (plan);
		  return plan;
		}

	      /* if all goes well, we have an indexed plan with order by skip: set the flag to all suitable subplans */
	      {
		bool yn = true;
		qo_walk_plan_tree (plan, qo_set_orderby_skip, &yn);
	      }
	    }
	  else
	    {
	      if (!groupby_skip && plan->iscan_sort_list)
		{
		  parser_free_tree (parser, plan->iscan_sort_list);
		  plan->iscan_sort_list = NULL;
		}

	      /* if the order by is not skipped we drop the plan because it didn't helped us */
	      if (qo_is_iscan_from_orderby (plan))
		{
		  qo_worst_cost (plan);
		  return plan;
		}

	      plan = qo_sort_new (plan, QO_UNORDERED, all_distinct == PT_DISTINCT ? SORT_DISTINCT : SORT_ORDERBY);
	    }
	}
    }

  return plan;
}

void
qo_generic_walk (QO_PLAN * plan, void (*child_fn) (QO_PLAN *, void *), void *child_data,
		 void (*parent_fn) (QO_PLAN *, void *), void *parent_data)
{
  if (parent_fn)
    {
      (*parent_fn) (plan, parent_data);
    }
}

void
qo_plan_print_sort_spec_helper (PT_NODE * list, bool is_iscan_asc, FILE * f, int howfar)
{
  const char *prefix;
  bool is_sort_spec_asc = true;

  if (list == NULL)
    {
      return;
    }

  fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "sort:  ");

  prefix = "";
  for (; list; list = list->next)
    {
      if (list->info.sort_spec.pos_descr.pos_no < 1)
	{			/* useless from here */
	  break;
	}
      fputs (prefix, f);

      if (list->info.sort_spec.asc_or_desc == PT_ASC)
	{
	  is_sort_spec_asc = true;
	}
      else
	{
	  is_sort_spec_asc = false;
	}

      fprintf (f, "%d %s", list->info.sort_spec.pos_descr.pos_no, (is_sort_spec_asc == is_iscan_asc) ? "asc" : "desc");

      if (TP_TYPE_HAS_COLLATION (TP_DOMAIN_TYPE (list->info.sort_spec.pos_descr.dom))
	  && TP_DOMAIN_COLLATION (list->info.sort_spec.pos_descr.dom) != LANG_SYS_COLLATION
	  && TP_DOMAIN_COLLATION_FLAG (list->info.sort_spec.pos_descr.dom) != TP_DOMAIN_COLL_LEAVE)
	{
	  fprintf (f, " collate %s",
		   lang_get_collation_name (TP_DOMAIN_COLLATION (list->info.sort_spec.pos_descr.dom)));
	}
      prefix = ", ";
    }
}

void
qo_plan_print_sort_spec (QO_PLAN * plan, FILE * f, int howfar)
{
  bool is_iscan_asc = true;

  if (plan->top_rooted != true)
    {				/* check for top level plan */
      return;
    }

  is_iscan_asc = plan->use_iscan_descending ? false : true;

  qo_plan_print_sort_spec_helper (plan->iscan_sort_list, is_iscan_asc, f, howfar);

  if (plan->plan_type == QO_PLANTYPE_SORT)
    {
      QO_ENV *env;
      PT_NODE *tree;

      env = (plan->info)->env;
      if (env == NULL)
	{
	  assert (false);
	  return;		/* give up */
	}
      tree = QO_ENV_PT_TREE (env);
      if (tree == NULL)
	{
	  assert (false);
	  return;		/* give up */
	}

      if (plan->plan_un.sort.sort_type == SORT_GROUPBY && tree->node_type == PT_SELECT)
	{
	  qo_plan_print_sort_spec_helper (tree->info.query.q.select.group_by, true, f, howfar);
	}

      if ((plan->plan_un.sort.sort_type == SORT_DISTINCT || plan->plan_un.sort.sort_type == SORT_ORDERBY)
	  && PT_IS_QUERY (tree))
	{
	  qo_plan_print_sort_spec_helper (tree->info.query.order_by, true, f, howfar);
	}
    }
}

void
qo_plan_print_costs (QO_PLAN * plan, FILE * f, int howfar)
{
  double fixed = plan->fixed_cpu_cost + plan->fixed_io_cost;
  double variable = plan->variable_cpu_cost + plan->variable_io_cost;
  double card = (plan->plan_type == QO_PLANTYPE_JOIN && QO_IS_NL_JOIN (plan) && plan->limit_nljoin_guessed_card > 0)
    ? plan->limit_nljoin_guessed_card : (plan->info)->cardinality;

  fprintf (f, "\n" INDENTED_TITLE_FMT "%.0f card %.0f", (int) howfar, ' ', "cost:", fixed + variable, card);

#if TEST_DUMP_PLAN_SCAN_COST
  fprintf (f, "\n" INDENTED_TITLE_FMT "%.0f expected %.0f scan %.0f total %.0f group %.0f hit_prob %.5f", (int) howfar,
	   ' ', "cost:", fixed + variable, (plan->info)->cardinality, (plan->info)->scan_rows, (plan->info)->total_rows,
	   (plan->info)->group_rows, (plan->info)->hit_prob);
#endif /* TEST_DUMP_PLAN_SCAN_COST */
}

void
qo_plan_print_projected_segs (QO_PLAN * plan, FILE * f, int howfar)
{
  int sx;
  const char *prefix = "";
  BITSET_ITERATOR si;

  if (!((plan->info)->env->dump_enable))
    {
      return;
    }

  fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "segs:");
  for (sx = bitset_iterate (&((plan->info)->projected_segs), &si); sx != -1; sx = bitset_next_member (&si))
    {
      fputs (prefix, f);
      qo_seg_fprint (&(plan->info)->env->segs[sx], f);
      prefix = ", ";
    }
}

void
qo_plan_print_sarged_terms (QO_PLAN * plan, FILE * f, int howfar)
{
  if (!bitset_is_empty (&(plan->sarged_terms)))
    {
      fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "sargs:");
      qo_termset_fprint ((plan->info)->env, &plan->sarged_terms, f);
    }
}

void
qo_plan_print_outer_join_terms (QO_PLAN * plan, FILE * f, int howfar)
{
  if (!bitset_is_empty (&(plan->plan_un.join.during_join_terms)))
    {
      fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "during:");
      qo_termset_fprint ((plan->info)->env, &(plan->plan_un.join.during_join_terms), f);
    }
  if (!bitset_is_empty (&(plan->plan_un.join.after_join_terms)))
    {
      fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "after:");
      qo_termset_fprint ((plan->info)->env, &(plan->plan_un.join.after_join_terms), f);
    }
}

void
qo_plan_print_subqueries (QO_PLAN * plan, FILE * f, int howfar)
{
  if (!bitset_is_empty (&(plan->subqueries)))
    {
      fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "subqs: ");
      bitset_print (&(plan->subqueries), f);
    }
}

void
qo_plan_print_analytic_eval (QO_PLAN * plan, FILE * f, int howfar)
{
  ANALYTIC_EVAL_TYPE *eval;
  ANALYTIC_TYPE *func;
  SORT_LIST *sort;
  int i, j, k;
  char buf[32];

  if (plan->analytic_eval_list != NULL)
    {
      fprintf (f, "\n\nAnalytic functions:");

      /* list functions */
      for (i = 0, k = 0, eval = plan->analytic_eval_list; eval != NULL; eval = eval->next, k++)
	{
	  /* run info */
	  sprintf (buf, "run[%d]: ", k);
	  fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', buf);
	  fprintf (f, "sort with key (");

	  /* eval sort list */
	  for (sort = eval->sort_list; sort != NULL; sort = sort->next)
	    {
	      fprintf (f, SORT_SPEC_FMT (sort));
	      if (sort->next != NULL)
		{
		  fputs (", ", f);
		}
	    }
	  fputs (")", f);

	  for (func = eval->head; func != NULL; func = func->next, i++)
	    {
	      /* func info */
	      fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "");
	      fprintf (f, "func[%d]: ", i);
	      fputs (fcode_get_lowercase_name (func->function), f);

	      /* func partition by */
	      fputs (" partition by (", f);
	      for (sort = eval->sort_list, j = func->sort_prefix_size; sort != NULL && j > 0; sort = sort->next, j--)
		{
		  fprintf (f, SORT_SPEC_FMT (sort));
		  if (sort->next != NULL && j != 1)
		    {
		      fputs (", ", f);
		    }
		}

	      /* func order by */
	      fputs (") order by (", f);
	      for (j = func->sort_list_size - func->sort_prefix_size; sort != NULL && j > 0; sort = sort->next, j--)
		{
		  fprintf (f, SORT_SPEC_FMT (sort));
		  if (sort->next != NULL && j != 1)
		    {
		      fputs (", ", f);
		    }
		}
	      fputs (")", f);
	    }
	}
    }
}

QO_PLAN *
qo_scan_new (QO_INFO * info, QO_NODE * node, QO_SCANMETHOD scan_method)
{
  QO_PLAN *plan;

  plan = qo_plan_malloc (info->env);
  if (plan == NULL)
    {
      return NULL;
    }

  plan->info = info;
  plan->refcount = 0;
  plan->top_rooted = false;
  plan->well_rooted = true;
  plan->iscan_sort_list = NULL;
  plan->analytic_eval_list = NULL;
  plan->plan_type = QO_PLANTYPE_SCAN;
  plan->order = QO_UNORDERED;

  plan->plan_un.scan.scan_method = scan_method;
  plan->plan_un.scan.node = node;

  bitset_assign (&(plan->sarged_terms), &(QO_NODE_SARGS (node)));

  bitset_assign (&(plan->subqueries), &(QO_NODE_SUBQUERIES (node)));
  bitset_init (&(plan->plan_un.scan.terms), info->env);
  bitset_init (&(plan->plan_un.scan.kf_terms), info->env);
  bitset_init (&(plan->plan_un.scan.hash_terms), info->env);
  plan->plan_un.scan.index_equi = false;
  plan->plan_un.scan.index_cover = false;
  plan->plan_un.scan.index_iss = false;
  plan->plan_un.scan.index_loose = false;
  plan->plan_un.scan.index = NULL;

  plan->multi_range_opt_use = PLAN_MULTI_RANGE_OPT_NO;
  bitset_init (&(plan->plan_un.scan.multi_col_range_segs), info->env);

  return plan;
}

void
qo_scan_free (QO_PLAN * plan)
{
  bitset_delset (&(plan->plan_un.scan.terms));
  bitset_delset (&(plan->plan_un.scan.kf_terms));
  bitset_delset (&(plan->plan_un.scan.hash_terms));
  bitset_delset (&(plan->plan_un.scan.multi_col_range_segs));
}

QO_PLAN *
qo_seq_scan_new (QO_INFO * info, QO_NODE * node)
{
  QO_PLAN *plan;

  plan = qo_scan_new (info, node, QO_SCANMETHOD_SEQ_SCAN);
  if (plan == NULL)
    {
      return NULL;
    }

  plan->vtbl = &qo_seq_scan_plan_vtbl;

  assert (bitset_is_empty (&(plan->plan_un.scan.terms)));
  assert (bitset_is_empty (&(plan->plan_un.scan.kf_terms)));
  assert (plan->plan_un.scan.index_equi == false);
  assert (plan->plan_un.scan.index_cover == false);
  assert (plan->plan_un.scan.index_iss == false);
  assert (plan->plan_un.scan.index_loose == false);
  assert (plan->plan_un.scan.index == NULL);

  qo_plan_compute_cost (plan);

  plan = qo_top_plan_new (plan);

  return plan;
}

bool
qo_index_has_bit_attr (QO_INDEX_ENTRY * index_entryp)
{
  TP_DOMAIN *domain;
  int col_num = index_entryp->col_num;
  int j;

  for (j = 0; j < col_num; j++)
    {
      domain = index_entryp->constraints->attributes[j]->domain;
      if (TP_DOMAIN_TYPE (domain) == DB_TYPE_BIT || TP_DOMAIN_TYPE (domain) == DB_TYPE_VARBIT)
	{
	  return true;
	}
    }

  return false;
}

QO_PLAN *
qo_index_scan_new (QO_INFO * info, QO_NODE * node, QO_NODE_INDEX_ENTRY * ni_entry, QO_SCANMETHOD scan_method,
		   BITSET * range_terms, BITSET * indexable_terms)
{
  QO_PLAN *plan = NULL;
  BITSET_ITERATOR iter;
  int t = -1;
  QO_ENV *env = info->env;
  QO_INDEX_ENTRY *index_entryp = NULL;
  QO_TERM *term = NULL;
  BITSET index_segs;
  BITSET term_segs;
  BITSET remaining_terms;
  int first_seg;
  bool first_col_present = false;

  assert (ni_entry != NULL);
  assert (ni_entry->head != NULL);

  assert (scan_method == QO_SCANMETHOD_INDEX_SCAN || scan_method == QO_SCANMETHOD_INDEX_ORDERBY_SCAN
	  || scan_method == QO_SCANMETHOD_INDEX_GROUPBY_SCAN || scan_method == QO_SCANMETHOD_INDEX_SCAN_INSPECT);

  assert (scan_method != QO_SCANMETHOD_INDEX_SCAN || !(ni_entry->head->force < 0));
  assert (scan_method == QO_SCANMETHOD_INDEX_SCAN_INSPECT || range_terms != NULL);

  plan = qo_scan_new (info, node, scan_method);
  if (plan == NULL)
    {
      return NULL;
    }

  bitset_init (&index_segs, env);
  bitset_init (&term_segs, env);
  bitset_init (&remaining_terms, env);

  if (range_terms != NULL)
    {
      /* remove key-range terms from sarged terms */
      bitset_difference (&(plan->sarged_terms), range_terms);
    }

  /* remove key-range terms from remaining terms */
  if (indexable_terms != NULL)
    {
      bitset_assign (&remaining_terms, indexable_terms);
      bitset_difference (&remaining_terms, range_terms);
    }
  bitset_union (&remaining_terms, &(plan->sarged_terms));

  /*
   * This is, in essence, the selectivity of the index.  We
   * really need to do a better job of figuring out the cost of
   * an indexed scan.
   */
  plan->vtbl = &qo_index_scan_plan_vtbl;
  plan->plan_un.scan.index = ni_entry;

  index_entryp = (plan->plan_un.scan.index)->head;
  first_seg = index_entryp->seg_idxs[0];

  if (range_terms != NULL)
    {
      /* set key-range terms */
      bitset_assign (&(plan->plan_un.scan.terms), range_terms);
      bitset_assign (&(plan->plan_un.scan.multi_col_range_segs), &(index_entryp->multi_col_range_segs));
      for (t = bitset_iterate (range_terms, &iter); t != -1; t = bitset_next_member (&iter))
	{
	  term = QO_ENV_TERM (env, t);

	  if (first_seg != -1 && BITSET_MEMBER (QO_TERM_SEGS (term), first_seg))
	    {
	      first_col_present = true;
	    }

	  if (!QO_TERM_IS_FLAGED (term, QO_TERM_EQUAL_OP))
	    {
	      break;
	    }
	}
    }

  if (!bitset_is_empty (&(plan->plan_un.scan.terms)) && t == -1)
    {
      /* is all equi-cond key-range terms */
      plan->plan_un.scan.index_equi = true;
    }
  else
    {
      plan->plan_un.scan.index_equi = false;
    }

  if (index_entryp->constraints->func_index_info && index_entryp->cover_segments == false)
    {
      /* do not permit key-filter */
      assert (bitset_is_empty (&(plan->plan_un.scan.kf_terms)));
    }
  else
    {
      /* all segments consisting in key columns */
      for (t = 0; t < index_entryp->nsegs; t++)
	{
	  if ((index_entryp->seg_idxs[t]) != -1)
	    {
	      bitset_add (&index_segs, (index_entryp->seg_idxs[t]));
	    }
	}

      for (t = bitset_iterate (&remaining_terms, &iter); t != -1; t = bitset_next_member (&iter))
	{
	  term = QO_ENV_TERM (env, t);

	  if (QO_TERM_IS_FLAGED (term, QO_TERM_NON_IDX_SARG_COLL))
	    {
	      /* term contains a collation that prevents us from using this term as a key range/filter */
	      continue;
	    }

	  if (!bitset_is_empty (&(QO_TERM_SUBQUERIES (term))))
	    {
	      continue;		/* term contains correlated subquery */
	    }

	  /* check for no key-range index scan */
	  if (bitset_is_empty (&(plan->plan_un.scan.terms)))
	    {
	      if (qo_is_filter_index (index_entryp) || qo_is_iscan_from_orderby (plan)
		  || qo_is_iscan_from_groupby (plan))
		{
		  /* filter index has a pre-defined key-range. ordery/groupby scan already checked nullable terms */
		  ;		/* go ahead */
		}
	      else
		{
		  /* do not permit non-indexable term as key-filter */
		  if (!term->can_use_index)
		    {
		      continue;
		    }
		}
	    }

	  bitset_assign (&term_segs, &(QO_TERM_SEGS (term)));
	  bitset_intersect (&term_segs, &(QO_NODE_SEGS (node)));

	  /* if the term is consisted by only the node's segments which appear in scan terms, it will be key-filter.
	   * otherwise will be data filter
	   */
	  if (!bitset_is_empty (&term_segs))
	    {
	      if (bitset_subset (&index_segs, &term_segs))
		{
		  bitset_add (&(plan->plan_un.scan.kf_terms), t);
		}
	    }
	}

      /* exclude key filter terms from sargs terms */
      bitset_difference (&(plan->sarged_terms), &(plan->plan_un.scan.kf_terms));
      bitset_difference (&remaining_terms, &(plan->plan_un.scan.kf_terms));
    }

  /* check for index cover scan */
  plan->plan_un.scan.index_cover = false;	/* init */
  if (index_entryp->cover_segments)
    {
      /* do not consider prefix index */
      if (qo_is_prefix_index (index_entryp) == false)
	{
	  for (t = bitset_iterate (&remaining_terms, &iter); t != -1; t = bitset_next_member (&iter))
	    {
	      term = QO_ENV_TERM (env, t);

	      if (!bitset_is_empty (&(QO_TERM_SUBQUERIES (term))))
		{
		  /* term contains correlated subquery */
		  continue;
		}

	      break;		/* found data-filter */
	    }

	  if (t == -1)
	    {
	      /* not found data-filter; mark as covering index scan */
	      plan->plan_un.scan.index_cover = true;
	    }
	}
    }

  assert (!bitset_intersects (&(plan->plan_un.scan.terms), &(plan->plan_un.scan.kf_terms)));

  assert (!bitset_intersects (&(plan->plan_un.scan.terms), &(plan->sarged_terms)));
  assert (!bitset_intersects (&(plan->plan_un.scan.kf_terms), &(plan->sarged_terms)));

  assert (!bitset_intersects (&(plan->plan_un.scan.terms), &remaining_terms));
  assert (!bitset_intersects (&(plan->plan_un.scan.kf_terms), &remaining_terms));

  bitset_delset (&remaining_terms);
  bitset_delset (&term_segs);
  bitset_delset (&index_segs);

  /* check for index skip scan */
  plan->plan_un.scan.index_iss = false;	/* init */
  if (index_entryp->is_iss_candidate)
    {
      assert (!bitset_is_empty (&(plan->plan_un.scan.terms)));
      assert (index_entryp->ils_prefix_len == 0);
      assert (!qo_is_filter_index (index_entryp));

      if (first_col_present == false)
	{
	  plan->plan_un.scan.index_iss = true;
	}
    }

  /* check for loose index scan */
  plan->plan_un.scan.index_loose = false;	/* init */
  if (index_entryp->ils_prefix_len > 0)
    {
      assert (plan->plan_un.scan.index_iss == false);

      /* do not consider prefix index */
      if (qo_is_prefix_index (index_entryp) == false)
	{
	  if (scan_method == QO_SCANMETHOD_INDEX_SCAN && qo_is_index_covering_scan (plan)
	      && !qo_index_has_bit_attr (index_entryp))
	    {
	      /* covering index, no key-range, no data-filter; mark as loose index scan */
	      plan->plan_un.scan.index_loose = true;
	    }

	  /* keep out not good index scan */
	  if (!qo_is_index_loose_scan (plan))
	    {
	      /* check for no key-range, no key-filter index scan */
	      if (qo_is_iscan (plan) && bitset_is_empty (&(plan->plan_un.scan.terms))
		  && bitset_is_empty (&(plan->plan_un.scan.kf_terms)))
		{
		  assert (!qo_is_iscan_from_groupby (plan));
		  assert (!qo_is_iscan_from_orderby (plan));

		  /* is not good index scan */
		  qo_plan_release (plan);
		  return NULL;
		}
	    }
	}
    }

  /* check for no key-range, no key-filter index scan */
  if (qo_is_iscan (plan) && bitset_is_empty (&(plan->plan_un.scan.terms))
      && bitset_is_empty (&(plan->plan_un.scan.kf_terms)) && scan_method != QO_SCANMETHOD_INDEX_SCAN_INSPECT)
    {
      assert (!qo_is_iscan_from_groupby (plan));
      assert (!qo_is_iscan_from_orderby (plan));

      /* check for filter-index, loose index scan */
      if (qo_is_filter_index (index_entryp) || qo_is_index_loose_scan (plan))
	{
	  /* filter index has a pre-defined key-range. */
	  assert (bitset_is_empty (&(plan->plan_un.scan.terms)));

	  ;			/* go ahead */
	}
      else
	{
	  assert (false);
	  qo_plan_release (plan);
	  return NULL;
	}
    }

  if (qo_check_iscan_for_multi_range_opt (plan))
    {
      bool dummy;

      plan->multi_range_opt_use = PLAN_MULTI_RANGE_OPT_USE;
      plan->iscan_sort_list = qo_plan_compute_iscan_sort_list (plan, NULL, &dummy, false);
    }

  assert (plan->plan_un.scan.index != NULL);

  qo_plan_compute_cost (plan);

  plan = qo_top_plan_new (plan);

  return plan;
}

void
qo_scan_fprint (QO_PLAN * plan, FILE * f, int howfar)
{
  bool natural_desc_index = false;

  if (plan->plan_un.scan.node->entity_spec->info.spec.cte_pointer)
    {
      PT_NODE *spec = plan->plan_un.scan.node->entity_spec;
      if (spec->info.spec.cte_pointer->info.pointer.node->info.cte.recursive_part)
	{
	  fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "recursive CTE: ");
	}
      else
	{
	  fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "simple CTE:");
	}
    }
  else
    {
      fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "class:");
    }

  qo_node_fprint (plan->plan_un.scan.node, f);

  if (qo_is_interesting_order_scan (plan))
    {
      fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "index: ");
      fprintf (f, "%s ", plan->plan_un.scan.index->head->constraints->name);

      /* print key limit */
      if (plan->plan_un.scan.index->head->key_limit)
	{
	  PT_NODE *key_limit = plan->plan_un.scan.index->head->key_limit;
	  PT_NODE *saved_next = key_limit->next;
	  PARSER_CONTEXT *parser = QO_ENV_PARSER (plan->info->env);
	  PT_PRINT_VALUE_FUNC saved_func = parser->print_db_value;

	  parser->print_db_value = pt_print_node_value;
	  if (saved_next)
	    {
	      saved_next->next = key_limit;
	      key_limit->next = NULL;
	    }
	  fprintf (f, "keylimit %s ", parser_print_tree_list (parser, saved_next ? saved_next : key_limit));
	  parser->print_db_value = saved_func;
	  if (saved_next)
	    {
	      key_limit->next = saved_next;
	      saved_next->next = NULL;
	    }
	}

      qo_termset_fprint ((plan->info)->env, &plan->plan_un.scan.terms, f);

      /* print index covering */
      if (qo_is_index_covering_scan (plan))
	{
	  if (bitset_cardinality (&(plan->plan_un.scan.terms)) > 0)
	    {
	      fprintf (f, " ");
	    }
	  fprintf (f, "(covers)");
	}

      if (qo_is_index_iss_scan (plan))
	{
	  fprintf (f, " (index skip scan)");
	}

      if (qo_is_index_loose_scan (plan))
	{
	  fprintf (f, " (loose index scan on prefix %d)", plan->plan_un.scan.index->head->ils_prefix_len);
	}

      if (qo_plan_multi_range_opt (plan))
	{
	  fprintf (f, " (multi_range_opt)");
	}

      if (plan->plan_un.scan.index && plan->plan_un.scan.index->head->use_descending)
	{
	  fprintf (f, " (desc_index)");
	  natural_desc_index = true;
	}

      if (!natural_desc_index && (QO_ENV_PT_TREE (plan->info->env)->info.query.q.select.hint & PT_HINT_USE_IDX_DESC))
	{
	  fprintf (f, " (desc_index forced)");
	}

      if (!bitset_is_empty (&(plan->plan_un.scan.kf_terms)))
	{
	  fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "filtr: ");
	  qo_termset_fprint ((plan->info)->env, &(plan->plan_un.scan.kf_terms), f);
	}
    }
}

void
qo_scan_info (QO_PLAN * plan, FILE * f, int howfar)
{
  QO_NODE *node = plan->plan_un.scan.node;
  int i, n = 1;
  const char *name;
  char buf[257] = { '\0', };

  fprintf (f, "\n%*c%s(", (int) howfar, ' ', (plan->vtbl)->info_string);
  if (QO_NODE_INFO (node))
    {
      for (i = 0, n = QO_NODE_INFO_N (node); i < n; i++)
	{
	  name = QO_NODE_INFO (node)->info[i].name;
	  fprintf (f, "%s ", (name ? name : "(anon)"));
	}
    }
  name = QO_NODE_NAME (node);
  if (n == 1)
    {
      fprintf (f, "%s", (name ? name : "(unknown)"));
    }
  else
    {
      fprintf (f, "as %s", (name ? name : "(unknown)"));
    }

  if (qo_is_iscan (plan) || qo_is_iscan_from_orderby (plan))
    {
      BITSET_ITERATOR bi;
      QO_ENV *env;
      int i;
      const char *separator;
      bool natural_desc_index = false;

      env = (plan->info)->env;
      separator = ", ";

      fprintf (f, "%s%s", separator, plan->plan_un.scan.index->head->constraints->name);

      /* print key limit */
      if (plan->plan_un.scan.index->head->key_limit)
	{
	  PT_NODE *key_limit = plan->plan_un.scan.index->head->key_limit;
	  PT_NODE *saved_next = key_limit->next;
	  PARSER_CONTEXT *parser = QO_ENV_PARSER (plan->info->env);
	  PT_PRINT_VALUE_FUNC saved_func = parser->print_db_value;
	  parser->print_db_value = pt_print_node_value;
	  if (saved_next)
	    {
	      saved_next->next = key_limit;
	      key_limit->next = NULL;
	    }
	  fprintf (f, "(keylimit %s) ", parser_print_tree_list (parser, saved_next ? saved_next : key_limit));
	  parser->print_db_value = saved_func;
	  if (saved_next)
	    {
	      key_limit->next = saved_next;
	      saved_next->next = NULL;
	    }
	}

      for (i = bitset_iterate (&(plan->plan_un.scan.terms), &bi); i != -1; i = bitset_next_member (&bi))
	{
	  fprintf (f, "%s%s", separator, qo_term_string (QO_ENV_TERM (env, i), buf));
	  separator = " and ";
	}
      if (bitset_cardinality (&(plan->plan_un.scan.kf_terms)) > 0)
	{
	  separator = ", [";
	  for (i = bitset_iterate (&(plan->plan_un.scan.kf_terms), &bi); i != -1; i = bitset_next_member (&bi))
	    {
	      fprintf (f, "%s%s", separator, qo_term_string (QO_ENV_TERM (env, i), buf));
	      separator = " and ";
	    }
	  fprintf (f, "]");
	}

      /* print index covering */
      if (qo_is_index_covering_scan (plan))
	{
	  fprintf (f, " (covers)");
	}

      if (qo_is_index_iss_scan (plan))
	{
	  fprintf (f, " (index skip scan)");
	}

      if (qo_is_index_loose_scan (plan))
	{
	  fprintf (f, " (loose index scan on prefix %d)", plan->plan_un.scan.index->head->ils_prefix_len);
	}

      if (qo_plan_multi_range_opt (plan))
	{
	  fprintf (f, " (multi_range_opt)");
	}

      if (plan->plan_un.scan.index && plan->plan_un.scan.index->head->use_descending)
	{
	  fprintf (f, " (desc_index)");
	  natural_desc_index = true;
	}

      if (!natural_desc_index && (QO_ENV_PT_TREE (plan->info->env)->info.query.q.select.hint & PT_HINT_USE_IDX_DESC))
	{
	  fprintf (f, " (desc_index forced)");
	}
    }

  fprintf (f, ")");
}

QO_PLAN *
qo_sort_new (QO_PLAN * root, QO_EQCLASS * order, SORT_TYPE sort_type)
{
  QO_PLAN *subplan, *plan;

  subplan = root;

  if (sort_type == SORT_TEMP)
    {				/* is not top-level plan */
      /* skip out top-level sort plan */
      for (; subplan && subplan->plan_type == QO_PLANTYPE_SORT && subplan->plan_un.sort.sort_type != SORT_LIMIT;
	   subplan = subplan->plan_un.sort.subplan)
	{
	  if (subplan->top_rooted && subplan->plan_un.sort.sort_type != SORT_TEMP)
	    {
	      ;			/* skip and go ahead */
	    }
	  else
	    {
	      break;		/* is not top-level sort plan */
	    }
	}

      /* check for dummy sort plan */
      if (order == QO_UNORDERED && subplan != NULL && subplan->plan_type == QO_PLANTYPE_SORT)
	{
	  return qo_plan_add_ref (root);
	}

      /* skip out empty sort plan */
      for (; subplan && subplan->plan_type == QO_PLANTYPE_SORT && subplan->plan_un.sort.sort_type != SORT_LIMIT;
	   subplan = subplan->plan_un.sort.subplan)
	{
	  if (!bitset_is_empty (&(subplan->sarged_terms)))
	    {
	      break;
	    }
	}
    }

  if (subplan == NULL)
    {
      return NULL;
    }

  plan = qo_plan_malloc ((subplan->info)->env);
  if (plan == NULL)
    {
      return NULL;
    }

  plan->info = subplan->info;
  plan->refcount = 0;
  plan->top_rooted = subplan->top_rooted;
  plan->well_rooted = false;
  plan->iscan_sort_list = NULL;
  plan->analytic_eval_list = NULL;
  plan->order = order;
  plan->plan_type = QO_PLANTYPE_SORT;
  plan->vtbl = &qo_sort_plan_vtbl;

  plan->plan_un.sort.sort_type = sort_type;
  plan->plan_un.sort.subplan = qo_plan_add_ref (subplan);
  plan->plan_un.sort.xasl = NULL;	/* To be determined later */

  plan->multi_range_opt_use = PLAN_MULTI_RANGE_OPT_NO;
  plan->has_sort_limit = (sort_type == SORT_LIMIT || subplan->has_sort_limit);
  plan->need_final_sort = subplan->need_final_sort;

  qo_plan_compute_cost (plan);
  if (sort_type == SORT_GROUPBY || sort_type == SORT_DISTINCT)
    {
      qo_estimate_ngroups (plan, sort_type);
    }

  plan = qo_top_plan_new (plan);

  return plan;
}

void
qo_sort_walk (QO_PLAN * plan, void (*child_fn) (QO_PLAN *, void *), void *child_data,
	      void (*parent_fn) (QO_PLAN *, void *), void *parent_data)
{
  if (child_fn)
    {
      (*child_fn) (plan->plan_un.sort.subplan, child_data);
    }
  if (parent_fn)
    {
      (*parent_fn) (plan, parent_data);
    }
}

void
qo_sort_fprint (QO_PLAN * plan, FILE * f, int howfar)
{
  switch (plan->plan_un.sort.sort_type)
    {
    case SORT_TEMP:
      fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "order:");
      qo_eqclass_fprint_wrt (plan->order, &(plan->info->nodes), f);
      break;

    case SORT_LIMIT:
      fprintf (f, "(sort limit)");
      break;

    case SORT_GROUPBY:
      fprintf (f, "(group by)");
      break;

    case SORT_ORDERBY:
      fprintf (f, "(order by)");
      break;

    case SORT_DISTINCT:
      fprintf (f, "(distinct)");
      break;

    default:
      break;
    }

  qo_plan_fprint (plan->plan_un.sort.subplan, f, howfar, "subplan: ");
}

void
qo_sort_info (QO_PLAN * plan, FILE * f, int howfar)
{

  switch (plan->plan_un.sort.sort_type)
    {
    case SORT_TEMP:
      if (plan->order != QO_UNORDERED)
	{
#if 0
	  /*
	   * Don't bother printing these out; they're almost always
	   * superfluous from the standpoint of a naive user trying to
	   * figure out what's going on.
	   */
	  fprintf (f, "\n%*c%s(", (int) howfar, ' ', (plan->vtbl)->info_string);
	  qo_eqclass_fprint_wrt (plan->order, &(plan->info->nodes), f);
	  fprintf (f, ")");
#endif
	}
      break;
    case SORT_LIMIT:
      fprintf (f, "\n%*c%s(%s)", (int) howfar, ' ', (plan->vtbl)->info_string, "sort limit");
      howfar += INDENT_INCR;
      break;
    case SORT_GROUPBY:
      fprintf (f, "\n%*c%s(%s)", (int) howfar, ' ', (plan->vtbl)->info_string, "group by");
      howfar += INDENT_INCR;
      break;

    case SORT_ORDERBY:
      fprintf (f, "\n%*c%s(%s)", (int) howfar, ' ', (plan->vtbl)->info_string, "order by");
      howfar += INDENT_INCR;
      break;

    case SORT_DISTINCT:
      fprintf (f, "\n%*c%s(%s)", (int) howfar, ' ', (plan->vtbl)->info_string, "distinct");
      howfar += INDENT_INCR;
      break;

    default:
      break;
    }

  qo_plan_lite_print (plan->plan_un.sort.subplan, f, howfar);
}

QO_PLAN *
qo_join_new (QO_INFO * info, JOIN_TYPE join_type, QO_JOINMETHOD join_method, QO_PLAN * outer, QO_PLAN * inner,
	     BITSET * join_terms, BITSET * duj_terms, BITSET * afj_terms, BITSET * sarged_terms,
	     BITSET * pinned_subqueries, BITSET * hash_terms)
{
  QO_PLAN *plan = NULL;
  QO_NODE *node = NULL;
  PT_NODE *spec = NULL;
  BITSET sarg_out_terms;

  bitset_init (&sarg_out_terms, info->env);

  if (inner->has_sort_limit && join_method != QO_JOINMETHOD_MERGE_JOIN)
    {
      /* SORT-LIMIT plans are allowed on inner nodes only for merge joins */
      return NULL;
    }

  plan = qo_plan_malloc (info->env);
  if (plan == NULL)
    {
      return NULL;
    }

  QO_ASSERT (info->env, outer != NULL);
  QO_ASSERT (info->env, inner != NULL);

  plan->info = info;
  plan->refcount = 0;
  plan->top_rooted = false;
  plan->well_rooted = false;
  plan->iscan_sort_list = NULL;
  plan->analytic_eval_list = NULL;
  plan->plan_type = QO_PLANTYPE_JOIN;
  plan->multi_range_opt_use = PLAN_MULTI_RANGE_OPT_NO;
  plan->has_sort_limit = (outer->has_sort_limit || inner->has_sort_limit);

  switch (join_method)
    {

    case QO_JOINMETHOD_NL_JOIN:
    case QO_JOINMETHOD_IDX_JOIN:
      if (join_method == QO_JOINMETHOD_NL_JOIN)
	{
	  plan->vtbl = &qo_nl_join_plan_vtbl;
	}
      else
	{
	  plan->vtbl = &qo_idx_join_plan_vtbl;
	}
      plan->order = QO_UNORDERED;

      /* These checks are necessary because of restrictions in the current XASL implementation of nested loop joins.
       * Never put anything on the inner plan that isn't file-based (i.e., a scan of either a heap file or a list
       * file).
       */
      if (!VALID_INNER (inner))
	{
	  inner = qo_sort_new (inner, inner->order, SORT_TEMP);
	}
      else if (IS_OUTER_JOIN_TYPE (join_type))
	{
	  /* for outer join, if inner plan is a scan of classes in hierarchy */
	  if (inner->plan_type == QO_PLANTYPE_SCAN && QO_NODE_IS_CLASS_HIERARCHY (inner->plan_un.scan.node))
	    {
	      inner = qo_sort_new (inner, inner->order, SORT_TEMP);
	    }
	}

      break;

    case QO_JOINMETHOD_MERGE_JOIN:

      plan->vtbl = &qo_merge_join_plan_vtbl;
#if 0
      /* Don't do this anymore; it relies on symmetry, which definitely doesn't apply anymore with the advent of outer
       * joins.
       */

      /* Arrange to always put the smallest cardinality on the outer term; this may lead to some savings given the
       * current merge join implementation.
       */
      if ((inner->info)->cardinality < (outer->info)->cardinality)
	{
	  QO_PLAN *tmp;
	  tmp = inner;
	  inner = outer;
	  outer = tmp;
	}
#endif

      /* The merge join result has the same nominal order as the two subjoins that feed it.  However, if it happens
       * that none of the segments in that order are to be projected from the result, the result is effectively
       * *unordered*.  Check for that condition here.
       */
      plan->order =
	bitset_intersects (&(QO_EQCLASS_SEGS (outer->order)),
			   &((plan->info)->projected_segs)) ? outer->order : QO_UNORDERED;

      /* The current implementation of merge joins always produces a list file These two checks are necessary because
       * of restrictions in the current XASL implementation of merge joins.
       */
      if (outer->plan_type != QO_PLANTYPE_SORT)
	{
	  outer = qo_sort_new (outer, outer->order, SORT_TEMP);
	}
      if (inner->plan_type != QO_PLANTYPE_SORT)
	{
	  inner = qo_sort_new (inner, inner->order, SORT_TEMP);
	}

      break;

    case QO_JOINMETHOD_HASH_JOIN:
      plan->vtbl = &qo_hash_join_plan_vtbl;
      plan->order = QO_UNORDERED;

      break;
    }

  assert (inner != NULL && outer != NULL);
  if (inner == NULL || outer == NULL)
    {
      return NULL;
    }

  node = QO_ENV_NODE (info->env, bitset_first_member (&((inner->info)->nodes)));

  assert (node != NULL);
  if (node == NULL)
    {
      return NULL;
    }

  /* check for cselect of method */
  spec = QO_NODE_ENTITY_SPEC (node);
  if (spec && spec->info.spec.flat_entity_list == NULL && spec->info.spec.derived_table_type == PT_IS_CSELECT)
    {
      /* mark as cselect join */
      plan->plan_un.join.join_type = JOIN_CSELECT;
    }
  else
    {
      plan->plan_un.join.join_type = join_type;
    }

  plan->plan_un.join.join_method = join_method;
  plan->plan_un.join.outer = qo_plan_add_ref (outer);
  plan->plan_un.join.inner = qo_plan_add_ref (inner);

  bitset_init (&(plan->plan_un.join.join_terms), info->env);
  bitset_init (&(plan->plan_un.join.during_join_terms), info->env);
  bitset_init (&(plan->plan_un.join.after_join_terms), info->env);
  bitset_init (&(plan->plan_un.join.hash_terms), info->env);

  /* set join terms */
  bitset_assign (&(plan->plan_un.join.join_terms), join_terms);
  /* set hash terms */
  bitset_assign (&(plan->plan_un.join.hash_terms), hash_terms);
  /* add to out terms */
  bitset_union (&sarg_out_terms, &(plan->plan_un.join.join_terms));

  if (IS_OUTER_JOIN_TYPE (join_type))
    {
      /* set during join terms */
      bitset_assign (&(plan->plan_un.join.during_join_terms), duj_terms);
      bitset_difference (&(plan->plan_un.join.during_join_terms), &sarg_out_terms);
      /* add to out terms */
      bitset_union (&sarg_out_terms, &(plan->plan_un.join.during_join_terms));

      /* set after join terms */
      bitset_assign (&(plan->plan_un.join.after_join_terms), afj_terms);
      bitset_difference (&(plan->plan_un.join.after_join_terms), &sarg_out_terms);
      /* add to out terms */
      bitset_union (&sarg_out_terms, &(plan->plan_un.join.after_join_terms));
    }

  /* set plan's sarged terms */
  bitset_assign (&(plan->sarged_terms), sarged_terms);
  bitset_difference (&(plan->sarged_terms), &sarg_out_terms);

  /* Make sure that the pinned subqueries and the sargs are placed on the same node: by now the pinned subqueries are
   * very likely pinned here precisely because they're used by these sargs. Separating them (so that they get evaluated
   * in some different order) will yield incorrect results.
   */
  bitset_assign (&(plan->subqueries), pinned_subqueries);

  plan->parallel_opt_use = qo_check_hjoin_for_parallel_opt (plan);

  if (qo_check_join_for_multi_range_opt (plan))
    {
      bool dummy;

      plan->multi_range_opt_use = PLAN_MULTI_RANGE_OPT_USE;
      plan->iscan_sort_list = qo_plan_compute_iscan_sort_list (plan, NULL, &dummy, false);
    }

  qo_plan_compute_cost (plan);

  if (QO_ENV_USE_SORT_LIMIT (info->env) && !plan->has_sort_limit
      && bitset_is_equivalent (&info->env->sort_limit_nodes, &info->nodes))
    {
      /* Consider creating a SORT_LIMIT plan over this plan only if it cannot skip order by. Since we know that we
       * already have all ORDER BY nodes in this plan, we can verify orderby_skip at this point
       */
      plan = qo_sort_new (plan, QO_UNORDERED, SORT_LIMIT);
      if (plan == NULL)
	{
	  return NULL;
	}
    }

#if 1				/* MERGE_ALWAYS_MAKES_LISTFILE */
  /* This is necessary to get the proper cost model for merge joins, which always build their result into a listfile
   * right now.  At the moment the cost model for a merge plan just models the cost of producing the result tuples, but
   * not storing them into a listfile. We could push the cost into the merge plan itself, I suppose, but a rational
   * implementation wouldn't impose this cost, and so I have hope that one day we'll be able to eliminate it.
   */
  if (join_method == QO_JOINMETHOD_MERGE_JOIN)
    {
      /* As noted in the comment on plan->order in qo_join_new,
       * the sort-merge join preserves the outer plan’s sort order but does not consider the sort direction.
       * It always sorts the join columns in S_ASC order (gen_outer).
       * Therefore, even if the ORDER BY sort column matches the sort column used for the sort-merge join,
       * a final sort may be required if the sort directions differ.
       */
      plan->need_final_sort = true;

      plan = qo_sort_new (plan, plan->order, SORT_TEMP);
    }
#endif /* MERGE_ALWAYS_MAKES_LISTFILE */

  if (join_method == QO_JOINMETHOD_HASH_JOIN)
    {
      plan->need_final_sort = true;
    }

  bitset_delset (&sarg_out_terms);

  plan = qo_top_plan_new (plan);

  return plan;
}

void
qo_join_free (QO_PLAN * plan)
{
  bitset_delset (&(plan->plan_un.join.join_terms));
  bitset_delset (&(plan->plan_un.join.during_join_terms));
  bitset_delset (&(plan->plan_un.join.after_join_terms));
}

void
qo_join_walk (QO_PLAN * plan, void (*child_fn) (QO_PLAN *, void *), void *child_data,
	      void (*parent_fn) (QO_PLAN *, void *), void *parent_data)
{
  if (child_fn)
    {
      (*child_fn) (plan->plan_un.join.outer, child_data);
      (*child_fn) (plan->plan_un.join.inner, child_data);
    }
  if (parent_fn)
    {
      (*parent_fn) (plan, parent_data);
    }
}

void
qo_join_fprint (QO_PLAN * plan, FILE * f, int howfar)
{
  switch (plan->plan_un.join.join_type)
    {
    case JOIN_INNER:
      if (!bitset_is_empty (&(plan->plan_un.join.join_terms)))
	{
	  fputs (" (inner join)", f);
	}
      else
	{
	  if (plan->plan_un.join.join_method == QO_JOINMETHOD_IDX_JOIN)
	    {
	      fputs (" (inner join)", f);
	    }
	  else
	    {
	      fputs (" (cross join)", f);
	    }
	}
      break;
    case JOIN_LEFT:
      fputs (" (left outer join)", f);
      break;
    case JOIN_RIGHT:
      fputs (" (right outer join)", f);
      break;
    case JOIN_OUTER:		/* not used */
      fputs (" (full outer join)", f);
      break;
    case JOIN_CSELECT:
      fputs (" (cselect join)", f);
      break;
    case NO_JOIN:
    default:
      fputs (" (unknown join type)", f);
      break;
    }
  if (!bitset_is_empty (&(plan->plan_un.join.join_terms)))
    {
      fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "edge:");
      qo_termset_fprint ((plan->info)->env, &(plan->plan_un.join.join_terms), f);
    }
  qo_plan_fprint (plan->plan_un.join.outer, f, howfar, "outer: ");
  qo_plan_fprint (plan->plan_un.join.inner, f, howfar, "inner: ");
  qo_plan_print_outer_join_terms (plan, f, howfar);
}

void
qo_join_info (QO_PLAN * plan, FILE * f, int howfar)
{
  if (!bitset_is_empty (&(plan->plan_un.join.join_terms)))
    {
      QO_ENV *env;
      const char *separator;
      int i;
      BITSET_ITERATOR bi;
      char buf[257] = { '\0', };

      env = (plan->info)->env;
      separator = "";

      fprintf (f, "\n%*c%s(", (int) howfar, ' ', (plan->vtbl)->info_string);
      for (i = bitset_iterate (&(plan->plan_un.join.join_terms), &bi); i != -1; i = bitset_next_member (&bi))
	{
	  fprintf (f, "%s%s", separator, qo_term_string (QO_ENV_TERM (env, i), buf));
	  separator = " and ";
	}
      fprintf (f, ")");
    }
  else
    {
      fprintf (f, "\n%*cNested loops", (int) howfar, ' ');
    }

  if (plan->plan_un.join.join_type == JOIN_LEFT)
    {
      fprintf (f, ": left outer");
    }
  else if (plan->plan_un.join.join_type == JOIN_RIGHT)
    {
      fprintf (f, ": right outer");
    }

  qo_plan_lite_print (plan->plan_un.join.outer, f, howfar + INDENT_INCR);
  qo_plan_lite_print (plan->plan_un.join.inner, f, howfar + INDENT_INCR);
}

void
qo_plan_fprint (QO_PLAN * plan, FILE * f, int howfar, const char *title)
{
  if (howfar < 0)
    {
      howfar = -howfar;
    }
  else
    {
      fputs ("\n", f);
      if (howfar)
	{
	  fprintf (f, INDENT_FMT, (int) howfar, ' ');
	}
    }

  if (title)
    {
      fprintf (f, TITLE_FMT, title);
    }

  fputs ((plan->vtbl)->plan_string, f);

  {
    int title_len;

    title_len = title ? (int) strlen (title) : 0;
    howfar += (title_len + INDENT_INCR);
  }

  (*((plan->vtbl)->fprint_fn)) (plan, f, howfar);

  qo_plan_print_projected_segs (plan, f, howfar);
  qo_plan_print_sarged_terms (plan, f, howfar);
  qo_plan_print_subqueries (plan, f, howfar);
  qo_plan_print_sort_spec (plan, f, howfar);
  qo_plan_print_costs (plan, f, howfar);
  qo_plan_print_analytic_eval (plan, f, howfar);
}

void
qo_plan_lite_print (QO_PLAN * plan, FILE * f, int howfar)
{
  (*((plan->vtbl)->info_fn)) (plan, f, howfar);
}

QO_PLAN *
qo_plan_finalize (QO_PLAN * plan)
{
  return qo_plan_add_ref (plan);
}

void
qo_plan_discard (QO_PLAN * plan)
{
  if (plan)
    {
      QO_ENV *env;
      bool dump_enable;

      /*
       * Be sure to capture dump_enable *before* we free the env
       * structure!
       */
      env = (plan->info)->env;
      dump_enable = env->dump_enable;

      qo_plan_del_ref (plan);
      qo_env_free (env);

      if (dump_enable)
	{
	  qo_print_stats (stdout);
	}
    }
}

void
qo_plan_walk (QO_PLAN * plan, void (*child_fn) (QO_PLAN *, void *), void *child_data,
	      void (*parent_fn) (QO_PLAN *, void *), void *parent_data)
{
  (*(plan->vtbl)->walk_fn) (plan, child_fn, child_data, parent_fn, parent_data);
}

void
qo_plan_del_ref_func (QO_PLAN * plan, void *ignore)
{
  qo_plan_del_ref (plan);	/* use the macro */
}

void
qo_plan_add_to_free_list (QO_PLAN * plan, void *ignore)
{
  bitset_delset (&(plan->sarged_terms));
  bitset_delset (&(plan->subqueries));
  if (plan->iscan_sort_list)
    {
      parser_free_tree (QO_ENV_PARSER ((plan->info)->env), plan->iscan_sort_list);
    }

  if (qo_accumulating_plans)
    {
      memset ((void *) plan, 0, sizeof (*plan));
      plan->plan_un.free.link = qo_plan_free_list;
      qo_plan_free_list = plan;
    }
  else
    {
      ++qo_plans_demalloced;
      free_and_init (plan);
    }
  ++qo_plans_deallocated;
}

void
qo_plan_free (QO_PLAN * plan)
{
  if (plan->refcount != 0)
    {
#if defined(CUBRID_DEBUG)
      fprintf (stderr, "*** optimizer problem: plan refcount = %d ***\n", plan->refcount);
#endif /* CUBRID_DEBUG */
    }
  else
    {
      if ((plan->vtbl)->free_fn)
	{
	  (*(plan->vtbl)->free_fn) (plan);
	}

      qo_plan_walk (plan, qo_plan_del_ref_func, NULL, qo_plan_add_to_free_list, NULL);
    }
}

void
qo_plans_init (QO_ENV * env)
{
  qo_plan_free_list = NULL;
  qo_plans_allocated = 0;
  qo_plans_deallocated = 0;
  qo_plans_malloced = 0;
  qo_plans_demalloced = 0;
  qo_accumulating_plans = false /* true */ ;
  qo_next_tmpfile = 0;
}

void
qo_plans_teardown (QO_ENV * env)
{
  while (qo_plan_free_list)
    {
      QO_PLAN *plan = qo_plan_free_list;

      qo_plan_free_list = plan->plan_un.free.link;
      if (plan)
	{
	  free_and_init (plan);
	}
      ++qo_plans_demalloced;
    }
  qo_accumulating_plans = false;
}

void
qo_plans_stats (FILE * f)
{
  fprintf (f, "%d/%d plans allocated/deallocated\n", qo_plans_allocated, qo_plans_deallocated);
  fprintf (f, "%d/%d plans malloced/demalloced\n", qo_plans_malloced, qo_plans_demalloced);
}

void
qo_plan_dump (QO_PLAN * plan, FILE * output)
{
  int level;

  if (output == NULL)
    {
      output = stdout;
    }

  if (plan == NULL)
    {
      fputs ("\nNo optimized plan!\n", output);
      return;
    }

  qo_get_optimization_param (&level, QO_PARAM_LEVEL);
  if (DETAILED_DUMP (level))
    {
      qo_plan_fprint (plan, output, 0, NULL);
    }
  else if (SIMPLE_DUMP (level))
    {
      qo_plan_lite_print (plan, output, 0);
    }

  fputs ("\n", output);
}

int
qo_plan_get_cost_fn (const char *plan_name)
{
  int n = DIM (all_vtbls);
  int i = 0;
  int cost = 'u';

  for (i = 0; i < n; i++)
    {
      if (intl_mbs_ncasecmp (plan_name, all_vtbls[i]->plan_string, strlen (all_vtbls[i]->plan_string)) == 0)
	{
	  if (all_vtbls[i]->cost_fn == &qo_zero_cost)
	    {
	      cost = '0';
	    }
	  else if (all_vtbls[i]->cost_fn == &qo_worst_cost)
	    {
	      cost = 'i';
	    }
	  else
	    {
	      cost = 'd';
	    }
	  break;
	}
    }

  return cost;
}

const char *
qo_plan_set_cost_fn (const char *plan_name, int fn)
{
  int n = DIM (all_vtbls);
  int i = 0;

  for (i = 0; i < n; i++)
    {
      if (intl_mbs_ncasecmp (plan_name, all_vtbls[i]->plan_string, strlen (all_vtbls[i]->plan_string)) == 0)
	{
	  switch (fn)
	    {
	    case 0:
	    case '0':
	    case 'b':		/* best */
	    case 'B':		/* BEST */
	    case 'z':		/* zero */
	    case 'Z':		/* ZERO */
	      all_vtbls[i]->cost_fn = &qo_zero_cost;
	      break;

	    case 1:
	    case '1':
	    case 'i':		/* infinite */
	    case 'I':		/* INFINITE */
	    case 'w':		/* worst */
	    case 'W':		/* WORST */
	      all_vtbls[i]->cost_fn = &qo_worst_cost;
	      break;

	    default:
	      all_vtbls[i]->cost_fn = all_vtbls[i]->default_cost;
	      break;
	    }
	  return all_vtbls[i]->plan_string;
	}
    }

  return NULL;

}				/* qo_plan_set_cost_fn */

QO_PLAN_PARALLEL_OPT_USE
qo_check_hjoin_for_parallel_opt (QO_PLAN * plan)
{
  PARSER_CONTEXT *parser = NULL;
  PT_NODE *tree = NULL, *expr = NULL;

  QO_ENV *env = NULL;
  QO_TERM *term = NULL;
  BITSET_ITERATOR bitset_iter;
  int bitset_index;

  bool is_method_call = false;

  if (plan == NULL || plan->info == NULL || plan->plan_type != QO_PLANTYPE_JOIN
      || plan->plan_un.join.join_method != QO_JOINMETHOD_HASH_JOIN)
    {
      return PLAN_PARALLEL_OPT_CANNOT_USE;
    }

  env = plan->info->env;
  if (env == NULL)
    {
      /* impossible case */
      assert (false);
      return PLAN_PARALLEL_OPT_CANNOT_USE;
    }

  parser = QO_ENV_PARSER (env);
  if (parser == NULL)
    {
      /* impossible case */
      assert (false);
      return PLAN_PARALLEL_OPT_CANNOT_USE;
    }

  tree = QO_ENV_PT_TREE (env);
  if (tree == NULL)
    {
      /* impossible case */
      assert (false);
      return PLAN_PARALLEL_OPT_CANNOT_USE;
    }

  if (!PT_IS_SELECT (tree))	// TODO: check merge, update, delete
    {
      /* impossible case */
      assert (false);
      return PLAN_PARALLEL_OPT_CANNOT_USE;
    }

  if (PT_SELECT_INFO_IS_FLAGED (tree, PT_SELECT_INFO_IS_MERGE_QUERY))
    {
      /* MERGE queries cannot use parallel execution because they use
       * xtran_server_start_topop() to ensure statement atomicity, as each row may
       * require both UPDATE and INSERT operations to be performed atomically.
       * xtran_server_start_topop() internally calls log_sysop_start().
       * log_sysop_start() uses a transaction-level mutex (rmutex_topop) that does not
       * support concurrent access from multiple worker threads sharing the same transaction. */
      return PLAN_PARALLEL_OPT_CANNOT_USE;
    }

  if (!bitset_is_empty (&plan->plan_un.join.during_join_terms))
    {
      for (bitset_index = bitset_iterate (&plan->plan_un.join.during_join_terms, &bitset_iter); bitset_index != -1;
	   bitset_index = bitset_next_member (&bitset_iter))
	{
	  term = QO_ENV_TERM (env, bitset_index);
	  if (term == NULL)
	    {
	      return PLAN_PARALLEL_OPT_CANNOT_USE;
	    }

	  expr = QO_TERM_PT_EXPR (term);
	  if (expr == NULL)
	    {
	      return PLAN_PARALLEL_OPT_CANNOT_USE;
	    }

	  (void) parser_walk_tree (parser, expr, pt_is_method_call_node, &is_method_call, NULL, NULL);

	  if (is_method_call)
	    {
	      return PLAN_PARALLEL_OPT_CANNOT_USE;
	    }
	}
    }

  if (tree->info.query.q.select.hint & PT_HINT_NO_PARALLEL_HASH_JOIN)
    {
      return PLAN_PARALLEL_OPT_NO;
    }

  if (tree->info.query.q.select.hint & PT_HINT_PARALLEL)
    {
      assert (tree->info.query.q.select.num_parallel_threads >= 0);

      if (tree->info.query.q.select.num_parallel_threads > 1)
	{
	  return PLAN_PARALLEL_OPT_USE;
	}
      else
	{
	  /* hint 0 or 1 disables parallel execution */
	  return PLAN_PARALLEL_OPT_NO;
	}
    }

  return PLAN_PARALLEL_OPT_CAN_USE;
}

void
qo_hjoin_fprint (QO_PLAN * plan, FILE * f, int howfar)
{
  switch (plan->plan_un.join.join_type)
    {
    case JOIN_INNER:
      fputs (" (inner join)", f);
      break;

    case JOIN_LEFT:
      fputs (" (left outer join)", f);
      break;

    case JOIN_RIGHT:
      fputs (" (right outer join)", f);
      break;

    case JOIN_OUTER:
      /* Unsupported. */
      assert (false);
      fputs (" (full outer join)", f);
      break;

    case JOIN_CSELECT:
      /* Unsupported. */
      assert (false);
      fputs (" (cselect join)", f);
      break;

    case NO_JOIN:
    default:
      /* Impossible. */
      assert (false);
      fputs (" (unknown join type)", f);
      break;
    }

  if (!bitset_is_empty (&(plan->plan_un.join.join_terms)))
    {
      fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "edge:");
      qo_termset_fprint ((plan->info)->env, &(plan->plan_un.join.join_terms), f);
    }

  qo_plan_fprint (plan->plan_un.join.outer, f, howfar, "outer: ");
  qo_plan_fprint (plan->plan_un.join.inner, f, howfar, "inner: ");
  qo_plan_print_outer_join_terms (plan, f, howfar);
}

QO_PLAN *
qo_follow_new (QO_INFO * info, QO_PLAN * head_plan, QO_TERM * path_term, BITSET * sarged_terms,
	       BITSET * pinned_subqueries)
{
  QO_PLAN *plan;

  plan = qo_plan_malloc (info->env);
  if (plan == NULL)
    {
      return NULL;
    }

  QO_ASSERT (info->env, head_plan != NULL);

  plan->info = info;
  plan->refcount = 0;
  plan->top_rooted = false;
  plan->well_rooted = head_plan->well_rooted;
  plan->iscan_sort_list = NULL;
  plan->analytic_eval_list = NULL;
  plan->plan_type = QO_PLANTYPE_FOLLOW;
  plan->vtbl = &qo_follow_plan_vtbl;
  plan->order = QO_UNORDERED;

  plan->plan_un.follow.head = qo_plan_add_ref (head_plan);
  plan->plan_un.follow.path = path_term;

  plan->multi_range_opt_use = PLAN_MULTI_RANGE_OPT_NO;

  bitset_assign (&(plan->sarged_terms), sarged_terms);
  bitset_remove (&(plan->sarged_terms), QO_TERM_IDX (path_term));

  bitset_assign (&(plan->subqueries), pinned_subqueries);

  bitset_union (&(plan->sarged_terms), &(QO_NODE_SARGS (QO_TERM_TAIL (path_term))));
  bitset_union (&(plan->subqueries), &(QO_NODE_SUBQUERIES (QO_TERM_TAIL (path_term))));

  qo_plan_compute_cost (plan);

  plan = qo_top_plan_new (plan);

  return plan;
}

void
qo_follow_walk (QO_PLAN * plan, void (*child_fn) (QO_PLAN *, void *), void *child_data,
		void (*parent_fn) (QO_PLAN *, void *), void *parent_data)
{
  if (child_fn)
    {
      (*child_fn) (plan->plan_un.follow.head, child_data);
    }
  if (parent_fn)
    {
      (*parent_fn) (plan, parent_data);
    }
}

void
qo_follow_fprint (QO_PLAN * plan, FILE * f, int howfar)
{
  fprintf (f, "\n" INDENTED_TITLE_FMT, (int) howfar, ' ', "edge:");
  qo_term_fprint (plan->plan_un.follow.path, f);
  qo_plan_fprint (plan->plan_un.follow.head, f, howfar, "head: ");
}

void
qo_follow_info (QO_PLAN * plan, FILE * f, int howfar)
{
  char buf[257] = { '\0', };
  fprintf (f, "\n%*c%s(%s)", (int) howfar, ' ', (plan->vtbl)->info_string,
	   qo_term_string (plan->plan_un.follow.path, buf));
  qo_plan_lite_print (plan->plan_un.follow.head, f, howfar + INDENT_INCR);
}

QO_PLAN *
qo_cp_new (QO_INFO * info, QO_PLAN * outer, QO_PLAN * inner, BITSET * sarged_terms, BITSET * pinned_subqueries)
{
  QO_PLAN *plan;
  BITSET empty_terms;

  bitset_init (&empty_terms, info->env);

  plan = qo_join_new (info, JOIN_INNER /* default */ ,
		      QO_JOINMETHOD_NL_JOIN, outer, inner, &empty_terms /* join_terms */ ,
		      &empty_terms /* duj_terms */ ,
		      &empty_terms /* afj_terms */ ,
		      sarged_terms, pinned_subqueries, &empty_terms /* hash_terms */ );

  bitset_delset (&empty_terms);

  return plan;
}

QO_PLAN *
qo_worst_new (QO_ENV * env)
{
  QO_PLAN *plan;

  plan = qo_plan_malloc (env);
  if (plan == NULL)
    {
      return NULL;
    }

  plan->info = NULL;
  plan->refcount = 0;
  plan->top_rooted = true;
  plan->well_rooted = false;
  plan->iscan_sort_list = NULL;
  plan->analytic_eval_list = NULL;
  plan->order = QO_UNORDERED;
  plan->plan_type = QO_PLANTYPE_WORST;
  plan->vtbl = &qo_worst_plan_vtbl;

  plan->multi_range_opt_use = PLAN_MULTI_RANGE_OPT_NO;

  qo_plan_compute_cost (plan);

  return plan;
}

void
qo_worst_fprint (QO_PLAN * plan, FILE * f, int howfar)
{
}

void
qo_worst_info (QO_PLAN * plan, FILE * f, int howfar)
{
  fprintf (f, "\n%*c%s", (int) howfar, ' ', (plan->vtbl)->info_string);
}
