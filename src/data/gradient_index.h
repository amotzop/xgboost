/**
 * Copyright 2017-2025, XGBoost Contributors
 * \brief Data type for fast histogram aggregation.
 */
#ifndef XGBOOST_DATA_GRADIENT_INDEX_H_
#define XGBOOST_DATA_GRADIENT_INDEX_H_

#include <algorithm>  // for min
#include <atomic>     // for atomic
#include <cstddef>    // for size_t
#include <cstdint>    // for uint32_t
#include <limits>     // for numeric_limits
#include <memory>     // for make_unique
#include <vector>     // for vector

#include "../common/categorical.h"
#include "../common/error_msg.h"  // for InfInData
#include "../common/hist_util.h"
#include "../common/numeric.h"
#include "../common/ref_resource_view.h"  // for RefResourceView
#include "../common/threading_utils.h"
#include "../common/transform_iterator.h"  // for MakeIndexTransformIter
#include "adapter.h"
#include "xgboost/base.h"
#include "xgboost/data.h"

namespace xgboost {
namespace common {
class ColumnMatrix;
class AlignedFileWriteStream;
}  // namespace common

float GetFvalueImpl(std::vector<std::uint32_t> const& ptrs, std::vector<float> const& values,
                    std::vector<float> const& mins, bst_idx_t ridx, bst_feature_t fidx,
                    bst_idx_t base_rowid, std::unique_ptr<common::ColumnMatrix> const& columns_);

/**
 * @brief preprocessed global index matrix, in CSR format.
 *
 *  Transform floating values to integer index in histogram This is a global histogram
 *  index for CPU histogram.  On GPU ellpack page is used.
 */
class GHistIndexMatrix {
  // Get the size of each row
  template <typename AdapterBatchT>
  static auto GetRowCounts(AdapterBatchT const& batch, float missing, int32_t n_threads) {
    std::vector<size_t> valid_counts(batch.Size(), 0);
    auto is_valid = data::IsValidFunctor{missing};
    common::ParallelFor(batch.Size(), n_threads, [&](size_t i) {
      auto line = batch.GetLine(i);
      for (size_t j = 0; j < line.Size(); ++j) {
        data::COOTuple elem = line.GetElement(j);
        if (is_valid(elem)) {
          valid_counts[i]++;
        }
      }
    });
    return valid_counts;
  }

  /**
   * @brief Push a sparse page into the index matrix.
   */
  void PushBatch(SparsePage const& batch, common::Span<FeatureType const> ft,
                 std::int32_t n_threads);

  template <typename Batch, typename BinIdxType, typename GetOffset, typename IsValid>
  void SetIndexData(common::Span<BinIdxType> index_data_span, size_t rbegin,
                    common::Span<FeatureType const> ft, size_t batch_threads, Batch const& batch,
                    IsValid&& is_valid, size_t nbins, GetOffset&& get_offset) {
    auto batch_size = batch.Size();
    BinIdxType* index_data = index_data_span.data();
    auto const& ptrs = cut.Ptrs();
    auto const& values = cut.Values();
    std::atomic<bool> valid{true};
    common::ParallelFor(batch_size, batch_threads, [&](size_t i) {
      auto line = batch.GetLine(i);
      size_t ibegin = row_ptr[rbegin + i];  // index of first entry for current block
      size_t k = 0;
      auto tid = omp_get_thread_num();
      for (size_t j = 0; j < line.Size(); ++j) {
        data::COOTuple elem = line.GetElement(j);
        if (is_valid(elem)) {
          if (XGBOOST_EXPECT((std::isinf(elem.value)), false)) {
            valid = false;
          }
          bst_bin_t bin_idx{-1};
          if (common::IsCat(ft, elem.column_idx)) {
            bin_idx = cut.SearchCatBin(elem.value, elem.column_idx, ptrs, values);
          } else {
            bin_idx = cut.SearchBin(elem.value, elem.column_idx, ptrs, values);
          }
          index_data[ibegin + k] = get_offset(bin_idx, j);
          ++hit_count_tloc_[tid * nbins + bin_idx];
          ++k;
        }
      }
    });

    CHECK(valid) << error::InfInData();
  }

  // Gather hit_count from all threads
  void GatherHitCount(int32_t n_threads, bst_bin_t n_bins_total) {
    CHECK_EQ(hit_count.size(), n_bins_total);
    common::ParallelFor(n_bins_total, n_threads, [&](bst_omp_uint idx) {
      for (int32_t tid = 0; tid < n_threads; ++tid) {
        hit_count[idx] += hit_count_tloc_[tid * n_bins_total + idx];
        hit_count_tloc_[tid * n_bins_total + idx] = 0;  // reset for next batch
      }
    });
  }

  template <typename Batch, typename IsValid>
  void PushBatchImpl(int32_t n_threads, Batch const& batch, size_t rbegin, IsValid&& is_valid,
                     common::Span<FeatureType const> ft) {
    // The number of threads is pegged to the batch size. If the OMP block is parallelized
    // on anything other than the batch/block size, it should be reassigned
    size_t batch_threads =
        std::max(static_cast<size_t>(1), std::min(batch.Size(), static_cast<size_t>(n_threads)));

    auto n_bins_total = cut.TotalBins();
    const size_t n_index = row_ptr[rbegin + batch.Size()];  // number of entries in this page
    ResizeIndex(n_index, isDense_);
    if (isDense_) {
      index.SetBinOffset(cut.Ptrs());
    }
    if (isDense_) {
      common::DispatchBinType(index.GetBinTypeSize(), [&](auto dtype) {
        using T = decltype(dtype);
        common::Span<T> index_data_span = {index.data<T>(), index.Size()};
        SetIndexData(index_data_span, rbegin, ft, batch_threads, batch, is_valid, n_bins_total,
                     index.MakeCompressor<T>());
      });
    } else {
      common::Span<uint32_t> index_data_span = {index.data<uint32_t>(), n_index};
      // no compression
      SetIndexData(index_data_span, rbegin, ft, batch_threads, batch, is_valid, n_bins_total,
                   [](auto idx, auto) { return idx; });
    }
    this->GatherHitCount(n_threads, n_bins_total);
  }

  // The function is only created to avoid using the column matrix in the header.
  void ResizeColumns(double sparse_thresh);

 public:
  /** @brief row pointer to rows by element position */
  common::RefResourceView<std::size_t> row_ptr;
  /** @brief data storage for index. */
  common::RefResourceView<std::uint8_t> data;
  /** @brief The histogram index. */
  common::Index index;
  /** @brief hit count of each index, used for constructing the ColumnMatrix */
  common::RefResourceView<std::size_t> hit_count;
  /** @brief The corresponding cuts */
  common::HistogramCuts cut;
  /** @brief max_bin for each feature. */
  bst_bin_t max_numeric_bins_per_feat;
  /** @brief base row index for current page (used by external memory) */
  bst_idx_t base_rowid{0};

  [[nodiscard]] bst_bin_t MaxNumBinPerFeat() const {
    return std::max(static_cast<bst_bin_t>(cut.MaxCategory() + 1), max_numeric_bins_per_feat);
  }

  ~GHistIndexMatrix();
  /**
   * @brief Constrcutor for SimpleDMatrix.
   */
  GHistIndexMatrix(Context const* ctx, DMatrix* x, bst_bin_t max_bins_per_feat,
                   double sparse_thresh, bool sorted_sketch, common::Span<float const> hess = {});
  /**
   * @brief Constructor for Quantile DMatrix. Initialize basic information and prepare
   *        for push batch.
   */
  GHistIndexMatrix(MetaInfo const& info, common::HistogramCuts&& cuts,
                   bst_bin_t max_bin_per_feat);

  /**
   * @brief Constructor for the external memory Quantile DMatrix. Initialize basic
   *        information and prepare for push batch.
   */
  GHistIndexMatrix(bst_idx_t n_samples, bst_idx_t base_rowid, common::HistogramCuts&& cuts,
                   bst_bin_t max_bin_per_feat, bool is_dense);

  /**
   * @brief Constructor fro Quantile DMatrix where we might copy an existing ellpack page
   *        to host gradient index.
   */
  GHistIndexMatrix(Context const* ctx, MetaInfo const& info, EllpackPage const& page,
                   BatchParam const& p);

  /**
   * @brief Constructor for external memory.
   */
  GHistIndexMatrix(SparsePage const& page, common::Span<FeatureType const> ft,
                   common::HistogramCuts cuts, bst_bin_t max_bins_per_feat, bool is_dense,
                   double sparse_thresh, std::int32_t n_threads);
  GHistIndexMatrix();  // also for ext mem, empty ctor so that we can read the cache back.

  /**
   * @brief Push a single batch into the gradient index.
   *
   * @param n_samples_total The total number of rows for all batches, create a column
   *        matrix once all batches are pushed.
   */
  template <typename Batch>
  void PushAdapterBatch(Context const* ctx, std::size_t rbegin, std::size_t prev_sum,
                        Batch const& batch, float missing, common::Span<FeatureType const> ft,
                        double sparse_thresh, bst_idx_t n_samples_total) {
    auto n_bins_total = cut.TotalBins();
    hit_count_tloc_.clear();
    hit_count_tloc_.resize(ctx->Threads() * n_bins_total, 0);

    auto n_threads = ctx->Threads();
    auto valid_counts = GetRowCounts(batch, missing, n_threads);

    auto it = common::MakeIndexTransformIter([&](size_t ridx) { return valid_counts[ridx]; });
    common::PartialSum(n_threads, it, it + batch.Size(), prev_sum, row_ptr.begin() + rbegin);
    auto is_valid = data::IsValidFunctor{missing};

    PushBatchImpl(ctx->Threads(), batch, rbegin, is_valid, ft);

    if (rbegin + batch.Size() == n_samples_total) {
      // finished
      this->ResizeColumns(sparse_thresh);
    }
  }

  // Call ColumnMatrix::PushBatch
  template <typename Batch>
  void PushAdapterBatchColumns(Context const* ctx, Batch const& batch, float missing,
                               size_t rbegin);

  void ResizeIndex(const size_t n_index, const bool isDense);

  void GetFeatureCounts(size_t* counts) const {
    auto nfeature = cut.Ptrs().size() - 1;
    for (unsigned fid = 0; fid < nfeature; ++fid) {
      auto ibegin = cut.Ptrs()[fid];
      auto iend = cut.Ptrs()[fid + 1];
      for (auto i = ibegin; i < iend; ++i) {
        counts[fid] += hit_count[i];
      }
    }
  }

  [[nodiscard]] bool IsDense() const { return isDense_; }
  void SetDense(bool is_dense) { isDense_ = is_dense; }
  [[nodiscard]] bst_idx_t BaseRowId() const { return base_rowid; }
  /**
   * @brief Get the local row index from the global row index.
   */
  [[nodiscard]] bst_idx_t RowIdx(bst_idx_t gridx) const {
    return row_ptr[gridx - this->base_rowid];
  }

  [[nodiscard]] bst_idx_t Size() const { return row_ptr.empty() ? 0 : row_ptr.size() - 1; }
  [[nodiscard]] bst_feature_t Features() const { return cut.NumFeatures(); }

  [[nodiscard]] bool ReadColumnPage(common::AlignedResourceReadStream* fi);
  [[nodiscard]] std::size_t WriteColumnPage(common::AlignedFileWriteStream* fo) const;

  [[nodiscard]] common::ColumnMatrix const& Transpose() const;

  [[nodiscard]] bst_bin_t GetGindex(size_t ridx, size_t fidx) const;

  [[nodiscard]] float GetFvalue(size_t ridx, size_t fidx, bool is_cat) const;
  [[nodiscard]] float GetFvalue(std::vector<std::uint32_t> const& ptrs,
                                std::vector<float> const& values, std::vector<float> const& mins,
                                bst_idx_t ridx, bst_feature_t fidx, bool is_cat) const {
    if (is_cat) {
      auto gidx = GetGindex(ridx, fidx);
      if (gidx == -1) {
        return std::numeric_limits<float>::quiet_NaN();
      }
      return values[gidx];
    }
    if (this->IsDense()) {
      auto begin = RowIdx(ridx);
      auto bin_idx = this->index[begin + fidx];
      return common::HistogramCuts::NumericBinValue(ptrs, values, mins, fidx, bin_idx);
    }
    return GetFvalueImpl(ptrs, values, mins, ridx, fidx, this->base_rowid, this->columns_);
  }

  [[nodiscard]] common::HistogramCuts& Cuts() { return cut; }
  [[nodiscard]] common::HistogramCuts const& Cuts() const { return cut; }

 private:
  std::unique_ptr<common::ColumnMatrix> columns_;
  std::vector<size_t> hit_count_tloc_;
  bool isDense_;
};

/**
 * \brief Helper for recovering feature index from row-based storage of histogram
 *        bin. (`GHistIndexMatrix`).
 *
 * \param assign A callback function that takes bin index, index into the whole batch, row
 *               index and feature index
 */
template <typename Fn>
void AssignColumnBinIndex(GHistIndexMatrix const& page, Fn&& assign) {
  auto const batch_size = page.Size();
  auto const& ptrs = page.cut.Ptrs();
  std::size_t k{0};

  auto dense = page.IsDense();

  common::DispatchBinType(page.index.GetBinTypeSize(), [&](auto t) {
    using BinT = decltype(t);
    auto const& index = page.index;
    for (std::size_t ridx = 0; ridx < batch_size; ++ridx) {
      auto r_beg = page.row_ptr[ridx];
      auto r_end = page.row_ptr[ridx + 1];
      bst_feature_t fidx{0};
      if (dense) {
        // compressed, use the operator to obtain the true value.
        for (std::size_t j = r_beg; j < r_end; ++j) {
          bst_feature_t fidx = j - r_beg;
          std::uint32_t bin_idx = index[k];
          assign(bin_idx, k, ridx, fidx);
          ++k;
        }
      } else {
        // not compressed
        auto const* row_index = index.data<BinT>() + page.row_ptr[page.base_rowid];
        for (std::size_t j = r_beg; j < r_end; ++j) {
          std::uint32_t bin_idx = row_index[k];
          // find the feature index for current bin.
          while (bin_idx >= ptrs[fidx + 1]) {
            fidx++;
          }
          assign(bin_idx, k, ridx, fidx);
          ++k;
        }
      }
    }
  });
}
}  // namespace xgboost
#endif  // XGBOOST_DATA_GRADIENT_INDEX_H_
