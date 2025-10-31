#include "dbtype_def.h"
#include "histogram_cl.h"
#include "db.h"
#include "histogram_builder.hpp"
#include "thread_entry.hpp"


/*
 * analyze_all_classes
 *
 * return:
 *   with_fullscan(in): true iff WITH FULLSCAN
 *
 * NOTE:
 */


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

  if (error != NO_ERROR)
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
      error = db_query_get_tuple_value (query_result, 0, &value[0]);	// bid
      error = db_query_get_tuple_value (query_result, 1, &value[1]);	// endpoint
      error = db_query_get_tuple_value (query_result, 2, &value[2]);	// rows_in_bucket
      error = db_query_get_tuple_value (query_result, 3, &value[3]);	// cumulative
      error = db_query_get_tuple_value (query_result, 4, &value[4]);	// approx_ndv

      if (error != NO_ERROR)
	{
	  return error;
	}

      switch (value[1].domain.general_info.type)
	{
	case DB_TYPE_INTEGER:
	  histogram_builder.add (db_get_int (&value[1]), db_get_double (&value[3]), db_get_double (&value[4]));
	  type = DB_TYPE_INTEGER;
	  break;
	case DB_TYPE_BIGINT:
	  histogram_builder.add (db_get_int (&value[1]), db_get_double (&value[3]), db_get_double (&value[4]));
	  type = DB_TYPE_BIGINT;
	  break;
	case DB_TYPE_DOUBLE:
	  histogram_builder.add (db_get_int (&value[1]), db_get_double (&value[3]), db_get_double (&value[4]));
	  type = DB_TYPE_DOUBLE;
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
