#pragma once

#include "cutlass/cutlass.h"
#include "cutlass/gemm/threadblock/mma_multistage.h"
#include "cp_cutlass_jackpot.cuh"

namespace cutlass {
namespace gemm {
namespace threadblock {

template <typename MmaMultistage_, int kMilestoneIters>
class MmaMultistageMilestone : public MmaMultistage_ {
public:
  using Base = MmaMultistage_;
  static constexpr int kItersPerMs = kMilestoneIters;

  CUTLASS_DEVICE
  MmaMultistageMilestone(typename Base::SharedStorage &shared_storage, int thread_idx,
                         int warp_idx, int lane_idx)
      : Base(shared_storage, thread_idx, warp_idx, lane_idx) {}

  using Shape = typename Base::Shape;
  using FragmentC = typename Base::FragmentC;
  using IteratorA = typename Base::IteratorA;
  using IteratorB = typename Base::IteratorB;

  CUTLASS_DEVICE
  static void skip_residue_tile(IteratorA &iterator_A, IteratorB &iterator_B) {
    ++iterator_A;
    ++iterator_B;
  }

  template <typename Callback>
  CUTLASS_DEVICE
  void inline_operator(int total_iters, FragmentC &accum, IteratorA iterator_A,
                       IteratorB iterator_B, FragmentC const &src_accum,
                       Callback &&cb) {
    this->prologue(iterator_A, iterator_B, total_iters);
    this->gmem_wait();
    accum = src_accum;

    typename Base::PipeState pipe_state;
    int gemm_k_iterations = total_iters;
    iterator_A.clear_mask(gemm_k_iterations == 0);
    iterator_B.clear_mask(gemm_k_iterations == 0);

    this->warp_tile_iterator_A_.set_kgroup_index(0);
    this->warp_tile_iterator_A_.load(pipe_state.warp_loaded_frag_A_[0]);
    ++this->warp_tile_iterator_A_;

    this->warp_tile_iterator_B_.set_kgroup_index(0);
    this->warp_tile_iterator_B_.load(pipe_state.warp_loaded_frag_B_[0]);
    ++this->warp_tile_iterator_B_;

    this->warp_mma_.transform(
      pipe_state.warp_transformed_frag_A_[0],
      pipe_state.warp_transformed_frag_B_[0],
      pipe_state.warp_loaded_frag_A_[0],
      pipe_state.warp_loaded_frag_B_[0]);

    if (Base::Detail::kStagedAccumulation) {
      pipe_state.tmp_accum_.clear();
    }

    int k_tile_count = 0;
    int ms_idx = 0;

    CUTLASS_GEMM_LOOP
    for (; gemm_k_iterations > (-Base::kStages + 1);) {
      this->mac_loop_iter(
        pipe_state,
        accum,
        iterator_A,
        iterator_B,
        gemm_k_iterations);

      ++k_tile_count;
      if (k_tile_count % kMilestoneIters == 0) {
        uint32_t xv = 0u;
        if constexpr (FragmentC::kElements == 64) {
          xv = cp_cutlass_reduce_accum64_lop3(accum);
        } else {
          CUTLASS_PRAGMA_UNROLL
          for (int i = 0; i < FragmentC::kElements; ++i)
            xv ^= static_cast<uint32_t>(accum[i]);
        }
        cb(ms_idx, xv);
        ++ms_idx;
      }
    }

    if (Base::Detail::kStagedAccumulation) {
      plus<FragmentC> plus_accum;
      accum = plus_accum(accum, pipe_state.tmp_accum_);
    }
  }
};

} // namespace threadblock
} // namespace gemm
} // namespace cutlass
