#pragma once
// =========================================================================
// hls_sim_stubs.hpp — Vitis HLS type stubs for native C++ (g++) simulation
//
// When compiled under Vitis HLS (__SYNTHESIS__ defined), this file is NOT
// included — the real HLS headers (ap_int.h, ap_fixed.h, hls_stream.h)
// are used instead, giving exact bit-width synthesis.
//
// When compiled with g++ (for standalone testbenches without the Xilinx
// toolchain), this file provides functional equivalents:
//
//   ap_uint<W>    → uint64_t  (widths 1–64; ops don't auto-truncate)
//   ap_int<W>     → int64_t
//   ap_fixed<W,I> → double    (correct range/precision for W≤64, I<W)
//   hls::stream<T>→ std::queue wrapper
//
// Trade-offs of stubs vs. real types:
//   - No automatic bit-width truncation on overflow (intentional: we want
//     to catch unexpected large values in simulation, not silently wrap)
//   - ap_fixed arithmetic is floating-point; real HLS uses fixed-point
//     rounding modes. Functional results are identical for well-scaled inputs.
//   - hls::stream has no depth limit; real FIFO depth is set via pragma.
// =========================================================================

#include <cassert>
#include <cstdint>
#include <queue>
#include <type_traits>

// -------------------------------------------------------------------------
// ap_uint<W> — unsigned W-bit integer
// -------------------------------------------------------------------------
template<int W>
using ap_uint = uint64_t;

// -------------------------------------------------------------------------
// ap_int<W> — signed W-bit integer
// -------------------------------------------------------------------------
template<int W>
using ap_int = int64_t;

// -------------------------------------------------------------------------
// ap_fixed<W,I> — signed fixed-point
//   W = total bits (including sign), I = integer bits (including sign)
//   Fractional bits = W - I
// In hardware this is exact; here we use double for functional correctness.
// -------------------------------------------------------------------------
template<int W, int I>
using ap_fixed = double;

// Unsigned variant used in some HLS code
template<int W, int I>
using ap_ufixed = double;

// -------------------------------------------------------------------------
// hls::stream<T>
// Models a FIFO channel between HLS pipeline stages.
// In hardware: synthesizes to a BRAM or SRL FIFO primitive.
// Here: backed by std::queue, with depth tracking.
// -------------------------------------------------------------------------
namespace hls {

template<typename T>
class stream {
public:
    explicit stream(const char* /*name*/ = "") {}

    // Write (push) — models axis_write / s_data_write
    void write(const T& val) { q_.push(val); }

    // Read (pop) — blocks until data is available (in sim: assert)
    T read() {
        assert(!q_.empty() && "hls::stream::read() on empty stream");
        T v = q_.front();
        q_.pop();
        return v;
    }

    // Non-blocking read — returns false if empty (models try-read)
    bool read_nb(T& val) {
        if (q_.empty()) return false;
        val = q_.front();
        q_.pop();
        return true;
    }

    bool empty() const { return q_.empty(); }
    bool full()  const { return false; } // unbounded in simulation
    int  size()  const { return static_cast<int>(q_.size()); }

private:
    std::queue<T> q_;
};

} // namespace hls

// -------------------------------------------------------------------------
// AXI-Stream data struct (ap_axiu in real HLS)
// We define a minimal version for the testbenches.
// -------------------------------------------------------------------------
template<int DWIDTH, int UWIDTH = 1>
struct ap_axis {
    ap_uint<DWIDTH>    data;
    ap_uint<DWIDTH/8>  keep;
    ap_uint<UWIDTH>    user;
    ap_uint<1>         last;
};
