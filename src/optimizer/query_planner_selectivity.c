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

double
qo_expr_selectivity (QO_ENV * env, PT_NODE * pt_expr)
{
  double lhs_selectivity, rhs_selectivity, selectivity, total_selectivity;
  PT_NODE *node;
  bool not_null_calculated = false;

  QO_ASSERT (env, pt_expr != NULL);
  QO_ASSERT (env, pt_expr->node_type == PT_EXPR);

  selectivity = 0.0;
  total_selectivity = 0.0;

  /* traverse OR list */
  for (node = pt_expr; node; node = node->or_next)
    {
      switch (node->info.expr.op)
	{
	case PT_OR:
	  lhs_selectivity = qo_expr_selectivity (env, node->info.expr.arg1);
	  rhs_selectivity = qo_expr_selectivity (env, node->info.expr.arg2);
	  selectivity = qo_or_selectivity (env, lhs_selectivity, rhs_selectivity);
	  break;

	case PT_AND:
	  lhs_selectivity = qo_expr_selectivity (env, node->info.expr.arg1);
	  rhs_selectivity = qo_expr_selectivity (env, node->info.expr.arg2);
	  selectivity = qo_and_selectivity (env, lhs_selectivity, rhs_selectivity);
	  break;

	case PT_NOT:
	  lhs_selectivity = qo_expr_selectivity (env, node->info.expr.arg1);
	  selectivity = qo_not_selectivity (env, lhs_selectivity);
	  break;

	case PT_EQ:
	  selectivity = qo_equal_selectivity (env, node);
	  break;

	case PT_NE:
	  lhs_selectivity = qo_equal_selectivity (env, node);
	  selectivity = qo_not_selectivity (env, lhs_selectivity);
	  break;

	case PT_NULLSAFE_EQ:
	  selectivity = qo_equal_selectivity (env, node);
	  break;

	case PT_GE:
	case PT_GT:
	case PT_LT:
	case PT_LE:
	  selectivity = qo_comp_selectivity (env, node);
	  break;

	case PT_BETWEEN:
	  selectivity = qo_between_selectivity (env, node);
	  break;

	case PT_NOT_BETWEEN:
	  lhs_selectivity = qo_between_selectivity (env, node);
	  selectivity = qo_not_selectivity (env, lhs_selectivity);
	  break;

	case PT_RANGE:
	  selectivity = qo_range_selectivity (env, node);
	  break;

	case PT_LIKE_ESCAPE:
	  selectivity = (double) prm_get_float_value (PRM_ID_LIKE_TERM_SELECTIVITY);
	  break;
	case PT_LIKE:
	  {
	    selectivity = qo_like_selectivity (env, node);
	    break;
	  }
	case PT_NOT_LIKE:
	  {
	    selectivity = 1 - qo_like_selectivity (env, node);
	    break;
	  }
	case PT_SETNEQ:
	case PT_SETEQ:
	case PT_SUPERSETEQ:
	case PT_SUPERSET:
	case PT_SUBSET:
	case PT_SUBSETEQ:
	case PT_IS:
	case PT_XOR:
	  selectivity = DEFAULT_SELECTIVITY;
	  break;

	case PT_IS_NOT:
	  selectivity = qo_not_selectivity (env, DEFAULT_SELECTIVITY);
	  break;

	case PT_EQ_SOME:
	case PT_NE_SOME:
	case PT_GE_SOME:
	case PT_GT_SOME:
	case PT_LT_SOME:
	case PT_LE_SOME:
	case PT_EQ_ALL:
	case PT_NE_ALL:
	case PT_GE_ALL:
	case PT_GT_ALL:
	case PT_LT_ALL:
	case PT_LE_ALL:
	case PT_IS_IN:
	  selectivity = qo_all_some_in_selectivity (env, node);
	  break;

	case PT_IS_NOT_IN:
	  lhs_selectivity = qo_all_some_in_selectivity (env, node);
	  selectivity = qo_not_selectivity (env, lhs_selectivity);
	  break;

	case PT_IS_NULL:
	  if (pt_expr->info.expr.arg1->node_type == PT_NAME && pt_expr->info.expr.arg1->info.name.null_frequency >= 0.0)
	    {
	      selectivity = pt_expr->info.expr.arg1->info.name.null_frequency;
	    }
	  else
	    {
	      selectivity = DEFAULT_NULL_SELECTIVITY;
	    }
	  not_null_calculated = true;
	  break;

	case PT_IS_NOT_NULL:
	  if (pt_expr->info.expr.arg1->node_type == PT_NAME && pt_expr->info.expr.arg1->info.name.null_frequency >= 0.0)
	    {
	      selectivity = pt_expr->info.expr.arg1->info.name.null_frequency;
	    }
	  else
	    {
	      selectivity = DEFAULT_NULL_SELECTIVITY;
	    }
	  selectivity = 1 - selectivity;
	  not_null_calculated = true;
	  break;

	case PT_EXISTS:
	  selectivity = DEFAULT_EXISTS_SELECTIVITY;	/* make a guess */
	  break;

	default:
	  break;
	}

      if (!not_null_calculated)
	{
	  if (pt_expr->info.expr.arg1 && pt_expr->info.expr.arg1->node_type == PT_NAME
	      && pt_expr->info.expr.arg1->info.name.null_frequency >= 0.0)
	    {
	      selectivity = selectivity * (1 - pt_expr->info.expr.arg1->info.name.null_frequency);
	    }
	  if (pt_expr->info.expr.arg2 && pt_expr->info.expr.arg2->node_type == PT_NAME
	      && pt_expr->info.expr.arg2->info.name.null_frequency >= 0.0)
	    {
	      selectivity = selectivity * (1 - pt_expr->info.expr.arg2->info.name.null_frequency);
	    }
	}

      total_selectivity = qo_or_selectivity (env, total_selectivity, selectivity);
      total_selectivity = MAX (total_selectivity, 0.0);
      total_selectivity = MIN (total_selectivity, 1.0);
    }

  return total_selectivity;
}

double
qo_or_selectivity (QO_ENV * env, double lhs_sel, double rhs_sel)
{
  double result;

  QO_ASSERT (env, lhs_sel >= 0.0);
  QO_ASSERT (env, lhs_sel <= 1.0);
  QO_ASSERT (env, rhs_sel >= 0.0);
  QO_ASSERT (env, rhs_sel <= 1.0);

  result = lhs_sel + rhs_sel - (lhs_sel * rhs_sel);

  return result;
}

double
qo_and_selectivity (QO_ENV * env, double lhs_sel, double rhs_sel)
{
  double result;

  QO_ASSERT (env, lhs_sel >= 0.0);
  QO_ASSERT (env, lhs_sel <= 1.0);
  QO_ASSERT (env, rhs_sel >= 0.0);
  QO_ASSERT (env, rhs_sel <= 1.0);

  result = lhs_sel * rhs_sel;

  return result;
}

double
qo_not_selectivity (QO_ENV * env, double sel)
{
  QO_ASSERT (env, sel >= 0.0);
  QO_ASSERT (env, sel <= 1.0);

  return 1.0 - sel;
}

double
qo_equal_selectivity (QO_ENV * env, PT_NODE * pt_expr)
{
  PT_NODE *lhs, *rhs, *multi_attr;
  DB_VALUE *host_var;
  PRED_CLASS pc_lhs, pc_rhs;
  int lhs_icard, rhs_icard, icard;
  double selectivity;

  lhs = pt_expr->info.expr.arg1;
  rhs = pt_expr->info.expr.arg2;

  /* the class of lhs and rhs */
  pc_lhs = qo_classify (lhs);
  pc_rhs = qo_classify (rhs);

  selectivity = DEFAULT_EQUAL_SELECTIVITY;

  bool success = false;

  switch (pc_lhs)
    {
    case PC_ATTR:

      switch (pc_rhs)
	{
	case PC_ATTR:
	  {
	    /* attr = attr */
	    bool success = false;
	    double eqjoin_selectivity = 0.0;
	    histogram_get_eqjoin_selectivity (lhs, rhs, &eqjoin_selectivity, &success);
	    if (success)
	      {
		selectivity = eqjoin_selectivity;
	      }
	    else
	      {

		/* check for indexes on either of the attributes */
		lhs_icard = qo_index_cardinality (env, lhs);
		rhs_icard = qo_index_cardinality (env, rhs);

		icard = MAX (lhs_icard, rhs_icard);
		if (icard != 0)
		  {
		    selectivity = (1.0 / icard);
		  }
		else
		  {
		    selectivity = DEFAULT_EQUIJOIN_SELECTIVITY;
		  }
	      }
	  }
	  break;

	case PC_CONST:
	case PC_HOST_VAR:
	  if (pc_rhs == PC_HOST_VAR)
	    {
	      host_var = &env->parser->host_variables[rhs->info.host_var.index];
	    }
	  else
	    {
	      host_var = &rhs->info.value.db_value;
	    }
	  histogram_get_equal_selectivity (lhs, host_var, &selectivity, &success);
	  if (success)
	    {
	      break;
	    }
	  [[fallthrough]];

	case PC_SUBQUERY:
	case PC_SET:
	case PC_OTHER:
	  /* attr = const */

	  /* check for index on the attribute.  NOTE: For an equality predicate, we treat subqueries as constants. */
	  lhs_icard = qo_index_cardinality (env, lhs);
	  if (lhs_icard != 0)
	    {
	      selectivity = (1.0 / lhs_icard);
	    }
	  else
	    {
	      selectivity = DEFAULT_EQUAL_SELECTIVITY;
	    }

	  break;

	case PC_MULTI_ATTR:
	  /* attr = (attr,attr) syntactic impossible case */
	  selectivity = DEFAULT_EQUAL_SELECTIVITY;
	  break;
	}

      break;

    case PC_CONST:
      switch (pc_rhs)
	{
	case PC_ATTR:
	  histogram_get_equal_selectivity (rhs, &lhs->info.value.db_value, &selectivity, &success);
	  break;

	default:
	  break;
	}
      if (success)
	{
	  break;
	}
      [[fallthrough]];

    case PC_HOST_VAR:
      switch (pc_rhs)
	{
	case PC_ATTR:
	  host_var = &env->parser->host_variables[lhs->info.host_var.index];
	  histogram_get_equal_selectivity (rhs, host_var, &selectivity, &success);
	  break;

	default:
	  break;
	}
      if (success)
	{
	  break;
	}
      [[fallthrough]];
    case PC_SUBQUERY:
    case PC_SET:
    case PC_OTHER:

      switch (pc_rhs)
	{
	case PC_ATTR:
	  /* const = attr */

	  /* check for index on the attribute.  NOTE: For an equality predicate, we treat subqueries as constants. */
	  rhs_icard = qo_index_cardinality (env, rhs);
	  if (rhs_icard != 0)
	    {
	      selectivity = (1.0 / rhs_icard);
	    }
	  else
	    {
	      selectivity = DEFAULT_EQUAL_SELECTIVITY;
	    }

	  break;

	case PC_CONST:
	case PC_HOST_VAR:
	case PC_SUBQUERY:
	case PC_SET:
	case PC_OTHER:
	  /* const = const */

	  selectivity = DEFAULT_EQUAL_SELECTIVITY;
	  break;

	case PC_MULTI_ATTR:
	  /* const = (attr,attr) */
	  multi_attr = rhs->info.function.arg_list;
	  rhs_icard = 0;
	  for ( /* none */ ; multi_attr; multi_attr = multi_attr->next)
	    {
	      /* get index cardinality */
	      icard = qo_index_cardinality (env, multi_attr);
	      if (icard <= 0)
		{
		  /* the only interesting case is PT_BETWEEN_EQ_NA */
		  icard = 1 / DEFAULT_EQUAL_SELECTIVITY;
		}
	      if (rhs_icard == 0)
		{
		  /* first time */
		  rhs_icard = icard;
		}
	      else
		{
		  rhs_icard *= icard;
		}
	    }
	  if (rhs_icard != 0)
	    {
	      selectivity = (1.0 / rhs_icard);
	    }
	  else
	    {
	      selectivity = DEFAULT_EQUAL_SELECTIVITY;
	    }
	  break;
	}
      break;

    case PC_MULTI_ATTR:
      switch (pc_rhs)
	{
	case PC_ATTR:
	  /* (attr,attr) = attr  syntactic impossible case */
	  selectivity = DEFAULT_EQUAL_SELECTIVITY;
	  break;

	case PC_CONST:
	case PC_HOST_VAR:
	case PC_SUBQUERY:
	case PC_SET:
	case PC_OTHER:
	  /* (attr,attr) = const */

	  multi_attr = lhs->info.function.arg_list;
	  lhs_icard = 0;
	  for ( /* none */ ; multi_attr; multi_attr = multi_attr->next)
	    {
	      /* get index cardinality */
	      icard = qo_index_cardinality (env, multi_attr);
	      if (icard <= 0)
		{
		  /* the only interesting case is PT_BETWEEN_EQ_NA */
		  icard = 1 / DEFAULT_EQUAL_SELECTIVITY;
		}
	      if (lhs_icard == 0)
		{
		  /* first time */
		  lhs_icard = icard;
		}
	      else
		{
		  lhs_icard *= icard;
		}
	    }
	  if (lhs_icard != 0)
	    {
	      selectivity = (1.0 / lhs_icard);
	    }
	  else
	    {
	      selectivity = DEFAULT_EQUAL_SELECTIVITY;
	    }
	  break;

	case PC_MULTI_ATTR:
	  /* (attr,attr) = (attr,attr) */
	  multi_attr = lhs->info.function.arg_list;
	  lhs_icard = 0;
	  for ( /* none */ ; multi_attr; multi_attr = multi_attr->next)
	    {
	      /* get index cardinality */
	      icard = qo_index_cardinality (env, multi_attr);
	      if (icard <= 0)
		{
		  /* the only interesting case is PT_BETWEEN_EQ_NA */
		  icard = 1 / DEFAULT_EQUAL_SELECTIVITY;
		}
	      if (lhs_icard == 0)
		{
		  /* first time */
		  lhs_icard = icard;
		}
	      else
		{
		  lhs_icard *= icard;
		}
	    }

	  multi_attr = rhs->info.function.arg_list;
	  rhs_icard = 0;
	  for ( /* none */ ; multi_attr; multi_attr = multi_attr->next)
	    {
	      /* get index cardinality */
	      icard = qo_index_cardinality (env, multi_attr);
	      if (icard <= 0)
		{
		  /* the only interesting case is PT_BETWEEN_EQ_NA */
		  icard = 1 / DEFAULT_EQUAL_SELECTIVITY;
		}
	      if (rhs_icard == 0)
		{
		  /* first time */
		  rhs_icard = icard;
		}
	      else
		{
		  rhs_icard *= icard;
		}
	    }

	  icard = MAX (lhs_icard, rhs_icard);
	  if (icard != 0)
	    {
	      selectivity = (1.0 / icard);
	    }
	  else
	    {
	      selectivity = DEFAULT_EQUIJOIN_SELECTIVITY;
	    }
	  break;
	}

      break;
      break;
    }

  return selectivity;
}

double
qo_comp_selectivity (QO_ENV * env, PT_NODE * pt_expr)
{
  PT_NODE *lhs, *rhs, *multi_attr;
  PRED_CLASS pc_lhs, pc_rhs;
  DB_VALUE *rhs_db_value;
  DB_VALUE *lhs_db_value;
  int lhs_icard, rhs_icard, icard;
  double selectivity;

  lhs = pt_expr->info.expr.arg1;
  rhs = pt_expr->info.expr.arg2;

  /* the class of lhs and rhs */
  pc_lhs = qo_classify (lhs);
  pc_rhs = qo_classify (rhs);

  selectivity = DEFAULT_COMP_SELECTIVITY;

  bool success = false;
  switch (pc_lhs)
    {
    case PC_ATTR:

      switch (pc_rhs)
	{
	case PC_ATTR:
	  /* TODO: add histogram selectivity */
	  break;

	case PC_CONST:
	case PC_HOST_VAR:
	  {
	    if (pc_rhs == PC_HOST_VAR)
	      {
		rhs_db_value = &env->parser->host_variables[rhs->info.host_var.index];
	      }
	    else
	      {
		rhs_db_value = &rhs->info.value.db_value;
	      }

	    if (pt_expr->info.expr.op == PT_GE)
	      {
		histogram_get_comp_selectivity (lhs, rhs_db_value, true, true, &selectivity, &success);
	      }
	    else if (pt_expr->info.expr.op == PT_GT)
	      {
		histogram_get_comp_selectivity (lhs, rhs_db_value, true, false, &selectivity, &success);
	      }
	    else if (pt_expr->info.expr.op == PT_LE)
	      {
		histogram_get_comp_selectivity (lhs, rhs_db_value, false, true, &selectivity, &success);
	      }
	    else if (pt_expr->info.expr.op == PT_LT)
	      {
		histogram_get_comp_selectivity (lhs, rhs_db_value, false, false, &selectivity, &success);
	      }
	  }
	  break;

	default:
	  break;
	}

      break;

    case PC_CONST:
    case PC_HOST_VAR:
      {
	switch (pc_rhs)
	  {
	  case PC_ATTR:
	    {
	      if (pc_lhs == PC_HOST_VAR)
		{
		  lhs_db_value = &env->parser->host_variables[lhs->info.host_var.index];
		}
	      else
		{
		  lhs_db_value = &lhs->info.value.db_value;
		}
	      if (pt_expr->info.expr.op == PT_GE)
		{
		  histogram_get_comp_selectivity (rhs, lhs_db_value, false, false, &selectivity, &success);
		}
	      else if (pt_expr->info.expr.op == PT_GT)
		{
		  histogram_get_comp_selectivity (rhs, lhs_db_value, false, true, &selectivity, &success);
		}
	      else if (pt_expr->info.expr.op == PT_LE)
		{
		  histogram_get_comp_selectivity (rhs, lhs_db_value, true, false, &selectivity, &success);
		}
	      else if (pt_expr->info.expr.op == PT_LT)
		{
		  histogram_get_comp_selectivity (rhs, lhs_db_value, true, true, &selectivity, &success);
		}
	      break;
	    }
	  default:
	    break;
	  }
      }
      break;

    case PC_MULTI_ATTR:
      switch (pc_rhs)
	{
	case PC_MULTI_ATTR:
	  /* (attr,attr) = (attr,attr) */
	  /* TODO: add histogram selectivity */
	  break;

	default:
	  break;
	}

      break;
    default:
      break;
    }

  return success ? selectivity : DEFAULT_COMP_SELECTIVITY;
}

double
qo_between_selectivity (QO_ENV * env, PT_NODE * pt_expr)
{
  PT_NODE *and_node;

  and_node = pt_expr->info.expr.arg2;

  QO_ASSERT (env, and_node->node_type == PT_EXPR);
  QO_ASSERT (env, pt_is_between_range_op (and_node->info.expr.op));

  return DEFAULT_BETWEEN_SELECTIVITY;
}

double
qo_range_selectivity (QO_ENV * env, PT_NODE * pt_expr)
{
  PT_NODE *lhs, *arg1, *arg2;
  DB_VALUE *lhs_db_value;
  DB_VALUE *arg1_db_value;
  DB_VALUE *arg2_db_value;
  PRED_CLASS pc1, pc2;
  PRED_CLASS pc_arg1, pc_arg2;

  double total_selectivity;
  double selectivity = DEFAULT_BETWEEN_SELECTIVITY;
  int lhs_icard = 0, rhs_icard = 0, icard = 0;
  PT_NODE *range_node;
  PT_OP_TYPE op_type;

  lhs = pt_expr->info.expr.arg1;

  pc2 = qo_classify (lhs);

  /* the only interesting case is 'attr RANGE {=1,=2}' or '(attr,attr) RANGE {={..},..}' */
  if (pc2 == PC_MULTI_ATTR)
    {
      lhs = lhs->info.function.arg_list;
      lhs_icard = 0;
      for ( /* none */ ; lhs; lhs = lhs->next)
	{
	  /* get index cardinality */
	  icard = qo_index_cardinality (env, lhs);
	  if (icard <= 0)
	    {
	      /* the only interesting case is PT_BETWEEN_EQ_NA */
	      icard = 1 / DEFAULT_EQUAL_SELECTIVITY;
	    }
	  if (lhs_icard == 0)
	    {
	      /* first time */
	      lhs_icard = icard;
	    }
	  else
	    {
	      lhs_icard *= icard;
	    }
	}
    }
  else if (pc2 == PC_ATTR)
    {
      /* get index cardinality */
      lhs_icard = qo_index_cardinality (env, lhs);
    }
  else
    {
      return DEFAULT_RANGE_SELECTIVITY;
    }
#if 1				/* unused anymore - DO NOT DELETE ME */
  QO_ASSERT (env, !PT_EXPR_INFO_IS_FLAGED (pt_expr, PT_EXPR_INFO_FULL_RANGE));
#endif

  total_selectivity = 0.0;

  for (range_node = pt_expr->info.expr.arg2; range_node; range_node = range_node->or_next)
    {
      QO_ASSERT (env, range_node->node_type == PT_EXPR);

      op_type = range_node->info.expr.op;
      QO_ASSERT (env, pt_is_between_range_op (op_type));

      arg1 = range_node->info.expr.arg1;
      arg2 = range_node->info.expr.arg2;

      pc_arg1 = qo_classify (arg1);
      pc1 = pc_arg1;

      if (pc_arg1 == PC_HOST_VAR)
	{
	  arg1_db_value = &env->parser->host_variables[arg1->info.host_var.index];
	}
      else if (pc_arg1 == PC_CONST)
	{
	  arg1_db_value = &arg1->info.value.db_value;
	}
      else
	{
	  arg1_db_value = NULL;
	}

      if (arg2 != NULL)
	{
	  pc_arg2 = qo_classify (arg2);

	  if (pc_arg2 == PC_HOST_VAR)
	    {
	      arg2_db_value = &env->parser->host_variables[arg2->info.host_var.index];
	    }
	  else if (pc_arg2 == PC_CONST)
	    {
	      arg2_db_value = &arg2->info.value.db_value;
	    }
	  else
	    {
	      arg2_db_value = NULL;
	    }
	}
      else
	{
	  arg2_db_value = NULL;
	}

      if (op_type == PT_BETWEEN_GE_LE || op_type == PT_BETWEEN_GE_LT || op_type == PT_BETWEEN_GT_LE
	  || op_type == PT_BETWEEN_GT_LT || op_type == PT_BETWEEN_INF_LT || op_type == PT_BETWEEN_INF_LE
	  || op_type == PT_BETWEEN_GE_INF || op_type == PT_BETWEEN_GT_INF)
	{
	  double selectivity_a = 0.0, selectivity_b = 0.0, selectivity_backup = selectivity;
	  bool success1 = false;
	  bool success2 = false;
	  bool success3 = false;
	  switch (op_type)
	    {
	    case PT_BETWEEN_GE_LE:
	      {
		/* selectivity = sel_le(b) - sel_lt(a) */
		histogram_get_comp_selectivity (lhs, arg1_db_value, false, false, &selectivity_a, &success1);
		histogram_get_comp_selectivity (lhs, arg2_db_value, false, true, &selectivity_b, &success2);
		selectivity = selectivity_b - selectivity_a;
		break;
	      }
	    case PT_BETWEEN_GE_LT:
	      {
		/* selectivity = sel_lt(b) - sel_lt(a) */
		histogram_get_comp_selectivity (lhs, arg1_db_value, false, false, &selectivity_a, &success1);
		histogram_get_comp_selectivity (lhs, arg2_db_value, false, false, &selectivity_b, &success2);
		selectivity = selectivity_b - selectivity_a;
		break;
	      }
	    case PT_BETWEEN_GT_LE:
	      {
		/* selectivity = sel_le(b) - sel_le(a) */
		histogram_get_comp_selectivity (lhs, arg1_db_value, false, true, &selectivity_a, &success1);
		histogram_get_comp_selectivity (lhs, arg2_db_value, false, true, &selectivity_b, &success2);
		selectivity = selectivity_b - selectivity_a;
		break;
	      }
	    case PT_BETWEEN_GT_LT:
	      {
		/* selectivity = sel_lt(b) - sel_le(a) */
		histogram_get_comp_selectivity (lhs, arg1_db_value, false, true, &selectivity_a, &success1);
		histogram_get_comp_selectivity (lhs, arg2_db_value, false, false, &selectivity_b, &success2);
		selectivity = selectivity_b - selectivity_a;
		break;
	      }
	    case PT_BETWEEN_INF_LT:
	      {
		histogram_get_comp_selectivity (lhs, arg1_db_value, false, false, &selectivity_a, &success1);
		success2 = true;
		selectivity = selectivity_a;
		break;
	      }
	    case PT_BETWEEN_INF_LE:
	      {
		histogram_get_comp_selectivity (lhs, arg1_db_value, false, true, &selectivity_a, &success1);
		success2 = true;
		selectivity = selectivity_a;
		break;
	      }
	    case PT_BETWEEN_GT_INF:
	      {
		histogram_get_comp_selectivity (lhs, arg1_db_value, true, false, &selectivity_a, &success1);
		success2 = true;
		selectivity = selectivity_a;
		break;
	      }
	    case PT_BETWEEN_GE_INF:
	      {
		histogram_get_comp_selectivity (lhs, arg1_db_value, true, true, &selectivity_a, &success1);
		success2 = true;
		selectivity = selectivity_a;
		break;
	      }
	    default:
	      break;
	    }

	  double default_zero_selectivity = 0.0;
	  histogram_get_default_selectivity (lhs, arg1_db_value, &default_zero_selectivity, &success3);

	  if (success3)
	    {
	      selectivity = MAX (selectivity, default_zero_selectivity);
	    }

	  if (!(success1 && success2))
	    {
	      if (op_type == PT_BETWEEN_INF_LT || op_type == PT_BETWEEN_INF_LE || op_type == PT_BETWEEN_GE_INF
		  || op_type == PT_BETWEEN_GT_INF)
		{
		  selectivity = DEFAULT_COMP_SELECTIVITY;
		}
	      else
		{
		  selectivity = DEFAULT_BETWEEN_SELECTIVITY;
		}
	    }
	}
      else if (op_type == PT_BETWEEN_EQ_NA)
	{
	  /* PT_BETWEEN_EQ_NA have only one argument */

	  selectivity = DEFAULT_EQUAL_SELECTIVITY;

	  if (pc1 == PC_ATTR)
	    {
	      /* attr1 range (attr2 = ) */
	      rhs_icard = qo_index_cardinality (env, arg1);

	      icard = MAX (lhs_icard, rhs_icard);
	      if (icard != 0)
		{
		  selectivity = (1.0 / icard);
		}
	      else
		{
		  selectivity = DEFAULT_EQUIJOIN_SELECTIVITY;
		}
	    }
	  else
	    {
	      bool success = false;

	      histogram_get_equal_selectivity (lhs, arg1_db_value, &selectivity, &success);

	      if (!success)
		{
		  /* attr1 range (const = ) */
		  if (lhs_icard != 0)
		    {
		      selectivity = (1.0 / lhs_icard);
		    }
		  else
		    {
		      selectivity = DEFAULT_EQUAL_SELECTIVITY;
		    }
		}
	    }
	}


      selectivity = MAX (selectivity, 0.0);
      selectivity = MIN (selectivity, 1.0);

      total_selectivity = qo_or_selectivity (env, total_selectivity, selectivity);
      total_selectivity = MAX (total_selectivity, 0.0);
      total_selectivity = MIN (total_selectivity, 1.0);
    }

  return total_selectivity;
}

double
qo_all_some_in_selectivity (QO_ENV * env, PT_NODE * pt_expr)
{
  PRED_CLASS pc_lhs, pc_rhs;
  double list_card = 0.0, icard = 0.0;
  double equal_selectivity = 1.0;
  PT_NODE *lhs;
  PT_NODE *arg1, *arg2;

  /* To avoid repeated dereferencing */
  arg1 = pt_expr->info.expr.arg1;
  arg2 = pt_expr->info.expr.arg2;

  /* determine the class of each side of the range */
  pc_lhs = qo_classify (arg1);
  pc_rhs = qo_classify (arg2);

  /* The only interesting cases are: attr IN set or (attr,attr) IN set or attr IN subquery */
  if ((pc_lhs == PC_MULTI_ATTR || pc_lhs == PC_ATTR) && (pc_rhs == PC_SET || pc_rhs == PC_SUBQUERY))
    {
      if (pc_lhs == PC_MULTI_ATTR)
	{
	  for (lhs = arg1->info.function.arg_list; lhs; lhs = lhs->next)
	    {
	      /* get index cardinality */
	      icard = qo_index_cardinality (env, lhs);
	      if (icard > 0.0)
		{
		  equal_selectivity *= (1.0 / icard);
		}
	      else
		{
		  /* If no index, multiply by default selectivity for each attribute */
		  equal_selectivity *= DEFAULT_EQUAL_SELECTIVITY;
		}
	    }
	}
      else if (pc_lhs == PC_ATTR)
	{
	  /* check for index on the attribute.  */
	  icard = qo_index_cardinality (env, arg1);
	  if (icard > 0.0)
	    {
	      equal_selectivity *= (1.0 / icard);
	    }
	  else
	    {
	      equal_selectivity = DEFAULT_EQUAL_SELECTIVITY;
	    }
	}

      /* determine cardinality of set or subquery */
      if (pc_rhs == PC_SET)
	{
	  if (pt_is_function (arg2))
	    {
	      list_card = pt_length_of_list (arg2->info.function.arg_list);
	    }
	  else
	    {
	      list_card = pt_length_of_list (arg2->info.value.data_value.set);
	    }
	}
      else if (pc_rhs == PC_SUBQUERY)
	{
	  if (arg2->info.query.xasl)
	    {
	      list_card = ((XASL_NODE *) arg2->info.query.xasl)->cardinality;
	    }
	  else
	    {
	      /* legacy default list_card is 1000. Maybe it won't come in here */
	      list_card = 1000.0;
	    }
	}

      /* compute selectivity--cap at 0.5 */
      double in_selectivity = list_card * equal_selectivity;
      return in_selectivity > 0.5 ? 0.5 : in_selectivity;
    }

  return DEFAULT_IN_SELECTIVITY;
}

PRED_CLASS
qo_classify (PT_NODE * attr)
{
  switch (attr->node_type)
    {
    case PT_NAME:
    case PT_DOT_:
      return PC_ATTR;

    case PT_VALUE:
      if (PT_IS_SET_TYPE (attr))
	{
	  return PC_SET;
	}
      else if (attr->type_enum == PT_TYPE_NULL)
	{
	  return PC_OTHER;
	}
      else
	{
	  return PC_CONST;
	}

    case PT_HOST_VAR:
      return PC_HOST_VAR;

    case PT_SELECT:
    case PT_UNION:
    case PT_INTERSECTION:
    case PT_DIFFERENCE:
      return PC_SUBQUERY;

    case PT_FUNCTION:
      /* (attr,attr) or (?,?) */
      if (PT_IS_SET_TYPE (attr))
	{
	  PT_NODE *func_arg;
	  func_arg = attr->info.function.arg_list;
	  for ( /* none */ ; func_arg; func_arg = func_arg->next)
	    {
	      if (func_arg->node_type == PT_NAME)
		{
		  /* none */
		}
	      else if (func_arg->node_type == PT_HOST_VAR)
		{
		  return PC_SET;
		}
	      else
		{
		  return PC_OTHER;
		}
	    }
	  return PC_MULTI_ATTR;
	}
      else
	{
	  return PC_OTHER;
	}

    case PT_EXPR:
      if (pt_is_function_index_expression (attr))
	{
	  return PC_ATTR;
	}
      [[fallthrough]];
    default:
      return PC_OTHER;
    }
}

double
qo_like_selectivity (QO_ENV * env, PT_NODE * pt_expr)
{
  PT_NODE *lhs, *rhs;
  DB_VALUE *host_var = NULL;
  PRED_CLASS pc_lhs, pc_rhs;

  double selectivity = 0.0;

  PT_NODE *like_node = pt_expr;
  bool success = false;

  lhs = like_node->info.expr.arg1;
  rhs = like_node->info.expr.arg2;

  if (lhs && rhs)
    {
      pc_lhs = qo_classify (lhs);
      pc_rhs = qo_classify (rhs);

      if (pc_lhs == PC_ATTR)
	{
	  if (pc_rhs == PC_CONST)
	    {
	      host_var = &rhs->info.value.db_value;
	    }
	  else if (pc_rhs == PC_HOST_VAR)
	    {
	      host_var = &env->parser->host_variables[rhs->info.host_var.index];
	    }

	  histogram_get_like_selectivity (lhs, host_var, &selectivity, &success);

	  if (!success)
	    {
	      selectivity = (double) prm_get_float_value (PRM_ID_LIKE_TERM_SELECTIVITY);
	    }
	}
      else
	{
	  selectivity = (double) prm_get_float_value (PRM_ID_LIKE_TERM_SELECTIVITY);
	}
    }

  return selectivity;
}
