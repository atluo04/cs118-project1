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

void init_buffer(Buffer *buffer)
{
    buffer->head = NULL;
    buffer->tail = NULL;
}

packet *create_packet(uint16_t seq, uint16_t ack, uint16_t flags, uint16_t win, uint8_t *payload, size_t payload_len)
{
    packet *pkt = (packet *)malloc(sizeof(packet) + payload_len);

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
        memset(pkt->payload, 0, MAX_PAYLOAD);
        pkt->length = 0;
    }
    if (compute_xor_checksum(pkt, sizeof(packet) + payload_len))
    {
        pkt->flags |= PARITY;
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
        print("Removed %d", ntohs(previous->pkt->seq));
        free(previous->pkt);
        free(previous);
    }

    return removed;
}

int output_packet(Buffer *buffer, int ack, void (*output_p)(uint8_t *, size_t))
{
    BufferNode *previous = NULL;
    BufferNode *current = buffer->head;
    // print("BUFFER IS FOLLOWING:");
    // print_buffer(buffer);
    while (current && ntohs(current->pkt->seq) == ack){
        // print("Current ACK: %d", ack);
        // print("Current Packet SEQ: %d", ntohs(current->pkt->seq));

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

void print_buffer(Buffer* buffer){
    BufferNode* current = buffer->head;
    while(current != NULL){
        print("%d", ntohs(current->pkt->seq));
        current = current->next;
    }
}

// Main function of transport layer; never quits
void listen_loop(int sockfd, struct sockaddr_in* addr, int type,
                 ssize_t (*input_p)(uint8_t*, size_t),
                 void (*output_p)(uint8_t*, size_t)) {
    char buffer[MAX_PAYLOAD];
    int connected = 0;
    int seq = 0;
    int ack = 0;
    srand(time(NULL) ^ type);

    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

    Buffer send_buffer;
    Buffer recv_buffer;
    init_buffer(&send_buffer);
    init_buffer(&recv_buffer);

    int sent_bytes = 0;
    int window_size = MIN_WINDOW;
    BufferNode *current_pkt = NULL;

    while (true) {
        socklen_t addr_len = sizeof(struct sockaddr_in);

        char in_buf[sizeof(packet) + MAX_PAYLOAD] = {0};
        packet *in_pkt = (packet *)in_buf;

        int bytes_recvd = recvfrom(sockfd, in_pkt, sizeof(packet) + MAX_PAYLOAD, 0,
                                   (struct sockaddr *)addr, &addr_len);

        if(bytes_recvd > 0){
            if (!compute_xor_checksum(in_pkt, bytes_recvd))
            {
                continue;
            }
            print_diag(in_pkt, RECV);
            window_size = ntohs(in_pkt->win);
            sent_bytes -= remove_sent_packets(&send_buffer, ntohs(in_pkt->ack));
            if(connected > 1){
                if (current_pkt)
                {
                    current_pkt->pkt->ack = ntohs(ack);
                    current_pkt->pkt->flags |= ACK;
                }
                packet *pkt = (packet *)malloc(sizeof(packet) + ntohs(in_pkt->length));
                memcpy(pkt, in_pkt, sizeof(packet) + ntohs(in_pkt->length));
                add_packet(&recv_buffer, pkt);
                ack = output_packet(&recv_buffer, ack, output_p);
            }
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
                if (input_len)
                {
                    seq = 0;
                }
                packet* ack_pkt = create_packet(seq, ack, ACK, MAX_WINDOW, buffer, input_len);
                add_packet(&send_buffer, ack_pkt);
                current_pkt = send_buffer.tail;
                connected = 2;
                seq += 1;
            }
        }
        else if (type == SERVER && connected < 2)
        {
            if(bytes_recvd > 0){
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
                    connected = 1;
                    seq += 1;
                }
                // Check if last ACK is part of three way handshake
                else if (connected == 1 && (ntohs(in_pkt->seq) == 0 || ntohs(in_pkt->seq) == ack))
                {
                    print("Three-way Handshake Complete");
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
            ssize_t input_len = input_p((uint8_t *)buffer, MAX_PAYLOAD);

            if(input_len > 0){
                packet *pkt = create_packet(seq, 0, 0, MAX_WINDOW, buffer, input_len);
                add_packet(&send_buffer, pkt);
                if (current_pkt == NULL)
                {
                    current_pkt = send_buffer.tail;
                }
                seq += 1;
            }

        }
        if (current_pkt != NULL && sent_bytes + ntohs(current_pkt->pkt->length) <= window_size)
        {
            // current_pkt->pkt->seq = htons(seq);
            print_diag(current_pkt->pkt, SEND);
            sendto(sockfd, current_pkt->pkt, sizeof(packet) + ntohs(current_pkt->pkt->length), 0, (struct sockaddr *)addr, sizeof(*addr));
            sent_bytes += ntohs(current_pkt->pkt->length);
            current_pkt = current_pkt->next;
        }
    }
}
                