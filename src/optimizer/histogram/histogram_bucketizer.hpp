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
 * histogram_bucketizer.hpp - turn a (reservoir) sample into histogram buckets
 *
 *  Pure, dependency-free re-implementation of the bucketing logic that used to be
 *  expressed as the HISTOGRAM_QUERY_TEMPLATE CTE in histogram_cl.hpp. It takes the
 *  distinct (value, count) pairs observed in a sample and produces the same bucket
 *  rows (endpoint / rows_in_bucket / cumulative / approx_ndv) that the query path
 *  fed into hist::HistogramBuilder::add ().
 *
 *  Algorithm (mirrors the deprecated CTE):
 *    1. Most-Common-Value buckets: the `num_mcv` values with the highest count
 *       (ties broken by smaller value) each become their own singleton bucket
 *       (rows = count, approx_ndv = 1).
 *    2. The remaining (non-MCV) values are split into segments delimited by the
 *       MCVs in value order (seg_id = number of MCVs seen so far in value order),
 *       and each segment is equi-depth bucketed with capacity
 *       cap = ceil (total_non_mcv_rows / max_buckets).
 *       Each bucket: endpoint = max value, rows = sum of counts,
 *       approx_ndv = number of distinct values in the bucket.
 *    3. All buckets are emitted ordered by endpoint, with a running `cumulative`.
 *
 *  Counts are taken verbatim from the sample (the deprecated query path likewise
 *  produced sample-relative counts); selectivity estimation works on ratios.
 */

#ifndef _HISTOGRAM_BUCKETIZER_HPP_
#define _HISTOGRAM_BUCKETIZER_HPP_

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace hist
{
  template <typename V>
  struct sample_bucket
  {
    V endpoint;			/* upper bound value of the bucket */
    std::int64_t rows_in_bucket;
    std::int64_t cumulative;	/* running sum of rows_in_bucket over endpoint order */
    std::int64_t approx_ndv;	/* distinct values represented by the bucket */
    bool is_mcv;
  };

  /*
   * bucketize_sample () - build histogram buckets from distinct (value, count) pairs.
   *   value_counts(in): distinct values with their observed counts (any order)
   *   max_buckets(in)  : target number of equi-depth buckets for the non-MCV part
   *   num_mcv(in)      : number of most-common-values to peel off as singleton buckets
   *   return           : buckets ordered by endpoint, with cumulative filled in
   */
  template <typename V>
  std::vector<sample_bucket<V>>
  bucketize_sample (std::vector<std::pair<V, std::int64_t>> value_counts, int max_buckets, int num_mcv)
  {
    std::vector<sample_bucket<V>> result;
    if (value_counts.empty ())
      {
	return result;
      }
    if (max_buckets < 1)
      {
	max_buckets = 1;
      }

    /* canonical order: by value ascending */
    std::sort (value_counts.begin (), value_counts.end (),
	       [] (const std::pair<V, std::int64_t> &a, const std::pair<V, std::int64_t> &b)
    {
      return a.first < b.first;
    });

    const std::size_t n = value_counts.size ();

    /* 1. pick MCVs: highest count first, ties broken by smaller value (matches
     *    ROW_NUMBER () OVER (ORDER BY c DESC, val) LIMIT num_mcv). */
    std::vector<std::size_t> order (n);
    for (std::size_t i = 0; i < n; i++)
      {
	order[i] = i;
      }
    if (num_mcv < 0)
      {
	num_mcv = 0;
      }
    if (static_cast<std::size_t> (num_mcv) > n)
      {
	num_mcv = static_cast<int> (n);
      }
    std::partial_sort (order.begin (), order.begin () + num_mcv, order.end (),
		       [&value_counts] (std::size_t a, std::size_t b)
    {
      if (value_counts[a].second != value_counts[b].second)
	{
	  return value_counts[a].second > value_counts[b].second;
	}
      return value_counts[a].first < value_counts[b].first;
    });

    std::vector<bool> is_mcv (n, false);
    for (int i = 0; i < num_mcv; i++)
      {
	is_mcv[order[i]] = true;
      }

    /* total rows held by non-MCV values -> equi-depth capacity */
    std::int64_t non_mcv_rows = 0;
    for (std::size_t i = 0; i < n; i++)
      {
	if (!is_mcv[i])
	  {
	    non_mcv_rows += value_counts[i].second;
	  }
      }
    std::int64_t cap = 1;
    if (non_mcv_rows > 0)
      {
	cap = (non_mcv_rows + max_buckets - 1) / max_buckets;	/* ceil */
	if (cap < 1)
	  {
	    cap = 1;
	  }
      }

    /* 2. walk values in ascending order; MCVs delimit segments and emit singletons.
     *    Within a segment, equi-depth bucket by running count using `cap`. */
    int seg_id = 0;
    std::int64_t seg_cum = 0;	/* running count inside current segment */
    std::int64_t cur_local_bid = -1;
    bool bucket_open = false;
    sample_bucket<V> cur{};

    auto flush_bucket = [&result, &cur, &bucket_open] ()
    {
      if (bucket_open)
	{
	  result.push_back (cur);
	  bucket_open = false;
	}
    };

    for (std::size_t i = 0; i < n; i++)
      {
	if (is_mcv[i])
	  {
	    /* close any open non-MCV bucket, bump segment, emit MCV singleton */
	    flush_bucket ();
	    seg_id++;
	    seg_cum = 0;
	    cur_local_bid = -1;

	    sample_bucket<V> mcv{};
	    mcv.endpoint = value_counts[i].first;
	    mcv.rows_in_bucket = value_counts[i].second;
	    mcv.approx_ndv = 1;
	    mcv.is_mcv = true;
	    result.push_back (mcv);
	    continue;
	  }

	seg_cum += value_counts[i].second;
	std::int64_t local_bid = (seg_cum - 1) / cap;	/* floor */

	if (!bucket_open || local_bid != cur_local_bid)
	  {
	    flush_bucket ();
	    cur = sample_bucket<V> {};
	    cur.endpoint = value_counts[i].first;
	    cur.rows_in_bucket = value_counts[i].second;
	    cur.approx_ndv = 1;
	    cur.is_mcv = false;
	    cur_local_bid = local_bid;
	    bucket_open = true;
	  }
	else
	  {
	    cur.endpoint = value_counts[i].first;	/* MAX(val) within bucket (ascending) */
	    cur.rows_in_bucket += value_counts[i].second;
	    cur.approx_ndv += 1;
	  }
      }
    flush_bucket ();

    /* 3. order by endpoint, fill cumulative */
    std::sort (result.begin (), result.end (),
	       [] (const sample_bucket<V> &a, const sample_bucket<V> &b)
    {
      return a.endpoint < b.endpoint;
    });
    std::int64_t running = 0;
    for (auto &b : result)
      {
	running += b.rows_in_bucket;
	b.cumulative = running;
      }
    return result;
  }

} // namespace hist

#endif // _HISTOGRAM_BUCKETIZER_HPP_
