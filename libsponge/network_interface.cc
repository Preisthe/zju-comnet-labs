#include "network_interface.hh"

#include "arp_message.hh"
#include "ethernet_frame.hh"

#include <iostream>
#include <unordered_map>
#include <deque>

// Dummy implementation of a network interface
// Translates from {IP datagram, next hop address} to link-layer frame, and from link-layer frame to IP datagram

// For Lab 5, please replace with a real implementation that passes the
// automated checks run by `make check_lab5`.

// You will need to add private members to the class declaration in `network_interface.hh`

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface(const EthernetAddress &ethernet_address, const Address &ip_address)
    : _ethernet_address(ethernet_address), _ip_address(ip_address) {
    cerr << "DEBUG: Network interface has Ethernet address " << to_string(_ethernet_address) << " and IP address "
         << ip_address.ip() << "\n";
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but may also be another host if directly connected to the same network as the destination)
//! (Note: the Address type can be converted to a uint32_t (raw 32-bit IP address) with the Address::ipv4_numeric() method.)
void NetworkInterface::send_datagram(const InternetDatagram &dgram, const Address &next_hop) {
    // convert IP address of next hop to raw 32-bit representation (used in ARP header)
    const uint32_t next_hop_ip = next_hop.ipv4_numeric();
    // If we know the Ethernet address for the next hop, immediately send the
    // IPv4 datagram encapsulated in an Ethernet frame.
    const auto cache_it = _arp_cache.find(next_hop_ip);
    if (cache_it != _arp_cache.end()) {
        EthernetFrame frame;
        frame.header().src = _ethernet_address;
        frame.header().dst = cache_it->second.ethernet_address;
        frame.header().type = EthernetHeader::TYPE_IPv4;
        frame.payload() = dgram.serialize();
        _frames_out.push(frame);
        return;
    }

    // Otherwise, queue the datagram and send an ARP request if one is not
    // already pending. Pending requests expire after 5 seconds (5000 ms).
    const auto pending_it = _pending_requests.find(next_hop_ip);
    if (pending_it != _pending_requests.end()) {
        pending_it->second.queued_datagrams.push_back(dgram);
        return;
    }

    // create a new pending request and send ARP request
    PendingRequest pending;
    pending.retry_ttl_ms = 5000;
    pending.queued_datagrams.push_back(dgram);

    ARPMessage arp;
    arp.opcode = ARPMessage::OPCODE_REQUEST;
    arp.sender_ethernet_address = _ethernet_address;
    arp.sender_ip_address = _ip_address.ipv4_numeric();
    // target_ethernet_address left zeroed
    arp.target_ip_address = next_hop_ip;

    EthernetFrame frame;
    frame.header().src = _ethernet_address;
    frame.header().dst = ETHERNET_BROADCAST;
    frame.header().type = EthernetHeader::TYPE_ARP;
    frame.payload() = arp.serialize();
    _frames_out.push(frame);

    _pending_requests.emplace(next_hop_ip, std::move(pending));

}

//! \param[in] frame the incoming Ethernet frame
optional<InternetDatagram> NetworkInterface::recv_frame(const EthernetFrame &frame) {
    // drop frames not addressed to us and not broadcast
    if ((frame.header().dst != _ethernet_address) && (frame.header().dst != ETHERNET_BROADCAST)) {
        return {};
    }

    switch (frame.header().type) {
        case EthernetHeader::TYPE_IPv4: {
            InternetDatagram dgram;
            const Buffer payload_single = frame.payload().concatenate();
            if (dgram.parse(payload_single) == ParseResult::NoError) {
                return dgram;
            }
            return {};
        }
        case EthernetHeader::TYPE_ARP: {
            ARPMessage arp;
            const Buffer payload_single = frame.payload().concatenate();
            if (arp.parse(payload_single) != ParseResult::NoError) {
                return {};
            }

            // learn mapping from sender -> ethernet address for 30 seconds
            ARPCacheEntry entry;
            entry.ethernet_address = arp.sender_ethernet_address;
            entry.ttl_ms = 30000;
            _arp_cache[arp.sender_ip_address] = entry;

            // if this is an ARP request for our IP, reply
            if ((arp.opcode == ARPMessage::OPCODE_REQUEST) &&
                (arp.target_ip_address == _ip_address.ipv4_numeric())) {
                ARPMessage reply;
                reply.opcode = ARPMessage::OPCODE_REPLY;
                reply.sender_ethernet_address = _ethernet_address;
                reply.sender_ip_address = _ip_address.ipv4_numeric();
                reply.target_ethernet_address = arp.sender_ethernet_address;
                reply.target_ip_address = arp.sender_ip_address;

                EthernetFrame reply_frame;
                reply_frame.header().src = _ethernet_address;
                reply_frame.header().dst = arp.sender_ethernet_address;
                reply_frame.header().type = EthernetHeader::TYPE_ARP;
                reply_frame.payload() = reply.serialize();
                _frames_out.push(reply_frame);
            }

            // if this is an ARP reply, send any queued datagrams
            if (arp.opcode == ARPMessage::OPCODE_REPLY) {
                const auto pend_it = _pending_requests.find(arp.sender_ip_address);
                if (pend_it != _pending_requests.end()) {
                    for (const auto &d : pend_it->second.queued_datagrams) {
                        EthernetFrame out;
                        out.header().src = _ethernet_address;
                        out.header().dst = arp.sender_ethernet_address;
                        out.header().type = EthernetHeader::TYPE_IPv4;
                        out.payload() = d.serialize();
                        _frames_out.push(out);
                    }
                    _pending_requests.erase(pend_it);
                }
            }

            return {};
        }
        default:
            return {};
    }
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick(const size_t ms_since_last_tick) {
    // expire ARP cache entries
    std::vector<uint32_t> to_remove;
    for (auto &kv : _arp_cache) {
        if (kv.second.ttl_ms <= ms_since_last_tick) {
            to_remove.push_back(kv.first);
        } else {
            kv.second.ttl_ms -= ms_since_last_tick;
        }
    }
    for (auto k : to_remove) {
        _arp_cache.erase(k);
    }

    // expire pending ARP requests
    std::vector<uint32_t> pend_remove;
    for (auto &kv : _pending_requests) {
        if (kv.second.retry_ttl_ms <= ms_since_last_tick) {
            pend_remove.push_back(kv.first);
        } else {
            kv.second.retry_ttl_ms -= ms_since_last_tick;
        }
    }
    for (auto k : pend_remove) {
        _pending_requests.erase(k);
    }
}
