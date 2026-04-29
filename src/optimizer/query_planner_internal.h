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

#ifndef _QUERY_PLANNER_INTERNAL_H_
#define _QUERY_PLANNER_INTERNAL_H_

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#if !defined(WINDOWS)
#include <values.h>
#endif /* !WINDOWS */
#include "jansson.h"

#include "parser.h"
#include "object_primitive.h"
#include "optimizer.h"
#include "query_planner.h"
#include "query_graph.h"
#include "environment_variable.h"
#include "misc_string.h"
#include "system_parameter.h"
#include "parser.h"
#include "parser_message.h"
#include "intl_support.h"
#include "storage_common.h"
#include "xasl_analytic.hpp"
#include "xasl_generation.h"
#include "schema_manager.h"
#include "network_interface_cl.h"
#include "dbtype.h"
#include "regu_var.hpp"
#include "histogram_cl.hpp"

/* Structural equivalence classes for expressions */
typedef enum PRED_CLASS
{
  PC_ATTR,
  PC_CONST,
  PC_HOST_VAR,
  PC_SUBQUERY,
  PC_SET,
  PC_OTHER,
  PC_MULTI_ATTR
} PRED_CLASS;

struct ndv_info
{
  QO_ENV *env;
  int total_ndv;
  BITSET seg_bitset;
};
typedef struct ndv_info NDV_INFO;

double log3 (double n);
void qo_estimate_ngroups (QO_PLAN * plan, SORT_TYPE sort_type);
double qo_estimate_ndv (double N, double p, double n);
int qo_get_group_ndv (QO_PLAN * plan, SORT_TYPE sort_type);
void qo_plan_compute_cost (QO_PLAN * plan);
void qo_plan_compute_subquery_cost (PT_NODE * subquery, double *subq_cpu_cost, double *subq_io_cost);
void qo_sscan_cost (QO_PLAN * planp);
void qo_iscan_cost (QO_PLAN * planp);
void qo_sort_cost (QO_PLAN * planp);
bool qo_can_apply_limit_card (QO_ENV * env);
void qo_nljoin_cost (QO_PLAN * planp);
void qo_mjoin_cost (QO_PLAN * planp);
void qo_hjoin_cost (QO_PLAN * plan_p);
void qo_follow_cost (QO_PLAN * planp);
void qo_worst_cost (QO_PLAN * planp);
void qo_zero_cost (QO_PLAN * planp);
double qo_or_selectivity (QO_ENV * env, double lhs_sel, double rhs_sel);
double qo_and_selectivity (QO_ENV * env, double lhs_sel, double rhs_sel);
double qo_not_selectivity (QO_ENV * env, double sel);
double qo_equal_selectivity (QO_ENV * env, PT_NODE * pt_expr);
double qo_comp_selectivity (QO_ENV * env, PT_NODE * pt_expr);
double qo_between_selectivity (QO_ENV * env, PT_NODE * pt_expr);
double qo_range_selectivity (QO_ENV * env, PT_NODE * pt_expr);
double qo_all_some_in_selectivity (QO_ENV * env, PT_NODE * pt_expr);
PRED_CLASS qo_classify (PT_NODE * attr);
double qo_like_selectivity (QO_ENV * env, PT_NODE * pt_expr);
double qo_sum_bitset_term_cost_weights (QO_ENV * env, BITSET * terms);
void qo_apply_scan_term_cpu_overhead (QO_PLAN * planp);
double qo_get_join_term_cost_weight (QO_TERM * term);
double qo_sum_join_term_cost_weights (QO_ENV * env, BITSET * terms);
double qo_get_nljoin_term_cpu_overhead (QO_PLAN * planp, double guessed_result_cardinality);
double qo_get_term_cost_weight (QO_TERM * term);
bool qo_info_is_small_filtered_side (QO_INFO * info);
double qo_apply_mcv_hotkey_join_guard (QO_TERM * term, QO_INFO * head_info, QO_INFO * tail_info,
				       double base_cardinality, double term_sel);
double qo_get_delayed_sarg_lookup_penalty (QO_PLAN * planp, double guessed_outer_cardinality);

#endif /* _QUERY_PLANNER_INTERNAL_H_ */
