#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <limits>
#include "histogram_reader.hpp"
#include "object_representation.h"

namespace hist
{

  class HistogramBuilder
  {
    public:
      void add (HistogramTypes hi, double cumulative, double approx_ndv = std::numeric_limits<double>::quiet_NaN());
      char *build (THREAD_ENTRY *thread_p, DB_TYPE type);

    private:
      struct Bucket
      {
	HistogramTypes   data_hi;
	double        cumulative;
	double        approx_ndv;
      };

      HeaderV1 header_;
      std::vector<Bucket> buckets_;
      std::int32_t cur_str_off_ = 0;

      // endian writers
      template<typename T>
      void write (char *&dest, T v);
  };

} // namespace histo
