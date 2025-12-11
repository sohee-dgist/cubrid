/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * histogram_cl.cpp - Histogram Client implementation
 */


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
#include "class_object.h"
#include "object_accessor.h"
#include "authenticate.h"
#include "query_planner.h"

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
		 bool with_fullscan, MOP classop)
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
	       bool with_fullscan, char **histogram_blob, int *histogram_total_length)
{
  int error = NO_ERROR;
  DB_QUERY_RESULT *query_result;
  DB_QUERY_ERROR query_error;
  hist::HistogramBuilder histogram_builder;
  DB_TYPE type = DB_TYPE_UNKNOWN;
  int number_of_mcv = 3;	// TODO

  char query_buf[1024+222+254]; // TODO GET MAX TABLE NAME LENGTH FROM SQL.H
  if (!with_fullscan)
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

static bool
histogram_init_reader_from_lhs (PT_NODE *lhs, hist::HistogramReader &reader)
{
  if (lhs == NULL || lhs->node_type != PT_NAME)
    {
      return false;
    }

  DB_VALUE *histogram_value = lhs->info.name.histogram;
  if (histogram_value == NULL)
    {
      return false;
    }

  int histogram_total_length = 0;
  const char *histogram_blob_ptr = db_get_bit (histogram_value, &histogram_total_length);
  if (histogram_blob_ptr == NULL || histogram_total_length <= 0)
    {
      return false;
    }

  std::string_view histogram_blob (histogram_blob_ptr,
				   static_cast<std::size_t> (histogram_total_length / 8));

  int error = reader.reset (histogram_blob);
  if (error != NO_ERROR)
    {
      return false;
    }

  return true;
}

static bool
histogram_extract_key (const DB_VALUE *db_val, histogram_key &key)
{
  const DB_TYPE type = static_cast<DB_TYPE> (db_val->domain.general_info.type);

  switch (type)
    {
    case DB_TYPE_INTEGER:
      key.kind = histogram_key_kind::i32;
      key.i32 = db_get_int (db_val);
      return true;

    case DB_TYPE_SHORT:
      key.kind = histogram_key_kind::i32;
      key.i32 = static_cast<std::int32_t> (db_get_short (db_val));
      return true;

    case DB_TYPE_FLOAT:
      key.kind = histogram_key_kind::dbl;
      key.dbl = static_cast<double> (db_get_float (db_val));
      return true;

    case DB_TYPE_DOUBLE:
      key.kind = histogram_key_kind::dbl;
      key.dbl = db_get_double (db_val);
      return true;

    case DB_TYPE_NUMERIC:
      key.kind = histogram_key_kind::dbl;
      numeric_coerce_num_to_double (db_get_numeric (db_val), db_value_scale (db_val), &key.dbl);
      return true;

    case DB_TYPE_BIT:
    case DB_TYPE_VARBIT:
    {
      int length = 0;
      const char *str = db_get_bit (db_val, &length);
      if (str == NULL)
	{
	  return false;
	}
      key.kind = histogram_key_kind::str;
      key.str.assign (str, length);
      return true;
    }

    case DB_TYPE_CHAR:   /* later consider for null trailing exists */
    case DB_TYPE_STRING:
    {
      const char *str = db_get_string (db_val);
      if (str == NULL)
	{
	  return false;
	}
      key.kind = histogram_key_kind::str;
      key.str.assign (str);
      return true;
    }

    case DB_TYPE_TIME:
    {
      DB_TIME *timep = db_get_time (db_val);
      key.kind = histogram_key_kind::u64;
      key.u64 = static_cast<std::uint64_t> (*timep);
      return true;
    }

    case DB_TYPE_TIMESTAMP:
    case DB_TYPE_TIMESTAMPLTZ:
    {
      DB_TIMESTAMP *tsp = db_get_timestamp (db_val);
      key.kind = histogram_key_kind::u64;
      key.u64 = static_cast<std::uint64_t> (*tsp);
      return true;
    }

    case DB_TYPE_DATE:
    {
      DB_DATE *datep = db_get_date (db_val);
      key.kind = histogram_key_kind::u64;
      key.u64 = static_cast<std::uint64_t> (*datep);
      return true;
    }

    case DB_TYPE_MONETARY:
    {
      DB_MONETARY *monetary = db_get_monetary (db_val);
      key.kind = histogram_key_kind::u64;
      key.u64 = static_cast<std::uint64_t> (monetary->amount);
      return true;
    }

    case DB_TYPE_TIMESTAMPTZ:
    {
      DB_TIMESTAMPTZ *timestamptz = db_get_timestamptz (db_val);
      key.kind = histogram_key_kind::u64;
      key.u64 = static_cast<std::uint64_t> (timestamptz->timestamp);
      return true;
    }

    case DB_TYPE_DATETIMETZ:
    case DB_TYPE_DATETIMELTZ:
    {
      DB_DATETIMETZ *datetimetz = db_get_datetimetz (db_val);
      key.kind = histogram_key_kind::u64;
      key.u64 = (static_cast<std::uint64_t> (datetimetz->datetime.date) << 32)
		| static_cast<std::uint64_t> (datetimetz->datetime.time);
      return true;
    }

    default:
      assert (false); /* impossible to reach here - blocked at parser layer first */
      return false;
    }
}

void
histogram_get_equal_selectivity (PT_NODE *lhs, PT_NODE *rhs, double *selectivity)
{

  assert (selectivity != NULL);

  hist::HistogramReader histogram_reader;
  if (!histogram_init_reader_from_lhs (lhs, histogram_reader))
    {
      *selectivity = DEFAULT_EQUAL_SELECTIVITY;
      return;
    }

  histogram_key key;
  if (!histogram_extract_key (&rhs->info.value.db_value, key))
    {
      *selectivity = DEFAULT_EQUAL_SELECTIVITY;
      return;
    }

  int bucket_index = -1;
  bool found = false;

  switch (key.kind)
    {
    case histogram_key_kind::i32:
      found = histogram_reader.find_bucket_and_check<std::int32_t> (key.i32, bucket_index);
      break;

    case histogram_key_kind::dbl:
      found = histogram_reader.find_bucket_and_check<double> (key.dbl, bucket_index);
      break;

    case histogram_key_kind::str:
      found = histogram_reader.find_bucket_and_check<std::string> (key.str, bucket_index);
      break;

    case histogram_key_kind::u64:
      found = histogram_reader.find_bucket_and_check<std::uint64_t> (key.u64, bucket_index);
      break;

    case histogram_key_kind::invalid:
    default:
      assert (false);
      break;
    }

  if (!found || bucket_index < 0)
    {
      /* not found in histogram */
      *selectivity = 0.0;
      return;
    }

  const double bucket_rows = static_cast<double> (histogram_reader.bucket_rows (bucket_index));
  const double total_rows = static_cast<double> (histogram_reader.total_rows ());
  const double approx_ndv = static_cast<double> (histogram_reader.bucket_approx_ndv (bucket_index));

  if (total_rows <= 0.0 || approx_ndv <= 0.0)
    {
      /* safe default */
      *selectivity = DEFAULT_EQUAL_SELECTIVITY;
      return;
    }

  *selectivity = (bucket_rows / total_rows) / approx_ndv;
  return;
}

void
histogram_get_comp_selectivity (PT_NODE *lhs, PT_NODE *rhs, double *selectivity)
{
  return;
}

void
histogram_get_between_selectivity (PT_NODE *lhs, PT_NODE *rhs, double *selectivity)
{
  return;
}

void
histogram_get_range_selectivity (PT_NODE *lhs, PT_NODE *rhs, double *selectivity)
{
  return;
}

void
histogram_get_all_some_in_selectivity (PT_NODE *lhs, PT_NODE *rhs, double *selectivity)
{
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

int
stats_get_histogram (MOP classop, HIST_STATS **histogram)
{
  int error = NO_ERROR;
  DB_OBJECT *histogram_obj = NULL;
  SM_ATTRIBUTE *att;
  SM_CLASS *class_ = NULL;
  error = au_fetch_class (classop, &class_, AU_FETCH_READ, AU_SELECT);
  if (error != NO_ERROR)
    {
      return error;
    }

  *histogram = (HIST_STATS *) db_ws_alloc (sizeof (HIST_STATS));
  if (*histogram == NULL)
    {
      return ER_OUT_OF_VIRTUAL_MEMORY;
    }
  (*histogram)->n_attrs = class_->att_count;
  (*histogram)->histogram = (DB_VALUE **) db_ws_alloc (sizeof (DB_VALUE *) * class_->att_count);
  if ((*histogram)->histogram == NULL)
    {
      return ER_OUT_OF_VIRTUAL_MEMORY;
    }

  int i = 0;
  for (att = class_->attributes; att != NULL; att = (SM_ATTRIBUTE *) att->header.next)
    {
      const char *attname = (char *) att->header.name;
      DB_VALUE *histogram_value = NULL;
      error = db_get_histogram (classop, attname, &histogram_obj);
      if (error != NO_ERROR)
	{
	  return error;
	}

      if (histogram_obj == NULL)
	{
	  (*histogram)->histogram[i] = nullptr;
	  i++;
	  continue;
	}

      histogram_value = (DB_VALUE *) db_ws_alloc (sizeof (DB_VALUE));
      error = db_get (histogram_obj, "histogram_values", histogram_value);
      if (error != NO_ERROR)
	{
	  return error;
	}

      (*histogram)->histogram[i] = histogram_value; // should clear histogram_value
      i++;
    }
  return NO_ERROR;
}

int stats_free_histogram_and_init (HIST_STATS *histogram)
{
  if (histogram == NULL)
    {
      return NO_ERROR;
    }
  for (int i = 0; i < histogram->n_attrs; i++)
    {
      if (histogram->histogram[i] == nullptr)
	{
	  continue;
	}
      db_value_clear (histogram->histogram[i]);
      db_ws_free (histogram->histogram[i]);
      histogram->histogram[i] = nullptr;
    }
  db_ws_free (histogram->histogram);
  db_ws_free (histogram);
  return NO_ERROR;
}

bool
is_histogrammable_type (DB_TYPE type)
{
  switch (type)
    {
    /* numeric */
    case DB_TYPE_INTEGER:
    case DB_TYPE_SHORT:
    case DB_TYPE_FLOAT:
    case DB_TYPE_DOUBLE:
    case DB_TYPE_NUMERIC:
    case DB_TYPE_MONETARY:
      return true;

    /* bit string */
    case DB_TYPE_BIT:
    case DB_TYPE_VARBIT:
      return true;

    /* character string */
    case DB_TYPE_CHAR:
    case DB_TYPE_STRING:
      return true;

    /* date / time */
    case DB_TYPE_TIME:
    case DB_TYPE_DATE:
    case DB_TYPE_TIMESTAMP:
    case DB_TYPE_TIMESTAMPLTZ:
    case DB_TYPE_TIMESTAMPTZ:
    case DB_TYPE_DATETIMELTZ:
    case DB_TYPE_DATETIMETZ:
      return true;

    default:
      return false;
    }
}

/*===========================================================================*/
/* dump_histogram */

/*
+------------------ HISTOGRAM ------------------+
| column : age (int)                            |
| rows   : 100000   sample : 10000 (10.0%)      |
| pages  : 120 / 500                            |
| buckets: 16        nulls  : 123               |
+------------------------------------------------+
#00 [-inf,  10] rows=  1234(0.012) ndv=10  cum=0.012

*/

/*===========================================================================*/
#define HIST_DUMP_WIDTH 47  /* inner width of the histogram */

int
dump_histogram (MOP classop, const char *attr_name, DB_TYPE attr_type, bool with_fullscan, int error, FILE *f)
{
  char line[HIST_DUMP_WIDTH + 1];
  SM_CLASS *class_ = NULL;
  const char *col_name = attr_name;
  const char *type_name = db_get_type_name (attr_type);
  int rows_scanned = 0;
  int bucket_count = 0;
  DB_VALUE histogram_value;
  DB_OBJECT *histogram_obj = NULL;
  int histogram_total_length = 0;

  double null_frequency = 0.0;
  if (error != NO_ERROR)
    {
      snprintf (line, sizeof (line), "ERROR: Failed to dump histogram column: %s", attr_name);
      fprintf (f, "| %-47s|\n", line);
      fprintf (f, "+------------------------------------------------+\n");
      return NO_ERROR;
    }

  class_ = sm_get_class_with_statistics (classop);
  if (class_ == NULL)
    {
      return ER_FAILED;
    }

  error = db_get_histogram (classop, attr_name, &histogram_obj);
  if (error != NO_ERROR)
    {
      return ER_FAILED;
    }

  if (histogram_obj == NULL)
    {
      return ER_FAILED;
    }

  /* get histgoram */
  error = db_get (histogram_obj, "histogram_values", &histogram_value);
  if (error != NO_ERROR)
    {
      return ER_FAILED;
    }

  const char *histogram_blob_ptr = db_get_bit (&histogram_value, &histogram_total_length);
  if (histogram_blob_ptr == NULL || histogram_total_length <= 0)
    {
      return ER_FAILED;
    }

  /* need length of histogram_blob_ptr */
  std::string_view histogram_blob (histogram_blob_ptr, static_cast<std::size_t> (histogram_total_length / 8));

  hist::HistogramReader histogram_reader;
  error = histogram_reader.reset (histogram_blob);
  if (error != NO_ERROR)
    {
      return ER_FAILED;
    }

  /* top border */
  fputs ("+------------------ HISTOGRAM -------------------+\n", f);

  /* column line */
  snprintf (line, sizeof (line), " column : %s (%s)", col_name, type_name);
  fprintf (f, "| %-47s|\n", line);

  /* rows + sample line */
  rows_scanned = static_cast<int> (histogram_reader.total_rows());

  if (class_->stats->heap_num_objects <= 0 || class_->stats->heap_num_pages <= 0)
    {
      snprintf (line, sizeof (line), "Empty histogram for column: %s", attr_name);
      fprintf (f, "| %-47s|\n", line);
      fprintf (f, "+------------------------------------------------+\n");
      return NO_ERROR;
    }

  if (!with_fullscan)
    {
      snprintf (line, sizeof (line),
		" rows   : %d   sample : %d (%.1f%%)",
		class_->stats->heap_num_objects, rows_scanned, (double) rows_scanned / class_->stats->heap_num_objects * 100.0);
    }
  else
    {
      snprintf (line, sizeof (line),
		" rows   : %d ", static_cast<int> (histogram_reader.total_rows()));
    }
  fprintf (f, "| %-47s|\n", line);

  /* buckets + null frec line : TODO add null frequency */
  snprintf (line, sizeof (line),
	    " buckets + mcv: %d",
	    static_cast<int> (histogram_reader.bucket_count()));
  fprintf (f, "| %-47s|\n", line);

  /* bottom border */
  fputs ("+------------------------------------------------+\n", f);

  const double total_rows = static_cast<double> (histogram_reader.total_rows ());
  const int bucket_cnt = static_cast<int> (histogram_reader.bucket_count ());

  for (int i = 0; i < bucket_cnt; i++)
    {
      const int rows = static_cast<int> (histogram_reader.bucket_rows (i));
      const double sel =
	      (total_rows > 0.0
	       ? static_cast<double> (rows) / total_rows
	       : 0.0);

      const std::int32_t ndv =
	      static_cast<std::int32_t> (histogram_reader.bucket_approx_ndv (i));
      const bool is_mcv = (ndv == 1);
      const double cum_sel =
	      (total_rows > 0.0
	       ? static_cast<double> (histogram_reader.bucket_cumulative (i)) / total_rows
	       : 0.0);

      const char *mcv_suffix = is_mcv ? " (MCV)" : "";

      if (i == 0)
	{
	  std::string hi = histogram_reader.bucket_hi_dump_with_type (i, attr_type);
	  std::fprintf (f,
			"#%02d (-inf, %s] rows=%d(%.3f) ndv=%d%s  cum=%.3f\n",
			i,
			hi.c_str (),
			rows,
			sel,
			ndv,
			mcv_suffix,
			cum_sel);
	}
      else
	{
	  std::string lo = histogram_reader.bucket_hi_dump_with_type (i - 1, attr_type);
	  std::string hi = histogram_reader.bucket_hi_dump_with_type (i, attr_type);
	  std::fprintf (f,
			"#%02d (%s, %s] rows=%d(%.3f) ndv=%d%s  cum=%.3f\n",
			i,
			lo.c_str (),
			hi.c_str (),
			rows,
			sel,
			ndv,
			mcv_suffix,
			cum_sel);
	}
    }

  return NO_ERROR;
}