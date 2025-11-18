#pragma once
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
  struct HeaderV1
  {
    char           magic[4];   // "HST1"
    std::uint32_t  version;
    std::uint32_t  nbuckets;
    std::uint32_t  str_size;
    std::uint32_t  type;       // DB_TYPE
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

      std::int64_t bucket_cumulative (std::uint32_t i) const;
      std::int64_t bucket_approx_ndv (std::uint32_t i) const;

      template<typename T>
      T bucket_hi (std::uint32_t i) const;
      std::int64_t bucket_rows (std::uint32_t i) const;
      template <typename T>
      std::uint32_t find_bucket (const T &value) const
      {
	// nb_ == 0 은 설계상 거의 없겠지만, 방어적으로 처리
	if (nb_ == 0)
	  {
	    return 0;
	  }

	std::uint32_t lo = 0;
	std::uint32_t hi = nb_ - 1;
	std::uint32_t ans = nb_ - 1; // 기본값: 마지막 버킷

	while (lo <= hi)
	  {
	    std::uint32_t mid = lo + (hi - lo) / 2;
	    T hi_val = bucket_hi<T> (mid);

	    if (value <= hi_val)
	      {
		// (low, hi] 에서 hi 부분에 들어감 → 후보 인덱스
		ans = mid;
		if (mid == 0)
		  {
		    break; // 더 왼쪽은 없음
		  }
		hi = mid - 1;
	      }
	    else
	      {
		// value > HI[mid] → 더 오른쪽 버킷을 봐야 함
		lo = mid + 1;
	      }
	  }

	// ans 는 항상 [0, nb_-1] 범위
	//  - value > HI[nb_-1] 이면 갱신이 안 돼서 nb_-1 유지
	//  - 그 외에는 lower_bound(HI, value) 결과
	return ans;
      }
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
      std::uint32_t type_ = DB_TYPE_UNKNOWN;
  };


} // namespace hist
