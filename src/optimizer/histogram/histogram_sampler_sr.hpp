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
 * histogram_sampler_sr.hpp - server-side histogram collection by full-scan reservoir sampling
 */

#ifndef _HISTOGRAM_SAMPLER_SR_HPP_
#define _HISTOGRAM_SAMPLER_SR_HPP_

#ifdef RESERVOIR_SAMPLING

#include "dbtype_def.h"
#include "storage_common.h"
#include "thread_compat.hpp"

/*
 * xhistogram_build_by_fullscan_reservoir () - full-scan a class, reservoir-sample one
 *   attribute, and build the histogram blob server-side (replaces the query-based path).
 *
 *   thread_p(in)       : thread entry
 *   class_oid(in)      : class OID
 *   hfid(in)           : heap file id of the class
 *   attr_id(in)        : attribute id to build the histogram for
 *   attr_type(in)      : attribute domain type
 *   max_buckets(in)    : maximum number of buckets
 *   sample_size(in)    : reservoir capacity (number of non-null values to keep)
 *   null_frequency(out): fraction of NULLs over all rows (exact; full scan)
 *   histogram_blob(out): db_private_alloc'd blob (caller frees); NULL if no data
 *   blob_length(out)   : length of histogram_blob
 *
 *   return: NO_ERROR or error code
 */
extern int xhistogram_build_by_fullscan_reservoir (THREAD_ENTRY *thread_p, const OID *class_oid, const HFID *hfid,
    ATTR_ID attr_id, DB_TYPE attr_type, int max_buckets, int sample_size, double *null_frequency,
    char **histogram_blob, int *blob_length);

#endif /* RESERVOIR_SAMPLING */
#endif /* _HISTOGRAM_SAMPLER_SR_HPP_ */
