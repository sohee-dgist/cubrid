#include "dbtype_def.h"
#include "histogram_cl.h"
#include "db.h"
#include "histogram_builder.hpp"
#include "thread_compat.hpp"
#include "db_query.h"


/*
 * analyze_all_classes
 *
 * return:
 *   with_fullscan(in): true iff WITH FULLSCAN
 *
 * NOTE:
 */
int
analyze_classes (THREAD_ENTRY * thread_p, const char *tbl_name, const char *attr_name, int max_number_of_buckets,
		 int with_fullscan)
{
  int error = NO_ERROR;
  char *histogram_blob = NULL;
  error = get_histogram (thread_p, tbl_name, attr_name, max_number_of_buckets, with_fullscan, histogram_blob);
  if (error != NO_ERROR)
    {
      return error;
    }
  db_private_free (thread_p, histogram_blob);

  return NO_ERROR;
}

int
get_histogram (THREAD_ENTRY * thread_p, const char *tbl_name, const char *attr_name, int max_number_of_buckets,
	       int with_fullscan, char *histogram_blob)
{
  int error = NO_ERROR;
  DB_QUERY_RESULT *query_result;
  DB_QUERY_ERROR query_error;
  hist::HistogramBuilder histogram_builder;
  DB_TYPE type = DB_TYPE_UNKNOWN;

  char query_buf[1024];
  snprintf (query_buf, sizeof (query_buf), HISTOGRAM_QUERY_TEMPLATE, attr_name, tbl_name, attr_name,
	    max_number_of_buckets, max_number_of_buckets);

  error = db_compile_and_execute_local (query_buf, &query_result, &query_error);

  if (error < 0)
    {
      return error;
    }

  error = db_query_first_tuple (query_result);
  if (error != DB_CURSOR_SUCCESS)
    {
      if (error == DB_CURSOR_END)
	{
	  error = NO_ERROR;
	}
      else
	{
	  ASSERT_ERROR ();
	}
      return error;
    }


  do
    {
      DB_VALUE value[5];
      error = db_query_get_tuple_value_by_name (query_result, const_cast < char *>("bid"), &value[0]);
      error = db_query_get_tuple_value_by_name (query_result, const_cast < char *>("endpoint"), &value[1]);
      error = db_query_get_tuple_value_by_name (query_result, const_cast < char *>("rows_in_bucket"), &value[2]);
      error = db_query_get_tuple_value_by_name (query_result, const_cast < char *>("cumulative"), &value[3]);
      error = db_query_get_tuple_value_by_name (query_result, const_cast < char *>("approx_ndv"), &value[4]);

      if (error != NO_ERROR)
	{
	  return error;
	}

      switch (value[1].domain.general_info.type)
	{
	case DB_TYPE_INTEGER:
	  {
	    // int를 std::int64_t로 변환하여 variant 생성
	    hist::HistogramTypes hi = static_cast < std::int64_t > (db_get_int (&value[1]));
	    histogram_builder.add (hi, db_get_bigint (&value[3]), db_get_bigint (&value[4]));
	  }
	  type = DB_TYPE_INTEGER;
	  break;
	case DB_TYPE_BIGINT:
	  {
	    // int64_t를 variant로 생성
	    std::int64_t val = db_get_bigint (&value[1]);
	    hist::HistogramTypes hi
	    {
	    val};
	    histogram_builder.add (hi, db_get_bigint (&value[3]), db_get_bigint (&value[4]));
	  }
	  type = DB_TYPE_BIGINT;
	  break;
	case DB_TYPE_DOUBLE:
	  {
	    // double을 variant로 생성
	    double val = db_get_double (&value[1]);
	    hist::HistogramTypes hi
	    {
	    val};
	    histogram_builder.add (hi, db_get_bigint (&value[3]), db_get_bigint (&value[4]));
	  }
	  type = DB_TYPE_DOUBLE;
	  break;
	case DB_TYPE_STRING:
	  {
	    // string을 variant로 생성 (복사 생성으로 안전하게)
	    const char *str = db_get_string (&value[1]);
	    if (str == NULL)
	      {
		return ER_FAILED;
	      }
	    std::string str_val (str);	// 복사 생성 - 안전
	    hist::HistogramTypes hi
	    {
	    str_val};
	    histogram_builder.add (hi, db_get_bigint (&value[3]), db_get_bigint (&value[4]));
	  }
	  type = DB_TYPE_STRING;
	  break;
	default:
	  assert (false);
	  break;
	}
    }
  while (db_query_next_tuple (query_result) == DB_CURSOR_SUCCESS);

  histogram_blob = histogram_builder.build (thread_p, type);
  if (histogram_blob == NULL)
    {
      return ER_FAILED;
    }

  return NO_ERROR;
}
