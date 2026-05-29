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
 * The executor passes sample-level statistics (d, f1, row counts, weight); this module
 * extrapolates to the full table NDV stored in disk_attr.ndv.
 */

#ident "$Id$"

#include "statistics.h"

#include <math.h>

/*
 * stats_estimate_ndv_from_sample () - estimate table NDV from sample statistics
 *
 *   return: estimated NDV for non-null values (at least 1 when the sample has data)
 *
 * Notation (all counts refer to the sample unless noted):
 *   n           - non-null rows in the sample
 *   d           - distinct non-null values in the sample (sample_distinct)
 *   f1          - values that appear exactly once in the sample (sample_singleton)
 *   N_nn        - estimated non-null rows in the table (sample_rows * weight * (1 - null_frac))
 *
 * Estimation pipeline (sampling_weight > 1 only uses extrapolation beyond d):
 *   1. Fast paths for full scan, all-unique sample, or no singletons in sample.
 *   2. Base estimator: n*d / (n - f1 + f1*n/N_nn)  (duplicate-aware extrapolation).
 *   3. Singleton boost: d + f1 * (N_nn/n - 1) when base estimate is too low for
 *      heavy-skew columns where almost every sample distinct is a singleton.
 *   4. Density boost: d * (N_nn/n) when sample has many multi-occurrence distincts
 *      (mixed frequency) and a large fraction of table distincts were not seen.
 *   5. Clamp to [d, N_nn].
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

  /* Table row estimate before null adjustment (COUNT(*) * SAMPLING_SCAN weight). */
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

  d = (double) in->sample_distinct;
  f1 = (double) in->sample_singleton;
  n = (double) (in->sample_rows - in->sample_nulls);

  if (n <= 0.0 || d <= 0.0)
    {
      return 1;
    }

  /* Full scan or sample already contains one row per distinct (non-null). */
  if (weight <= 1 && d >= n_nn_total - 0.5)
    {
      ndv = (INT64) (n_nn_total + 0.5);
      return ndv;
    }

  nmultiple = d - f1;

  /*
   * Sample has no within-value duplicates (f1 == d): every sampled row is a distinct
   * value, so table NDV cannot exceed the scaled sample size.
   */
  if (nmultiple <= 0.0)
    {
      ndv = (INT64) (n_nn_total + 0.5);
      if (ndv < 1)
	{
	  ndv = 1;
	}
      return ndv;
    }

  /*
   * Every distinct value in the sample appears at least twice: no singleton signal.
   * Use sample distinct count as the estimate (bounded by N_nn).
   */
  if (f1 <= 0.0)
    {
      ndv = (INT64) (d + 0.5);
      if (ndv < 1)
	{
	  ndv = 1;
	}
      if (ndv > (INT64) (n_nn_total + 0.5))
	{
	  ndv = (INT64) (n_nn_total + 0.5);
	}
      return ndv;
    }

  if (n_nn_total <= 0.0)
    {
      ndv = (INT64) (d + 0.5);
      return ndv;
    }

  /* Base duplicate-aware extrapolation. */
  est = (n * d) / ((n - f1) + (f1 * n / n_nn_total));

  /*
   * Singleton boost (skew / long-tail):
   * When f1 is a large fraction of d but d << N_nn, the base formula often falls below d
   * after clamping. Each sample singleton represents roughly (N_nn/n) additional singleton
   * values in the table that were not observed in the sample.
   */
  if (weight > 1 && f1 > 0.0 && n > 0.0 && d < n_nn_total)
    {
      double singleton_boost = d + f1 * ((n_nn_total / n) - 1.0);

      if (singleton_boost > est)
	{
	  est = singleton_boost;
	}
    }

  /*
   * Density boost (mixed frequency):
   * When many distinct values in the sample appear more than once (f1 << d) but a large
   * share of table distincts never appear in the sample, scale observed distinct count
   * linearly to table size. Skipped when f1 ~ d (skew) or d/n is very small (low NDV).
   */
  if (weight > 1 && f1 > 0.0 && n > 0.0 && d < n_nn_total)
    {
      double f1_ratio = f1 / d;
      double d_ratio = d / n;
      double density_boost = d * (n_nn_total / n);

      if (f1_ratio < 0.95 && d_ratio > 0.1 && density_boost > est)
	{
	  est = density_boost;
	}
    }

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
