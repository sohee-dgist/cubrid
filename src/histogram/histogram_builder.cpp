#include "histogram_builder.hpp"
#include "histogram_reader.hpp"
#include <cstring>
#include "object_domain.h"

namespace hist
{
  // ---------- endian writers for buffer (explicit specializations before use) ----------
  template<>
  void HistogramBuilder::write<std::int32_t> (char *&dest, std::int32_t v)
  {
    OR_PUT_INT (dest, v);
    dest += OR_INT64_SIZE; // keep 8-byte slot for data_hi (4B value + 4B padding)
  }

  template<>
  void HistogramBuilder::write<std::int64_t> (char *&dest, std::int64_t v)
  {
    OR_PUT_INT64 (dest, &v);  // 포인터 전달 필요
    dest += OR_INT64_SIZE;
  }

  template<>
  void HistogramBuilder::write<double> (char *&dest, double v)
  {
    OR_PUT_DOUBLE (dest, v);
    dest += OR_DOUBLE_SIZE;
  }

  template<>
  void HistogramBuilder::write<std::string> (char *&dest, std::string v)
  {
    // write length and offset or inline data
    OR_PUT_INT (dest, v.length());
    dest += OR_INT_SIZE;
    if (v.length() <= 4)
      {
	memcpy (dest, v.data(), v.length());
      }
    else
      {
	OR_PUT_INT (dest, cur_str_off_);
	cur_str_off_ += v.length();
      }
    dest += OR_INT_SIZE;
  }

  void HistogramBuilder::add (HistogramTypes data_hi, std::int64_t cumulative, std::int64_t approx_ndv)
  {
    assert (cumulative >= 0);
    buckets_.push_back (Bucket{data_hi, cumulative, approx_ndv});
  }

  char *HistogramBuilder::build (THREAD_ENTRY *thread_p, DB_TYPE type)
  {
    // ---- precompute record sizes  ----
    const std::uint32_t bucket_area_size = hist::BUCKET_RECORD_SIZE * buckets_.size();

    // ---- header ----
    HeaderV1 H{};
    std::memcpy (H.magic, "HST1", 4);
    H.version  = ntohl (1);
    H.nbuckets = ntohl (static_cast<std::uint32_t> (buckets_.size()));
    H.type = ntohl (static_cast<std::uint32_t> (type));
    H.str_size = 0; // Fix Later
    H.total_size = 0; // Fix Later

    char *buffer = static_cast<char *> (db_private_alloc (thread_p, sizeof (H) + bucket_area_size)); // records
    if (buffer == NULL)
      {
	return NULL;
      }
    std::memset (buffer, 0, sizeof (H) + bucket_area_size); // initialize to zero
    char *end_buffer = buffer + sizeof (H) + bucket_area_size;
    char *buffer_ptr = buffer + sizeof (H);
    char *str_blob_ptr;
    // buckets area
    if (buckets_.empty())
      {
	return buffer; // return empty buffer if no buckets
      }

    // Use index-based loop for safer access
    for (size_t i = 0; i < buckets_.size(); ++i)
      {
	const Bucket b = buckets_[i];
	switch (type)
	  {
	  case DB_TYPE_INTEGER:
	  {
	    // DB_TYPE_INTEGER는 std::int64_t로 저장됨 (HistogramTypes에 std::int32_t 없음)
	    if (std::holds_alternative<std::int64_t> (b.data_hi))
	      {
		// int64_t 값을 int32_t로 변환하여 저장 (실제로는 32bit 값이므로)
		std::int64_t val = std::get<std::int64_t> (b.data_hi);
		write<std::int32_t> (buffer_ptr, static_cast<std::int32_t> (val));
	      }
	    else
	      {
		assert (false);
		return NULL;
	      }
	  }
	  break;
	  case DB_TYPE_DOUBLE:
	  {
	    if (std::holds_alternative<double> (b.data_hi))
	      {
		write<double> (buffer_ptr, std::get<double> (b.data_hi));
	      }
	    else
	      {
		assert (false);
		return NULL;
	      }
	  }
	  break;
	  case DB_TYPE_BIGINT:
	  {
	    if (std::holds_alternative<std::int64_t> (b.data_hi))
	      {
		write<std::int64_t> (buffer_ptr, std::get<std::int64_t> (b.data_hi));
	      }
	    else
	      {
		assert (false);
		return NULL;
	      }
	  }
	  break;
	  case DB_TYPE_STRING:
	  {
	    // variant에 string_view나 string이 있을 수 있음
	    if (std::holds_alternative<std::string> (b.data_hi))
	      {
		write<std::string> (buffer_ptr, std::get<std::string> (b.data_hi));
	      }
	    else if (std::holds_alternative<std::string_view> (b.data_hi))
	      {
		// string_view를 string으로 변환
		std::string_view sv = std::get<std::string_view> (b.data_hi);
		write<std::string> (buffer_ptr, std::string (sv));
	      }
	    else
	      {
		assert (false);
		return NULL;
	      }
	  }
	  break;
	  default:
	    // not_implemented
	    assert (false);
	    return NULL;
	  }
	write<std::int64_t> (buffer_ptr, b.cumulative);
	write<std::int64_t> (buffer_ptr, b.approx_ndv);
      }

    assert (buffer_ptr == end_buffer);

    // build string blob
    if (cur_str_off_ > 0)
      {
	assert (DB_TYPE_STRING == type);
	str_blob_ptr = static_cast<char *> (db_private_alloc (thread_p, cur_str_off_));
	if (str_blob_ptr == NULL)
	  {
	    return NULL;
	  }
	std::memset (str_blob_ptr, 0, cur_str_off_); // initialize to zero
	char *str_blob_ptr_end = str_blob_ptr + cur_str_off_;
	for (const auto &b : buckets_)
	  {
	    if (DB_TYPE_STRING == type)
	      {
		std::string str_val;
		if (std::holds_alternative<std::string> (b.data_hi))
		  {
		    str_val = std::get<std::string> (b.data_hi);
		  }
		else if (std::holds_alternative<std::string_view> (b.data_hi))
		  {
		    str_val = std::string (std::get<std::string_view> (b.data_hi));
		  }
		else
		  {
		    assert (false);
		    return NULL;
		  }

		if (str_val.length() > 4)
		  {
		    memcpy (str_blob_ptr, str_val.data(), str_val.length());
		    str_blob_ptr += str_val.length();
		  }
	      }
	  }
	// write string
	assert (str_blob_ptr == str_blob_ptr_end);
	buffer = static_cast<char *> (db_private_realloc (thread_p, buffer, sizeof (H) + bucket_area_size + cur_str_off_));
	if (buffer == NULL)
	  {
	    db_private_free (thread_p, str_blob_ptr);
	    return NULL;
	  }
	memcpy (buffer + sizeof (H) + bucket_area_size, str_blob_ptr, cur_str_off_);
	end_buffer += cur_str_off_;
	db_private_free (thread_p, str_blob_ptr);
      }

    H.str_size = ntohl (cur_str_off_);
    H.total_size = ntohl (sizeof (H) + bucket_area_size + cur_str_off_);
    memcpy (buffer, &H, sizeof (H));
    assert (end_buffer - buffer == sizeof (H) + bucket_area_size + cur_str_off_);

    // write header
    return buffer;
  }
} // namespace hist
