#include "wrapping_integers.hh"

// Dummy implementation of a 32-bit wrapping integer

// For Lab 2, please replace with a real implementation that passes the
// automated checks run by `make check_lab2`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! Transform an "absolute" 64-bit sequence number (zero-indexed) into a WrappingInt32
//! \param n The input absolute 64-bit sequence number
//! \param isn The initial sequence number
WrappingInt32 wrap(uint64_t n, WrappingInt32 isn) {
    // The wrapped value is simply (isn + n) modulo 2^32
    const uint32_t raw = static_cast<uint32_t>(isn.raw_value() + static_cast<uint32_t>(n));
    return WrappingInt32{raw};
}

//! Transform a WrappingInt32 into an "absolute" 64-bit sequence number (zero-indexed)
//! \param n The relative sequence number
//! \param isn The initial sequence number
//! \param checkpoint A recent absolute 64-bit sequence number
//! \returns the 64-bit sequence number that wraps to `n` and is closest to `checkpoint`
//!
//! \note Each of the two streams of the TCP connection has its own ISN. One stream
//! runs from the local TCPSender to the remote TCPReceiver and has one ISN,
//! and the other stream runs from the remote TCPSender to the local TCPReceiver and
//! has a different ISN.
uint64_t unwrap(WrappingInt32 n, WrappingInt32 isn, uint64_t checkpoint) {
    // Compute the 32-bit offset of n relative to isn.
    const uint32_t offset = static_cast<uint32_t>(n.raw_value() - isn.raw_value());

    const uint64_t MOD = 1ULL << 32;

    // Compute a candidate aligned to the same 2^32 window as checkpoint
    const uint64_t high = checkpoint >> 32; // the upper 32 bits
    uint64_t candidate = (high << 32) + offset;

    // Prepare nearby candidates (previous and next window) and pick the one closest to checkpoint
    uint64_t best = candidate;
    auto dist = [&](uint64_t a) -> uint64_t { return a > checkpoint ? a - checkpoint : checkpoint - a; };
    uint64_t best_dist = dist(best);

    // previous window
    if (high > 0) {
        uint64_t prev = candidate - MOD;
        uint64_t d = dist(prev);
        if (d < best_dist) {
            best = prev;
            best_dist = d;
        }
    }

    // next window
    uint64_t next = candidate + MOD;
    uint64_t dnext = dist(next);
    if (dnext < best_dist) {
        best = next;
        best_dist = dnext;
    }

    return best;
}
