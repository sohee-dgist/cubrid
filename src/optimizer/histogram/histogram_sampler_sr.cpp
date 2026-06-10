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

#ifdef RESERVOIR_SAMPLING

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

#include "memory_wrapper.hpp"	// XXX: SHOULD BE THE LAST INCLUDE HEADER

/* default reservoir capacity when the caller does not specify one */
#define HISTOGRAM_DEFAULT_SAMPLE_SIZE 10000

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
      case DB_TYPE_VARCHAR:
      case DB_TYPE_NCHAR:
      case DB_TYPE_VARNCHAR:
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

  /* number of most-common-values: distinct values whose sample frequency exceeds
   * total_sample * (0.5 / max_buckets). Mirrors MCV_COUNT_QUERY_TEMPLATE. */
  template <typename T>
  int
  count_mcv (const std::vector<std::pair<T, std::int64_t>> &vc, std::int64_t total_sample, int max_buckets)
  {
    if (total_sample <= 0 || max_buckets <= 0)
      {
	return 0;
      }
    double threshold = static_cast<double> (total_sample) * (0.5 / max_buckets);
    int n = 0;
    for (const auto &p : vc)
      {
	if (static_cast<double> (p.second) > threshold)
	  {
	    n++;
	  }
      }
    return n;
  }

  /* build the histogram blob from a typed sample */
  template <typename T>
  char *
  build_blob (THREAD_ENTRY *thread_p, std::vector<T> &samples, DB_TYPE attr_type, int max_buckets, int *blob_length)
  {
    std::vector<std::pair<T, std::int64_t>> vc = group_counts (samples);
    std::int64_t total_sample = 0;
    for (const auto &p : vc)
      {
	total_sample += p.second;
      }
    int num_mcv = count_mcv (vc, total_sample, max_buckets);

    std::vector<hist::sample_bucket<T>> buckets = hist::bucketize_sample<T> (vc, max_buckets, num_mcv);

    hist::HistogramBuilder builder;
    for (const hist::sample_bucket<T> &b : buckets)
      {
	builder.add (b.endpoint, b.cumulative, b.approx_ndv);
      }
    return builder.build (thread_p, attr_type, blob_length);
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

  if (sample_size <= 0)
    {
      sample_size = HISTOGRAM_DEFAULT_SAMPLE_SIZE;
    }
  if (max_buckets < 1)
    {
      max_buckets = 1;
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
      if (error == NO_ERROR && !samples.empty ())
	{
	  *histogram_blob = build_blob<std::int64_t> (thread_p, samples, attr_type, max_buckets, blob_length);
	}
      break;
    }
    case value_category::real:
    {
      std::vector<double> samples;
      error = scan_and_collect<double> (thread_p, class_oid, hfid, attr_id, sample_size, samples, &total_rows,
					&null_rows);
      if (error == NO_ERROR && !samples.empty ())
	{
	  *histogram_blob = build_blob<double> (thread_p, samples, attr_type, max_buckets, blob_length);
	}
      break;
    }
    case value_category::string:
    {
      std::vector<std::string> samples;
      error = scan_and_collect<std::string> (thread_p, class_oid, hfid, attr_id, sample_size, samples, &total_rows,
	      &null_rows);
      if (error == NO_ERROR && !samples.empty ())
	{
	  *histogram_blob = build_blob<std::string> (thread_p, samples, attr_type, max_buckets, blob_length);
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

#endif /* RESERVOIR_SAMPLING */
