#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <limits>
#include "histogram_reader.hpp"
#include "object_representation.h"

namespace hist
{
  using HistogramTypes = std::variant<std::int64_t, double, std::string_view, std::string>;
  class HistogramBuilder
  {
    public:
      void add (HistogramTypes data_hi, std::int64_t cumulative,
		std::int64_t approx_ndv = std::numeric_limits<std::int64_t>::quiet_NaN());
      char *build (THREAD_ENTRY *thread_p, DB_TYPE type);

    private:
      struct Bucket
      {
	HistogramTypes   data_hi;  // std::variant: int32_t, int64_t, double, string 중 하나
	std::int64_t     cumulative;
	std::int64_t     approx_ndv;
      };

      HeaderV1 header_;
      std::vector<Bucket> buckets_;
      std::int32_t cur_str_off_ = 0;

      // endian writers
      template<typename T>
      void write (char *&dest, T v);
  };

} // namespace histo
