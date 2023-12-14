/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "tenann/builder/faiss_ivf_pq_index_builder.h"

#include <sstream>

#include "faiss/IndexIVFPQ.h"
#include "faiss/index_factory.h"
#include "faiss_ivf_pq_index_builder.h"
#include "tenann/common/logging.h"
#include "tenann/common/typed_seq_view.h"
#include "tenann/index/index.h"
#include "tenann/index/internal/index_ivfpq.h"
#include "tenann/index/internal/faiss_index_util.h"
#include "tenann/index/parameter_serde.h"

namespace tenann {
FaissIvfPqIndexBuilder::FaissIvfPqIndexBuilder(const IndexMeta& meta)
    : FaissIndexBuilderWithBuffer(meta) {
  FetchParameters(meta, &index_params_);
  FetchParameters(meta, &search_params_);
  T_CHECK(common_params_.metric_type == MetricType::kL2Distance)
      << "got unsupported metric, only l2_distance is supported for IVF-PQ";
}

FaissIvfPqIndexBuilder::~FaissIvfPqIndexBuilder() = default;

IndexRef FaissIvfPqIndexBuilder::InitIndex() {
  try {
    // use bruteforce coarse quantizer by default
    auto quantizer = std::make_unique<faiss::IndexFlatL2>(common_params_.dim);
    auto index_ivfpq =
        std::make_unique<IndexIvfPq>(quantizer.release(), common_params_.dim, index_params_.nlist,
                                      index_params_.M, index_params_.nbits);
    index_ivfpq->own_fields = true;

    // default search params
    index_ivfpq->nprobe = search_params_.nprobe;
    index_ivfpq->max_codes = search_params_.max_codes;
    index_ivfpq->scan_table_threshold = search_params_.scan_table_threshold;
    index_ivfpq->polysemous_ht = search_params_.polysemous_ht;
    index_ivfpq->range_search_confidence = search_params_.range_search_confidence;

    VLOG(VERBOSE_DEBUG) << "nlist: " << index_ivfpq->invlists->nlist << ", M: " << index_ivfpq->pq.M
                        << ", nbits: " << index_ivfpq->pq.nbits;

    return std::make_shared<Index>(index_ivfpq.release(),  //
                                   IndexType::kFaissIvfPq,  //
                                   [](void* index) { delete static_cast<IndexIvfPq*>(index); });
  }
  CATCH_FAISS_ERROR
  CATCH_JSON_ERROR
}

}  // namespace tenann