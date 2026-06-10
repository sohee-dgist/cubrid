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
 *  Draws a fixed-size uniform random sample of K items from a stream of unknown
 *  length N in a single pass, using O(K) memory and without needing N in advance.
 *  Every item that passes through the stream ends up in the final reservoir with
 *  probability K / N.
 *
 *  Two interfaces are provided:
 *
 *    reservoir_selector  - decision only. consider() returns the reservoir slot
 *                          an incoming item should occupy, or NOT_SELECTED to drop
 *                          it. Use this when the items own external resources whose
 *                          lifetime the caller must manage (e.g. DB_VALUE that needs
 *                          db_value_clone / db_value_clear).
 *
 *    reservoir_sampler<T> - value storing. add() copies the item into the reservoir.
 *                          Use this for self-contained value types (std::int64_t,
 *                          double, std::string, ...).
 *
 *  Note: this is exact Algorithm R (O(N) RNG draws). Algorithm Z (O(K log(N/K))
 *  draws via skip distances) can be layered on later for very large streams; the
 *  public contract here is unaffected.
 */

#ifndef _RESERVOIR_SAMPLER_HPP_
#define _RESERVOIR_SAMPLER_HPP_

#include <cstddef>
#include <cstdint>
#include <random>
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

} // namespace cubsampling

#endif // _RESERVOIR_SAMPLER_HPP_
