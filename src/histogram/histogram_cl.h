#include "thread_compat.hpp"

static const char *HISTOGRAM_QUERY_TEMPLATE =
  "WITH src AS (\n"
  "  SELECT /*+ RECOMPILE */\n"
  "         %s AS val\n"
  "  FROM %s\n"
  "  WHERE %s IS NOT NULL\n"
  "),\n"
  "cnt AS (\n"
  "  SELECT val, COUNT(*) AS c\n"
  "  FROM src\n"
  "  GROUP BY val\n"
  "),\n"
  "acc AS (\n"
  "  SELECT\n"
  "    val, c,\n"
  "    SUM(c) OVER (ORDER BY val) AS cum,\n"
  "    SUM(c) OVER ()             AS n\n"
  "  FROM cnt\n"
  "),\n"
  "param AS (\n"
  "  SELECT\n"
  "    CASE WHEN n > 0 THEN CEIL(n * 1.0 / %d) ELSE 1 END AS cap,\n"
  "    n\n"
  "  FROM acc\n"
  "  LIMIT 1\n"
  "),\n"
  "b AS (\n"
  "  SELECT\n"
  "    LEAST(FLOOR((acc.cum - 1) / param.cap), %d - 1) AS bid,\n"
  "    acc.val,\n"
  "    acc.c AS rows_for_val\n"
  "  FROM acc, param\n"
  ")\n"
  "SELECT\n"
  "  b.bid,\n"
  "  MAX(b.val)                                    AS endpoint,\n"
  "  SUM(b.rows_for_val)                            AS rows_in_bucket,\n"
  "  SUM(SUM(b.rows_for_val)) OVER (ORDER BY b.bid) AS cumulative,\n"
  "  COUNT(*)                                       AS approx_ndv\n" "FROM b\n" "GROUP BY b.bid\n" "ORDER BY b.bid\n";

int analyze_classes (THREAD_ENTRY * thread_p, const char *tbl_name, const char *attr_name, int max_number_of_buckets,
		     int with_fullscan, MOP classop);
int get_histogram (THREAD_ENTRY * thread_p, const char *tbl_name, const char *attr_name, int max_number_of_buckets,
		   int with_fullscan, char **histogram_blob, int *histogram_total_length);
int set_histogram (THREAD_ENTRY * thread_p, const char *tbl_name, const char *attr_name, char *histogram_blob,
		   int histogram_total_length, MOP classop);
