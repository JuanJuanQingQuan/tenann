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

#include <sys/time.h>

#include <cmath>
#include <cstdio>
#include <iostream>
#include <random>

#include "tenann/index/parameters.h"
#include "tenann/searcher/internal/id_filter_adapter.h"
#include "test/faiss_test_base.h"

namespace tenann {
class IvfPqRangeSearchTest : public FaissTestBase {
 public:
  IvfPqRangeSearchTest() : FaissTestBase() {
    InitFaissIvfPqMeta();
    faiss_ivf_pq_meta_.index_writer_options()["write_index_cache"] = true;
    faiss_ivf_pq_meta_.index_writer_options()["custom_cache_key"] = kCacheKey;
    faiss_ivf_pq_index_builder_ = IndexFactory::CreateBuilderFromMeta(faiss_ivf_pq_meta_);
  }

  void BuildInMemoryIvfPq() {
    auto* cache = IndexCache::GetGlobalInstance();
    IndexCacheHandle handle;
    if (!cache->Lookup(kCacheKey, &handle)) {
      faiss_ivf_pq_index_builder_
          ->Open()            //
          .Add({base_view_})  //
          .Flush()            //
          .Close();
    }
  }

  std::shared_ptr<AnnSearcher> GetAnnSearcher() {
    faiss_ivf_pq_meta_.index_reader_options()[tenann::IndexReaderOptions::cache_index_file_key] = true;
    faiss_ivf_pq_meta_.index_reader_options()["custom_cache_key"] = kCacheKey;
    auto searcher = AnnSearcherFactory::CreateSearcherFromMeta(faiss_ivf_pq_meta_);
    searcher->ReadIndex("");
    return searcher;
  };

  static constexpr const char* kCacheKey = "IvfPqRangeSearchTest";
  static constexpr const float radius = 1;
};

TEST_F(IvfPqRangeSearchTest, test_range_search_asending) {
  BuildInMemoryIvfPq();
  auto searcher = GetAnnSearcher();

  std::vector<int64_t> result_ids;
  std::vector<float> result_distances;
  int64_t limit = 10;

  // default range search with confidence = 0
  searcher->RangeSearch(query_view()[0], radius, limit, AnnSearcher::ResultOrder::kAscending,
                        &result_ids, &result_distances);
  EXPECT_EQ(result_ids.size(), limit);
  EXPECT_EQ(result_distances.size(), limit);
  // check asending order

  for (int i = 0; i < result_distances.size() - 1; i++) {
    // when range_search_confidence=0, the approximate pq distance is directly used for filtering
    EXPECT_LE(result_distances[i], limit);
    EXPECT_LE(result_distances[i], result_distances[i + 1]);
  }

  // range search with confidence 1
  searcher->SetSearchParamItem(FaissIvfPqSearchParams::range_search_confidence_key, 1.0f);
  searcher->RangeSearch(query_view()[0], radius, -1, AnnSearcher::ResultOrder::kAscending,
                        &result_ids, &result_distances);

  // check asending order
  for (int i = 0; i < result_distances.size() - 1; i++) {
    EXPECT_LE(result_distances[i], result_distances[i + 1]);
  }
}

TEST_F(IvfPqRangeSearchTest, test_range_search_desending) {
  BuildInMemoryIvfPq();
  auto searcher = GetAnnSearcher();

  std::vector<int64_t> result_ids;
  std::vector<float> result_distances;
  int64_t limit = 10;

  // descending order is not supported now
  EXPECT_THROW(
      searcher->RangeSearch(query_view()[0], radius, limit, AnnSearcher::ResultOrder::kDescending,
                            &result_ids, &result_distances),
      Error);
}

TEST_F(IvfPqRangeSearchTest, test_range_search_with_filter) {
  BuildInMemoryIvfPq();
  auto searcher = GetAnnSearcher();

  std::vector<int64_t> result_ids;
  std::vector<float> result_distances;
  int64_t limit = 10;

  int64_t start = 0, end = 10;
  RangeIdFilter filter(start, end);
  searcher->RangeSearch(query_view()[0], INFINITY, limit, AnnSearcher::ResultOrder::kAscending,
                        &result_ids, &result_distances, &filter);

  for (auto id : result_ids) {
    EXPECT_GE(id, start);
    EXPECT_LT(id, end);
  }
}

}  // namespace tenann