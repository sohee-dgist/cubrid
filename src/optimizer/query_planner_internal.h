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
#include "query_planner_constants.h"

#define VALID_INNER(plan)	((plan)->well_rooted || \
				 ((plan)->plan_type == QO_PLANTYPE_SORT))

#define	qo_scan_walk	qo_generic_walk
#define	qo_worst_walk	qo_generic_walk

#define	qo_generic_free	NULL
#define	qo_sort_free	qo_generic_free
#define	qo_follow_free	qo_generic_free
#define	qo_worst_free	qo_generic_free

#define QO_INFO_INDEX(_M_offset, _bitset)  \
    (_M_offset + (unsigned int)(BITPATTERN(_bitset) & planner->node_mask))

#define QO_IS_LIMIT_NODE(env, node) \
  (BITSET_MEMBER (QO_ENV_SORT_LIMIT_NODES ((env)), QO_NODE_IDX ((node))))

typedef enum
{ JOIN_RIGHT_ORDER, JOIN_OPPOSITE_ORDER } JOIN_ORDER_TRY;

struct ndv_info
{
  QO_ENV *env;
  int total_ndv;
  BITSET seg_bitset;
};
typedef struct ndv_info NDV_INFO;

typedef int (*QO_WALK_FUNCTION) (QO_PLAN *, void *);

double log3 (double n);
QO_PLAN *qo_plan_malloc (QO_ENV * env);
const char *qo_term_string (QO_TERM * term, char *buf);
void qo_estimate_ngroups (QO_PLAN * plan, SORT_TYPE sort_type);
double qo_estimate_ndv (double N, double p, double n);
int qo_get_group_ndv (QO_PLAN * plan, SORT_TYPE sort_type);
void qo_plan_compute_cost (QO_PLAN * plan);
void qo_plan_compute_subquery_cost (PT_NODE * subquery, double *subq_cpu_cost, double *subq_io_cost);
int qo_walk_plan_tree (QO_PLAN * plan, QO_WALK_FUNCTION f, void *arg);
void qo_set_use_desc (QO_PLAN * plan);
int qo_unset_multi_range_optimization (QO_PLAN * plan, void *arg);
int qo_set_orderby_skip (QO_PLAN * plan, void *arg);
int qo_unset_hint_use_desc_idx (QO_PLAN * plan, void *arg);
int qo_validate_indexes_for_orderby (QO_PLAN * plan, void *arg);
QO_PLAN *qo_top_plan_new (QO_PLAN * plan);
void
qo_generic_walk (QO_PLAN * plan, void (*child_fn) (QO_PLAN *, void *), void *child_data,
		 void (*parent_fn) (QO_PLAN *, void *), void *parent_data);
void qo_plan_print_sort_spec_helper (PT_NODE * list, bool is_iscan_asc, FILE * f, int howfar);
void qo_plan_print_sort_spec (QO_PLAN * plan, FILE * f, int howfar);
void qo_plan_print_costs (QO_PLAN * plan, FILE * f, int howfar);
void qo_plan_print_projected_segs (QO_PLAN * plan, FILE * f, int howfar);
void qo_plan_print_sarged_terms (QO_PLAN * plan, FILE * f, int howfar);
void qo_plan_print_outer_join_terms (QO_PLAN * plan, FILE * f, int howfar);
void qo_plan_print_subqueries (QO_PLAN * plan, FILE * f, int howfar);
void qo_plan_print_analytic_eval (QO_PLAN * plan, FILE * f, int howfar);
QO_PLAN *qo_scan_new (QO_INFO * info, QO_NODE * node, QO_SCANMETHOD scan_method);
void qo_scan_free (QO_PLAN * plan);
QO_PLAN *qo_seq_scan_new (QO_INFO * info, QO_NODE * node);
double qo_sum_bitset_term_cost_weights (QO_ENV * env, BITSET * terms);
void qo_sscan_cost (QO_PLAN * planp);
bool qo_index_has_bit_attr (QO_INDEX_ENTRY * index_entryp);
QO_PLAN *qo_index_scan_new (QO_INFO * info, QO_NODE * node, QO_NODE_INDEX_ENTRY * ni_entry, QO_SCANMETHOD scan_method,
			    BITSET * range_terms, BITSET * indexable_terms);
void qo_apply_scan_term_cpu_overhead (QO_PLAN * planp);
void qo_iscan_cost (QO_PLAN * planp);
void qo_scan_fprint (QO_PLAN * plan, FILE * f, int howfar);
void qo_scan_info (QO_PLAN * plan, FILE * f, int howfar);
QO_PLAN *qo_sort_new (QO_PLAN * root, QO_EQCLASS * order, SORT_TYPE sort_type);
void
qo_sort_walk (QO_PLAN * plan, void (*child_fn) (QO_PLAN *, void *), void *child_data,
	      void (*parent_fn) (QO_PLAN *, void *), void *parent_data);
void qo_sort_fprint (QO_PLAN * plan, FILE * f, int howfar);
void qo_sort_info (QO_PLAN * plan, FILE * f, int howfar);
void qo_sort_cost (QO_PLAN * planp);
QO_PLAN *qo_join_new (QO_INFO * info, JOIN_TYPE join_type, QO_JOINMETHOD join_method, QO_PLAN * outer, QO_PLAN * inner,
		      BITSET * join_terms, BITSET * duj_terms, BITSET * afj_terms, BITSET * sarged_terms,
		      BITSET * pinned_subqueries, BITSET * hash_terms);
void qo_join_free (QO_PLAN * plan);
void
qo_join_walk (QO_PLAN * plan, void (*child_fn) (QO_PLAN *, void *), void *child_data,
	      void (*parent_fn) (QO_PLAN *, void *), void *parent_data);
void qo_join_fprint (QO_PLAN * plan, FILE * f, int howfar);
void qo_join_info (QO_PLAN * plan, FILE * f, int howfar);
bool qo_can_apply_limit_card (QO_ENV * env);
double qo_get_join_term_cost_weight (QO_TERM * term);
double qo_sum_join_term_cost_weights (QO_ENV * env, BITSET * terms);
double qo_get_nljoin_term_cpu_overhead (QO_PLAN * planp, double guessed_result_cardinality);
void qo_nljoin_cost (QO_PLAN * planp);
void qo_mjoin_cost (QO_PLAN * planp);
void qo_hjoin_cost (QO_PLAN * plan_p);
void qo_hjoin_fprint (QO_PLAN * plan, FILE * f, int howfar);
QO_PLAN *qo_follow_new (QO_INFO * info, QO_PLAN * head_plan, QO_TERM * path_term, BITSET * sarged_terms,
			BITSET * pinned_subqueries);
void
qo_follow_walk (QO_PLAN * plan, void (*child_fn) (QO_PLAN *, void *), void *child_data,
		void (*parent_fn) (QO_PLAN *, void *), void *parent_data);
void qo_follow_fprint (QO_PLAN * plan, FILE * f, int howfar);
void qo_follow_info (QO_PLAN * plan, FILE * f, int howfar);
void qo_follow_cost (QO_PLAN * planp);
QO_PLAN *qo_cp_new (QO_INFO * info, QO_PLAN * outer, QO_PLAN * inner, BITSET * sarged_terms,
		    BITSET * pinned_subqueries);
QO_PLAN *qo_worst_new (QO_ENV * env);
void qo_worst_fprint (QO_PLAN * plan, FILE * f, int howfar);
void qo_worst_info (QO_PLAN * plan, FILE * f, int howfar);
void qo_worst_cost (QO_PLAN * planp);
void qo_zero_cost (QO_PLAN * planp);
double qo_get_term_cost_weight (QO_TERM * term);
bool qo_info_is_small_filtered_side (QO_INFO * info);
double
qo_apply_mcv_hotkey_join_guard (QO_TERM * term, QO_INFO * head_info, QO_INFO * tail_info,
				double base_cardinality, double term_sel);
double qo_get_delayed_sarg_lookup_penalty (QO_PLAN * planp, double guessed_outer_cardinality);
QO_PLAN *qo_plan_order_by (QO_PLAN * plan, QO_EQCLASS * order);
QO_PLAN_COMPARE_RESULT qo_plan_cmp_prefer_covering_index (QO_PLAN * scan_plan_p, QO_PLAN * sort_plan_p);
QO_PLAN_COMPARE_RESULT qo_plan_cmp (QO_PLAN * a, QO_PLAN * b);
QO_PLAN_COMPARE_RESULT qo_multi_range_opt_plans_cmp (QO_PLAN * a, QO_PLAN * b);
QO_PLAN_COMPARE_RESULT qo_index_covering_plans_cmp (QO_PLAN * a, QO_PLAN * b);
void qo_plan_fprint (QO_PLAN * plan, FILE * f, int howfar, const char *title);
void qo_plan_lite_print (QO_PLAN * plan, FILE * f, int howfar);
QO_PLAN *qo_plan_finalize (QO_PLAN * plan);
void
qo_plan_walk (QO_PLAN * plan, void (*child_fn) (QO_PLAN *, void *), void *child_data,
	      void (*parent_fn) (QO_PLAN *, void *), void *parent_data);
void qo_plan_del_ref_func (QO_PLAN * plan, void *ignore);
void qo_plan_add_to_free_list (QO_PLAN * plan, void *ignore);
void qo_plan_free (QO_PLAN * plan);
void qo_plans_init (QO_ENV * env);
void qo_plans_teardown (QO_ENV * env);
void qo_init_planvec (QO_PLANVEC * planvec);
void qo_uninit_planvec (QO_PLANVEC * planvec);
void qo_dump_planvec (QO_PLANVEC * planvec, FILE * f, int indent);
QO_PLAN_COMPARE_RESULT qo_check_planvec (QO_PLANVEC * planvec, QO_PLAN * plan);
QO_PLAN_COMPARE_RESULT qo_cmp_planvec (QO_PLANVEC * planvec, QO_PLAN * plan);
QO_PLAN *qo_find_best_plan_on_planvec (QO_PLANVEC * planvec, double n);
void qo_info_nodes_init (QO_ENV * env);
QO_INFO *qo_alloc_info (QO_PLANNER * planner, BITSET * nodes, BITSET * terms, BITSET * eqclasses, double cardinality,
			double total_rows);
void qo_free_info (QO_INFO * info);
void qo_detach_info (QO_INFO * info);
bool qo_check_new_best_plan_on_info (QO_INFO * info, QO_PLAN * plan);
int qo_check_plan_on_info (QO_INFO * info, QO_PLAN * plan);
QO_PLAN *qo_find_best_nljoin_inner_plan_on_info (QO_PLAN * outer, QO_INFO * info, JOIN_TYPE join_type,
						 int idx_join_plan_n);
QO_PLAN *qo_find_best_plan_on_info (QO_INFO * info, QO_EQCLASS * order, double n);
int
qo_examine_idx_join (QO_INFO * info, JOIN_TYPE join_type, QO_INFO * outer, QO_INFO * inner, BITSET * afj_terms,
		     BITSET * sarged_terms, BITSET * pinned_subqueries);
int
qo_examine_nl_join (QO_INFO * info, JOIN_TYPE join_type, QO_INFO * outer, QO_INFO * inner, BITSET * nl_join_terms,
		    BITSET * duj_terms, BITSET * afj_terms, BITSET * sarged_terms, BITSET * pinned_subqueries,
		    int idx_join_plan_n, BITSET * hash_terms);
int
qo_examine_merge_join (QO_INFO * info, JOIN_TYPE join_type, QO_INFO * outer, QO_INFO * inner, BITSET * sm_join_terms,
		       BITSET * duj_terms, BITSET * afj_terms, BITSET * sarged_terms, BITSET * pinned_subqueries);
int
qo_examine_hash_join (QO_INFO * info, JOIN_TYPE join_type, QO_INFO * outer, QO_INFO * inner, BITSET * hash_join_terms,
		      BITSET * duj_terms, BITSET * afj_terms, BITSET * sarged_terms, BITSET * pinned_subqueries);
int
qo_examine_correlated_index (QO_INFO * info, JOIN_TYPE join_type, QO_INFO * outer, QO_INFO * inner, BITSET * afj_terms,
			     BITSET * sarged_terms, BITSET * pinned_subqueries);
int
qo_examine_follow (QO_INFO * info, QO_TERM * path_term, QO_INFO * head_info, BITSET * sarged_terms,
		   BITSET * pinned_subqueries);
void qo_compute_projected_segs (QO_PLANNER * planner, BITSET * nodes, BITSET * terms, BITSET * projected);
int qo_compute_projected_size (QO_PLANNER * planner, BITSET * segset);
void qo_dump_info (QO_INFO * info, FILE * f);
QO_PLANNER *qo_alloc_planner (QO_ENV * env);
void qo_dump_planner_info (QO_PLANNER * planner, QO_PARTITION * partition, FILE * f);
void
qo_get_term_hit_prob (QO_TERM * term, QO_INFO * head_info, QO_INFO * tail_info, QO_ENV * env,
		      double *out_head_factor, double *out_tail_factor);
void
planner_visit_node (QO_PLANNER * planner, QO_PARTITION * partition, PT_HINT_ENUM hint, QO_NODE * head_node,
		    QO_NODE * tail_node, BITSET * visited_nodes, BITSET * visited_rel_nodes, BITSET * visited_terms,
		    BITSET * nested_path_nodes, BITSET * remaining_nodes, BITSET * remaining_terms,
		    BITSET * remaining_subqueries, int num_path_inner);
double planner_nodeset_join_cost (QO_PLANNER * planner, BITSET * nodeset);
void
planner_permutate (QO_PLANNER * planner, QO_PARTITION * partition, PT_HINT_ENUM hint, QO_NODE * prev_head_node,
		   BITSET * visited_nodes, BITSET * visited_rel_nodes, BITSET * visited_terms,
		   BITSET * nested_path_nodes, BITSET * remaining_nodes, BITSET * remaining_terms,
		   BITSET * remaining_subqueries, int num_path_inner, int *node_idxp);
int
qo_generate_join_index_scan (QO_INFO * infop, JOIN_TYPE join_type, QO_PLAN * outer_plan, QO_INFO * inner,
			     QO_NODE * nodep, QO_NODE_INDEX_ENTRY * ni_entryp, BITSET * indexable_terms,
			     BITSET * afj_terms, BITSET * sarged_terms, BITSET * pinned_subqueries);
void qo_generate_seq_scan (QO_INFO * infop, QO_NODE * nodep);
int qo_generate_index_scan (QO_INFO * infop, QO_NODE * nodep, QO_NODE_INDEX_ENTRY * ni_entryp, int nsegs);
int qo_generate_loose_index_scan (QO_INFO * infop, QO_NODE * nodep, QO_NODE_INDEX_ENTRY * ni_entryp);
int qo_generate_sort_limit_plan (QO_ENV * env, QO_INFO * infop, QO_PLAN * subplan);
int qo_has_is_not_null_term (QO_NODE * node);
QO_PLAN *qo_search_planner (QO_PLANNER * planner);
void qo_clean_planner (QO_PLANNER * planner);
QO_INFO *qo_search_partition_join (QO_PLANNER * planner, QO_PARTITION * partition, BITSET * remaining_subqueries);
QO_PLAN *qo_search_partition (QO_PLANNER * planner, QO_PARTITION * partition, QO_EQCLASS * order,
			      BITSET * remaining_subqueries);
void sort_partitions (QO_PLANNER * planner);
QO_PLAN *qo_combine_partitions (QO_PLANNER * planner, BITSET * reamining_subqueries);
double qo_like_selectivity (QO_ENV * env, PT_NODE * pt_expr);
double qo_or_selectivity (QO_ENV * env, double lhs_sel, double rhs_sel);
double qo_and_selectivity (QO_ENV * env, double lhs_sel, double rhs_sel);
double qo_not_selectivity (QO_ENV * env, double sel);
double qo_equal_selectivity (QO_ENV * env, PT_NODE * pt_expr);
double qo_comp_selectivity (QO_ENV * env, PT_NODE * pt_expr);
double qo_between_selectivity (QO_ENV * env, PT_NODE * pt_expr);
double qo_range_selectivity (QO_ENV * env, PT_NODE * pt_expr);
double qo_all_some_in_selectivity (QO_ENV * env, PT_NODE * pt_expr);
int qo_index_cardinality (QO_ENV * env, PT_NODE * attr);
int qo_index_cardinality_with_dedup (QO_ENV * env, PT_NODE * attr, BITSET * seg_bitset);
bool qo_validate_index_term_notnull (QO_ENV * env, QO_INDEX_ENTRY * index_entryp);
bool qo_validate_index_attr_notnull (QO_ENV * env, QO_INDEX_ENTRY * index_entryp, PT_NODE * col);
int qo_validate_index_for_orderby (QO_ENV * env, QO_NODE_INDEX_ENTRY * ni_entryp);
PT_NODE *qo_search_isnull_key_expr (PARSER_CONTEXT * parser, PT_NODE * tree, void *arg, int *continue_walk);
PT_NODE *qo_get_col_product_ndv (PARSER_CONTEXT * parser, PT_NODE * tree, void *arg, int *continue_walk);
QO_PLAN_COMPARE_RESULT qo_plan_iscan_terms_cmp (QO_PLAN * a, QO_PLAN * b);
QO_PLAN_COMPARE_RESULT qo_group_by_skip_plans_cmp (QO_PLAN * a, QO_PLAN * b);
QO_PLAN_COMPARE_RESULT qo_order_by_skip_plans_cmp (QO_PLAN * a, QO_PLAN * b);
bool qo_check_orderby_skip_descending (QO_PLAN * plan);
bool
qo_check_skip_term (QO_ENV * env, BITSET visited_segs, QO_TERM * term, BITSET * visited_terms,
		    BITSET * cur_visited_terms);
int qo_validate_index_for_groupby (QO_ENV * env, QO_NODE_INDEX_ENTRY * ni_entryp);
bool qo_check_groupby_skip_descending (QO_PLAN * plan, PT_NODE * list);
bool qo_plan_is_orderby_skip_candidate (QO_PLAN * plan);
bool qo_is_sort_limit (QO_PLAN * plan);
int qo_check_like_recompile_candidate (QO_PLAN * plan, void *arg);
json_t *qo_plan_scan_print_json (QO_PLAN * plan);
json_t *qo_plan_sort_print_json (QO_PLAN * plan);
json_t *qo_plan_join_print_json (QO_PLAN * plan);
json_t *qo_plan_follow_print_json (QO_PLAN * plan);
json_t *qo_plan_print_json (QO_PLAN * plan);
void qo_plan_scan_print_text (FILE * fp, QO_PLAN * plan, int indent);
void qo_plan_sort_print_text (FILE * fp, QO_PLAN * plan, int indent);
void qo_plan_join_print_text (FILE * fp, QO_PLAN * plan, int indent);
void qo_plan_follow_print_text (FILE * fp, QO_PLAN * plan, int indent);
void qo_plan_print_text (FILE * fp, QO_PLAN * plan, int indent);

#endif /* _QUERY_PLANNER_INTERNAL_H_ */
