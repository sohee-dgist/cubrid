#include "dbtype_def.h"
#include "histogram_cl.hpp"
#include "db.h"
#include "histogram_builder.hpp"
#include "thread_compat.hpp"
#include "db_query.h"
#include "locator_cl.h"
#include "schema_manager.h"
#include "schema_system_catalog_constants.h"
#include <stdio.h>
#include <stdbool.h>
#include <string>
#include "parser.h"

/*
 * analyze_all_classes
 *
 * return:
 *   with_fullscan(in): true iff WITH FULLSCAN
 *
 * NOTE:
 */
int
analyze_classes (THREAD_ENTRY *thread_p, const char *tbl_name, const char *attr_name, int max_number_of_buckets,
		 int with_fullscan, MOP classop)
{
  int error = NO_ERROR;
  char *histogram_blob = NULL;
  int histogram_total_length = 0;
  error =
	  get_histogram (thread_p, tbl_name, attr_name, max_number_of_buckets, with_fullscan, &histogram_blob,
			 &histogram_total_length);
  if (error != NO_ERROR)
    {
      return error;
    }
  error = set_histogram (thread_p, tbl_name, attr_name, histogram_blob, histogram_total_length, classop);
  if (error != NO_ERROR)
    {
      return error;
    }
  db_private_free (thread_p, histogram_blob);

  return NO_ERROR;
}

int
get_histogram (THREAD_ENTRY *thread_p, const char *tbl_name, const char *attr_name, int max_number_of_buckets,
	       int with_fullscan, char **histogram_blob, int *histogram_total_length)
{
  int error = NO_ERROR;
  DB_QUERY_RESULT *query_result;
  DB_QUERY_ERROR query_error;
  hist::HistogramBuilder histogram_builder;
  DB_TYPE type = DB_TYPE_UNKNOWN;
  bool sampling_scan = true;
  int number_of_mcv = 3;	// TODO

  char query_buf[1024+222+254]; // TODO GET MAX TABLE NAME LENGTH FROM SQL.H
  if (sampling_scan)
    {
      snprintf (query_buf, sizeof (query_buf), HISTOGRAM_WITH_SAMPLING_SCAN_QUERY_TEMPLATE, attr_name, tbl_name,
		attr_name, number_of_mcv, max_number_of_buckets, max_number_of_buckets);
    }
  else
    {
      snprintf (query_buf, sizeof (query_buf), HISTOGRAM_QUERY_TEMPLATE, attr_name, tbl_name, attr_name,
		number_of_mcv, max_number_of_buckets, max_number_of_buckets);
    }

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
      hist::HistogramTypes hi{};
      error = db_query_get_tuple_value_by_name (query_result, const_cast < char *> ("bid"), &value[0]);
      error = db_query_get_tuple_value_by_name (query_result, const_cast < char *> ("endpoint"), &value[1]);
      error = db_query_get_tuple_value_by_name (query_result, const_cast < char *> ("rows_in_bucket"), &value[2]);
      error = db_query_get_tuple_value_by_name (query_result, const_cast < char *> ("cumulative"), &value[3]);
      error = db_query_get_tuple_value_by_name (query_result, const_cast < char *> ("approx_ndv"), &value[4]);
      if (error != NO_ERROR)
	{
	  return error;
	}
      switch (value[1].domain.general_info.type)
	{
	case DB_TYPE_INTEGER:
	{
	  hi = static_cast < std::int64_t > (db_get_int (&value[1]));
	  break;
	}
	case DB_TYPE_SHORT:
	{
	  hi = static_cast < std::int64_t > (db_get_short (&value[1]));
	  break;
	}
	case DB_TYPE_FLOAT:
	{
	  double val = db_get_float (&value[1]);
	  hi = val;
	  break;
	}
	case DB_TYPE_DOUBLE:
	{
	  double val = db_get_double (&value[1]);
	  hi = val;
	  break;
	}
	case DB_TYPE_NUMERIC:
	{
	  /* Actually, the numeric type is a 16-byte value with very high precision,
	   * but for approximate statistical calculations it's probably better not to
	   * rely on the full 16-byte precision. */
	  double val;
	  numeric_coerce_num_to_double (db_get_numeric (&value[1]), db_value_scale (&value[1]), &val);
	  hi = val;
	  break;
	}
	case DB_TYPE_BIT:
	case DB_TYPE_VARBIT:
	{
	  /* deal as char type */
	  int length = 0;
	  const char *str = db_get_bit (&value[1], &length);
	  if (str == NULL)
	    {
	      return ER_FAILED;
	    }
	  std::string str_val (str, length);
	  hi = str_val;
	  break;
	}
	case DB_TYPE_CHAR: /* later consider for null trailing exists */
	case DB_TYPE_STRING:
	{
	  const char *str = db_get_string (&value[1]);
	  if (str == NULL)
	    {
	      return ER_FAILED;
	    }
	  std::string str_val (str);
	  hi = str_val;
	  break;
	}
	case DB_TYPE_TIME:
	{
	  DB_TIME *time = db_get_time (&value[1]);
	  hi = static_cast<std::uint64_t> (*time);
	  break;
	}
	case DB_TYPE_TIMESTAMP:
	case DB_TYPE_TIMESTAMPLTZ:
	{
	  DB_TIMESTAMP *timestamp = db_get_timestamp (&value[1]);
	  hi = static_cast<std::uint64_t> (*timestamp);
	  break;
	}
	case DB_TYPE_DATE:
	{
	  DB_DATE *date = db_get_date (&value[1]);
	  hi = static_cast<std::uint64_t> (*date);
	  break;
	}
	case DB_TYPE_MONETARY:
	{
	  /* Its use is deprecated, but it has been kept for backporting purposes. */
	  DB_MONETARY *monetary = db_get_monetary (&value[1]);
	  hi = static_cast<std::uint64_t> (monetary->amount);
	  break;
	}
	case DB_TYPE_TIMESTAMPTZ:
	{
	  DB_TIMESTAMPTZ *timestamptz = db_get_timestamptz (&value[1]);
	  hi = static_cast<std::uint64_t> (timestamptz->timestamp);
	  break;
	}
	case DB_TYPE_DATETIMETZ:
	case DB_TYPE_DATETIMELTZ:
	{
	  /* in comparison, the order is maintained by date and time */
	  DB_DATETIMETZ *datetimetz = db_get_datetimetz (&value[1]);
	  hi = static_cast<std::uint64_t> (datetimetz->datetime.date) << 32 | datetimetz->datetime.time;
	  break;
	}
	default:
	  assert (false); /* impossible to reach here - blocked at parser layer first */
	  break;
	}
      histogram_builder.add (hi, db_get_bigint (&value[3]), db_get_bigint (&value[4]));
      type = static_cast<DB_TYPE> (value[1].domain.general_info.type);
    }
  while (db_query_next_tuple (query_result) == DB_CURSOR_SUCCESS);

  *histogram_blob = histogram_builder.build (thread_p, type, histogram_total_length);
  if (*histogram_blob == NULL)
    {
      return ER_FAILED;
    }

  return NO_ERROR;
}

int
set_histogram (THREAD_ENTRY *thread_p, const char *tbl_name, const char *attr_name, char *histogram_blob,
	       int histogram_total_length, MOP classop)
{
  int error = NO_ERROR;
  DB_OBJECT *histogram_obj, *edit_histogram_object = NULL;
  DB_OTMPL *obj_tmpl = NULL;
  DB_VALUE histogram_value;
  error = db_get_histogram (classop, attr_name, &histogram_obj);
  if (error != NO_ERROR)
    {
      return error;
    }

  obj_tmpl = dbt_edit_object (histogram_obj);
  if (obj_tmpl == NULL)
    {
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      goto end;
    }

  db_make_varbit (&histogram_value, 1073741823, histogram_blob, histogram_total_length * 8);
  error = dbt_put (obj_tmpl, "histogram_values", &histogram_value);
  if (error != NO_ERROR)
    {
      goto end;
    }

  edit_histogram_object = dbt_finish_object (obj_tmpl);
  if (edit_histogram_object == NULL)
    {
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      goto end;
    }

  assert (edit_histogram_object == histogram_obj);
  obj_tmpl = NULL;

  error = locator_flush_instance (edit_histogram_object);
  if (error != NO_ERROR)
    {
      goto end;
    }

end:
  db_value_clear (&histogram_value);
  assert (error == NO_ERROR);	// for debug
  return error;
}

void
histogram_get_equal_selectivity (PT_NODE *lhs, PT_NODE *rhs, double *selectivity)
{
  *selectivity = 0.0;
  int error = NO_ERROR;
  std::uint32_t bucket_index = 0;
  /* get object from db histogram class */
  assert (lhs->node_type == PT_NAME);
  const char *tbl_name = lhs->info.name.resolved;
  const char *attr_name = lhs->info.name.original;
  MOP classop = db_find_class (tbl_name);
  DB_VALUE histogram_value;

  DB_OBJECT *histogram_obj = NULL;
  int histogram_total_length = 0;
  error = db_get_histogram (classop, attr_name, &histogram_obj);
  if (error != NO_ERROR)
    {
      return;
    }

  if (histogram_obj == NULL)
    {
      *selectivity = (double) 0.001;
      return;
    }

  /* get histgoram */
  error = db_get (histogram_obj, "histogram_values", &histogram_value);
  if (error != NO_ERROR)
    {
      *selectivity = (double) 0.001;
      return;
    }
  const char *histogram_blob_ptr = db_get_bit (&histogram_value, &histogram_total_length);
  if (histogram_blob_ptr == NULL || histogram_total_length <= 0)
    {
      *selectivity = (double) 0.001;
      return;
    }

  /* need length of histogram_blob_ptr */
  std::string_view histogram_blob (histogram_blob_ptr, static_cast<std::size_t> (histogram_total_length / 8));

  hist::HistogramReader histogram_reader;
  error = histogram_reader.reset (histogram_blob);
  if (error != NO_ERROR)
    {
      *selectivity = (double) 0.001;
      return;
    }

  switch (rhs->info.value.db_value.domain.general_info.type)
    {
    case DB_TYPE_INTEGER:
    {
      std::int32_t val = db_get_int (&rhs->info.value.db_value);
      bucket_index = histogram_reader.find_bucket<std::int32_t> (val);
      break;
    }
    case DB_TYPE_SHORT:
    {
      std::int32_t val = static_cast<std::int32_t> (db_get_short (&rhs->info.value.db_value));
      bucket_index = histogram_reader.find_bucket<std::int32_t> (val);
      break;
    }
    case DB_TYPE_FLOAT:
    {
      double val = db_get_float (&rhs->info.value.db_value);
      bucket_index = histogram_reader.find_bucket<double> (val);
      break;
    }
    case DB_TYPE_DOUBLE:
    {
      double val = db_get_double (&rhs->info.value.db_value);
      bucket_index = histogram_reader.find_bucket<double> (val);
      break;
    }
    case DB_TYPE_NUMERIC:
    {
      double val;
      numeric_coerce_num_to_double (db_get_numeric (&rhs->info.value.db_value), db_value_scale (&rhs->info.value.db_value),
				    &val);
      bucket_index = histogram_reader.find_bucket<double> (val);
      break;
    }
    case DB_TYPE_BIT:
    case DB_TYPE_VARBIT:
    {
      int length = 0;
      const char *str = db_get_bit (&rhs->info.value.db_value, &length);
      if (str == NULL)
	{
	  *selectivity = (double) 0.001;
	  return;
	}
      std::string str_val (str, length);
      bucket_index = histogram_reader.find_bucket<std::string> (str_val);
      break;
    }
    case DB_TYPE_CHAR: /* later consider for null trailing exists */
    case DB_TYPE_STRING:
    {
      const char *str = db_get_string (&rhs->info.value.db_value);
      if (str == NULL)
	{
	  *selectivity = (double) 0.001;
	  return;
	}
      std::string str_val (str);
      bucket_index = histogram_reader.find_bucket<std::string> (str_val);
      break;
    }
    case DB_TYPE_TIME:
    {

      DB_TIME *time = db_get_time (&rhs->info.value.db_value);
      bucket_index = histogram_reader.find_bucket<std::uint64_t> (static_cast<std::uint64_t> (*time));
      break;
    }
    case DB_TYPE_TIMESTAMP:
    case DB_TYPE_TIMESTAMPLTZ:
    {

      DB_TIMESTAMP *timestamp = db_get_timestamp (&rhs->info.value.db_value);
      bucket_index = histogram_reader.find_bucket<std::uint64_t> (static_cast<std::uint64_t> (*timestamp));
      break;
    }
    case DB_TYPE_DATE:
    {
      DB_DATE *date = db_get_date (&rhs->info.value.db_value);
      bucket_index = histogram_reader.find_bucket<std::uint64_t> (static_cast<std::uint64_t> (*date));
      break;
    }
    case DB_TYPE_MONETARY:
    {
      DB_MONETARY *monetary = db_get_monetary (&rhs->info.value.db_value);
      bucket_index = histogram_reader.find_bucket<std::uint64_t> (static_cast<std::uint64_t> (monetary->amount));
      break;
    }
    case DB_TYPE_TIMESTAMPTZ:
    {
      DB_TIMESTAMPTZ *timestamptz = db_get_timestamptz (&rhs->info.value.db_value);
      bucket_index = histogram_reader.find_bucket<std::uint64_t> (static_cast<std::uint64_t> (timestamptz->timestamp));
      break;
    }
    case DB_TYPE_DATETIMETZ:
    case DB_TYPE_DATETIMELTZ:
    {
      DB_DATETIMETZ *datetimetz = db_get_datetimetz (&rhs->info.value.db_value);
      bucket_index = histogram_reader.find_bucket<std::uint64_t> (static_cast<std::uint64_t>
		     (datetimetz->datetime.date) << 32 | datetimetz->datetime.time);
      break;
    }
    default:
      assert (false); /* impossible to reach here - blocked at parser layer first */
      break;
    }


  *selectivity = (static_cast<double> (histogram_reader.bucket_rows (bucket_index)) / static_cast<double>
		  (histogram_reader.total_rows())) /
		 static_cast<double> (histogram_reader.bucket_approx_ndv (bucket_index));
  return;
}

int
db_get_histogram (MOP classop, const char *attr_name, DB_OBJECT **histogram_obj)
{
  int error = NO_ERROR;
  DB_OBJECT *histogram_class;
  DB_OTMPL *obj_tmpl = NULL;
  DB_VALUE value[2];
  DB_VALUE *value_ptrs[2] = { &value[0], &value[1] };
  DB_VALUE histogram_value;
  const char *search_attrs[2] = { "class_of", "key_attr" };

  histogram_class = sm_find_class (CT_DB_HISTOGRAM_NAME);
  if (histogram_class == NULL)
    {
      error = ER_BO_MISSING_OR_INVALID_CATALOG;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 0);
      return error;
    }

  db_make_object (&value[0], classop);
  db_make_string (&value[1], attr_name);

  *histogram_obj = db_find_multi_unique (histogram_class, 2, (char **) search_attrs, value_ptrs, DB_FETCH_READ);

  db_value_clear (value_ptrs[0]);
  db_value_clear (value_ptrs[1]);

  return NO_ERROR;
}