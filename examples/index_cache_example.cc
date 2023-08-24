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

#include <iostream>

#include "tenann/common/logging.h"
#include "tenann/store/index_cache.h"

class IndexMock {
 public:
  IndexMock() : name() {}
  IndexMock(const std::string& name) : name(name){};
  ~IndexMock() { T_LOG(INFO) << "Index destroyed: " << name; }

  std::string name;
};

int main() {
  using namespace tenann;
  IndexRef index_ref =
      std::make_shared<Index>(new IndexMock("index1"),  //
                              IndexType::kFaissHnsw,    //
                              [](void* index) { delete reinterpret_cast<IndexMock*>(index); });

  T_LOG(INFO) << "Index built: " << reinterpret_cast<IndexMock*>(index_ref->index_raw())->name;

  // write index to cache
  auto* cache = IndexCache::GetGlobalInstance();
  cache->Insert("index1", index_ref);

  // read index from cache
  IndexCacheHandle read_handle;
  auto found = cache->Lookup("index1", &read_handle);
  T_CHECK(found);

  // There should be two references to the index: one is original index_ref, and another is held by
  // the cache.
  T_LOG(INFO) << "IndexRef use count: " << index_ref.use_count();

  auto shared_ref_from_cache = read_handle.index_ref();
  // There should be three references to the index:
  // 1. `index_ref`
  // 2. the reference held by the cache
  // 3. `shared_ref_from_cache`
  T_LOG(INFO) << "IndexRef use count: " << index_ref.use_count();

  T_LOG(INFO) << "Index read from cache: "
              << reinterpret_cast<const IndexMock*>(shared_ref_from_cache->index_raw())->name;
  return 0;
}