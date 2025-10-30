#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <limits>
#include "error_manager.h"
#include <variant>
#include "dbtype.h"
namespace hist
{

// ---- Flat binary layout (LE) ----
// [Header] [Index table] [Buckets area] [String blob....]
//
// Header (fixed):
//   magic     : 'HST2' (4B)
//   version   : u32 (1)
//   nbuckets  : u32
//   str_size  : u32
//
// Index table (fixed-size):
//   offsets[nbuckets] : u32 each, offset of bucket i record
//                       relative to bucket_area_begin (first record is usually 0)
//
// Buckets area (variable):
//   For each i in [0, nbuckets):
//     data_hi    : 8B   (ptr or Value)
//     cumulative: f64
//     approx_ndv: f64  (present only if has_ndv==1)
//
// String blob (trailing):
//   str_size bytes; bucket string data points to (len, off) inside this blob.
  constexpr std::uint32_t BUCKET_RECORD_SIZE = 8 + 8 + 8; // data + cumulative + approx_ndv
  using HistogramTypes = std::variant<std::int64_t, double, std::string_view, std::string>;
  struct HeaderV1
  {
    char           magic[4];   // "HST1"
    std::uint32_t  version;
    std::uint32_t  nbuckets;
    std::uint32_t  str_size;
    std::uint32_t type;       // Not Same to DB Type
    std::uint32_t  total_size; // total size of the histogram
  };

  class HistogramReader
  {
    public:
      HistogramReader() = default;
      int create (HistogramReader &reader, std::string_view blob)
      {
	int error = reader.reset (blob);
	return error;
      }
      int reset (std::string_view blob);

      std::uint64_t bucket_count() const noexcept
      {
	return nb_;
      }
      std::uint64_t total_rows()   const
      {
	return nb_ ? bucket_cumulative (nb_ - 1) : 0;
      }

      double bucket_cumulative (std::uint32_t i) const;
      double bucket_approx_ndv (std::uint32_t i) const;

      template<typename T>
      T bucket_hi (std::uint32_t i) const;

      double bucket_rows (std::uint32_t i) const;

    private:
      template<typename T>
      T get_value (const void *ptr) const;

      const char *bucket_rec (std::uint32_t i) const;
      const char *bucket_hi_value_ptr (std::uint32_t i) const;

    private:
      std::string_view blob_{};
      std::string_view str_blob_{};
      const char *bucket_area_begin_ = nullptr;
      const char *buckets_end_       = nullptr;

      std::uint32_t nb_ = 0;
      std::uint32_t str_size_ = 0;
      std::uint32_t total_size_ = 0;
      TypeIndex type_ = DB_TYPE_UNKNOWN;
  };


} // namespace hist
