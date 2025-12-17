#include "tcp_receiver.hh"

// Dummy implementation of a TCP receiver

// For Lab 2, please replace with a real implementation that passes the
// automated checks run by `make check_lab2`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

void TCPReceiver::segment_received(const TCPSegment &seg) {
    // If we haven't seen a SYN yet, only accept a segment that has SYN
    const TCPHeader &h = seg.header();
    if (not _isn.has_value()) {
        if (!h.syn)
            return; // ignore until SYN

        // record ISN
        _isn.emplace(h.seqno);
        // if SYN carries payload/fin, treat payload's first byte index as 0
        const uint64_t abs_seqno = unwrap(h.seqno, _isn.value(), 0);
        // payload index = abs_seqno - 1 (SYN consumes one sequence number)
        const uint64_t index = (abs_seqno > 0) ? (abs_seqno - 1) : 0;
        _reassembler.push_substring(std::string(seg.payload().str()), index, h.fin);
        return;
    }

    // Already have ISN: accept payload and FIN (if any)
    // Use a checkpoint near current assembled bytes to unwrap correctly
    uint64_t checkpoint = _reassembler.stream_out().bytes_written();
    // the absolute seqno of the incoming segment
    const uint64_t abs_seqno = unwrap(h.seqno, _isn.value(), checkpoint + 1);
    if (abs_seqno == 0 && !h.syn) {
        return; // drop invalid segment
    }

    // payload first byte index is abs_seqno - 1 (syn consumed)
    uint64_t index = 0;
    if (abs_seqno > 0)
        index = abs_seqno - 1;
    // push payload and FIN flag
    _reassembler.push_substring(std::string(seg.payload().str()), index, h.fin);
}

optional<WrappingInt32> TCPReceiver::ackno() const {
    if (not _isn.has_value())
        return std::nullopt;

    // base ack is 1 for SYN plus number of bytes assembled
    uint64_t ack_abs = _reassembler.stream_out().bytes_written() + 1;
    // if we've seen/assembled FIN (stream input ended), account for FIN consumption
    if (_reassembler.stream_out().input_ended())
        ack_abs += 1;

    return wrap(ack_abs, _isn.value());
}

size_t TCPReceiver::window_size() const {
    // available capacity is total capacity minus bytes currently buffered (not-yet-read)
    size_t used = _reassembler.stream_out().buffer_size();
    if (_capacity >= used)
        return _capacity - used;
    return 0;
}
