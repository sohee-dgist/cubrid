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
 * statistics_ndv.c - NDV (number of distinct values) estimation from a heap sample
 *
 * Used by UPDATE STATISTICS when SAMPLING_SCAN collects COUNT(DISTINCT col) and COUNT(*).
 * The executor passes sample-level statistics; this module extrapolates to the full
 * table NDV stored in disk_attr.ndv.
 */

#ident "$Id$"

#include "statistics.h"

#include "heap_file.h"

#include <math.h>

/* NDV: visit ~6x more heap pages (lower page skip weight) and ~1/12 row inclusion. */
#define STATS_NDV_PAGE_WEIGHT_DIVISOR	6
#define STATS_NDV_ROW_BERNOULLI_P	(1.0f / 12.0f)

/*
 * stats_ndv_enable_row_bernoulli_sample () - NDV page + row sampling setup
 *
 * After scan_open_heap_scan () sets page weight, divide it by STATS_NDV_PAGE_WEIGHT_DIVISOR
 * so more pages are read.
 *
 * Bernoulli row thinning (STATS_NDV_ROW_BERNOULLI_P) is enabled only when page sampling
 * still skips pages (weight > 1). Small tables already use weight 1 (every page visited);
 * row sampling on top of that would double-thin without matching the estimator assumptions.
 */
void
stats_ndv_enable_row_bernoulli_sample (SAMPLING_INFO * sampling)
{
  if (sampling == NULL)
    {
      return;
    }

  sampling->ndv_row_sample_p = 0.0f;

  if (sampling->weight > 0)
    {
      sampling->weight = MAX (1, sampling->weight / STATS_NDV_PAGE_WEIGHT_DIVISOR);
    }

  if (sampling->weight > 1)
    {
      sampling->ndv_row_sample_p = STATS_NDV_ROW_BERNOULLI_P;
    }
}

 /*
  * stats_ndv_effective_sampling_weight () - adjust sampling weight for row sampling
  *
  * If page sampling keeps about 1 / sampling_weight of the table and row sampling
  * keeps row_sample_p of those rows, the total sampling probability is:
  *
  *   row_sample_p / sampling_weight
  *
  * Therefore, the effective expansion weight is:
  *
  *   sampling_weight / row_sample_p
  *
  * This function is used when building STATS_NDV_SAMPLE_INPUT.
  */
int
stats_ndv_effective_sampling_weight (int sampling_weight, float row_sample_p)
{
  int weight;
  int effective;

  weight = (sampling_weight > 0) ? sampling_weight : 1;

  if (row_sample_p <= 0.0f || row_sample_p >= 1.0f)
    {
      return weight;
    }

  effective = (int) ((double) weight / (double) row_sample_p + 0.5);

  return (effective < 1) ? 1 : effective;
}

 /*
  * stats_estimate_ndv_from_sample () - estimate table NDV from sample statistics
  *
  *   return: estimated NDV for non-null values
  *
  * Notation:
  *   n      - non-null rows in the sample
  *   d      - distinct non-null values in the sample
  *   f1     - values that appear exactly once in the sample
  *   N_nn   - estimated non-null rows in the table
  *
  * Estimation pipeline:
  *   1. If the sample has no repeated non-null value, treat the column as
  *      unique-like and return N_nn.
  *   2. If every sample distinct value is repeated, treat the column as a
  *      fixed-domain column and return d.
  *   3. Otherwise use the duplicate-aware estimator:
  *
  *        n * d / (n - f1 + f1 * n / N_nn)
  *
  *   4. Clamp the estimate to [d, N_nn].
  */
INT64
stats_estimate_ndv_from_sample (const STATS_NDV_SAMPLE_INPUT * in)
{
  double n;
  double d;
  double f1;
  double nmultiple;
  double total_rows;
  double null_frac;
  double n_nn_total;
  double est;
  INT64 ndv;
  int weight;

  if (in == NULL)
    {
      return 1;
    }

  weight = (in->sampling_weight > 0) ? in->sampling_weight : 1;

  if (in->sample_rows <= 0)
    {
      return 1;
    }

  /*
   * Estimate table rows before null adjustment.
   *
   * If Bernoulli row sampling is enabled, in->sampling_weight should already be
   * adjusted by stats_ndv_effective_sampling_weight().
   */
  total_rows = (double) in->sample_rows * (double) weight;
  if (total_rows < 1.0)
    {
      total_rows = 1.0;
    }

  null_frac = (double) in->sample_nulls / (double) in->sample_rows;
  if (null_frac < 0.0)
    {
      null_frac = 0.0;
    }
  else if (null_frac > 1.0)
    {
      null_frac = 1.0;
    }

  n_nn_total = total_rows * (1.0 - null_frac);

  n = (double) (in->sample_rows - in->sample_nulls);
  d = (double) in->sample_distinct;
  f1 = (double) in->sample_singleton;

  if (n <= 0.0 || d <= 0.0 || n_nn_total <= 0.0)
    {
      return 1;
    }

  /*
   * Sanitize inconsistent input.
   */
  if (d > n)
    {
      d = n;
    }

  if (f1 < 0.0)
    {
      f1 = 0.0;
    }
  else if (f1 > d)
    {
      f1 = d;
    }

  /*
   * Full scan or sample already covers the estimated non-null table rows.
   */
  if (weight <= 1 && d >= n_nn_total - 0.5)
    {
      ndv = (INT64) floor (n_nn_total + 0.5);
      return (ndv < 1) ? 1 : ndv;
    }

  nmultiple = d - f1;

  /*
   * No repeated non-null value was found in the sample.
   * Treat the column as unique-like.
   */
  if (nmultiple <= 0.0)
    {
      ndv = (INT64) floor (n_nn_total + 0.5);
      return (ndv < 1) ? 1 : ndv;
    }

  /*
   * Every distinct value in the sample appeared at least twice.
   * There is no singleton signal suggesting unseen distinct values, so use
   * the observed sample distinct count as the estimate.
   */
  if (f1 <= 0.0)
    {
      ndv = (INT64) floor (d + 0.5);

      if (ndv < 1)
	{
	  ndv = 1;
	}

      if (ndv > (INT64) floor (n_nn_total + 0.5))
	{
	  ndv = (INT64) floor (n_nn_total + 0.5);
	}

      return ndv;
    }

  /*
   * Duplicate-aware NDV extrapolation.
   */
  est = (n * d) / ((n - f1) + (f1 * n / n_nn_total));

  /*
   * Clamp to sane bounds.
   */
  if (est < d)
    {
      est = d;
    }

  if (est > n_nn_total)
    {
      est = n_nn_total;
    }

  ndv = (INT64) floor (est + 0.5);
  if (ndv < 1)
    {
      ndv = 1;
    }

  return ndv;
}
