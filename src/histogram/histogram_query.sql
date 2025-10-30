WITH src AS (
  /* NULL 제외 + 샘플링(전수면 WHERE RAND()<... 제거) */
  SELECT /*+ RECOMPILE */
         t. /*COLUMN*/ AS val
  FROM   /*TABLE*/ AS t
  WHERE  t. /*COLUMN*/ IS NOT NULL
    AND  RAND() < /*SAMPLE_RATIO*/      -- 예: 0.02 (2%). 전수면 이 줄 삭제
),
ranked AS (
  SELECT
      val,
      ROW_NUMBER() OVER (ORDER BY val) AS rn,
      COUNT(*)    OVER ()              AS n
  FROM src
),
bucketed AS (
  SELECT
      CAST(FLOOR((/*BUCKETS*/ * (rn - 1)) / n) AS INT) AS bid,  -- 0..B-1
      val
  FROM ranked
),
agg AS (
  SELECT
      bid,
      MAX(val)              AS endpoint,         -- 버킷 상한(hi)
      COUNT(*)              AS rows_in_bucket,
      COUNT(DISTINCT val)   AS approx_ndv        -- 샘플 기준 NDV 
  FROM bucketed
  GROUP BY bid
)
SELECT
    bid,
    endpoint,
    rows_in_bucket,
    SUM(rows_in_bucket) OVER (ORDER BY bid
        ROWS UNBOUNDED PRECEDING) AS cumulative,
    approx_ndv
FROM agg
ORDER BY bid;