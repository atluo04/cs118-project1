#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include "consts.h"
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

uint8_t compute_xor_checksum(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t checksum = 0;
    for (size_t i = 0; i < size; i++)
    {
        uint8_t byte = bytes[i];
        for (int j = 0; j < 8; j++) 
        {
            checksum ^= (byte >> j) & 1;
        }
    }
    return checksum;
}

int send_packet(int sockfd, struct sockaddr_in *addr, packet *pkt, int diag)
{
    if (compute_xor_checksum(pkt, sizeof(packet) + ntohs(pkt->length)))
    {
        pkt->flags |= PARITY;
    }
    print_diag(pkt, diag);
    return sendto(sockfd, pkt, sizeof(packet) + ntohs(pkt->length), 0, (struct sockaddr *)addr, sizeof(*addr));
}

void init_buffer(Buffer *buffer)
{
    buffer->head = NULL;
    buffer->tail = NULL;
}

packet *create_packet(uint16_t seq, uint16_t ack, uint16_t flags, uint16_t win, uint8_t *payload, size_t payload_len)
{
    packet *pkt = (packet *)malloc(sizeof(packet) + payload_len);
    memset(pkt, 0, sizeof(packet));
    pkt->seq = htons(seq);
    pkt->ack = htons(ack);
    pkt->flags = flags;
    pkt->win = htons(win);

    if (payload_len > 0)
    {
        memcpy(pkt->payload, payload, payload_len);
        pkt->length = htons(payload_len);
    }
    else
    {
        memset(pkt->payload, 0, 0);
        pkt->length = 0;
    }
    return pkt;
}

void add_packet(Buffer *buffer, packet *pkt)
{
    BufferNode *new_node = (BufferNode *)malloc(sizeof(BufferNode));

    new_node->pkt = pkt;
    new_node->next = NULL;
    BufferNode* current = buffer->head;
    BufferNode* previous = NULL;

    if (buffer->head == NULL || ntohs(pkt->seq) < ntohs(current->pkt->seq))
    {
        new_node->next = buffer->head;
        buffer->head = new_node;
        buffer->tail = new_node;
    }
    else{
        while (current && ntohs(current->pkt->seq) < ntohs(pkt->seq))
        {
            previous = current;
            current = current->next;
        }
        previous->next = new_node;
        new_node->next = current;
        if(current == NULL){
            buffer->tail = new_node;
        }
    }
}

int remove_sent_packets(Buffer *buffer, int ack)
{
    BufferNode* previous = NULL;
    BufferNode* current = buffer->head;
    int removed = 0;
    while(current && ntohs(current->pkt->seq) < ack){
        previous = current;
        current = current->next;
        buffer->head = current;
        removed += ntohs(previous->pkt->length);
        free(previous->pkt);
        free(previous);
    }

    return removed;
}

int output_packet(Buffer *buffer, uint16_t ack, void (*output_p)(uint8_t *, size_t))
{
    BufferNode *previous = NULL;
    BufferNode *current = buffer->head;

    while (current && ntohs(current->pkt->seq) == ack){
        previous = current;
        current = current->next;
        buffer->head = current;
        output_p(previous->pkt->payload, ntohs(previous->pkt->length));
        free(previous->pkt);
        free(previous);
        ack += 1;
    }
    return ack;
}

void print_buffer(Buffer *buffer, const char *type)
{
    BufferNode *current = buffer->head;
    fprintf(stderr, "%s BUF", type);  
    while (current != NULL)
    {
        fprintf(stderr, " %d", ntohs(current->pkt->seq));  
        current = current->next;
    }
    fprintf(stderr, "\n");  
}



// Main function of transport layer; never quits
void listen_loop(int sockfd, struct sockaddr_in *addr, int type,
                     ssize_t (*input_p)(uint8_t *, size_t),
                     void (*output_p)(uint8_t *, size_t))
{

    char buffer[MAX_PAYLOAD];
    int connected = 0;
    uint16_t seq = 0;
    uint16_t ack = 0;
    uint16_t received_ack = 0;
    int ack_count = 0;
    srand(time(NULL) ^ type);

    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

    Buffer send_buffer;
    Buffer recv_buffer;
    init_buffer(&send_buffer);
    init_buffer(&recv_buffer);

    int sent_bytes = 0;
    int window_size = MIN_WINDOW;
    BufferNode *current_pkt = NULL;

    struct timeval timer;
    bool timer_running = false;

    while (true) {
        socklen_t addr_len = sizeof(struct sockaddr_in);

        char in_buf[sizeof(packet) + MAX_PAYLOAD] = {0};
        packet *in_pkt = (packet *)in_buf;

        int bytes_recvd = recvfrom(sockfd, in_pkt, sizeof(packet) + MAX_PAYLOAD, 0,
                                   (struct sockaddr *)addr, &addr_len);
        bool received_packet = false;

        if(bytes_recvd > 0){
            print_diag(in_pkt, RECV);
            if (compute_xor_checksum(in_pkt, bytes_recvd))
            {
                print("CORRUPT");
                continue;
            }
            window_size = ntohs(in_pkt->win);
            // Process received ACK
            if(in_pkt->flags &= ACK | SYN){
                uint16_t new_ack = ntohs(in_pkt->ack);
                if(received_ack == new_ack){
                    ack_count += 1;
                    // Received 3 duplicate ACKs, retransmit lowest sequence packet in send buffer
                    if (ack_count >= DUP_ACKS && send_buffer.head)
                    {
                        packet *retransmit_pkt = send_buffer.head->pkt;
                        send_packet(sockfd, addr, retransmit_pkt, DUPS);
                    }
                }
                else{
                    // Received new ACK, remove packets from send buffer and renew timer
                    sent_bytes -= remove_sent_packets(&send_buffer, ntohs(in_pkt->ack));
                    received_ack = new_ack;
                    ack_count = 0;
                    // No packets left in sending buffer, stop timer
                    gettimeofday(&timer, NULL);
                    if(send_buffer.head == NULL){
                        timer_running = false;
                    }
                }
            }
            // Add received packet to receiving buffer
            if(connected > 1){
                packet *pkt = (packet *)malloc(sizeof(packet) + ntohs(in_pkt->length));
                memcpy(pkt, in_pkt, sizeof(packet) + ntohs(in_pkt->length));

                ack = output_packet(&recv_buffer, ack, output_p);

                // Do not reACK a dedicated ACK packet
                if(ntohs(in_pkt->length) > 0){
                    if(ntohs(in_pkt->seq) >= ack){
                        add_packet(&recv_buffer, pkt);
                    }
                    if (current_pkt)
                    {
                        current_pkt->pkt->ack = ntohs(ack);
                    }
                    received_packet = true;
                }
            }
            print_buffer(&recv_buffer, "RECV");
        }
        if(type == CLIENT && connected < 2)
        {
            // Send SYN packet
            if(connected == 0){
                ssize_t input_len = input_p((uint8_t *)buffer, MAX_PAYLOAD);
                seq = (rand() % 1000 + 1);
                packet* syn_pkt = create_packet(seq, 0, SYN, MAX_WINDOW, buffer, input_len);
                add_packet(&send_buffer, syn_pkt);
                current_pkt = send_buffer.head;
                gettimeofday(&timer, NULL);
                timer_running = true;
                connected = 1;
                seq += 1;
            }
            // Send last ACK part of three way handshake
            else if (bytes_recvd > 0 && in_pkt->flags & SYN)
            {
                ssize_t input_len = input_p((uint8_t *)buffer, MAX_PAYLOAD);
                if (ntohs(in_pkt->length) > 0){
                    output_p(in_pkt->payload, ntohs(in_pkt->length));
                }
                ack = ntohs(in_pkt->seq) + 1;
                if (input_len == 0)
                {
                    seq = 0;
                }
                packet* ack_pkt = create_packet(seq, ack, ACK, MAX_WINDOW, buffer, input_len);
                add_packet(&send_buffer, ack_pkt);
                current_pkt = send_buffer.tail;
                gettimeofday(&timer, NULL);
                timer_running = true;
                connected = 2;
                seq += 1;
            }
        }
        else if (type == SERVER && connected < 2)
        {
            if(bytes_recvd > 0){
                // Send SYNACK back to client
                if (in_pkt->flags & SYN)
                {
                    ssize_t input_len = input_p((uint8_t *)buffer, MAX_PAYLOAD);

                    seq = (rand() % 1000 + 1);
                    ack = ntohs(in_pkt->seq) + 1;
                    if (ntohs(in_pkt->length) > 0)
                    {
                        output_p(in_pkt->payload, ntohs(in_pkt->length));
                    }
                    packet* synack_pkt = create_packet(seq, ack, SYN | ACK, MAX_WINDOW, buffer, input_len);
                    add_packet(&send_buffer, synack_pkt);
                    current_pkt = send_buffer.tail;
                    gettimeofday(&timer, NULL);
                    timer_running = true;
                    connected = 1;
                    seq += 1;
                }
                // Check if last ACK is part of three way handshake
                else if (connected == 1 && (ntohs(in_pkt->seq) == 0 || ntohs(in_pkt->seq) == ack))
                {
                    // print("Three-way Handshake Complete");
                    if (ntohs(in_pkt->length) > 0)
                    {
                        output_p(in_pkt->payload, ntohs(in_pkt->length));
                    }
                    ack = ntohs(in_pkt->seq) + 1;
                    connected = 2;
                }
            }
        }
        else
        {
            // Create packet from stdin data and add to send buffer
            ssize_t input_len = input_p((uint8_t *)buffer, MAX_PAYLOAD);

            if(input_len > 0){
                packet *pkt = create_packet(seq, ack, 0, MAX_WINDOW, buffer, input_len);
                add_packet(&send_buffer, pkt);
                if (current_pkt == NULL)
                {
                    current_pkt = send_buffer.tail;
                }
                seq += 1;
            }

        }
        struct timeval now;
        gettimeofday(&now, NULL);
        // Timer has expired, resend packet
        if (timer_running && TV_DIFF(now, timer) >= RTO && send_buffer.head){
            packet *retransmit_pkt = send_buffer.head->pkt;
            send_packet(sockfd, addr, retransmit_pkt, RTOS);
            // gettimeofday(&timer, NULL);
        }
        // Send queued packet in send buffer
        if (current_pkt != NULL && sent_bytes + ntohs(current_pkt->pkt->length) <= window_size)
        {
            if (received_packet)
            {
                current_pkt->pkt->flags |= ACK;
            }
            send_packet(sockfd, addr, current_pkt->pkt, SEND);
            sent_bytes += ntohs(current_pkt->pkt->length);
            current_pkt = current_pkt->next;
            // If timer was stopped, restart timer because of newly transmitted packet
            if(!timer_running){
                gettimeofday(&timer, NULL);
                timer_running = true;
            }
            print_buffer(&send_buffer, "SEND");
        }
        else if (received_packet)
        {
            // Send dedicated ACK packet
            packet* ack_pkt = create_packet(0, ack, ACK, MAX_WINDOW, NULL, 0);
            // print("Sending dedicated ack packet: ");
            send_packet(sockfd, addr, ack_pkt, SEND);
        }
    }
}
                