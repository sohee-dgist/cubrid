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

json_t *
qo_plan_scan_print_json (QO_PLAN * plan)
{
  BITSET_ITERATOR bi;
  QO_ENV *env;
  bool natural_desc_index = false;
  json_t *scan, *range, *filter;
  const char *scan_string = "";
  const char *class_name;
  char buf[257] = { '\0', };
  int i;

  scan = json_object ();

  class_name = QO_NODE_NAME (plan->plan_un.scan.node);
  if (class_name == NULL)
    {
      class_name = "unknown";
    }

  json_object_set_new (scan, "table", json_string (class_name));

  switch (plan->plan_un.scan.scan_method)
    {
    case QO_SCANMETHOD_SEQ_SCAN:
      scan_string = "TABLE SCAN";
      break;

    case QO_SCANMETHOD_INDEX_SCAN:
    case QO_SCANMETHOD_INDEX_ORDERBY_SCAN:
    case QO_SCANMETHOD_INDEX_GROUPBY_SCAN:
    case QO_SCANMETHOD_INDEX_SCAN_INSPECT:
      scan_string = "INDEX SCAN";
      json_object_set_new (scan, "index", json_string (plan->plan_un.scan.index->head->constraints->name));

      env = (plan->info)->env;
      range = json_array ();

      for (i = bitset_iterate (&(plan->plan_un.scan.terms), &bi); i != -1; i = bitset_next_member (&bi))
	{
	  json_array_append_new (range, json_string (qo_term_string (QO_ENV_TERM (env, i), buf)));
	}

      json_object_set_new (scan, "key range", range);

      if (bitset_cardinality (&(plan->plan_un.scan.kf_terms)) > 0)
	{
	  filter = json_array ();
	  for (i = bitset_iterate (&(plan->plan_un.scan.kf_terms), &bi); i != -1; i = bitset_next_member (&bi))
	    {
	      json_array_append_new (filter, json_string (qo_term_string (QO_ENV_TERM (env, i), buf)));
	    }

	  json_object_set_new (scan, "key filter", filter);
	}

      if (qo_is_index_covering_scan (plan))
	{
	  json_object_set_new (scan, "covered", json_true ());
	}

      if (plan->plan_un.scan.index && plan->plan_un.scan.index->head->use_descending)
	{
	  json_object_set_new (scan, "desc_index", json_true ());
	  natural_desc_index = true;
	}

      if (!natural_desc_index && (QO_ENV_PT_TREE (plan->info->env)->info.query.q.select.hint & PT_HINT_USE_IDX_DESC))
	{
	  json_object_set_new (scan, "desc_index forced", json_true ());
	}

      if (qo_is_index_loose_scan (plan))
	{
	  json_object_set_new (scan, "loose", json_true ());
	}

      break;
    }

  return json_pack ("{s:o}", scan_string, scan);
}

json_t *
qo_plan_sort_print_json (QO_PLAN * plan)
{
  json_t *sort, *subplan = NULL;
  const char *type;

  switch (plan->plan_un.sort.sort_type)
    {
    case SORT_TEMP:
      type = "SORT (temp)";
      break;

    case SORT_GROUPBY:
      type = "SORT (group by)";
      break;

    case SORT_ORDERBY:
      type = "SORT (order by)";
      break;

    case SORT_DISTINCT:
      type = "SORT (distinct)";
      break;

    case SORT_LIMIT:
      type = "SORT (limit)";
      break;

    default:
      assert (false);
      type = "";
      break;
    }

  sort = json_object ();

  if (plan->plan_un.sort.subplan)
    {
      subplan = qo_plan_print_json (plan->plan_un.sort.subplan);
      json_object_set_new (sort, type, subplan);
    }
  else
    {
      json_object_set_new (sort, type, json_string (""));
    }

  return sort;
}

json_t *
qo_plan_join_print_json (QO_PLAN * plan)
{
  json_t *join, *outer, *inner;
  const char *type, *method = "";
  char buf[32];

  switch (plan->plan_un.join.join_method)
    {
    case QO_JOINMETHOD_NL_JOIN:
    case QO_JOINMETHOD_IDX_JOIN:
      method = "NESTED LOOPS";
      break;

    case QO_JOINMETHOD_MERGE_JOIN:
      method = "MERGE JOIN";
      break;

    case QO_JOINMETHOD_HASH_JOIN:
      method = "HASH JOIN";
      break;

    default:
      method = "UNKNOWN";
      break;
    }

  switch (plan->plan_un.join.join_type)
    {
    case JOIN_INNER:
      if (!bitset_is_empty (&(plan->plan_un.join.join_terms)))
	{
	  type = "inner join";
	}
      else
	{
	  if (plan->plan_un.join.join_method == QO_JOINMETHOD_IDX_JOIN)
	    {
	      type = "inner join";
	    }
	  else
	    {
	      type = "cross join";
	    }
	}
      break;
    case JOIN_LEFT:
      type = "left outer join";
      break;
    case JOIN_RIGHT:
      type = "right outer join";
      break;
    case JOIN_OUTER:		/* not used */
      type = "full outer join";
      break;
    case JOIN_CSELECT:
      type = "cselect";
      break;
    case NO_JOIN:
    default:
      type = "unknown";
      break;
    }

  outer = qo_plan_print_json (plan->plan_un.join.outer);
  inner = qo_plan_print_json (plan->plan_un.join.inner);

  sprintf (buf, "%s (%s)", method, type);

  join = json_pack ("{s:[o,o]}", buf, outer, inner);

  return join;
}

json_t *
qo_plan_follow_print_json (QO_PLAN * plan)
{
  json_t *head, *follow;
  char buf[257] = { '\0', };

  head = qo_plan_print_json (plan->plan_un.follow.head);

  follow = json_object ();
  json_object_set_new (follow, "edge", json_string (qo_term_string (plan->plan_un.follow.path, buf)));
  json_object_set_new (follow, "head", head);

  return json_pack ("{s:o}", "FOLLOW", follow);
}

json_t *
qo_plan_print_json (QO_PLAN * plan)
{
  json_t *json = NULL;

  switch (plan->plan_type)
    {
    case QO_PLANTYPE_SCAN:
      json = qo_plan_scan_print_json (plan);
      break;

    case QO_PLANTYPE_SORT:
      json = qo_plan_sort_print_json (plan);
      break;

    case QO_PLANTYPE_JOIN:
      json = qo_plan_join_print_json (plan);
      break;

    case QO_PLANTYPE_FOLLOW:
      json = qo_plan_follow_print_json (plan);
      break;

    default:
      break;
    }

  return json;
}

void
qo_top_plan_print_json (PARSER_CONTEXT * parser, xasl_node * xasl, PT_NODE * select, QO_PLAN * plan)
{
  json_t *json;
  unsigned int save_custom;

  assert (parser != NULL && xasl != NULL && plan != NULL && select != NULL);

  if (parser->num_plan_trace >= MAX_NUM_PLAN_TRACE)
    {
      return;
    }

  json = qo_plan_print_json (plan);

  if (select->info.query.order_by)
    {
      if (xasl && xasl->spec_list && xasl->spec_list->indexptr && xasl->spec_list->indexptr->orderby_skip)
	{
	  json_object_set_new (json, "skip order by", json_true ());
	}
    }

  if (select->info.query.q.select.group_by)
    {
      if (xasl && xasl->spec_list && xasl->spec_list->indexptr && xasl->spec_list->indexptr->groupby_skip)
	{
	  json_object_set_new (json, "group by nosort", json_true ());
	}
    }

  save_custom = parser->custom_print;
  parser->custom_print |= PT_CONVERT_RANGE;

  json_object_set_new (json, "rewritten query", json_string (parser_print_tree (parser, select)));

  parser->custom_print = save_custom;

  parser->plan_trace[parser->num_plan_trace].format = QUERY_TRACE_JSON;
  parser->plan_trace[parser->num_plan_trace].trace.json_plan = json;
  parser->num_plan_trace++;

  return;
}

void
qo_plan_scan_print_text (FILE * fp, QO_PLAN * plan, int indent)
{
  BITSET_ITERATOR bi;
  QO_ENV *env;
  bool natural_desc_index = false;
  const char *class_name;
  char buf[257] = { '\0', };
  int i;

  indent += 2;
  fprintf (fp, "%*c", indent, ' ');

  class_name = QO_NODE_NAME (plan->plan_un.scan.node);
  if (class_name == NULL)
    {
      class_name = "unknown";
    }

  switch (plan->plan_un.scan.scan_method)
    {
    case QO_SCANMETHOD_SEQ_SCAN:
      fprintf (fp, "TABLE SCAN (%s)", class_name);
      break;

    case QO_SCANMETHOD_INDEX_SCAN:
    case QO_SCANMETHOD_INDEX_ORDERBY_SCAN:
    case QO_SCANMETHOD_INDEX_GROUPBY_SCAN:
    case QO_SCANMETHOD_INDEX_SCAN_INSPECT:
      fprintf (fp, "INDEX SCAN (%s.%s)", class_name, plan->plan_un.scan.index->head->constraints->name);

      env = (plan->info)->env;
      fprintf (fp, " (");

      bool first = true;

      for (i = bitset_iterate (&(plan->plan_un.scan.terms), &bi); i != -1; i = bitset_next_member (&bi))
	{
	  fprintf (fp, "key range: %s", qo_term_string (QO_ENV_TERM (env, i), buf));
	  first = false;
	}

      if (bitset_cardinality (&(plan->plan_un.scan.kf_terms)) > 0)
	{
	  for (i = bitset_iterate (&(plan->plan_un.scan.kf_terms), &bi); i != -1; i = bitset_next_member (&bi))
	    {
	      fprintf (fp, "%skey filter: %s", first ? "" : ", ", qo_term_string (QO_ENV_TERM (env, i), buf));
	    }
	  first = false;
	}

      if (qo_is_index_covering_scan (plan))
	{
	  fprintf (fp, "%scovered: true", first ? "" : ", ");
	  first = false;
	}

      if (plan->plan_un.scan.index && plan->plan_un.scan.index->head->use_descending)
	{
	  fprintf (fp, "%sdesc_index: true", first ? "" : ", ");
	  natural_desc_index = true;
	  first = false;
	}

      if (!natural_desc_index && (QO_ENV_PT_TREE (plan->info->env)->info.query.q.select.hint & PT_HINT_USE_IDX_DESC))
	{
	  fprintf (fp, "%sdesc_index forced: true", first ? "" : ", ");
	  first = false;
	}

      if (qo_is_index_loose_scan (plan))
	{
	  fprintf (fp, "%sloose: true", first ? "" : ", ");
	  first = false;
	}

      fprintf (fp, ")");
      break;
    }

  fprintf (fp, "\n");
}

void
qo_plan_sort_print_text (FILE * fp, QO_PLAN * plan, int indent)
{
  const char *type;

  indent += 2;

  switch (plan->plan_un.sort.sort_type)
    {
    case SORT_TEMP:
      type = "SORT (temp)";
      break;

    case SORT_GROUPBY:
      type = "SORT (group by)";
      break;

    case SORT_ORDERBY:
      type = "SORT (order by)";
      break;

    case SORT_DISTINCT:
      type = "SORT (distinct)";
      break;

    case SORT_LIMIT:
      type = "SORT (limit)";
      break;

    default:
      assert (false);
      type = "";
      break;
    }

  fprintf (fp, "%*c%s\n", indent, ' ', type);

  if (plan->plan_un.sort.subplan)
    {
      qo_plan_print_text (fp, plan->plan_un.sort.subplan, indent);
    }
}

void
qo_plan_join_print_text (FILE * fp, QO_PLAN * plan, int indent)
{
  const char *type, *method = "";

  indent += 2;

  switch (plan->plan_un.join.join_method)
    {
    case QO_JOINMETHOD_NL_JOIN:
    case QO_JOINMETHOD_IDX_JOIN:
      method = "NESTED LOOPS";
      break;

    case QO_JOINMETHOD_MERGE_JOIN:
      method = "MERGE JOIN";
      break;

    case QO_JOINMETHOD_HASH_JOIN:
      method = "HASH JOIN";
      break;

    default:
      method = "UNKNOWN";
      break;
    }

  switch (plan->plan_un.join.join_type)
    {
    case JOIN_INNER:
      if (!bitset_is_empty (&(plan->plan_un.join.join_terms)))
	{
	  type = "inner join";
	}
      else
	{
	  if (plan->plan_un.join.join_method == QO_JOINMETHOD_IDX_JOIN)
	    {
	      type = "inner join";
	    }
	  else
	    {
	      type = "cross join";
	    }
	}
      break;
    case JOIN_LEFT:
      type = "left outer join";
      break;
    case JOIN_RIGHT:
      type = "right outer join";
      break;
    case JOIN_OUTER:		/* not used */
      type = "full outer join";
      break;
    case JOIN_CSELECT:
      type = "cselect";
      break;
    case NO_JOIN:
    default:
      type = "unknown";
      break;
    }

  fprintf (fp, "%*c%s (%s)\n", indent, ' ', method, type);
  qo_plan_print_text (fp, plan->plan_un.join.outer, indent);
  qo_plan_print_text (fp, plan->plan_un.join.inner, indent);
}

void
qo_plan_follow_print_text (FILE * fp, QO_PLAN * plan, int indent)
{
  char buf[257] = { '\0', };
  indent += 2;

  fprintf (fp, "%*cFOLLOW (edge: %s)\n", indent, ' ', qo_term_string (plan->plan_un.follow.path, buf));

  qo_plan_print_text (fp, plan->plan_un.follow.head, indent);
}

void
qo_plan_print_text (FILE * fp, QO_PLAN * plan, int indent)
{
  switch (plan->plan_type)
    {
    case QO_PLANTYPE_SCAN:
      qo_plan_scan_print_text (fp, plan, indent);
      break;

    case QO_PLANTYPE_SORT:
      qo_plan_sort_print_text (fp, plan, indent);
      break;

    case QO_PLANTYPE_JOIN:
      qo_plan_join_print_text (fp, plan, indent);
      break;

    case QO_PLANTYPE_FOLLOW:
      qo_plan_follow_print_text (fp, plan, indent);
      break;

    default:
      break;
    }
}

void
qo_top_plan_print_text (PARSER_CONTEXT * parser, xasl_node * xasl, PT_NODE * select, QO_PLAN * plan)
{
  size_t sizeloc;
  char *ptr, *sql;
  FILE *fp;
  int indent;
  unsigned int save_custom;

  assert (parser != NULL && xasl != NULL && plan != NULL && select != NULL);

  if (parser->num_plan_trace >= MAX_NUM_PLAN_TRACE)
    {
      return;
    }

  fp = port_open_memstream (&ptr, &sizeloc);
  if (fp == NULL)
    {
      return;
    }

  indent = 0;
  qo_plan_print_text (fp, plan, indent);

  indent += 2;

  if (select->info.query.order_by)
    {
      if (xasl && xasl->spec_list && xasl->spec_list->indexptr && xasl->spec_list->indexptr->orderby_skip)
	{
	  fprintf (fp, "%*cskip order by: true\n", indent, ' ');
	}
    }

  if (select->info.query.q.select.group_by)
    {
      if (xasl && xasl->spec_list && xasl->spec_list->indexptr && xasl->spec_list->indexptr->groupby_skip)
	{
	  fprintf (fp, "%*cgroup by nosort: true\n", indent, ' ');
	}
    }

  save_custom = parser->custom_print;
  parser->custom_print |= PT_CONVERT_RANGE;
  sql = parser_print_tree (parser, select);
  parser->custom_print = save_custom;

  if (sql != NULL)
    {
      fprintf (fp, "\n%*crewritten query: %s\n", indent, ' ', sql);
    }

  port_close_memstream (fp, &ptr, &sizeloc);

  if (ptr != NULL)
    {
      parser->plan_trace[parser->num_plan_trace].format = QUERY_TRACE_TEXT;
      parser->plan_trace[parser->num_plan_trace].trace.text_plan = ptr;
      parser->num_plan_trace++;
    }

  return;
}
