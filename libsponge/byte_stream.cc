#include "byte_stream.hh"
#include <algorithm>
#include <cstring>

// Dummy implementation of a flow-controlled in-memory byte stream.

// For Lab 0, please replace with a real implementation that passes the
// automated checks run by `make check_lab0`.

// You will need to add private members to the class declaration in `byte_stream.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

ByteStream::ByteStream(const size_t capacity) {
    _buffer = new char[capacity];
    _capacity = capacity;
}

size_t ByteStream::write(const string &data) {
    size_t len = data.size();
    if (len > _capacity - _write_pos) {
        len = _capacity - _write_pos;
    }
    memcpy(_buffer + _write_pos, data.c_str(), len);
    _write_pos += len;
    _bytes_written += len;
    return len;
}

//! \param[in] len bytes will be copied from the output side of the buffer
string ByteStream::peek_output(const size_t len) const {
    return string(_buffer, len);
}

//! \param[in] len bytes will be removed from the output side of the buffer
void ByteStream::pop_output(const size_t len) {
    if (len > _write_pos) {
        _write_pos = 0;
        _bytes_read += _write_pos;
    } else {
        _write_pos -= len;
        memmove(_buffer, _buffer + len, _write_pos);
        _bytes_read += len;
    }
}

//! Read (i.e., copy and then pop) the next "len" bytes of the stream
//! \param[in] len bytes will be popped and returned
//! \returns a string
std::string ByteStream::read(const size_t len) {
    string ret = peek_output(len);
    pop_output(len);
    return ret;
}

void ByteStream::end_input() { _end_input = true; }

bool ByteStream::input_ended() const { return _end_input; }

size_t ByteStream::buffer_size() const { return _write_pos; }

bool ByteStream::buffer_empty() const { return _write_pos == 0; }

bool ByteStream::eof() const { return _end_input && _write_pos == 0; }

size_t ByteStream::bytes_written() const { return _bytes_written; }

size_t ByteStream::bytes_read() const { return _bytes_read; }

size_t ByteStream::remaining_capacity() const { return _capacity - _write_pos; }
