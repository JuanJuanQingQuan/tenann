/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-
#include "tenann/index/internal/custom_ivfpq.h"

#include <faiss/Clustering.h>
#include <faiss/IndexFlat.h>
#include <faiss/impl/AuxIndexStructures.h>
#include <faiss/impl/FaissAssert.h>
#include <faiss/impl/IDSelector.h>
#include <faiss/impl/ProductQuantizer.h>
#include <faiss/utils/Heap.h>
#include <faiss/utils/distances.h>
#include <faiss/utils/hamming.h>
#include <faiss/utils/utils.h>
#include <stdint.h>

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstdio>

#ifdef __AVX2__
#include <immintrin.h>

#include "custom_ivfpq.h"
#endif

namespace tenann {

using namespace faiss;

static float* compute_residuals(const Index* quantizer, Index::idx_t n, const float* x,
                                const Index::idx_t* list_nos) {
  size_t d = quantizer->d;
  float* residuals = new float[n * d];
  // TODO: parallelize?
  for (size_t i = 0; i < n; i++) {
    if (list_nos[i] < 0)
      memset(residuals + i * d, 0, sizeof(*residuals) * d);
    else
      quantizer->compute_residual(x + i * d, residuals + i * d, list_nos[i]);
  }
  return residuals;
}

CustomIvfPq::CustomIvfPq(faiss::Index* quantizer, size_t d, size_t nlist, size_t M,
                         size_t nbits_per_idx)
    : IndexIVFPQ(quantizer, d, nlist, M, nbits_per_idx, faiss::METRIC_L2) {
  /* The following lines are added by tenann */
  reconstruction_errors.resize(nlist);
  /* End tenann.*/
}

void CustomIvfPq::add_core(idx_t n, const float* x, const idx_t* xids, const idx_t* coarse_idx) {
  // add_core_o(n, x, xids, nullptr, coarse_idx);

  /* The following lines are added by tenann */
  // Add vectors into the index
  // and compute the residuals between the raw vectors and the reconstructed vectors
  std::vector<float> residual2(n * d);
  custom_add_core_o(n, x, xids, residual2.data(), coarse_idx);
  /* End tenann.*/
}

// block size used in CustomIvfPq::add_core_o
static int index_ivfpq_add_core_o_bs = 32768;

void CustomIvfPq::custom_add_core_o(idx_t n, const float* x, const idx_t* xids, float* residuals_2,
                                    const idx_t* precomputed_idx) {
  idx_t bs = index_ivfpq_add_core_o_bs;
  if (n > bs) {
    for (idx_t i0 = 0; i0 < n; i0 += bs) {
      idx_t i1 = std::min(i0 + bs, n);
      if (verbose) {
        printf("CustomIvfPq::add_core_o: adding %" PRId64 ":%" PRId64 " / %" PRId64 "\n", i0, i1,
               n);
      }
      add_core_o(i1 - i0, x + i0 * d, xids ? xids + i0 : nullptr,
                 residuals_2 ? residuals_2 + i0 * d : nullptr,
                 precomputed_idx ? precomputed_idx + i0 : nullptr);
    }
    return;
  }

  InterruptCallback::check();

  direct_map.check_can_add(xids);

  FAISS_THROW_IF_NOT(is_trained);
  double t0 = getmillisecs();
  const idx_t* idx;
  ScopeDeleter<idx_t> del_idx;

  if (precomputed_idx) {
    idx = precomputed_idx;
  } else {
    idx_t* idx0 = new idx_t[n];
    del_idx.set(idx0);
    quantizer->assign(n, x, idx0);
    idx = idx0;
  }

  double t1 = getmillisecs();
  uint8_t* xcodes = new uint8_t[n * code_size];
  ScopeDeleter<uint8_t> del_xcodes(xcodes);

  const float* to_encode = nullptr;
  ScopeDeleter<float> del_to_encode;

  if (by_residual) {
    to_encode = compute_residuals(quantizer, n, x, idx);
    del_to_encode.set(to_encode);
  } else {
    to_encode = x;
  }
  pq.compute_codes(to_encode, xcodes, n);

  double t2 = getmillisecs();
  // TODO: parallelize?
  size_t n_ignore = 0;
  for (size_t i = 0; i < n; i++) {
    idx_t key = idx[i];
    idx_t id = xids ? xids[i] : ntotal + i;
    if (key < 0) {
      direct_map.add_single_id(id, -1, 0);
      n_ignore++;
      if (residuals_2) memset(residuals_2, 0, sizeof(*residuals_2) * d);
      continue;
    }

    uint8_t* code = xcodes + i * code_size;
    size_t offset = invlists->add_entry(key, id, code);

    if (residuals_2) {
      float* res2 = residuals_2 + i * d;
      const float* xi = to_encode + i * d;
      pq.decode(code, res2);
      for (int j = 0; j < d; j++) res2[j] = xi[j] - res2[j];

      /* The following lines are added by tenann */
      float reconstruction_error;
      fvec_norms_L2(&reconstruction_error, res2, d, 1);
      reconstruction_errors[key].push_back(reconstruction_error);
      FAISS_THROW_IF_NOT(reconstruction_errors[key].size() != offset);
      /* End tenann.*/
    }

    direct_map.add_single_id(id, key, offset);
  }

  double t3 = getmillisecs();
  if (verbose) {
    char comment[100] = {0};
    if (n_ignore > 0) snprintf(comment, 100, "(%zd vectors ignored)", n_ignore);
    printf(" add_core times: %.3f %.3f %.3f %s\n", t1 - t0, t2 - t1, t3 - t2, comment);
  }
  ntotal += n;
}

/// 2G by default, accommodates tables up to PQ32 w/ 65536 centroids
static size_t precomputed_table_max_bytes = ((size_t)1) << 31;

namespace {

using idx_t = Index::idx_t;

#define TIC t0 = get_cycles()
#define TOC get_cycles() - t0

/** QueryTables manages the various ways of searching an
 * CustomIvfPq. The code contains a lot of branches, depending on:
 * - metric_type: are we computing L2 or Inner product similarity?
 * - by_residual: do we encode raw vectors or residuals?
 * - use_precomputed_table: are x_R|x_C tables precomputed?
 * - polysemous_ht: are we filtering with polysemous codes?
 */
struct QueryTables {
  /*****************************************************
   * General data from the IVFPQ
   *****************************************************/

  const CustomIvfPq& ivfpq;
  const IVFSearchParameters* params;

  // copied from CustomIvfPq for easier access
  int d;
  const ProductQuantizer& pq;
  MetricType metric_type;
  bool by_residual;
  int use_precomputed_table;
  int polysemous_ht;

  /* The following lines are added by tenann */
  /*
   *  1. by_residual ==  true:
   *    d = || x - y_C ||^2 + || y_R ||^2 + 2 * (y_C|y_R) - 2 * (x|y_R)
   *        ---------------   ---------------------------       -------
   *             term 1                 term 2                   term 3
   *
   *    sim_table = term1 + term2 + term3, sim_table_2 = term3
   *  2. by_residual == false:
   *    d = || x - y_R ||^2
   *        ---------------
   *             term
   *
   *    sim_table = term, sim_table_2 = nullptr
   */
  /* End tenann. */
  // pre-allocated data buffers
  float *sim_table, *sim_table_2;
  float *residual_vec, *decoded_vec;

  // single data buffer
  std::vector<float> mem;

  // for table pointers
  std::vector<const float*> sim_table_ptrs;

  explicit QueryTables(const CustomIvfPq& ivfpq, const IVFSearchParameters* params)
      : ivfpq(ivfpq),
        d(ivfpq.d),
        pq(ivfpq.pq),
        metric_type(ivfpq.metric_type),
        by_residual(ivfpq.by_residual),
        use_precomputed_table(ivfpq.use_precomputed_table) {
    mem.resize(pq.ksub * pq.M * 2 + d * 2);
    sim_table = mem.data();
    sim_table_2 = sim_table + pq.ksub * pq.M;
    residual_vec = sim_table_2 + pq.ksub * pq.M;
    decoded_vec = residual_vec + d;

    // for polysemous
    polysemous_ht = ivfpq.polysemous_ht;
    if (auto ivfpq_params = dynamic_cast<const IVFPQSearchParameters*>(params)) {
      polysemous_ht = ivfpq_params->polysemous_ht;
    }
    if (polysemous_ht != 0) {
      q_code.resize(pq.code_size);
    }
    init_list_cycles = 0;
    sim_table_ptrs.resize(pq.M);
  }

  /*****************************************************
   * What we do when query is known
   *****************************************************/

  // field specific to query
  const float* qi;

  // query-specific initialization
  void init_query(const float* qi) {
    this->qi = qi;
    if (metric_type == METRIC_INNER_PRODUCT)
      init_query_IP();
    else
      init_query_L2();
    if (!by_residual && polysemous_ht != 0) pq.compute_code(qi, q_code.data());
  }

  void init_query_IP() {
    // precompute some tables specific to the query qi
    pq.compute_inner_prod_table(qi, sim_table);
  }

  void init_query_L2() {
    if (!by_residual) {
      pq.compute_distance_table(qi, sim_table);
    } else if (use_precomputed_table) {
      pq.compute_inner_prod_table(qi, sim_table_2);
    }
  }

  /*****************************************************
   * When inverted list is known: prepare computations
   *****************************************************/

  // fields specific to list
  Index::idx_t key;
  float coarse_dis;
  std::vector<uint8_t> q_code;

  uint64_t init_list_cycles;

  /// once we know the query and the centroid, we can prepare the
  /// sim_table that will be used for accumulation
  /// and dis0, the initial value
  float precompute_list_tables() {
    float dis0 = 0;
    uint64_t t0;
    TIC;
    if (by_residual) {
      if (metric_type == METRIC_INNER_PRODUCT)
        dis0 = precompute_list_tables_IP();
      else
        dis0 = precompute_list_tables_L2();
    }
    init_list_cycles += TOC;
    return dis0;
  }

  float precompute_list_table_pointers() {
    float dis0 = 0;
    uint64_t t0;
    TIC;
    if (by_residual) {
      if (metric_type == METRIC_INNER_PRODUCT)
        FAISS_THROW_MSG("not implemented");
      else
        dis0 = precompute_list_table_pointers_L2();
    }
    init_list_cycles += TOC;
    return dis0;
  }

  /*****************************************************
   * compute tables for inner prod
   *****************************************************/

  float precompute_list_tables_IP() {
    // prepare the sim_table that will be used for accumulation
    // and dis0, the initial value
    ivfpq.quantizer->reconstruct(key, decoded_vec);
    // decoded_vec = centroid
    float dis0 = fvec_inner_product(qi, decoded_vec, d);

    if (polysemous_ht) {
      for (int i = 0; i < d; i++) {
        residual_vec[i] = qi[i] - decoded_vec[i];
      }
      pq.compute_code(residual_vec, q_code.data());
    }
    return dis0;
  }

  /*****************************************************
   * compute tables for L2 distance
   *****************************************************/

  float precompute_list_tables_L2() {
    float dis0 = 0;

    if (use_precomputed_table == 0 || use_precomputed_table == -1) {
      ivfpq.quantizer->compute_residual(qi, residual_vec, key);
      pq.compute_distance_table(residual_vec, sim_table);

      if (polysemous_ht != 0) {
        pq.compute_code(residual_vec, q_code.data());
      }

    } else if (use_precomputed_table == 1) {
      dis0 = coarse_dis;

      fvec_madd(pq.M * pq.ksub, ivfpq.precomputed_table.data() + key * pq.ksub * pq.M, -2.0,
                sim_table_2, sim_table);

      if (polysemous_ht != 0) {
        ivfpq.quantizer->compute_residual(qi, residual_vec, key);
        pq.compute_code(residual_vec, q_code.data());
      }

    } else if (use_precomputed_table == 2) {
      dis0 = coarse_dis;

      const MultiIndexQuantizer* miq = dynamic_cast<const MultiIndexQuantizer*>(ivfpq.quantizer);
      FAISS_THROW_IF_NOT(miq);
      const ProductQuantizer& cpq = miq->pq;
      int Mf = pq.M / cpq.M;

      const float* qtab = sim_table_2;  // query-specific table
      float* ltab = sim_table;          // (output) list-specific table

      long k = key;
      for (int cm = 0; cm < cpq.M; cm++) {
        // compute PQ index
        int ki = k & ((uint64_t(1) << cpq.nbits) - 1);
        k >>= cpq.nbits;

        // get corresponding table
        const float* pc = ivfpq.precomputed_table.data() + (ki * pq.M + cm * Mf) * pq.ksub;

        if (polysemous_ht == 0) {
          // sum up with query-specific table
          fvec_madd(Mf * pq.ksub, pc, -2.0, qtab, ltab);
          ltab += Mf * pq.ksub;
          qtab += Mf * pq.ksub;
        } else {
          for (int m = cm * Mf; m < (cm + 1) * Mf; m++) {
            q_code[m] = fvec_madd_and_argmin(pq.ksub, pc, -2, qtab, ltab);
            pc += pq.ksub;
            ltab += pq.ksub;
            qtab += pq.ksub;
          }
        }
      }
    }

    return dis0;
  }

  float precompute_list_table_pointers_L2() {
    float dis0 = 0;

    if (use_precomputed_table == 1) {
      dis0 = coarse_dis;

      const float* s = ivfpq.precomputed_table.data() + key * pq.ksub * pq.M;
      for (int m = 0; m < pq.M; m++) {
        sim_table_ptrs[m] = s;
        s += pq.ksub;
      }
    } else if (use_precomputed_table == 2) {
      dis0 = coarse_dis;

      const MultiIndexQuantizer* miq = dynamic_cast<const MultiIndexQuantizer*>(ivfpq.quantizer);
      FAISS_THROW_IF_NOT(miq);
      const ProductQuantizer& cpq = miq->pq;
      int Mf = pq.M / cpq.M;

      long k = key;
      int m0 = 0;
      for (int cm = 0; cm < cpq.M; cm++) {
        int ki = k & ((uint64_t(1) << cpq.nbits) - 1);
        k >>= cpq.nbits;

        const float* pc = ivfpq.precomputed_table.data() + (ki * pq.M + cm * Mf) * pq.ksub;

        for (int m = m0; m < m0 + Mf; m++) {
          sim_table_ptrs[m] = pc;
          pc += pq.ksub;
        }
        m0 += Mf;
      }
    } else {
      FAISS_THROW_MSG("need precomputed tables");
    }

    if (polysemous_ht) {
      FAISS_THROW_MSG("not implemented");
      // Not clear that it makes sense to implemente this,
      // because it costs M * ksub, which is what we wanted to
      // avoid with the tables pointers.
    }

    return dis0;
  }
};

// This way of handling the sleector is not optimal since all distances
// are computed even if the id would filter it out.
template <class C, bool use_sel>
struct KnnSearchResults {
  idx_t key;
  const idx_t* ids;
  const IDSelector* sel;

  // heap params
  size_t k;
  float* heap_sim;
  idx_t* heap_ids;

  size_t nup;

  inline bool skip_entry(idx_t j) { return use_sel && !sel->is_member(ids[j]); }

  inline void add(idx_t j, float dis) {
    if (C::cmp(heap_sim[0], dis)) {
      idx_t id = ids ? ids[j] : lo_build(key, j);
      heap_replace_top<C>(k, heap_sim, heap_ids, dis, id);
      nup++;
    }
  }
};

template <class C, bool use_sel>
struct RangeSearchResults {
  idx_t key;
  const idx_t* ids;
  const IDSelector* sel;
  const CustomIvfPq* ivfpq;  // added by tenann.

  // wrapped result structure
  float radius;
  RangeQueryResult& rres;

  inline bool skip_entry(idx_t j) { return use_sel && !sel->is_member(ids[j]); }

  inline void add(idx_t j, float dis) {
    // if (C::cmp(radius, dis)) { // onl
    //   idx_t id = ids ? ids[j] : lo_build(key, j);
    //   rres.add(dis, id);
    // }

    /* The following lines are added by tenann */
    auto reconstruction_error = ivfpq->reconstruction_errors[key][j];
    // TODO：only works for l2 distance now, we should explicitly validate the given metric
    // The result is valid iff lower bound of distance <= radius.
    // Only works for L2 metric and ADC distance.
    auto lower_bound = std::abs(sqrtf(dis) - reconstruction_error * ivfpq->error_scale);
    if (lower_bound <= radius) {
      idx_t id = ids ? ids[j] : lo_build(key, j);
      rres.add(dis, id);
    }
    /* End tenann. */
  }
};

/*****************************************************
 * Scaning the codes.
 * The scanning functions call their favorite precompute_*
 * function to precompute the tables they need.
 *****************************************************/
template <typename IDType, MetricType METRIC_TYPE, class PQDecoder>
struct IVFPQScannerT : QueryTables {
  const uint8_t* list_codes;
  const IDType* list_ids;
  size_t list_size;

  IVFPQScannerT(const CustomIvfPq& ivfpq, const IVFSearchParameters* params)
      : QueryTables(ivfpq, params) {
    assert(METRIC_TYPE == metric_type);
  }

  float dis0;

  void init_list(idx_t list_no, float coarse_dis, int mode) {
    this->key = list_no;
    this->coarse_dis = coarse_dis;

    if (mode == 2) {
      dis0 = precompute_list_tables();
    } else if (mode == 1) {
      dis0 = precompute_list_table_pointers();
    }
  }

  /*****************************************************
   * Scaning the codes: simple PQ scan.
   *****************************************************/

#ifdef __AVX2__
  /// Returns the distance to a single code.
  /// General-purpose version.
  template <class SearchResultType, typename T = PQDecoder>
  typename std::enable_if<!(std::is_same<T, PQDecoder8>::value),
                          float>::type inline distance_single_code(const uint8_t* code) const {
    PQDecoder decoder(code, pq.nbits);

    const float* tab = sim_table;
    float result = 0;

    for (size_t m = 0; m < pq.M; m++) {
      result += tab[decoder.decode()];
      tab += pq.ksub;
    }

    return result;
  }

  /// Returns the distance to a single code.
  /// Specialized AVX2 PQDecoder8 version.
  template <class SearchResultType, typename T = PQDecoder>
  typename std::enable_if<(std::is_same<T, PQDecoder8>::value),
                          float>::type inline distance_single_code(const uint8_t* code) const {
    float result = 0;

    size_t m = 0;
    const size_t pqM16 = pq.M / 16;

    const float* tab = sim_table;

    if (pqM16 > 0) {
      // process 16 values per loop

      const __m256i ksub = _mm256_set1_epi32(pq.ksub);
      __m256i offsets_0 = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
      offsets_0 = _mm256_mullo_epi32(offsets_0, ksub);

      // accumulators of partial sums
      __m256 partialSum = _mm256_setzero_ps();

      // loop
      for (m = 0; m < pqM16 * 16; m += 16) {
        // load 16 uint8 values
        const __m128i mm1 = _mm_loadu_si128((const __m128i_u*)(code + m));
        {
          // convert uint8 values (low part of __m128i) to int32
          // values
          const __m256i idx1 = _mm256_cvtepu8_epi32(mm1);

          // add offsets
          const __m256i indices_to_read_from = _mm256_add_epi32(idx1, offsets_0);

          // gather 8 values, similar to 8 operations of tab[idx]
          __m256 collected = _mm256_i32gather_ps(tab, indices_to_read_from, sizeof(float));
          tab += pq.ksub * 8;

          // collect partial sums
          partialSum = _mm256_add_ps(partialSum, collected);
        }

        // move high 8 uint8 to low ones
        const __m128i mm2 = _mm_unpackhi_epi64(mm1, _mm_setzero_si128());
        {
          // convert uint8 values (low part of __m128i) to int32
          // values
          const __m256i idx1 = _mm256_cvtepu8_epi32(mm2);

          // add offsets
          const __m256i indices_to_read_from = _mm256_add_epi32(idx1, offsets_0);

          // gather 8 values, similar to 8 operations of tab[idx]
          __m256 collected = _mm256_i32gather_ps(tab, indices_to_read_from, sizeof(float));
          tab += pq.ksub * 8;

          // collect partial sums
          partialSum = _mm256_add_ps(partialSum, collected);
        }
      }

      // horizontal sum for partialSum
      const __m256 h0 = _mm256_hadd_ps(partialSum, partialSum);
      const __m256 h1 = _mm256_hadd_ps(h0, h0);

      // extract high and low __m128 regs from __m256
      const __m128 h2 = _mm256_extractf128_ps(h1, 1);
      const __m128 h3 = _mm256_castps256_ps128(h1);

      // get a final hsum into all 4 regs
      const __m128 h4 = _mm_add_ss(h2, h3);

      // extract f[0] from __m128
      const float hsum = _mm_cvtss_f32(h4);
      result += hsum;
    }

    //
    if (m < pq.M) {
      // process leftovers
      PQDecoder decoder(code + m, pq.nbits);

      for (; m < pq.M; m++) {
        result += tab[decoder.decode()];
        tab += pq.ksub;
      }
    }

    return result;
  }

#else
  /// Returns the distance to a single code.
  /// General-purpose version.
  template <class SearchResultType>
  inline float distance_single_code(const uint8_t* code) const {
    PQDecoder decoder(code, pq.nbits);

    const float* tab = sim_table;
    float result = 0;

    for (size_t m = 0; m < pq.M; m++) {
      result += tab[decoder.decode()];
      tab += pq.ksub;
    }

    return result;
  }
#endif

  /// version of the scan where we use precomputed tables.
  template <class SearchResultType>
  void scan_list_with_table(size_t ncode, const uint8_t* codes, SearchResultType& res) const {
    for (size_t j = 0; j < ncode; j++, codes += pq.code_size) {
      if (res.skip_entry(j)) {
        continue;
      }
      float dis = dis0 + distance_single_code<SearchResultType>(codes);
      res.add(j, dis);
    }
  }

  /// tables are not precomputed, but pointers are provided to the
  /// relevant X_c|x_r tables
  template <class SearchResultType>
  void scan_list_with_pointer(size_t ncode, const uint8_t* codes, SearchResultType& res) const {
    for (size_t j = 0; j < ncode; j++, codes += pq.code_size) {
      if (res.skip_entry(j)) {
        continue;
      }
      PQDecoder decoder(codes, pq.nbits);
      float dis = dis0;
      const float* tab = sim_table_2;

      for (size_t m = 0; m < pq.M; m++) {
        int ci = decoder.decode();
        dis += sim_table_ptrs[m][ci] - 2 * tab[ci];
        tab += pq.ksub;
      }
      res.add(j, dis);
    }
  }

  /// nothing is precomputed: access residuals on-the-fly
  template <class SearchResultType>
  void scan_on_the_fly_dist(size_t ncode, const uint8_t* codes, SearchResultType& res) const {
    const float* dvec;
    float dis0 = 0;
    if (by_residual) {
      if (METRIC_TYPE == METRIC_INNER_PRODUCT) {
        ivfpq.quantizer->reconstruct(key, residual_vec);
        dis0 = fvec_inner_product(residual_vec, qi, d);
      } else {
        ivfpq.quantizer->compute_residual(qi, residual_vec, key);
      }
      dvec = residual_vec;
    } else {
      dvec = qi;
      dis0 = 0;
    }

    for (size_t j = 0; j < ncode; j++, codes += pq.code_size) {
      if (res.skip_entry(j)) {
        continue;
      }
      pq.decode(codes, decoded_vec);

      float dis;
      if (METRIC_TYPE == METRIC_INNER_PRODUCT) {
        dis = dis0 + fvec_inner_product(decoded_vec, qi, d);
      } else {
        dis = fvec_L2sqr(decoded_vec, dvec, d);
      }
      res.add(j, dis);
    }
  }

  /*****************************************************
   * Scanning codes with polysemous filtering
   *****************************************************/

  template <class HammingComputer, class SearchResultType>
  void scan_list_polysemous_hc(size_t ncode, const uint8_t* codes, SearchResultType& res) const {
    int ht = ivfpq.polysemous_ht;
    size_t n_hamming_pass = 0, nup = 0;

    int code_size = pq.code_size;

    HammingComputer hc(q_code.data(), code_size);

    for (size_t j = 0; j < ncode; j++, codes += code_size) {
      if (res.skip_entry(j)) {
        continue;
      }
      const uint8_t* b_code = codes;
      int hd = hc.hamming(b_code);
      if (hd < ht) {
        n_hamming_pass++;

        float dis = dis0 + distance_single_code<SearchResultType>(codes);

        res.add(j, dis);
      }
    }
#pragma omp critical
    { indexIVFPQ_stats.n_hamming_pass += n_hamming_pass; }
  }

  template <class SearchResultType>
  void scan_list_polysemous(size_t ncode, const uint8_t* codes, SearchResultType& res) const {
    switch (pq.code_size) {
#define HANDLE_CODE_SIZE(cs)                                                           \
  case cs:                                                                             \
    scan_list_polysemous_hc<HammingComputer##cs, SearchResultType>(ncode, codes, res); \
    break
      HANDLE_CODE_SIZE(4);
      HANDLE_CODE_SIZE(8);
      HANDLE_CODE_SIZE(16);
      HANDLE_CODE_SIZE(20);
      HANDLE_CODE_SIZE(32);
      HANDLE_CODE_SIZE(64);
#undef HANDLE_CODE_SIZE
      default:
        scan_list_polysemous_hc<HammingComputerDefault, SearchResultType>(ncode, codes, res);
        break;
    }
  }
};

/* We put as many parameters as possible in template. Hopefully the
 * gain in runtime is worth the code bloat.
 *
 * C is the comparator < or >, it is directly related to METRIC_TYPE.
 *
 * precompute_mode is how much we precompute (2 = precompute distance tables,
 * 1 = precompute pointers to distances, 0 = compute distances one by one).
 * Currently only 2 is supported
 *
 * use_sel: store or ignore the IDSelector
 */
template <MetricType METRIC_TYPE, class C, class PQDecoder, bool use_sel>
struct IVFPQScanner : IVFPQScannerT<Index::idx_t, METRIC_TYPE, PQDecoder>, InvertedListScanner {
  int precompute_mode;
  const IDSelector* sel;
  const CustomIvfPq* ivfpq;

  IVFPQScanner(const CustomIvfPq& ivfpq, bool store_pairs, int precompute_mode,
               const IDSelector* sel)
      : IVFPQScannerT<Index::idx_t, METRIC_TYPE, PQDecoder>(ivfpq, nullptr),
        precompute_mode(precompute_mode),
        sel(sel),
        ivfpq(&ivfpq) {
    this->store_pairs = store_pairs;
  }

  void set_query(const float* query) override { this->init_query(query); }

  void set_list(idx_t list_no, float coarse_dis) override {
    this->list_no = list_no;
    this->init_list(list_no, coarse_dis, precompute_mode);
  }

  float distance_to_code(const uint8_t* code) const override {
    assert(precompute_mode == 2);
    float dis = this->dis0;
    const float* tab = this->sim_table;
    PQDecoder decoder(code, this->pq.nbits);

    for (size_t m = 0; m < this->pq.M; m++) {
      dis += tab[decoder.decode()];
      tab += this->pq.ksub;
    }
    return dis;
  }

  size_t scan_codes(size_t ncode, const uint8_t* codes, const idx_t* ids, float* heap_sim,
                    idx_t* heap_ids, size_t k) const override {
    KnnSearchResults<C, use_sel> res = {/* key */ this->key,
                                        /* ids */ this->store_pairs ? nullptr : ids,
                                        /* sel */ this->sel,
                                        /* k */ k,
                                        /* heap_sim */ heap_sim,
                                        /* heap_ids */ heap_ids,
                                        /* nup */ 0};

    if (this->polysemous_ht > 0) {
      assert(precompute_mode == 2);
      this->scan_list_polysemous(ncode, codes, res);
    } else if (precompute_mode == 2) {
      this->scan_list_with_table(ncode, codes, res);
    } else if (precompute_mode == 1) {
      this->scan_list_with_pointer(ncode, codes, res);
    } else if (precompute_mode == 0) {
      this->scan_on_the_fly_dist(ncode, codes, res);
    } else {
      FAISS_THROW_MSG("bad precomp mode");
    }
    return res.nup;
  }

  void scan_codes_range(size_t ncode, const uint8_t* codes, const idx_t* ids, float radius,
                        RangeQueryResult& rres) const override {
    RangeSearchResults<C, use_sel> res = {
        /* key */ this->key,
        /* ids */ this->store_pairs ? nullptr : ids,
        /* sel */ this->sel,
        /* ivfpq */ this->ivfpq,     // added by tenann.
        /* radius */ sqrtf(radius),  // modified by tenann, use squared root radius instead
        /* rres */ rres};

    if (this->polysemous_ht > 0) {
      assert(precompute_mode == 2);
      this->scan_list_polysemous(ncode, codes, res);
    } else if (precompute_mode == 2) {
      this->scan_list_with_table(ncode, codes, res);
    } else if (precompute_mode == 1) {
      this->scan_list_with_pointer(ncode, codes, res);
    } else if (precompute_mode == 0) {
      this->scan_on_the_fly_dist(ncode, codes, res);
    } else {
      FAISS_THROW_MSG("bad precomp mode");
    }
  }
};

template <class PQDecoder, bool use_sel>
InvertedListScanner* get_InvertedListScanner1(const CustomIvfPq& index, bool store_pairs,
                                              const IDSelector* sel) {
  if (index.metric_type == METRIC_INNER_PRODUCT) {
    return new IVFPQScanner<METRIC_INNER_PRODUCT, CMin<float, idx_t>, PQDecoder, use_sel>(
        index, store_pairs, 2, sel);
  } else if (index.metric_type == METRIC_L2) {
    return new IVFPQScanner<METRIC_L2, CMax<float, idx_t>, PQDecoder, use_sel>(index, store_pairs,
                                                                               2, sel);
  }
  return nullptr;
}

template <bool use_sel>
InvertedListScanner* get_InvertedListScanner2(const CustomIvfPq& index, bool store_pairs,
                                              const IDSelector* sel) {
  if (index.pq.nbits == 8) {
    return get_InvertedListScanner1<PQDecoder8, use_sel>(index, store_pairs, sel);
  } else if (index.pq.nbits == 16) {
    return get_InvertedListScanner1<PQDecoder16, use_sel>(index, store_pairs, sel);
  } else {
    return get_InvertedListScanner1<PQDecoderGeneric, use_sel>(index, store_pairs, sel);
  }
}

}  // anonymous namespace

InvertedListScanner* CustomIvfPq::get_InvertedListScanner(bool store_pairs,
                                                          const IDSelector* sel) const {
  if (sel) {
    return get_InvertedListScanner2<true>(*this, store_pairs, sel);
  } else {
    return get_InvertedListScanner2<false>(*this, store_pairs, sel);
  }
  return nullptr;
}

}  // namespace tenann
