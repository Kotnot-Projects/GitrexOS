#include "network_stack.hpp"

namespace gitrex {

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

static NetworkSendFrame send_frame;
static NetworkPump pump_network;
static u8 local_mac[6];
static u8 router_mac[6];
static const u8 local_ip[4] = {10, 0, 2, 15};
static const u8 router_ip[4] = {10, 0, 2, 2};
static const u8 dns_ip[4] = {10, 0, 2, 3};
static u8 router_known;
static u16 ipv4_identification;
static u16 dns_identifier;
static u8 dns_ready;
static u8 dns_address[4];
static u8 remote_address[4];
static u32 remote_ip;
static u16 tcp_local_port;
static u16 tcp_remote_port;
static u32 tcp_sequence;
static u32 tcp_acknowledgement;
static u8 tcp_state;
static char tcp_response[6144];
static u16 tcp_response_length;

static bool starts_with(const char* text, const char* prefix) {
    while (*prefix) {
        if (*text++ != *prefix++) return false;
    }
    return true;
}

static u16 read16(const u8* p) {
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static u32 read32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | p[3];
}

static void write16(u8* p, u16 value) {
    p[0] = (u8)(value >> 8);
    p[1] = (u8)value;
}

static void write32(u8* p, u32 value) {
    p[0] = (u8)(value >> 24);
    p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);
    p[3] = (u8)value;
}

static u16 checksum(const u8* data, u16 length, u32 sum) {
    u16 i = 0;
    while (i + 1 < length) {
        sum += read16(data + i);
        i += 2;
    }
    if (i < length) sum += (u16)((u16)data[i] << 8);
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (u16)~sum;
}

static bool same_ip(const u8* a, const u8* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static void send_ethernet(const u8* destination, u16 type,
                          const u8* payload, u16 length) {
    u8 frame[1536];
    u16 i;
    u16 frame_length = (u16)(14 + length);
    if (!send_frame || frame_length > sizeof(frame)) return;
    for (i = 0; i < 6; ++i) {
        frame[i] = destination[i];
        frame[6 + i] = local_mac[i];
    }
    write16(frame + 12, type);
    for (i = 0; i < length; ++i) frame[14 + i] = payload[i];
    while (frame_length < 60) frame[frame_length++] = 0;
    send_frame(frame, frame_length);
}

static void send_arp_request(void) {
    static const u8 broadcast[6] = {255,255,255,255,255,255};
    u8 arp[28];
    u8 i;
    write16(arp, 1);
    write16(arp + 2, 0x0800);
    arp[4] = 6;
    arp[5] = 4;
    write16(arp + 6, 1);
    for (i = 0; i < 6; ++i) arp[8 + i] = local_mac[i];
    for (i = 0; i < 4; ++i) arp[14 + i] = local_ip[i];
    for (i = 0; i < 10; ++i) arp[18 + i] = 0;
    for (i = 0; i < 4; ++i) arp[24 + i] = router_ip[i];
    send_ethernet(broadcast, 0x0806, arp, sizeof(arp));
}

static bool ensure_router(void) {
    u32 wait;
    if (router_known) return true;
    send_arp_request();
    for (wait = 0; wait < 4000000u && !router_known; ++wait) {
        if (pump_network) pump_network();
    }
    return router_known != 0;
}

static void send_ipv4(u8 protocol, const u8* destination,
                      const u8* payload, u16 payload_length) {
    u8 packet[1500];
    u16 total = (u16)(20 + payload_length);
    u16 i;
    const u8* ethernet;
    if (total > sizeof(packet)) return;
    if (!ensure_router()) return;
    ethernet = router_mac;
    packet[0] = 0x45;
    packet[1] = 0;
    write16(packet + 2, total);
    write16(packet + 4, ++ipv4_identification);
    write16(packet + 6, 0x4000);
    packet[8] = 64;
    packet[9] = protocol;
    packet[10] = packet[11] = 0;
    for (i = 0; i < 4; ++i) {
        packet[12 + i] = local_ip[i];
        packet[16 + i] = destination[i];
    }
    write16(packet + 10, checksum(packet, 20, 0));
    for (i = 0; i < payload_length; ++i) packet[20 + i] = payload[i];
    send_ethernet(ethernet, 0x0800, packet, total);
}

static void send_udp(const u8* destination, u16 source_port,
                     u16 destination_port, const u8* data, u16 length) {
    u8 packet[1100];
    u16 i;
    if ((u32)length + 8 > sizeof(packet)) return;
    write16(packet, source_port);
    write16(packet + 2, destination_port);
    write16(packet + 4, (u16)(length + 8));
    write16(packet + 6, 0);
    for (i = 0; i < length; ++i) packet[8 + i] = data[i];
    send_ipv4(17, destination, packet, (u16)(length + 8));
}

static u16 skip_dns_name(const u8* data, u16 length, u16 offset) {
    while (offset < length) {
        u8 label = data[offset++];
        if (!label) return offset;
        if ((label & 0xC0) == 0xC0) return offset < length ? (u16)(offset + 1) : 0;
        if (label & 0xC0 || (u32)offset + label > length) return 0;
        offset = (u16)(offset + label);
    }
    return 0;
}

static void receive_dns(const u8* data, u16 length) {
    u16 questions;
    u16 answers;
    u16 offset = 12;
    u16 i;
    if (length < 12 || read16(data) != dns_identifier ||
        !(read16(data + 2) & 0x8000)) return;
    questions = read16(data + 4);
    answers = read16(data + 6);
    for (i = 0; i < questions; ++i) {
        offset = skip_dns_name(data, length, offset);
        if (!offset || (u32)offset + 4 > length) return;
        offset += 4;
    }
    for (i = 0; i < answers; ++i) {
        u16 type;
        u16 klass;
        u16 size;
        offset = skip_dns_name(data, length, offset);
        if (!offset || (u32)offset + 10 > length) return;
        type = read16(data + offset);
        klass = read16(data + offset + 2);
        size = read16(data + offset + 8);
        offset += 10;
        if ((u32)offset + size > length) return;
        if (type == 1 && klass == 1 && size == 4) {
            dns_address[0] = data[offset];
            dns_address[1] = data[offset + 1];
            dns_address[2] = data[offset + 2];
            dns_address[3] = data[offset + 3];
            dns_ready = 1;
            return;
        }
        offset += size;
    }
}

static void send_tcp(const u8* destination, u16 source_port,
                     u16 destination_port, u32 sequence, u32 acknowledgement,
                     u8 flags, const u8* data, u16 length) {
    u8 segment[1200];
    u32 pseudo_sum;
    u16 i;
    if ((u32)length + 20 > sizeof(segment)) return;
    write16(segment, source_port);
    write16(segment + 2, destination_port);
    write32(segment + 4, sequence);
    write32(segment + 8, acknowledgement);
    segment[12] = 0x50;
    segment[13] = flags;
    write16(segment + 14, 4096);
    write16(segment + 16, 0);
    write16(segment + 18, 0);
    for (i = 0; i < length; ++i) segment[20 + i] = data[i];
    pseudo_sum = ((u32)local_ip[0] << 8) | local_ip[1];
    pseudo_sum += ((u32)local_ip[2] << 8) | local_ip[3];
    pseudo_sum += ((u32)destination[0] << 8) | destination[1];
    pseudo_sum += ((u32)destination[2] << 8) | destination[3];
    pseudo_sum += 6 + 20 + length;
    write16(segment + 16, checksum(segment, (u16)(20 + length), pseudo_sum));
    send_ipv4(6, destination, segment, (u16)(20 + length));
}

static void receive_tcp(const u8* source_ip, const u8* tcp, u16 length) {
    u16 source_port;
    u16 destination_port;
    u32 sequence;
    u32 acknowledgement;
    u16 header_length;
    u16 data_length;
    u8 flags;
    if (length < 20) return;
    source_port = read16(tcp);
    destination_port = read16(tcp + 2);
    if (source_port != tcp_remote_port || destination_port != tcp_local_port ||
        read32(source_ip) != remote_ip) return;
    sequence = read32(tcp + 4);
    acknowledgement = read32(tcp + 8);
    header_length = (u16)((tcp[12] >> 4) * 4);
    flags = tcp[13];
    if (header_length < 20 || header_length > length) return;
    if (flags & 0x04) { tcp_state = 6; return; }
    if (tcp_state == 1 && (flags & 0x12) == 0x12 &&
        acknowledgement == tcp_sequence + 1) {
        tcp_acknowledgement = sequence + 1;
        tcp_state = 2;
        send_tcp(remote_address, tcp_local_port, tcp_remote_port,
                 tcp_sequence + 1, tcp_acknowledgement, 0x10, 0, 0);
        return;
    }
    if (tcp_state < 3) return;
    data_length = (u16)(length - header_length);
    if (data_length && sequence == tcp_acknowledgement) {
        u16 i;
        for (i = 0; i < data_length &&
             tcp_response_length + 1 < sizeof(tcp_response); ++i) {
            tcp_response[tcp_response_length++] = (char)tcp[header_length + i];
        }
        tcp_acknowledgement += data_length;
        send_tcp(remote_address, tcp_local_port, tcp_remote_port,
                 tcp_sequence, tcp_acknowledgement, 0x10, 0, 0);
    }
    if (flags & 0x01) {
        ++tcp_acknowledgement;
        send_tcp(remote_address, tcp_local_port, tcp_remote_port,
                 tcp_sequence, tcp_acknowledgement, 0x10, 0, 0);
        tcp_state = 4;
    }
}

static void receive_ipv4(const u8* packet, u16 length) {
    u16 header_length;
    u16 total_length;
    u8 protocol;
    const u8* source_ip;
    if (length < 20 || (packet[0] >> 4) != 4) return;
    header_length = (u16)((packet[0] & 0x0F) * 4);
    total_length = read16(packet + 2);
    if (header_length < 20 || header_length > total_length ||
        total_length > length) return;
    if (!same_ip(packet + 16, local_ip)) return;
    protocol = packet[9];
    source_ip = packet + 12;
    if (protocol == 17 && total_length >= header_length + 8) {
        const u8* udp = packet + header_length;
        u16 udp_length = read16(udp + 4);
        if (udp_length < 8 || udp_length > total_length - header_length) return;
        if (read16(udp) == 53 && read16(udp + 2) == 20000) {
            receive_dns(udp + 8, (u16)(udp_length - 8));
        }
    } else if (protocol == 6) {
        receive_tcp(source_ip, packet + header_length,
                    (u16)(total_length - header_length));
    }
}

void network_stack_input(const unsigned char* frame, unsigned short length) {
    const u8* packet = (const u8*)frame;
    u16 type;
    if (!packet || length < 14) return;
    type = read16(packet + 12);
    if (type == 0x0806 && length >= 42) {
        const u8* arp = packet + 14;
        u16 operation = read16(arp + 6);
        const u8* sender_ip = arp + 14;
        if (same_ip(sender_ip, router_ip)) {
            u8 i;
            for (i = 0; i < 6; ++i) router_mac[i] = arp[8 + i];
            router_known = 1;
        }
        if (operation == 1 && same_ip(arp + 24, local_ip)) {
            u8 reply[28];
            u8 i;
            write16(reply, 1);
            write16(reply + 2, 0x0800);
            reply[4] = 6;
            reply[5] = 4;
            write16(reply + 6, 2);
            for (i = 0; i < 6; ++i) reply[8 + i] = local_mac[i];
            for (i = 0; i < 4; ++i) reply[14 + i] = local_ip[i];
            for (i = 0; i < 6; ++i) reply[18 + i] = arp[8 + i];
            for (i = 0; i < 4; ++i) reply[24 + i] = sender_ip[i];
            send_ethernet(arp + 8, 0x0806, reply, sizeof(reply));
        }
    } else if (type == 0x0800 && length >= 34) {
        receive_ipv4(packet + 14, (u16)(length - 14));
    }
}

static bool resolve_host(const char* host) {
    u8 query[256];
    u16 length = 12;
    u16 i = 0;
    u16 label_start = 0;
    u32 wait;
    query[0] = (u8)(dns_identifier >> 8);
    query[1] = (u8)dns_identifier;
    query[2] = 1;
    query[3] = 0;
    query[4] = 0;
    query[5] = 1;
    query[6] = query[7] = query[8] = query[9] = query[10] = query[11] = 0;
    while (host[i]) {
        if (host[i] == '.') {
            if (i == label_start || i - label_start > 63 || length + 1 >= sizeof(query)) return false;
            query[length++] = (u8)(i - label_start);
            while (label_start < i) query[length++] = (u8)host[label_start++];
            ++i;
            label_start = i;
        } else {
            ++i;
        }
    }
    if (i == label_start || i - label_start > 63 || length + i - label_start + 5 > sizeof(query)) return false;
    query[length++] = (u8)(i - label_start);
    while (label_start < i) query[length++] = (u8)host[label_start++];
    query[length++] = 0;
    query[length++] = 0;
    query[length++] = 1;
    query[length++] = 0;
    query[length++] = 1;
    dns_ready = 0;
    ++dns_identifier;
    query[0] = (u8)(dns_identifier >> 8);
    query[1] = (u8)dns_identifier;
    if (!ensure_router()) return false;
    send_udp(dns_ip, 20000, 53, query, length);
    for (wait = 0; wait < 6000000u && !dns_ready; ++wait) {
        if (pump_network) pump_network();
    }
    return dns_ready != 0;
}

static const char* find_header_end(const char* text) {
    while (text[0] && text[1] && text[2] && text[3]) {
        if (text[0] == '\r' && text[1] == '\n' &&
            text[2] == '\r' && text[3] == '\n') return text;
        ++text;
    }
    return 0;
}

bool network_http_get(const char* url, char* body, unsigned int capacity) {
    char host[128];
    char path[256];
    char request[512];
    const char* p;
    u16 host_length = 0;
    u16 path_length = 0;
    u16 request_length = 0;
    u16 i;
    u32 wait;
    const char* header_end;
    if (!url || !body || capacity < 2 || !starts_with(url, "http://")) return false;
    p = url + 7;
    while (*p && *p != '/' && *p != '?' && *p != ':' &&
           host_length + 1 < sizeof(host)) host[host_length++] = *p++;
    host[host_length] = 0;
    if (!host_length || *p == ':') return false;
    if (*p == '?') {
        path[path_length++] = '/';
        while (*p && path_length + 1 < sizeof(path)) path[path_length++] = *p++;
    } else if (*p == '/') {
        while (*p && path_length + 1 < sizeof(path)) path[path_length++] = *p++;
    }
    if (!path_length) path[path_length++] = '/';
    path[path_length] = 0;
    if (!ensure_router() || !resolve_host(host)) return false;
    remote_ip = ((u32)dns_address[0] << 24) | ((u32)dns_address[1] << 16) |
                ((u32)dns_address[2] << 8) | dns_address[3];
    for (i = 0; i < 4; ++i) remote_address[i] = dns_address[i];
    tcp_remote_port = 80;
    tcp_local_port = 49152;
    tcp_sequence = 0x47585201u;
    tcp_acknowledgement = 0;
    tcp_state = 1;
    tcp_response_length = 0;
    tcp_response[0] = 0;
    send_tcp(remote_address, tcp_local_port, tcp_remote_port,
             tcp_sequence, 0, 0x02, 0, 0);
    for (wait = 0; wait < 6000000u && tcp_state == 1; ++wait) {
        if (pump_network) pump_network();
    }
    if (tcp_state != 2) return false;
    ++tcp_sequence;
    request[request_length++] = 'G';
    request[request_length++] = 'E';
    request[request_length++] = 'T';
    request[request_length++] = ' ';
    for (i = 0; path[i] && request_length + 1 < sizeof(request); ++i) request[request_length++] = path[i];
    {
        static const char tail1[] = " HTTP/1.0\r\nHost: ";
        static const char tail2[] = "\r\nAccept: text/html,text/plain\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
        for (i = 0; tail1[i] && request_length + 1 < sizeof(request); ++i) request[request_length++] = tail1[i];
        for (i = 0; host[i] && request_length + 1 < sizeof(request); ++i) request[request_length++] = host[i];
        for (i = 0; tail2[i] && request_length + 1 < sizeof(request); ++i) request[request_length++] = tail2[i];
    }
    request[request_length] = 0;
    tcp_state = 3;
    send_tcp(remote_address, tcp_local_port, tcp_remote_port,
             tcp_sequence, tcp_acknowledgement, 0x18,
             (const u8*)request, request_length);
    tcp_sequence += request_length;
    for (wait = 0; wait < 12000000u && tcp_state == 3; ++wait) {
        if (pump_network) pump_network();
    }
    if (tcp_state != 4 || !tcp_response_length) return false;
    tcp_response[tcp_response_length] = 0;
    header_end = find_header_end(tcp_response);
    if (!header_end) return false;
    header_end += 4;
    i = 0;
    while (header_end[i] && i + 1 < capacity) {
        body[i] = header_end[i];
        ++i;
    }
    body[i] = 0;
    tcp_state = 0;
    return i != 0;
}

void network_stack_start(NetworkSendFrame sender, NetworkPump pump,
                         const unsigned char* mac_address) {
    u8 i;
    send_frame = sender;
    pump_network = pump;
    if (!mac_address) return;
    for (i = 0; i < 6; ++i) local_mac[i] = mac_address[i];
    dns_identifier = 0x4758;
}

}