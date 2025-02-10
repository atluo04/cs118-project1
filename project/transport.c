#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include "consts.h"
#include <fcntl.h>
#include <time.h>
#include <errno.h>

uint8_t compute_xor_checksum(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t checksum = 0;
    for (size_t i = 0; i < size; i++)
    {
        checksum ^= bytes[i]; 
    }
    return checksum;
}

packet* create_packet(uint16_t seq, uint16_t ack, uint16_t flags, uint16_t win, uint8_t *payload, size_t payload_len)
{
    char buf[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet *pkt = (packet *)buf;

    pkt->seq = htons(seq);
    pkt->ack = htons(ack);
    pkt->flags = flags;

    if (payload_len > 0)
    {
        memcpy(pkt->payload, payload, payload_len);
        pkt->length = htons(payload_len);
    }
    else
    {
        memset(pkt->payload, 0, MAX_PAYLOAD);
        pkt->length = 0;
    }
    if(compute_xor_checksum(pkt, sizeof(packet) + payload_len)){
        pkt->flags |= PARITY;
    }
    return pkt;
}

void init_buffer(Buffer *buffer)
{
    buffer->head = NULL;
    buffer->tail = NULL;
}

void add_packet(Buffer *buffer, packet *pkt)
{
    BufferNode *new_node = (BufferNode *)malloc(sizeof(BufferNode));
    new_node->pkt = pkt;
    new_node->next = NULL;
    if (buffer->tail)
    {
        buffer->tail->next = new_node;
    }
    else
    {
        buffer->head = new_node;
    }
    buffer->tail = new_node;
}

packet* receive_packets(Buffer* buffer, int sockfd, struct sockaddr_in* addr, socklen_t addr_len, int* window){
    char in_buf[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet *in_pkt = (packet *)in_buf;
    int bytes_recvd = recvfrom(sockfd, in_pkt, sizeof(packet) + MAX_PAYLOAD, 0,
                               (struct sockaddr *)addr, &addr_len);
    if (bytes_recvd > 0)
    {
        print_diag(in_pkt, RECV);
        add_packet(buffer, in_pkt);
        *window = in_pkt->win;
        return in_pkt;
    }
    return NULL;
}

// Main function of transport layer; never quits
void listen_loop(int sockfd, struct sockaddr_in* addr, int type,
                 ssize_t (*input_p)(uint8_t*, size_t),
                 void (*output_p)(uint8_t*, size_t)) {
    char stdin_buffer[MAX_PAYLOAD];
    uint8_t connected = 0;
    int seq = 0;
    int ack = 0;
    srand(time(NULL) ^ type);
    socklen_t addr_len = sizeof(struct sockaddr_in);

    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

    Buffer send_buffer;
    Buffer recv_buffer;
    init_buffer(&send_buffer);
    init_buffer(&recv_buffer);

    char in_buf[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet *in_pkt = (packet *)in_buf;

    int sent_bytes = 0;
    int window_size = 0;
    BufferNode* current_pkt = NULL;

    while (true) {
        ssize_t input_len = input_p((uint8_t *)stdin_buffer, MAX_PAYLOAD);
        // packet* in_pkt = receive_packets(&recv_buffer, sockfd, addr, addr_len, &window_size);
        int bytes_recvd = recvfrom(sockfd, in_pkt, sizeof(packet) + MAX_PAYLOAD, 0,
                                   (struct sockaddr *)addr, &addr_len);
        // if (bytes_recvd > 0)
        // {
        //     print_diag(in_pkt, RECV);
        //     add_packet(&recv_buffer, in_pkt);
        //     window_size = in_pkt->win;
        //     return in_pkt;
        // }

        if(input_len > 0){
            packet* pkt = create_packet(seq, 0, 0, MAX_WINDOW, stdin_buffer, input_len);
            add_packet(&send_buffer, pkt);
            if(current_pkt == NULL){
                current_pkt = send_buffer.tail;
            }
        }
        if(type == CLIENT)
        {
            if(connected == 0){
                seq = (rand() % 1000 + 1);
                packet *syn_pkt = create_packet(seq, 0, SYN, MAX_WINDOW, stdin_buffer, input_len);
                print_diag(syn_pkt, SEND);
                sendto(sockfd, syn_pkt, sizeof(packet) + syn_pkt->length, 0, (struct sockaddr *)addr, sizeof(*addr));
                connected += 1;
            }
            else if (bytes_recvd > 0)
            {
                if(!compute_xor_checksum(in_pkt, sizeof(packet) + in_pkt->length)){
                    continue;
                }
                if (in_pkt->flags & SYN){
                    print_diag(in_pkt, RECV);
                    if (input_len == 0)
                    {
                        seq = 0;
                    }
                    packet* handshake_ack_pkt = create_packet(seq, ntohs(in_pkt->seq) + 1, ACK, MAX_WINDOW, stdin_buffer, input_len);
                    print_diag(handshake_ack_pkt, SEND);
                    sendto(sockfd, handshake_ack_pkt, sizeof(packet) + handshake_ack_pkt->length, 0, (struct sockaddr *)addr, sizeof(*addr));
                    connected += 1;
                }
                else {
                    if(current_pkt){
                        current_pkt->pkt->ack = ack;
                        current_pkt->pkt->flags |= ACK;
                    }
                }
            }
        }
        else if (type == SERVER)
        {
            if(bytes_recvd > 0){
                if (!compute_xor_checksum(in_pkt, sizeof(packet) + in_pkt->length))
                {
                    continue;
                }
                if (in_pkt->flags & SYN)
                {
                    print_diag(in_pkt, RECV);
                    seq = (rand() % 1000 + 1);
                    ack = ntohs(in_pkt->seq) + 1;
                    packet* syn_ack_pkt = create_packet(seq, ack, SYN | ACK, MAX_WINDOW, stdin_buffer, input_len);
                    print_diag(syn_ack_pkt, SEND);
                    sendto(sockfd, syn_ack_pkt, sizeof(packet) + syn_ack_pkt->length, 0, (struct sockaddr *)addr, sizeof(*addr));
                }
                else if(connected == 0){
                    if (ntohs(in_pkt->seq) == ack || ntohs(in_pkt->seq) == 0){
                        ack = ntohs(in_pkt->seq) + 1;
                        packet *pkt = create_packet(seq, ack, ACK, MAX_WINDOW, stdin_buffer, input_len);
                        connected = 2;
                    }
                }
                else {
                    if (current_pkt)
                    {
                        current_pkt->pkt->ack = ack;
                        current_pkt->pkt->flags |= ACK;
                    }
                }
            }
        }
        if (connected > 1 && current_pkt && sent_bytes + current_pkt->pkt->length + sizeof(packet) <= window_size)
        {
            print_diag(current_pkt->pkt, SEND);
            sendto(sockfd, current_pkt->pkt, sizeof(packet) + current_pkt->pkt->length, 0, (struct sockaddr *)addr, sizeof(*addr));
            sent_bytes += current_pkt->pkt->length + sizeof(packet);
            current_pkt = current_pkt->next;
        }
        seq += 1;
    }
}
                 
