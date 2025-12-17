#include "tcp_sender.hh"

#include "tcp_config.hh"

#include <random>

// Dummy implementation of a TCP sender

// For Lab 3, please replace with a real implementation that passes the
// automated checks run by `make check_lab3`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _stream(capacity)
    , _initial_retransmission_timeout{retx_timeout}
    , _rto(retx_timeout) {}

uint64_t TCPSender::bytes_in_flight() const {
    uint64_t sum = 0;
    for (const auto &o : _outstanding) {
        sum += o.len;
    }
    return sum;
}

void TCPSender::fill_window() {
    // If we haven't sent SYN yet, send it first
    if (_next_seqno == 0) {
        TCPSegment seg;
        seg.header().syn = true;
        seg.header().seqno = wrap(_next_seqno, _isn);
        // no payload on initial SYN in this implementation
        // publish segment
        _segments_out.push(seg);
        OutstandingSegment o{seg, _next_seqno, static_cast<size_t>(seg.length_in_sequence_space())};
        _outstanding.push_back(o);
        _next_seqno += o.len;
        // start timer
        if (not _timer_running) {
            _timer_running = true;
            _time_since_last_tick = 0;
            _rto = _initial_retransmission_timeout;
        }
        return;
    }

    // Compute effective window (treat 0 advertised as 1)
    size_t eff_win = _remote_window == 0 ? 1u : _remote_window;

    // available space in window = eff_win - bytes_in_flight
    while (true) {
        const uint64_t in_flight = bytes_in_flight();
        if (in_flight >= eff_win)
            break;
        size_t available = eff_win - static_cast<size_t>(in_flight);

        // cannot send more than MAX_PAYLOAD_SIZE in one segment
        size_t payload_len = std::min(TCPConfig::MAX_PAYLOAD_SIZE, _stream.buffer_size());
        payload_len = std::min(payload_len, available);

        bool will_send_fin = false;
        if (payload_len == 0) {
            // maybe send FIN-only
            if (_stream.eof() && !_fin_sent && available >= 1) {
                // send FIN-only segment
                TCPSegment seg;
                seg.header().seqno = wrap(_next_seqno, _isn);
                seg.header().fin = true;
                _segments_out.push(seg);
                OutstandingSegment o{seg, _next_seqno, static_cast<size_t>(seg.length_in_sequence_space())};
                _outstanding.push_back(o);
                _next_seqno += o.len;
                _fin_sent = true;
                if (not _timer_running) {
                    _timer_running = true;
                    _time_since_last_tick = 0;
                    _rto = _initial_retransmission_timeout;
                }
            }
            break;
        }

        // read payload from stream_in
        TCPSegment seg;
        seg.header().seqno = wrap(_next_seqno, _isn);
        seg.payload() = Buffer(_stream.read(payload_len));

        // decide whether to set FIN in this segment
        if (_stream.eof() && !_fin_sent) {
            // if there's still room for FIN after payload
            if (available >= payload_len + 1) {
                seg.header().fin = true;
                will_send_fin = true;
            }
        }

        _segments_out.push(seg);
        OutstandingSegment o{seg, _next_seqno, static_cast<size_t>(seg.length_in_sequence_space())};
        _outstanding.push_back(o);
        _next_seqno += o.len;
        if (will_send_fin)
            _fin_sent = true;

        if (not _timer_running) {
            _timer_running = true;
            _time_since_last_tick = 0;
            _rto = _initial_retransmission_timeout;
        }

        // if we set FIN, we're done
        if (will_send_fin)
            break;
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
void TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    // convert ackno to absolute
    uint64_t ack_abs = unwrap(ackno, _isn, _next_seqno);

    // ignore ack that acknowledges beyond what we've sent
    if (ack_abs > _next_seqno)
        return;

    bool removed_any = false;
    // remove all outstanding segments that are fully acknowledged
    while (!_outstanding.empty()) {
        const OutstandingSegment &o = _outstanding.front();
        uint64_t seg_end = o.seqno + o.len;
        if (seg_end <= ack_abs) {
            _outstanding.pop_front();
            removed_any = true;
            continue;
        }
        break;
    }

    if (removed_any) {
        // reset retransmission timer and counters
        _consec_retransmissions = 0;
        _rto = _initial_retransmission_timeout;
        _time_since_last_tick = 0;
        _timer_running = !_outstanding.empty();
    }

    // update remote window
    _remote_window = window_size;

    // After processing ack, try to fill the window
    fill_window();
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    if (!_timer_running)
        return;

    _time_since_last_tick += ms_since_last_tick;
    if (_outstanding.empty()) {
        _timer_running = false;
        return;
    }

    if (_time_since_last_tick >= _rto) {
        // retransmit the oldest outstanding segment
        const OutstandingSegment &o = _outstanding.front();
        _segments_out.push(o.seg);

        if (_remote_window > 0) {
            // exponential backoff
            _consec_retransmissions++;
            _rto *= 2;
        }

        // restart timer
        _time_since_last_tick = 0;
        _timer_running = true;
    }
}

unsigned int TCPSender::consecutive_retransmissions() const { return _consec_retransmissions; }

void TCPSender::send_empty_segment() {
    TCPSegment seg;
    seg.header().seqno = wrap(_next_seqno, _isn);
    _segments_out.push(seg);
}
