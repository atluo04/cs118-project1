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

void fill_packet(packet *pkt, uint16_t seq, uint16_t ack, uint16_t flags, uint16_t win, uint8_t *payload, size_t payload_len)
{
    pkt->seq = htons(seq);
    pkt->ack = htons(ack);
    pkt->flags = flags;

    if (payload && payload_len > 0)
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
}

// Main function of transport layer; never quits
void listen_loop(int sockfd, struct sockaddr_in* addr, int type,
                 ssize_t (*input_p)(uint8_t*, size_t),
                 void (*output_p)(uint8_t*, size_t)) {
    char buffer[MAX_PAYLOAD];
    bool connected = false;
    int seq = 0;
    int ack = 0;
    srand(time(NULL) ^ type);

    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

    char in_buf[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet* in_pkt = (packet *)in_buf;

    char out_buf[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet *out_pkt = (packet *) out_buf;


    while (true) {
        ssize_t input_len = input_p((uint8_t *)buffer, MAX_PAYLOAD);
        socklen_t addr_len = sizeof(struct sockaddr_in);
        int bytes_recvd = recvfrom(sockfd, in_pkt, sizeof(packet) + MAX_PAYLOAD, 0,
                                   (struct sockaddr *)addr, &addr_len);


        if(type == CLIENT)
        {
            if(!connected){
                seq = (rand() % 1000 + 1);
                fill_packet(out_pkt, seq, 0, SYN, MAX_WINDOW, buffer, input_len);
                print_diag(out_pkt, SEND);
                sendto(sockfd, out_pkt, sizeof(packet) + MAX_PAYLOAD, 0, (struct sockaddr *)addr, sizeof(*addr));
                connected = true;
            }
            else if(bytes_recvd > 0){
                if(!compute_xor_checksum(in_pkt, bytes_recvd)){
                    continue;
                }
                if (in_pkt->flags & SYN){
                    print_diag(in_pkt, RECV);
                    output_p(in_pkt->payload, ntohs(in_pkt->length));
                    ack += ntohs(in_pkt->length);
                    if (input_len == 0)
                    {
                        seq = 0;
                    }
                    fill_packet(out_pkt, seq, ntohs(in_pkt->seq) + 1, ACK, MAX_WINDOW, buffer, input_len);
                    print_diag(out_pkt, SEND);
                    sendto(sockfd, out_pkt, sizeof(packet) + MAX_PAYLOAD, 0,  (struct sockaddr *)addr, sizeof(*addr));
                }
                else {

                }
            }
        }
        else if (type == SERVER)
        {
            if(bytes_recvd > 0){
                if (!compute_xor_checksum(in_pkt, bytes_recvd))
                {
                    continue;
                }
                print_diag(in_pkt, RECV);
                if (in_pkt->flags & SYN)
                {
                    seq = (rand() % 1000 + 1);
                    ack = ntohs(in_pkt->seq) + 1;
                    fill_packet(out_pkt, seq, ack, SYN | ACK, MAX_WINDOW, buffer, input_len);
                    print_diag(out_pkt, SEND);
                    sendto(sockfd, out_pkt, sizeof(packet) + MAX_PAYLOAD, 0, (struct sockaddr *)addr, sizeof(*addr));
                }
                else if(!connected){
                    if (ntohs(in_pkt->seq) != ack || ntohs(in_pkt->seq) != 0){
                        // Disard or store in buffer
                    }
                    else {
                        ack = ntohs(in_pkt->seq) + 1;
                        fill_packet(out_pkt, seq, ack, ACK, MAX_WINDOW, buffer, input_len);
                        connected = true;
                    }
                }
                else {

                }
            }
        }
        
        seq += 1;
    }
}
                 
