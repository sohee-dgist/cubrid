#include "thread_compat.hpp"

// Forward declaration for PT_NODE
struct parser_node;
typedef struct parser_node PT_NODE;

static const char *HISTOGRAM_QUERY_TEMPLATE =
	"WITH src AS (SELECT %s AS val FROM %s WHERE %s IS NOT NULL), "
	"cnt AS (SELECT val, COUNT(*) AS c FROM src GROUP BY val), "
	"mcv_ranked AS (SELECT val, c, ROW_NUMBER() OVER (ORDER BY c DESC, val) AS rn FROM cnt ORDER BY c DESC LIMIT %d), "
	"non_mcv_flagged AS (SELECT val, c FROM cnt WHERE val NOT IN (SELECT val FROM mcv_ranked)), "
	"hist_acc AS (SELECT val, c, SUM(c) OVER (ORDER BY val) AS cum, SUM(c) OVER () AS n FROM non_mcv_flagged), "
	"param AS (SELECT CASE WHEN n > 0 THEN CEIL(n * 1.0 / %d) ELSE 1 END AS cap, n FROM hist_acc LIMIT 1), "
	"hist_buckets AS (SELECT LEAST(FLOOR((cum - 1) / param.cap), %d - 1) AS bid, val, c, FALSE AS is_mcv FROM hist_acc, param), "
	"mcv_buckets AS (SELECT -rn AS bid, val, c, TRUE AS is_mcv FROM mcv_ranked), "
	"all_buckets AS (SELECT * FROM hist_buckets UNION ALL SELECT * FROM mcv_buckets) "
	"SELECT bid, MAX(val) AS endpoint, SUM(c) AS rows_in_bucket, SUM(SUM(c)) OVER (ORDER BY MAX(val)) AS cumulative, "
	"COUNT(*) AS approx_ndv, MAX(is_mcv) AS is_mcv FROM all_buckets GROUP BY bid ORDER BY MAX(val);";
static const char *HISTOGRAM_WITH_SAMPLING_SCAN_QUERY_TEMPLATE =
	"WITH src AS (SELECT /*+ SAMPLING_SCAN */ %s AS val FROM %s WHERE %s IS NOT NULL), "
	"cnt AS (SELECT val, COUNT(*) AS c FROM src GROUP BY val), "
	"mcv_ranked AS (SELECT val, c, ROW_NUMBER() OVER (ORDER BY c DESC, val) AS rn FROM cnt ORDER BY c DESC LIMIT %d), "
	"non_mcv_flagged AS (SELECT val, c FROM cnt WHERE val NOT IN (SELECT val FROM mcv_ranked)), "
	"hist_acc AS (SELECT val, c, SUM(c) OVER (ORDER BY val) AS cum, SUM(c) OVER () AS n FROM non_mcv_flagged), "
	"param AS (SELECT CASE WHEN n > 0 THEN CEIL(n * 1.0 / %d) ELSE 1 END AS cap, n FROM hist_acc LIMIT 1), "
	"hist_buckets AS (SELECT LEAST(FLOOR((cum - 1) / param.cap), %d - 1) AS bid, val, c, FALSE AS is_mcv FROM hist_acc, param), "
	"mcv_buckets AS (SELECT -rn AS bid, val, c, TRUE AS is_mcv FROM mcv_ranked), "
	"all_buckets AS (SELECT * FROM hist_buckets UNION ALL SELECT * FROM mcv_buckets) "
	"SELECT bid, MAX(val) AS endpoint, SUM(c) AS rows_in_bucket, SUM(SUM(c)) OVER (ORDER BY MAX(val)) AS cumulative, "
	"COUNT(*) AS approx_ndv, MAX(is_mcv) AS is_mcv FROM all_buckets GROUP BY bid ORDER BY MAX(val);";

int analyze_classes (THREAD_ENTRY *thread_p, const char *tbl_name, const char *attr_name, int max_number_of_buckets,
		     int with_fullscan, MOP classop);
int get_histogram (THREAD_ENTRY *thread_p, const char *tbl_name, const char *attr_name, int max_number_of_buckets,
		   int with_fullscan, char **histogram_blob, int *histogram_total_length);
int set_histogram (THREAD_ENTRY *thread_p, const char *tbl_name, const char *attr_name, char *histogram_blob,
		   int histogram_total_length, MOP classop);
void histogram_get_equal_selectivity (PT_NODE *lhs, PT_NODE *rhs, double *selectivity);
int db_get_histogram (MOP classop, const char *attr_name, DB_OBJECT **histogram_obj);