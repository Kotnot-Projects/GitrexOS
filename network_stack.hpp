#ifndef GITREX_NETWORK_STACK_HPP
#define GITREX_NETWORK_STACK_HPP

namespace gitrex {

typedef void (*NetworkSendFrame)(const unsigned char* frame, unsigned short length);
typedef void (*NetworkPump)(void);

void network_stack_start(NetworkSendFrame send_frame, NetworkPump pump,
                         const unsigned char* mac_address);
void network_stack_input(const unsigned char* frame, unsigned short length);
bool network_http_get(const char* url, char* body, unsigned int capacity);

}

#endif