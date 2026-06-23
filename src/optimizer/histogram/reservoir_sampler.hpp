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
 * reservoir_sampler.hpp - single-pass uniform reservoir sampling (Vitter, Algorithm R)
 *
 *  Draws a fixed-size uniform sample of K items from a stream of unknown length N in
 *  one pass, using O(K) memory. Every item ends up in the sample with probability K / N.
 *
 *  Two interfaces:
 *    reservoir_selector   - decision only. consider() returns the slot an item should
 *                           occupy, or NOT_SELECTED. Use when items own external
 *                           resources the caller must manage (e.g. DB_VALUE that needs
 *                           db_value_clone / db_value_clear).
 *    reservoir_sampler<T> - stores values. add() copies the item in. Use for
 *                           self-contained types (std::int64_t, double, std::string, ...).
 */

#ifndef _RESERVOIR_SAMPLER_HPP_
#define _RESERVOIR_SAMPLER_HPP_

#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace cubsampling
{
  /* default RNG seed: fixed so that statistics collection is reproducible for the
   * same input, mirroring the existing heap sampling scan (ftab_set.cpp). */
  constexpr std::uint64_t RESERVOIR_DEFAULT_SEED = 0x9E3779B97F4A7C15ULL;

  class reservoir_selector
  {
    public:
      static constexpr int NOT_SELECTED = -1;

      explicit reservoir_selector (std::size_t capacity, std::uint64_t seed = RESERVOIR_DEFAULT_SEED)
	: m_capacity (capacity)
	, m_filled (0)
	, m_seen (0)
	, m_rng (seed)
      {
      }

      /* Feed one stream item. Returns the slot index [0, capacity) the item must be
       * written to, or NOT_SELECTED if the item is dropped. */
      int consider ()
      {
	int slot;

	if (m_filled < m_capacity)
	  {
	    /* reservoir not full yet: always keep, fill next slot */
	    slot = static_cast<int> (m_filled);
	    m_filled++;
	  }
	else
	  {
	    /* reservoir full: replace a random slot with probability capacity / (seen + 1) */
	    std::uniform_int_distribution<std::uint64_t> dist (0, m_seen);
	    std::uint64_t j = dist (m_rng);
	    slot = (j < m_capacity) ? static_cast<int> (j) : NOT_SELECTED;
	  }

	m_seen++;
	return slot;
      }

      std::size_t capacity () const
      {
	return m_capacity;
      }

      /* number of slots currently occupied (== min (capacity, seen)) */
      std::size_t size () const
      {
	return m_filled;
      }

      /* total number of items fed so far (the population count seen on this pass) */
      std::uint64_t seen () const
      {
	return m_seen;
      }

      void reset ()
      {
	m_filled = 0;
	m_seen = 0;
      }

    private:
      std::size_t m_capacity;
      std::size_t m_filled;
      std::uint64_t m_seen;
      std::mt19937_64 m_rng;
  };

  template <typename T>
  class reservoir_sampler
  {
    public:
      explicit reservoir_sampler (std::size_t capacity, std::uint64_t seed = RESERVOIR_DEFAULT_SEED)
	: m_selector (capacity, seed)
      {
	m_reservoir.reserve (capacity);
      }

      void add (const T &value)
      {
	int slot = m_selector.consider ();
	if (slot == reservoir_selector::NOT_SELECTED)
	  {
	    return;
	  }
	if (static_cast<std::size_t> (slot) < m_reservoir.size ())
	  {
	    m_reservoir[slot] = value;
	  }
	else
	  {
	    /* slot == m_reservoir.size (): selector hands out fill slots in order */
	    m_reservoir.push_back (value);
	  }
      }

      void add (T &&value)
      {
	int slot = m_selector.consider ();
	if (slot == reservoir_selector::NOT_SELECTED)
	  {
	    return;
	  }
	if (static_cast<std::size_t> (slot) < m_reservoir.size ())
	  {
	    m_reservoir[slot] = std::move (value);
	  }
	else
	  {
	    m_reservoir.push_back (std::move (value));
	  }
      }

      /* Decide whether the next stream item is sampled WITHOUT materializing it. Returns the slot
       * to write via store (), or reservoir_selector::NOT_SELECTED to drop it. Lets the caller
       * build T only for items that are actually kept (e.g. skip a per-row std::string alloc for
       * the vast majority of values dropped once the reservoir is full). */
      int consider ()
      {
	return m_selector.consider ();
      }

      /* Write an item to the slot returned by consider (). */
      void store (int slot, T &&value)
      {
	if (static_cast<std::size_t> (slot) < m_reservoir.size ())
	  {
	    m_reservoir[slot] = std::move (value);
	  }
	else
	  {
	    m_reservoir.push_back (std::move (value));
	  }
      }

      void store (int slot, const T &value)
      {
	if (static_cast<std::size_t> (slot) < m_reservoir.size ())
	  {
	    m_reservoir[slot] = value;
	  }
	else
	  {
	    m_reservoir.push_back (value);
	  }
      }

      const std::vector<T> &samples () const
      {
	return m_reservoir;
      }

      std::vector<T> &samples ()
      {
	return m_reservoir;
      }

      std::size_t size () const
      {
	return m_reservoir.size ();
      }

      std::uint64_t seen () const
      {
	return m_selector.seen ();
      }

      void clear ()
      {
	m_reservoir.clear ();
	m_selector.reset ();
      }

    private:
      reservoir_selector m_selector;
      std::vector<T> m_reservoir;
  };

  /*
   * merge_partition_samples () - combine per-partition reservoir samples (produced by a
   *   parallel page-distributed scan) into a single sample of at most `capacity` items.
   *
   *   Each partition p contributed a uniform sample part_samples[p] drawn from the
   *   part_seen[p] non-null values it observed on its slice of the heap. They are combined
   *   with population-proportional (stratified) allocation: partition p donates
   *   round(capacity * part_seen[p] / total_seen) items, chosen uniformly at random without
   *   replacement from its own sample (capped at what it holds; rounding remainder spread
   *   over partitions with spare samples). This keeps each partition's contribution in
   *   proportion to how much of the population it covered, which is what the bucketizer needs
   *   for distribution estimation, and matches a single-pass reservoir in expectation.
   *
   *   Deterministic for a given seed. part_samples is consumed (items are moved out).
   */
  template <typename T>
  std::vector<T>
  merge_partition_samples (std::vector<std::vector<T>> &part_samples,
			   const std::vector<std::uint64_t> &part_seen,
			   std::size_t capacity,
			   std::uint64_t seed = RESERVOIR_DEFAULT_SEED)
  {
    std::vector<T> out;
    const std::size_t m = part_samples.size ();
    if (m == 0 || capacity == 0)
      {
	return out;
      }

    std::uint64_t total_seen = 0;
    std::size_t total_available = 0;
    for (std::size_t p = 0; p < m; p++)
      {
	total_seen += part_seen[p];
	total_available += part_samples[p].size ();
      }
    if (total_available == 0)
      {
	return out;
      }

    const std::size_t want = (capacity < total_available) ? capacity : total_available;

    std::mt19937_64 rng (seed);

    /* population-proportional target per partition, capped at what it actually holds */
    std::vector<std::size_t> take (m, 0);
    std::size_t assigned = 0;
    if (total_seen > 0)
      {
	for (std::size_t p = 0; p < m; p++)
	  {
	    std::size_t t =
		    (std::size_t) ((long double) want * (long double) part_seen[p] / (long double) total_seen);
	    if (t > part_samples[p].size ())
	      {
		t = part_samples[p].size ();
	      }
	    take[p] = t;
	    assigned += t;
	  }
      }

    /* spread any rounding remainder over partitions that still have spare samples */
    std::size_t rr = 0;
    while (assigned < want)
      {
	bool progressed = false;
	for (std::size_t step = 0; step < m && assigned < want; step++)
	  {
	    std::size_t p = (rr + step) % m;
	    if (take[p] < part_samples[p].size ())
	      {
		take[p]++;
		assigned++;
		progressed = true;
	      }
	  }
	rr++;
	if (!progressed)
	  {
	    break;		/* no partition has spare samples */
	  }
      }

    /* draw take[p] items uniformly at random (without replacement) from each partition */
    out.reserve (assigned);
    for (std::size_t p = 0; p < m; p++)
      {
	std::vector<T> &s = part_samples[p];
	const std::size_t k = take[p];
	/* partial Fisher-Yates: select k items into [0, k) then move them out */
	for (std::size_t i = 0; i < k; i++)
	  {
	    std::uniform_int_distribution<std::size_t> dist (i, s.size () - 1);
	    std::size_t j = dist (rng);
	    std::swap (s[i], s[j]);
	    out.push_back (std::move (s[i]));
	  }
      }

    return out;
  }

} // namespace cubsampling

#endif // _RESERVOIR_SAMPLER_HPP_
