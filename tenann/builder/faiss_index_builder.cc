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

#include "faiss_index_builder.h"

#include <sstream>

#include "faiss/IndexHNSW.h"
#include "faiss/IndexIDMap.h"
#include "faiss/index_factory.h"
#include "faiss_hnsw_index_builder.h"
#include "fmt/format.h"
#include "tenann/builder/faiss_hnsw_index_builder.h"
#include "tenann/common/logging.h"
#include "tenann/common/typed_seq_view.h"
#include "tenann/index/index.h"
#include "tenann/index/internal/faiss_index_util.h"
#include "tenann/index/parameter_serde.h"
#include "tenann/util/runtime_profile.h"
#include "tenann/util/runtime_profile_macros.h"

namespace tenann {

FaissIndexBuilder::FaissIndexBuilder(const IndexMeta& meta) : IndexBuilder(meta) {
  FetchParameters(meta, &common_params_);
  FetchParameters(meta, &extra_params_);

  T_CHECK(common_params_.metric_type == MetricType::kL2Distance ||
          common_params_.metric_type == MetricType::kCosineSimilarity ||
          common_params_.metric_type == MetricType::kInnerProduct)
      << "only l2_distance and cosine_similarity are supported";
}

FaissIndexBuilder::~FaissIndexBuilder(){};

IndexBuilder& FaissIndexBuilder::Open() {
  try {
    T_SCOPED_TIMER(open_total_timer_);
    T_LOG_IF(ERROR, is_opened_) << "index builder has already been opened";

    memory_only_ = true;
    index_save_path_ = "";
    index_ref_ = InitIndex();
    SetOpenState();
    return *this;
  }
  CATCH_FAISS_ERROR;
}

IndexBuilder& FaissIndexBuilder::Open(const std::string& path) {
  try {
    T_SCOPED_TIMER(open_total_timer_);
    T_LOG_IF(ERROR, is_opened_) << "index builder has already been opened";

    memory_only_ = false;
    index_save_path_ = path;
    index_ref_ = InitIndex();
    SetOpenState();
  }
  CATCH_FAISS_ERROR;

  return *this;
}

IndexBuilder& FaissIndexBuilder::Add(const std::vector<SeqView>& input_columns,
                                     const idx_t* row_ids, const uint8_t* null_flags,
                                     bool inputs_live_longer_than_this) {
  try {
    T_SCOPED_TIMER(add_total_timer_);

    // check builder states
    T_LOG_IF(ERROR, !is_opened_) << "index builder has not been opened";
    T_LOG_IF(ERROR, is_closed_) << "index builder has already been closed";
    T_LOG_IF(ERROR, this->use_custom_row_id_ && (row_ids == nullptr))
        << "custom rowid is enabled, please add data with rowids";
    T_LOG_IF(ERROR, !this->use_custom_row_id_ && (row_ids != nullptr))
        << "custom rowid is disabled, adding data with rowids is not supported";
    T_LOG_IF(ERROR, !this->use_custom_row_id_ && (null_flags != nullptr))
        << "custom rowid is disabled, adding data with null flags is not supported";

    // check input parameters
    T_CHECK(input_columns.size() == 1);
    auto input_seq_type = input_columns[0].seq_view_type;
    T_CHECK(input_seq_type == SeqViewType::kArraySeqView ||
            input_seq_type == SeqViewType::kVlArraySeqView);
    T_CHECK(input_columns[0].seq_view.array_seq_view.elem_type == PrimitiveType::kFloatType ||
            input_columns[0].seq_view.vl_array_seq_view.elem_type == PrimitiveType::kFloatType);

    inputs_live_longer_than_this_ = inputs_live_longer_than_this;

    // add data to index
    AddImpl(input_columns, row_ids, null_flags);
  }
  CATCH_FAISS_ERROR;

  return *this;
}

IndexBuilder& FaissIndexBuilder::Flush() {
  try {
    T_SCOPED_TIMER(flush_total_timer_);

    T_LOG_IF(ERROR, !is_opened_) << "index builder has not been opened";
    T_LOG_IF(ERROR, is_closed_) << "index builder has already been closed";
    T_LOG_IF(ERROR, index_ref_ == nullptr) << "index has not been built";

    index_writer_->WriteIndex(index_ref_, index_save_path_, memory_only_);
  }
  CATCH_FAISS_ERROR;

  return *this;
}

void FaissIndexBuilder::Close() {
  // @TODO(petri): clear buffer
  T_LOG_IF(ERROR, !is_opened_) << "index builder has not been opened";
  SetCloseState();
}

bool FaissIndexBuilder::is_opened() { return is_opened_; }

bool FaissIndexBuilder::is_closed() { return is_closed_; }

void FaissIndexBuilder::PrepareProfile() {
  open_total_timer_ = T_ADD_TIMER(profile_, "OpenTotalTime");
  add_total_timer_ = T_ADD_TIMER(profile_, "AddTotalTime");
  flush_total_timer_ = T_ADD_TIMER(profile_, "FlushTotalTime");
  close_total_timer_ = T_ADD_TIMER(profile_, "CloseTotalTime");
}

void FaissIndexBuilder::AddImpl(const std::vector<SeqView>& input_columns, const idx_t* row_ids,
                                const uint8_t* null_flags) {
  auto input_seq_type = input_columns[0].seq_view_type;

  std::unique_ptr<TypedSliceIterator<float>> input_row_iterator = nullptr;
  if (input_seq_type == SeqViewType::kArraySeqView) {
    input_row_iterator =
        std::make_unique<TypedSliceIterator<float>>(input_columns[0].seq_view.array_seq_view);
  } else if (input_seq_type == SeqViewType::kVlArraySeqView) {
    CheckDimension(input_columns[0].seq_view.vl_array_seq_view, common_params_.dim);
    input_row_iterator =
        std::make_unique<TypedSliceIterator<float>>(input_columns[0].seq_view.vl_array_seq_view);
  }
  T_DCHECK_NOTNULL(input_row_iterator);

  if (row_ids == nullptr && null_flags == nullptr) {
    AddRaw(*input_row_iterator);
    return;
  }

  if (row_ids != nullptr && null_flags == nullptr) {
    AddWithRowIds(*input_row_iterator, row_ids);
  }

  if (row_ids != nullptr && null_flags != nullptr) {
    AddWithRowIdsAndNullFlags(*input_row_iterator, row_ids, null_flags);
  }

  if (row_ids == nullptr && null_flags != nullptr) {
    T_LOG(ERROR) << "adding nullable data without rowids is not supported";
    return;
  }
}

void FaissIndexBuilder::AddRaw(const TypedSliceIterator<float>& input_row_iterator) {
  auto faiss_index = GetFaissIndex();
  faiss_index->add(input_row_iterator.size(), input_row_iterator.data());
}

void FaissIndexBuilder::AddWithRowIds(const TypedSliceIterator<float>& input_row_iterator,
                                      const idx_t* row_ids) {
  auto faiss_index = GetFaissIndex();
  FaissIndexAddBatch(faiss_index, input_row_iterator.size(), input_row_iterator.data(), row_ids);
}

void FaissIndexBuilder::AddWithRowIdsAndNullFlags(
    const TypedSliceIterator<float>& input_row_iterator, const idx_t* row_ids,
    const uint8_t* null_flags) {
  auto faiss_index = GetFaissIndex();
  input_row_iterator.ForEach([=](idx_t i, const float* slice_data, idx_t slice_length) {
    if (null_flags[i] == 0) {
      FaissIndexAddSingle(faiss_index, slice_data, row_ids + i);
    }
  });
}

void FaissIndexBuilder::FaissIndexAddBatch(faiss::Index* index, idx_t num_rows, const float* data,
                                           const idx_t* rowids) {
  if (rowids != nullptr) {
    index->add_with_ids(num_rows, data, rowids);
  } else {
    index->add(num_rows, data);
  }
}

void FaissIndexBuilder::FaissIndexAddSingle(faiss::Index* index, const float* data,
                                            const idx_t* rowid) {
  if (rowid != nullptr) {
    index->add_with_ids(1, data, rowid);
  } else {
    index->add(1, data);
  }
}

void FaissIndexBuilder::CheckDimension(const TypedSliceIterator<float>& input_column, idx_t dim) {
  // check vector sizes
  input_column.ForEach([=](idx_t i, const float* slice_data, idx_t slice_lengh) {
    T_LOG_IF(ERROR, slice_lengh != dim)
        << "invalid size for vector " << i << " : expected " << dim << " but got " << slice_lengh;
  });
}

faiss::Index* FaissIndexBuilder::GetFaissIndex() {
  return static_cast<faiss::Index*>(index_ref_->index_raw());
}

void FaissIndexBuilder::SetOpenState() {
  is_opened_ = true;
  is_closed_ = false;
}

void FaissIndexBuilder::SetCloseState() {
  is_opened_ = false;
  is_closed_ = true;
}

}  // namespace tenann
