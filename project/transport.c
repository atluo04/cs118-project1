#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include "consts.h"
#include <fcntl.h>
#include <time.h>

// Main function of transport layer; never quits
void listen_loop(int sockfd, struct sockaddr_in* addr, int type,
                 ssize_t (*input_p)(uint8_t*, size_t),
                 void (*output_p)(uint8_t*, size_t)) {

    char buffer[MAX_PAYLOAD];
    bool connected = false;
    int seq = 0;
    int ack = 0;
    srand(time(NULL));

    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

    uint8_t in_buf[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet *in_pkt = (packet *)in_buf;

    uint8_t out_buf[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet *out_pkt = (packet *) out_buf;
    while (true) { 
        ssize_t input_len = input_p((uint8_t *)buffer, MAX_PAYLOAD);

        socklen_t addr_len = sizeof(*addr);
        int bytes_recvd = recvfrom(sockfd, in_pkt, sizeof(packet) + MAX_PAYLOAD, 0,
                                   (struct sockaddr *)&addr, &addr_len);

        if (input_len > 0)
        {
            memcpy(out_pkt->payload, buffer, input_len);
            out_pkt->length = htons(input_len);
        }
        else
        {
            out_pkt->length = 0;
            memset(out_pkt->payload, 0, MAX_PAYLOAD);
        }

        if(type == CLIENT)
        {
            if(!connected){
                seq = (rand() % 1000 + 1);
                out_pkt->seq = htons(seq);
                out_pkt->flags = SYN;

                sendto(sockfd, out_pkt, sizeof(packet) + MAX_PAYLOAD, 0, (struct sockaddr *)addr, sizeof(*addr));
                connected = true;
            }
            else if(bytes_recvd > 0){
                if ((in_pkt->flags & SYN) && (in_pkt->flags & ACK))
                {
                    out_pkt->flags |= ACK;
                    out_pkt->ack = htons(in_pkt->seq + 1);
                    out_pkt->seq = htons(seq);
                }
                if (in_pkt->length > 0)
                {
                    output_p(in_pkt->payload, in_pkt->length);
                    ack += in_pkt->length;
                }

                sendto(sockfd, out_pkt, sizeof(packet) + MAX_PAYLOAD, 0,  (struct sockaddr *)addr, sizeof(*addr));
            }
        }

        else if (type == SERVER)
        {
            if(bytes_recvd > 0){
                if (in_pkt->flags & SYN)
                {
                    seq = (rand() % 1000 + 1);
                    out_pkt->seq = htons(seq);
                    out_pkt->flags = SYN | ACK;
                    out_pkt->ack = htons(in_pkt->seq + 1);

                    sendto(sockfd, out_pkt, sizeof(packet) + MAX_PAYLOAD, 0, (struct sockaddr *)addr, sizeof(*addr));
                }
            }
        }
        
        seq += input_len;
    }
}
                 
