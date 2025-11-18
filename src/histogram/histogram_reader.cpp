#include "histogram_reader.hpp"
#include <algorithm>
#include <utility>
#include "error_manager.h"
#include "object_representation.h"

namespace hist
{
  // ---------- get_value template specialization ----------
  template<>
  std::int32_t HistogramReader::get_value<std::int32_t> (const void *ptr) const
  {
    return OR_GET_INT (ptr);
  }

  template<>
  std::int64_t HistogramReader::get_value<std::int64_t> (const void *ptr) const
  {
    std::int64_t value;
    OR_GET_INT64 (ptr, &value);
    return value;
  }

  template<>
  double HistogramReader::get_value<double> (const void *ptr) const
  {
    double value;
    OR_GET_DOUBLE (ptr, &value);
    return value;
  }

  template<>
  std::uint32_t HistogramReader::get_value<std::uint32_t> (const void *ptr) const
  {
    return static_cast<std::uint32_t> (OR_GET_INT (ptr));
  }

// ---------- reset ----------
  int HistogramReader::reset (std::string_view blob)
  {
    int error = NO_ERROR;
    blob_ = blob;
    if (blob_.size() < sizeof (HeaderV1))
      {
	error = ER_FAILED;
	return error;
      }

    /* read header */
    const auto *H = reinterpret_cast<const HeaderV1 *> (blob_.data());
    if (std::string_view (H->magic, 4) != "HST1")
      {
	error = ER_FAILED;
	return error;
      }
    if (get_value<std::int32_t> (&H->version) != 1)
      {
	error = ER_FAILED;
	return error;
      }

    nb_       = get_value<std::uint32_t> (&H->nbuckets);
    str_size_ = get_value<std::uint32_t> (&H->str_size);
    type_ = static_cast<std::uint32_t> (get_value<std::int32_t> (&H->type));
    total_size_ = get_value<std::uint32_t> (&H->total_size);
    assert (total_size_ == blob_.size());

    /* read index table for O(1) access to bucket record */
    const char *p   = blob_.data() + sizeof (HeaderV1);
    const char *end = blob_.data() + total_size_;


    bucket_area_begin_ = p;

    /* find the last record */
    std::uint32_t max_off = BUCKET_RECORD_SIZE * nb_;
    const char *last = bucket_area_begin_ + max_off;

    if (last > end)
      {
	return ER_FAILED;
      }
    buckets_end_ = last + BUCKET_RECORD_SIZE; // data + cumulative
    if (buckets_end_ + str_size_ != end)
      {
	return ER_FAILED;
      }

    /* read string blob */
    str_blob_ = std::string_view{buckets_end_, static_cast<std::size_t> (str_size_)};
    return NO_ERROR;
  }

// ---------- record navigation ----------
  const char *HistogramReader::bucket_rec (std::uint32_t i) const
  {
    assert (i < nb_);
    std::uint32_t off = i*BUCKET_RECORD_SIZE;
    const char *rec = bucket_area_begin_ + off;
    assert (rec >= bucket_area_begin_ && rec < buckets_end_);
    return rec;
  }

  const char *HistogramReader::bucket_hi_value_ptr (std::uint32_t i) const
  {
    return bucket_rec (i);
  }

// ---------- access ----------
  std::int64_t HistogramReader::bucket_cumulative (std::uint32_t i) const
  {
    const char *p = bucket_rec (i) + 8;
    return get_value<std::int64_t> (p);
  }

  std::int64_t HistogramReader::bucket_approx_ndv (std::uint32_t i) const
  {
    assert (i < nb_);
    const char *rec = bucket_rec (i);
    const char *p = rec + 16;
    return get_value<std::int64_t> (p);
  }

  std::int64_t HistogramReader::bucket_rows (std::uint32_t i) const
  {
    assert (i < nb_);
    const std::int64_t cur  = bucket_cumulative (i);
    const std::int64_t prev = (i == 0) ? 0 : bucket_cumulative (i - 1);
    return cur - prev;
  }

// ---------- bucket_hi template specialization ----------
  template<>
  std::int64_t HistogramReader::bucket_hi<std::int64_t> (std::uint32_t i) const
  {
    return get_value<std::int64_t> (bucket_hi_value_ptr (i));
  }

  template<>
  double HistogramReader::bucket_hi<double> (std::uint32_t i) const
  {
    return get_value<double> (bucket_hi_value_ptr (i));
  }

  template<>
  std::string_view HistogramReader::bucket_hi<std::string_view> (std::uint32_t i) const
  {
    const char *p = bucket_hi_value_ptr (i);
    std::uint32_t len32 = get_value<std::uint32_t> (p);
    std::uint32_t off32 = get_value<std::uint32_t> (p + 4);

    if (len32 <= 4) // inline data
      {
	return std::string_view{ p+4, static_cast<std::size_t> (len32-4) };
      }
    assert (off32 + len32 <= str_size_);
    return std::string_view{str_blob_.data() + off32, static_cast<std::size_t> (len32)};
  }

} // namespace hist
