WITH src AS (
  SELECT /*+ RECOMPILE */
         t.<COLUMN> AS val
  FROM   <TABLE> t
  WHERE  val IS NOT NULL
),
cnt AS (
  SELECT val, COUNT(*) AS c
  FROM src
  GROUP BY val
),
acc AS (
  SELECT
    val, c,
    SUM(c)  OVER (ORDER BY val) AS cum,
    SUM(c)  OVER ()             AS n
  FROM cnt
),
param AS (
  SELECT
    CASE WHEN n > 0 THEN CEIL(n * 1.0 / <BUCKETS>) ELSE 1 END AS cap,
    n
  FROM acc
  LIMIT 1
),
b AS (
  SELECT
    LEAST( FLOOR( (acc.cum - 1) / param.cap ), <BUCKETS> - 1 ) AS bid,
    acc.val,
    acc.c                   AS rows_for_val
  FROM acc, param
)
SELECT
  b.bid,
  MAX(b.val)                                   AS endpoint,
  SUM(b.rows_for_val)                           AS rows_in_bucket,
  SUM(SUM(b.rows_for_val)) OVER (ORDER BY b.bid) AS cumulative,
  COUNT(*)                                     AS approx_ndv
FROM b
GROUP BY b.bid
ORDER BY b.bid;