#include "tcp_connection.hh"

#include <iostream>

// Dummy implementation of a TCP connection

// For Lab 4, please replace with a real implementation that passes the
// automated checks run by `make check`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

size_t TCPConnection::remaining_outbound_capacity() const { return _sender.stream_in().remaining_capacity(); }

size_t TCPConnection::bytes_in_flight() const { return _sender.bytes_in_flight(); }

size_t TCPConnection::unassembled_bytes() const { return _receiver.unassembled_bytes(); }

size_t TCPConnection::time_since_last_segment_received() const { return _time_since_last_segment_received; }

void TCPConnection::segment_received(const TCPSegment &seg) { 
    
    // bool need_send_ack = seg.length_in_sequence_space();
    // you code here.
    //你需要考虑到ACK包、RST包等多种情况
    
    //状态变化(按照个人的情况可进行修改)
    // 如果是 LISEN 到了 SYN
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::SYN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::CLOSED) {
        // 此时肯定是第一次调用 fill_window，因此会发送 SYN + ACK
        connect();
        return;
    }

    // 判断 TCP 断开连接时是否时需要等待
    // CLOSE_WAIT
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::SYN_ACKED) {
        _linger_after_streams_finish = false;
    }

    // 如果到了准备断开连接的时候。服务器端先断
    // CLOSED
    if (TCPState::state_summary(_receiver) == TCPReceiverStateSummary::FIN_RECV &&
        TCPState::state_summary(_sender) == TCPSenderStateSummary::FIN_ACKED && !_linger_after_streams_finish) {
        _is_active = false;
        return;
    }

    // reset timer on any received segment
    _time_since_last_segment_received = 0;

    // If RST, unclean shutdown: set error on streams and mark inactive
    if (seg.header().rst) {
        _receiver.stream_out().set_error();
        _sender.stream_in().set_error();
        _is_active = false;
        return;
    }

    // Pass the segment to the receiver so it can process seqno, SYN, payload, FIN
    _receiver.segment_received(seg);

    // If incoming ACK, inform sender
    if (seg.header().ack) {
        _sender.ack_received(seg.header().ackno, seg.header().win);
    }

    // If the incoming segment contained no sequence-space bytes, it may be a keep-alive;
    // ensure we reply (send an empty ACK)
    if (seg.length_in_sequence_space() == 0) {
        _sender.send_empty_segment();
    }

    // If receiver can ack, populate outgoing segments with the correct ackno/window
    // and move them to _segments_out
    while (!_sender.segments_out().empty()) {
        TCPSegment out = _sender.segments_out().front();
        _sender.segments_out().pop();

        if (_receiver.ackno().has_value()) {
            out.header().ack = true;
            out.header().ackno = _receiver.ackno().value();
        }
        out.header().win = static_cast<uint16_t>(_receiver.window_size());

        _segments_out.push(out);
    }
}

bool TCPConnection::active() const {
    if (!_is_active) {
        return false;
    }

    const bool inbound_finished = _receiver.stream_out().eof();
    const bool outbound_finished = _sender.stream_in().eof();
    const bool no_inflight = _sender.bytes_in_flight() == 0;

    if (inbound_finished && outbound_finished && no_inflight) {
        if (_linger_after_streams_finish) {
            return _time_since_last_segment_received < 10 * _cfg.rt_timeout;
        }
        return false;
    }

    return true;
}

size_t TCPConnection::write(const string &data) {
    const size_t written = _sender.stream_in().write(data);
    // try to fill sender window and flush any outgoing segments
    _sender.fill_window();

    while (!_sender.segments_out().empty()) {
        TCPSegment seg = _sender.segments_out().front();
        _sender.segments_out().pop();

        if (_receiver.ackno().has_value()) {
            seg.header().ack = true;
            seg.header().ackno = _receiver.ackno().value();
        }
        seg.header().win = static_cast<uint16_t>(_receiver.window_size());

        _segments_out.push(seg);
    }

    return written;
}

//! \param[in] ms_since_last_tick number of milliseconds since the last call to this method
void TCPConnection::tick(const size_t ms_since_last_tick) {
    // advance timers
    _time_since_last_segment_received += ms_since_last_tick;
    _sender.tick(ms_since_last_tick);

    // If too many retransmissions, abort and send RST
    if (_sender.consecutive_retransmissions() > _cfg.MAX_RETX_ATTEMPTS) {
        // put streams into error state
        _receiver.stream_out().set_error();
        _sender.stream_in().set_error();
        // send RST
        TCPSegment rst_seg;
        rst_seg.header().rst = true;
        rst_seg.header().seqno = _sender.next_seqno();
        _segments_out.push(rst_seg);
        _is_active = false;
        return;
    }

    // flush any segments the sender wants to send
    while (!_sender.segments_out().empty()) {
        TCPSegment seg = _sender.segments_out().front();
        _sender.segments_out().pop();

        if (_receiver.ackno().has_value()) {
            seg.header().ack = true;
            seg.header().ackno = _receiver.ackno().value();
        }
        seg.header().win = static_cast<uint16_t>(_receiver.window_size());

        _segments_out.push(seg);
    }

    // check for natural termination: both streams finished and no bytes in flight
    const bool inbound_finished = _receiver.stream_out().eof();
    const bool outbound_finished = _sender.stream_in().eof();
    const bool no_inflight = _sender.bytes_in_flight() == 0;

    if (inbound_finished && outbound_finished && no_inflight) {
        if (_linger_after_streams_finish) {
            if (_time_since_last_segment_received >= 10 * _cfg.rt_timeout) {
                _is_active = false;
            }
        } else {
            _is_active = false;
        }
    }
}

void TCPConnection::end_input_stream() {
    _sender.stream_in().end_input();
    _sender.fill_window();

    while (!_sender.segments_out().empty()) {
        TCPSegment seg = _sender.segments_out().front();
        _sender.segments_out().pop();

        if (_receiver.ackno().has_value()) {
            seg.header().ack = true;
            seg.header().ackno = _receiver.ackno().value();
        }
        seg.header().win = static_cast<uint16_t>(_receiver.window_size());

        _segments_out.push(seg);
    }
}

void TCPConnection::connect() {
    _sender.fill_window();

    while (!_sender.segments_out().empty()) {
        TCPSegment seg = _sender.segments_out().front();
        _sender.segments_out().pop();

        if (_receiver.ackno().has_value()) {
            seg.header().ack = true;
            seg.header().ackno = _receiver.ackno().value();
        }
        seg.header().win = static_cast<uint16_t>(_receiver.window_size());

        _segments_out.push(seg);
    }
}

TCPConnection::~TCPConnection() {
    try {
        if (active()) {
            cerr << "Warning: Unclean shutdown of TCPConnection\n";

            // send RST segment to peer
            TCPSegment seg;
            seg.header().rst = true;
            seg.header().seqno = _sender.next_seqno();
            // best effort: push to outgoing queue
            _segments_out.push(seg);
        }
    } catch (const exception &e) {
        std::cerr << "Exception destructing TCP FSM: " << e.what() << std::endl;
    }
}
