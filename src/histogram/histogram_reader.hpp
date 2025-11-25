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
	if (nb_ == 0)
	  {
	    return 0; // 방어용
	  }

	// 오른쪽으로 튀면 마지막 버킷으로 클램프
	T max_val = bucket_hi<T> (nb_ - 1);
	if (value > max_val)
	  {
	    return nb_ - 1;
	  }

	// [0, nb_-1] 에서 lower_bound(HI, value)
	std::uint32_t lo = 0;
	std::uint32_t hi = nb_ - 1;

	while (lo < hi)
	  {
	    std::uint32_t mid = lo + (hi - lo) / 2;
	    T hi_val = bucket_hi<T> (mid);

	    // hi 값은 포함이므로 value <= HI[mid] 면 왼쪽으로 좁힘
	    if (value <= hi_val)
	      {
		hi = mid;
	      }
	    else
	      {
		lo = mid + 1;
	      }
	  }

	// 여기 오면 lo == hi, 그리고 HI[lo] >= value 가 보장됨
	// 엔드포인트: [1, 2, 3]
	//   value = 0  → 0
	//   value = 1  → 0
	//   value = 2  → 1
	//   value = 3  → 2
	//   value > 3  → 위의 클램프 로직으로 2
        // 여기에서는 템플릿 특수화 (compare val에 대해서 필요할 것 같아 보임. (강조!))
        // 나머지 경우에 대해서는 잘 모르겠네..............
	return lo;

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
      // 타입은 디비 타입으로 도치시키는게 좋아 보인다. 뉴머릭 타입에 대해서는 double과 int로만 사용 되는 대충 히스토그램 비교성 비교만 해서 히스토그램을 만드는 것이 훨씬 더 이롭다.
      // 왜냐하면 굳이 정확할 필요는 없을 거 같다.

      // 그러면 CHARSET에 대한 비교가 필요한데, 왜 CHARSET에 대한 비교는 COLLATION이 필요한 것일까?
      
      std::uint32_t type_ = DB_TYPE_UNKNOWN;

  };


} // namespace hist
