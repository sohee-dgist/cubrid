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
 * histogram_sampler_sr.cpp - server-side histogram collection by full-scan reservoir sampling
 *
 *  Replaces the deprecated query-based path (db_compile_and_execute_local with the
 *  SAMPLING_SCAN hint). One server request performs a single full heap scan of the
 *  class, draws a fixed-size uniform reservoir sample of the target attribute's
 *  non-null values (Algorithm R), counts NULLs and total rows exactly, then builds
 *  the histogram blob from the sample using the same bucketing the CTE used to do.
 */

#include "config.h"


#include "histogram_sampler_sr.hpp"

#include <string>
#include <utility>
#include <vector>

#include "dbtype.h"
#include "error_manager.h"
#include "heap_file.h"
#include "histogram_bucketizer.hpp"
#include "histogram_builder.hpp"
#include "log_impl.h"
#include "object_representation.h"
#include "reservoir_sampler.hpp"
#include "statistics.h"

#include "memory_wrapper.hpp"	// XXX: SHOULD BE THE LAST INCLUDE HEADER

/* Row-based reservoir capacity, used when the caller does not specify a sample size.
 * Sized proportionally to the bucket count (à la PostgreSQL's 300 * statistics_target,
 * from the Chaudhuri-Motwani-Narasayya equi-depth error bound) and clamped to a sane
 * row range. NOTE: this is a number of ROWS (values), not pages. */
#define HISTOGRAM_SAMPLE_ROWS_PER_BUCKET 300
#define HISTOGRAM_MIN_SAMPLE_ROWS 10000
#define HISTOGRAM_MAX_SAMPLE_ROWS 300000

namespace
{
  /* value category derived from the attribute domain type */
  enum class value_category
  {
    unsupported,
    integer,			/* -> std::int64_t */
    real,			/* -> double */
    string			/* -> std::string */
  };

  value_category
  category_of (DB_TYPE type)
  {
    switch (type)
      {
      case DB_TYPE_SHORT:
      case DB_TYPE_INTEGER:
      case DB_TYPE_BIGINT:
	return value_category::integer;
      case DB_TYPE_FLOAT:
      case DB_TYPE_DOUBLE:
	return value_category::real;
      case DB_TYPE_CHAR:
      case DB_TYPE_VARCHAR:	/* == DB_TYPE_STRING */
	return value_category::string;
      default:
	return value_category::unsupported;
      }
  }

  /* group a sorted-able sample into distinct (value, count) pairs */
  template <typename T>
  std::vector<std::pair<T, std::int64_t>>
  group_counts (std::vector<T> &samples)
  {
    std::vector<std::pair<T, std::int64_t>> vc;
    if (samples.empty ())
      {
	return vc;
      }
    std::sort (samples.begin (), samples.end ());
    T cur = samples[0];
    std::int64_t cnt = 0;
    for (const T &v : samples)
      {
	if (v == cur)
	  {
	    cnt++;
	  }
	else
	  {
	    vc.emplace_back (cur, cnt);
	    cur = v;
	    cnt = 1;
	  }
      }
    vc.emplace_back (cur, cnt);
    return vc;
  }

  /* build the histogram blob (format v2) from a typed sample.
   *   n_total : population rows incl nulls
   *   n_nn    : population non-null rows (exact, from the full reservoir scan)
   * MCVs are selected by the PG analyze_mcv_list criterion and stored in their own blob
   * section with population frequencies; the remaining values form the equi-depth buckets. */
  template <typename T>
  char *
  build_blob (THREAD_ENTRY *thread_p, std::vector<T> &samples, DB_TYPE attr_type, int max_buckets,
	      std::int64_t n_total, std::int64_t n_nn, int *blob_length)
  {
    std::vector<std::pair<T, std::int64_t>> vc = group_counts (samples);
    std::int64_t total_sample = 0;
    std::int64_t f1_total = 0;
    for (const auto &p : vc)
      {
	total_sample += p.second;
	if (p.second == 1)
	  {
	    f1_total++;
	  }
      }
    const std::int64_t d_total = (std::int64_t) vc.size ();

    /* estimated population NDV (non-null) */
    INT64 d_pop = d_total;
    if (total_sample > 0 && d_total > 0)
      {
	STATS_NDV_SAMPLE_INPUT in;
	in.sample_rows = total_sample;
	in.sample_nulls = 0;
	in.sample_distinct = d_total;
	in.sample_singleton = f1_total;
	in.sampling_weight = 1;
	in.total_nn_rows = n_nn;	/* exact population non-null (full reservoir scan) */
	d_pop = stats_estimate_ndv_from_sample (&in);
      }

    /* ---- MCV selection (PG analyze_mcv_list) ----
     * candidates = top-`mcv_cap` distinct values by sample count (ties: smaller value).
     * analyze_mcv_list decides how many of those leading candidates genuinely qualify. */
    const std::size_t ncand = vc.size ();
    std::vector<std::size_t> order (ncand);
    for (std::size_t i = 0; i < ncand; i++)
      {
	order[i] = i;
      }
    int mcv_cap = max_buckets;
    if (mcv_cap < 0)
      {
	mcv_cap = 0;
      }
    if (static_cast<std::size_t> (mcv_cap) > ncand)
      {
	mcv_cap = static_cast<int> (ncand);
      }
    std::partial_sort (order.begin (), order.begin () + mcv_cap, order.end (),
		       [&vc] (std::size_t a, std::size_t b)
    {
      if (vc[a].second != vc[b].second)
	{
	  return vc[a].second > vc[b].second;
	}
      return vc[a].first < vc[b].first;
    });

    int num_mcv = 0;
    if (mcv_cap > 0)
      {
	std::vector<INT64> cand_counts (mcv_cap);
	for (int i = 0; i < mcv_cap; i++)
	  {
	    cand_counts[i] = vc[order[i]].second;
	  }
	num_mcv = stats_analyze_mcv_list (cand_counts.data (), mcv_cap, (double) d_pop, 0.0, total_sample,
					  (double) n_nn);
      }

    std::vector<bool> is_mcv (ncand, false);
    for (int i = 0; i < num_mcv; i++)
      {
	is_mcv[order[i]] = true;
      }

    /* MCVs sorted ascending by value (builder requires ascending for binary search) */
    std::vector<std::pair<T, std::int64_t>> mcvs;
    mcvs.reserve (num_mcv);
    for (int i = 0; i < num_mcv; i++)
      {
	mcvs.push_back (vc[order[i]]);
      }
    std::sort (mcvs.begin (), mcvs.end (),
	       [] (const std::pair<T, std::int64_t> &a, const std::pair<T, std::int64_t> &b)
    {
      return a.first < b.first;
    });

    /* non-MCV values -> equi-depth histogram */
    std::vector<std::pair<T, std::int64_t>> nonmcv;
    nonmcv.reserve (ncand - num_mcv);
    for (std::size_t i = 0; i < ncand; i++)
      {
	if (!is_mcv[i])
	  {
	    nonmcv.push_back (vc[i]);
	  }
      }
    std::vector<hist::sample_bucket<T>> buckets = hist::bucketize_sample<T> (nonmcv, max_buckets);

    /* scaling sample -> population */
    const double scale_nn = (total_sample > 0) ? (double) n_nn / (double) total_sample : 1.0;
    double nonmcv_ratio = 1.0;
    {
      double d_nonmcv_sample = (double) (d_total - num_mcv);
      double d_nonmcv_pop = (double) d_pop - (double) num_mcv;
      if (d_nonmcv_pop < d_nonmcv_sample)
	{
	  d_nonmcv_pop = d_nonmcv_sample;	/* population non-mcv distinct >= sample's */
	}
      if (d_nonmcv_sample > 0.0)
	{
	  nonmcv_ratio = d_nonmcv_pop / d_nonmcv_sample;
	}
    }

    hist::HistogramBuilder builder;

    /* MCV freq = (within-non-null sample fraction) * (population non-null fraction) */
    for (const auto &m : mcvs)
      {
	double freq = 0.0;
	if (total_sample > 0 && n_total > 0)
	  {
	    freq = ((double) m.second / (double) total_sample) * ((double) n_nn / (double) n_total);
	  }
	builder.add_mcv (m.first, freq);
      }

    /* buckets: scale cumulative rows + approx_ndv to the population */
    for (const hist::sample_bucket<T> &b : buckets)
      {
	std::int64_t pop_cum = (std::int64_t) ((double) b.cumulative * scale_nn + 0.5);
	std::int64_t ndv = (std::int64_t) (b.approx_ndv * nonmcv_ratio + 0.5);
	std::int64_t bucket_pop_rows = (std::int64_t) ((double) b.rows_in_bucket * scale_nn + 0.5);
	if (pop_cum < 0)
	  {
	    pop_cum = 0;
	  }
	if (ndv < 1)
	  {
	    ndv = 1;
	  }
	if (bucket_pop_rows >= 1 && ndv > bucket_pop_rows)
	  {
	    ndv = bucket_pop_rows;	/* distinct cannot exceed rows in the bucket */
	  }
	builder.add (b.endpoint, pop_cum, ndv);
      }

    const double null_frequency = (n_total > 0) ? (double) (n_total - n_nn) / (double) n_total : 0.0;
    return builder.build (thread_p, attr_type, n_total, null_frequency, blob_length);
  }

  /* extract the typed value of the target attribute from a decoded record into the
   * matching reservoir; returns false on NULL (so the caller counts it). */
  template <typename T>
  bool extract (const DB_VALUE *v, cubsampling::reservoir_sampler<T> &rs);

  template <>
  bool
  extract<std::int64_t> (const DB_VALUE *v, cubsampling::reservoir_sampler<std::int64_t> &rs)
  {
    std::int64_t out;
    switch (DB_VALUE_TYPE (v))
      {
      case DB_TYPE_SHORT:
	out = db_get_short (v);
	break;
      case DB_TYPE_INTEGER:
	out = db_get_int (v);
	break;
      case DB_TYPE_BIGINT:
	out = db_get_bigint (v);
	break;
      default:
	return false;
      }
    rs.add (out);
    return true;
  }

  template <>
  bool
  extract<double> (const DB_VALUE *v, cubsampling::reservoir_sampler<double> &rs)
  {
    switch (DB_VALUE_TYPE (v))
      {
      case DB_TYPE_FLOAT:
	rs.add (static_cast<double> (db_get_float (v)));
	return true;
      case DB_TYPE_DOUBLE:
	rs.add (db_get_double (v));
	return true;
      default:
	return false;
      }
  }

  template <>
  bool
  extract<std::string> (const DB_VALUE *v, cubsampling::reservoir_sampler<std::string> &rs)
  {
    const char *s = db_get_string (v);
    int len = db_get_string_size (v);
    if (s == NULL || len < 0)
      {
	return false;
      }
    rs.add (std::string (s, static_cast<std::size_t> (len)));
    return true;
  }

  /* full heap scan; reservoir-sample non-null target values; count nulls and rows */
  template <typename T>
  int
  scan_and_collect (THREAD_ENTRY *thread_p, const OID *class_oid, const HFID *hfid, ATTR_ID attr_id,
		    int sample_size, std::vector<T> &out_samples, std::int64_t *total_rows,
		    std::int64_t *null_rows)
  {
    HEAP_SCANCACHE scan_cache;
    HEAP_CACHE_ATTRINFO attr_info;
    RECDES recdes;
    OID inst_oid;
    OID scan_class_oid;
    SCAN_CODE sc;
    int error = NO_ERROR;
    bool scancache_inited = false;
    bool attrinfo_inited = false;
    MVCC_SNAPSHOT *snapshot = logtb_get_mvcc_snapshot (thread_p);

    cubsampling::reservoir_sampler<T> rs (static_cast<std::size_t> (sample_size));

    *total_rows = 0;
    *null_rows = 0;

    error = heap_scancache_start (thread_p, &scan_cache, hfid, class_oid, true /* cache_last_fix_page */ , snapshot);
    if (error != NO_ERROR)
      {
	ASSERT_ERROR ();
	goto cleanup;
      }
    scancache_inited = true;

    error = heap_attrinfo_start (thread_p, class_oid, 1, &attr_id, &attr_info);
    if (error != NO_ERROR)
      {
	ASSERT_ERROR ();
	goto cleanup;
      }
    attrinfo_inited = true;

    OID_SET_NULL (&inst_oid);
    recdes.data = NULL;
    scan_class_oid = *class_oid;

    while ((sc = heap_next (thread_p, hfid, &scan_class_oid, &inst_oid, &recdes, &scan_cache, PEEK)) == S_SUCCESS)
      {
	(*total_rows)++;

	error = heap_attrinfo_read_dbvalues (thread_p, &inst_oid, &recdes, &attr_info);
	if (error != NO_ERROR)
	  {
	    ASSERT_ERROR ();
	    goto cleanup;
	  }

	DB_VALUE *v = heap_attrinfo_access (attr_id, &attr_info);
	if (v == NULL || DB_IS_NULL (v))
	  {
	    (*null_rows)++;
	    continue;
	  }

	if (!extract<T> (v, rs))
	  {
	    /* type did not match the expected category: treat as null-ish, skip */
	    (*null_rows)++;
	  }
      }

    if (sc != S_END)
      {
	ASSERT_ERROR_AND_SET (error);
	goto cleanup;
      }

    out_samples = std::move (rs.samples ());
    error = NO_ERROR;

cleanup:
    if (attrinfo_inited)
      {
	heap_attrinfo_end (thread_p, &attr_info);
      }
    if (scancache_inited)
      {
	(void) heap_scancache_end (thread_p, &scan_cache);
      }
    return error;
  }

} // anonymous namespace

int
xhistogram_build_by_fullscan_reservoir (THREAD_ENTRY *thread_p, const OID *class_oid, const HFID *hfid,
					ATTR_ID attr_id, DB_TYPE attr_type, int max_buckets, int sample_size,
					double *null_frequency, char **histogram_blob, int *blob_length)
{
  int error = NO_ERROR;
  std::int64_t total_rows = 0;
  std::int64_t null_rows = 0;

  *histogram_blob = NULL;
  *blob_length = 0;
  *null_frequency = 0.0;

  if (max_buckets < 1)
    {
      max_buckets = 1;
    }
  if (sample_size <= 0)
    {
      /* size the row reservoir to the bucket count, clamped to [MIN, MAX] rows */
      std::int64_t s = (std::int64_t) HISTOGRAM_SAMPLE_ROWS_PER_BUCKET * max_buckets;
      if (s < HISTOGRAM_MIN_SAMPLE_ROWS)
	{
	  s = HISTOGRAM_MIN_SAMPLE_ROWS;
	}
      if (s > HISTOGRAM_MAX_SAMPLE_ROWS)
	{
	  s = HISTOGRAM_MAX_SAMPLE_ROWS;
	}
      sample_size = (int) s;
    }

  value_category cat = category_of (attr_type);
  if (cat == value_category::unsupported)
    {
      /* not histogrammable; nothing to build (caller pre-checks is_histogrammable_type) */
      return NO_ERROR;
    }

  switch (cat)
    {
    case value_category::integer:
    {
      std::vector<std::int64_t> samples;
      error = scan_and_collect<std::int64_t> (thread_p, class_oid, hfid, attr_id, sample_size, samples, &total_rows,
	      &null_rows);
      if (error == NO_ERROR)
	{
	  /* always build: an empty sample yields a header-only blob (matches the old path) */
	  *histogram_blob = build_blob<std::int64_t> (thread_p, samples, attr_type, max_buckets, total_rows,
			    total_rows - null_rows, blob_length);
	}
      break;
    }
    case value_category::real:
    {
      std::vector<double> samples;
      error = scan_and_collect<double> (thread_p, class_oid, hfid, attr_id, sample_size, samples, &total_rows,
					&null_rows);
      if (error == NO_ERROR)
	{
	  *histogram_blob = build_blob<double> (thread_p, samples, attr_type, max_buckets, total_rows,
			    total_rows - null_rows, blob_length);
	}
      break;
    }
    case value_category::string:
    {
      std::vector<std::string> samples;
      error = scan_and_collect<std::string> (thread_p, class_oid, hfid, attr_id, sample_size, samples, &total_rows,
	      &null_rows);
      if (error == NO_ERROR)
	{
	  *histogram_blob = build_blob<std::string> (thread_p, samples, attr_type, max_buckets, total_rows,
			    total_rows - null_rows, blob_length);
	}
      break;
    }
    default:
      break;
    }

  if (error != NO_ERROR)
    {
      return error;
    }

  if (total_rows > 0)
    {
      *null_frequency = static_cast<double> (null_rows) / static_cast<double> (total_rows);
    }

  if (*histogram_blob != NULL && *blob_length <= 0)
    {
      db_private_free_and_init (thread_p, *histogram_blob);
      return ER_FAILED;
    }

  return NO_ERROR;
}

/* target rows per column for the NDV reservoir (cf. PR #7228 STATS_NDV_TARGET_SAMPLE_ROWS) */
#define STATS_NDV_RESERVOIR_ROWS 30000

namespace
{
  /* canonical byte-string key of a DB_VALUE for distinct counting; false on unsupported/NULL */
  bool
  ndv_canonical_key (const DB_VALUE *v, value_category cat, std::string &key)
  {
    switch (cat)
      {
      case value_category::integer:
      {
	std::int64_t out;
	switch (DB_VALUE_TYPE (v))
	  {
	  case DB_TYPE_SHORT:
	    out = db_get_short (v);
	    break;
	  case DB_TYPE_INTEGER:
	    out = db_get_int (v);
	    break;
	  case DB_TYPE_BIGINT:
	    out = db_get_bigint (v);
	    break;
	  default:
	    return false;
	  }
	key.assign (reinterpret_cast<const char *> (&out), sizeof (out));
	return true;
      }
      case value_category::real:
      {
	double out;
	switch (DB_VALUE_TYPE (v))
	  {
	  case DB_TYPE_FLOAT:
	    out = static_cast<double> (db_get_float (v));
	    break;
	  case DB_TYPE_DOUBLE:
	    out = db_get_double (v);
	    break;
	  default:
	    return false;
	  }
	key.assign (reinterpret_cast<const char *> (&out), sizeof (out));
	return true;
      }
      case value_category::string:
      {
	const char *s = db_get_string (v);
	int len = db_get_string_size (v);
	if (s == NULL || len < 0)
	  {
	    return false;
	  }
	key.assign (s, static_cast<std::size_t> (len));
	return true;
      }
      default:
	return false;
      }
  }
} // anonymous namespace

int
xstats_collect_ndv_by_fullscan_reservoir (THREAD_ENTRY *thread_p, const OID *class_oid, const HFID *hfid,
					  const ATTR_ID *attr_ids, const DB_TYPE *attr_types, int attr_cnt,
					  INT64 *out_ndv, INT64 *out_total_rows)
{
  HEAP_SCANCACHE scan_cache;
  HEAP_CACHE_ATTRINFO attr_info;
  RECDES recdes;
  OID inst_oid;
  OID scan_class_oid;
  SCAN_CODE sc;
  int error = NO_ERROR;
  int i;
  bool scancache_inited = false;
  bool attrinfo_inited = false;
  MVCC_SNAPSHOT *snapshot = logtb_get_mvcc_snapshot (thread_p);

  *out_total_rows = 0;

  std::vector<value_category> cats (attr_cnt);
  std::vector<cubsampling::reservoir_sampler<std::string>> reservoirs;
  reservoirs.reserve (attr_cnt);
  for (i = 0; i < attr_cnt; i++)
    {
      cats[i] = category_of (attr_types[i]);
      reservoirs.emplace_back (STATS_NDV_RESERVOIR_ROWS);
      out_ndv[i] = -1;		/* unsupported / not computed by default */
    }

  error = heap_scancache_start (thread_p, &scan_cache, hfid, class_oid, true /* cache_last_fix_page */ , snapshot);
  if (error != NO_ERROR)
    {
      ASSERT_ERROR ();
      return error;
    }
  scancache_inited = true;

  error = heap_attrinfo_start (thread_p, class_oid, attr_cnt, attr_ids, &attr_info);
  if (error != NO_ERROR)
    {
      ASSERT_ERROR ();
      goto cleanup;
    }
  attrinfo_inited = true;

  OID_SET_NULL (&inst_oid);
  recdes.data = NULL;
  scan_class_oid = *class_oid;

  while ((sc = heap_next (thread_p, hfid, &scan_class_oid, &inst_oid, &recdes, &scan_cache, PEEK)) == S_SUCCESS)
    {
      (*out_total_rows)++;

      error = heap_attrinfo_read_dbvalues (thread_p, &inst_oid, &recdes, &attr_info);
      if (error != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  goto cleanup;
	}

      for (i = 0; i < attr_cnt; i++)
	{
	  if (cats[i] == value_category::unsupported)
	    {
	      continue;
	    }
	  DB_VALUE *v = heap_attrinfo_access (attr_ids[i], &attr_info);
	  if (v == NULL || DB_IS_NULL (v))
	    {
	      continue;		/* NULLs do not feed the NDV reservoir */
	    }
	  std::string key;
	  if (ndv_canonical_key (v, cats[i], key))
	    {
	      reservoirs[i].add (std::move (key));
	    }
	}
    }

  if (sc != S_END)
    {
      ASSERT_ERROR_AND_SET (error);
      goto cleanup;
    }

  /* per-column: (n, d, f1) from the reservoir -> dedicated NDV estimator */
  for (i = 0; i < attr_cnt; i++)
    {
      if (cats[i] == value_category::unsupported)
	{
	  continue;
	}

      std::vector<std::string> &samples = reservoirs[i].samples ();
      INT64 n_nn = (INT64) reservoirs[i].seen ();	/* exact non-null rows of the column */

      if (n_nn == 0)
	{
	  out_ndv[i] = 0;	/* all-NULL column */
	  continue;
	}

      std::sort (samples.begin (), samples.end ());
      INT64 d = 0, f1 = 0;
      std::size_t j = 0;
      while (j < samples.size ())
	{
	  std::size_t run = j + 1;
	  while (run < samples.size () && samples[run] == samples[j])
	    {
	      run++;
	    }
	  d++;
	  if (run - j == 1)
	    {
	      f1++;
	    }
	  j = run;
	}

      INT64 k = (INT64) samples.size ();
      STATS_NDV_SAMPLE_INPUT in;
      in.sample_rows = k;	/* reservoir holds non-null values only */
      in.sample_nulls = 0;
      in.sample_distinct = d;
      in.sample_singleton = f1;
      in.sampling_weight = (int) MAX (1, (n_nn + k / 2) / k);	/* >1 disables the full-scan short-circuit when sampled */
      in.total_nn_rows = n_nn;	/* exact (full reservoir scan): N_nn used directly, no integer-weight rounding */

      out_ndv[i] = stats_estimate_ndv_from_sample (&in);
    }

  error = NO_ERROR;

cleanup:
  if (attrinfo_inited)
    {
      heap_attrinfo_end (thread_p, &attr_info);
    }
  if (scancache_inited)
    {
      (void) heap_scancache_end (thread_p, &scan_cache);
    }
  return error;
}

