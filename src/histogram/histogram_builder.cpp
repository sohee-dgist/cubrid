#include "histogram_builder.hpp"
#include "histogram_reader.hpp"
#include <cstring>
#include "object_domain.h"

namespace hist
{
  void HistogramBuilder::add (HistogramTypes hi, double cumulative, double approx_ndv)
  {
    assert (cumulative >= 0.0);
    buckets_.push_back (Bucket{hi, cumulative, approx_ndv});
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
    char *end_buffer = buffer + sizeof (H) + bucket_area_size;
    char *buffer_ptr = buffer + sizeof (H);
    char *str_blob_ptr;
    // buckets area
    for (const auto &b : buckets_)
      {
	const auto &bucket = b;
	switch (type)
	  {
	  case DB_TYPE_INTEGER:
	    write<std::int32_t> (buffer_ptr, std::get<std::int32_t> (bucket.data_hi));
	    break;
	  case DB_TYPE_DOUBLE:
	    write<double> (buffer_ptr, std::get<double> (bucket.data_hi));
	    break;
	  case DB_TYPE_BIGINT:
	    write<std::int64_t> (buffer_ptr, std::get<std::int64_t> (bucket.data_hi));
	    break;
	  case DB_TYPE_STRING:
	    write<std::string> (buffer_ptr, std::get<std::string> (bucket.data_hi));
	    break;
	  default:
	    // not_implemented
	    assert (false);
	    break;
	  }
	write<double> (buffer_ptr, bucket.cumulative);
	write<double> (buffer_ptr, bucket.approx_ndv);
      }

    assert (buffer_ptr != end_buffer);

    // build string blob
    if (cur_str_off_ > 0)
      {
	assert (DB_TYPE_STRING == type);
	str_blob_ptr = static_cast<char *> (db_private_alloc (thread_p, cur_str_off_));
	char *str_blob_ptr_end = str_blob_ptr + cur_str_off_;
	for (const auto &b : buckets_)
	  {
	    const auto &bucket = b;
	    if (DB_TYPE_STRING == type)
	      {
		if ( std::get<std::string> (bucket.data_hi).length() > 4)
		  {
		    memcpy (str_blob_ptr, std::get<std::string> (bucket.data_hi).data(), std::get<std::string> (bucket.data_hi).length());
		    str_blob_ptr += std::get<std::string> (bucket.data_hi).length();
		  }
	      }
	  }
	// write string
	assert (str_blob_ptr == str_blob_ptr_end);
	buffer = static_cast<char *> (db_private_realloc (thread_p, buffer, sizeof (H) + bucket_area_size + cur_str_off_));
	memcpy (buffer + sizeof (H) + bucket_area_size, str_blob_ptr, cur_str_off_);
	end_buffer += cur_str_off_;
	db_private_free (thread_p, str_blob_ptr);
      }

    H.str_size = ntohl (cur_str_off_);
    H.total_size = ntohl (sizeof (H) + bucket_area_size + cur_str_off_);
    memcpy (buffer, &H, sizeof (H));
    assert (buffer - end_buffer == H.total_size);

    // write header
    return buffer;
  }

// ---------- endian writers for buffer ----------
  template<>
  void HistogramBuilder::write<std::int32_t> (char *&dest, std::int32_t v)
  {
    OR_PUT_INT (dest, v);
    dest += OR_INT64_SIZE;
  }

  template<>
  void HistogramBuilder::write<std::int64_t> (char *&dest, std::int64_t v)
  {
    OR_PUT_INT64 (dest, v);
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
} // namespace hist
