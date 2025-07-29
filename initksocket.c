/**
 * initksocket.c - KGP Transport Protocol (KTP) Implementation
 *
 * This file implements the daemon process for the KTP protocol which:
 * - Services socket creation and binding requests from user processes
 * - Handles reliable packet delivery with acknowledgment
 * - Manages retransmissions of lost packets
 * - Implements flow control using sliding window mechanism
 * - Cleans up abandoned sockets
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ksocket.h"

#define SEM1_SIGNAL 1
#define SEM2_SIGNAL 2

sem_t *Socket_sem, *Sem1, *Sem2, *sem3, *update_sem;
int total = 0; // Counter for message attempts
SharedKtp *ktp_sm;
SocketInfo *ktp_meta;

int drop_message();

/**
 * Receiver Thread
 *
 * Monitors all UDP sockets for incoming packets using select().
 * Processes incoming messages (data and acknowledgments) and
 * updates protocol state accordingly:
 * - For data packets: checks sequence numbers, buffers in-order messages
 * - For ACK packets: updates sliding window and acknowledged status
 */
void *R_thread(void *arg)
{
    fd_set read_fds;      // Set of file descriptors to monitor
    struct timeval timer; // Timeout for select()
    int max_fd = 0;       // Highest socket descriptor (for select)
    char buffer[512];     // Buffer for messages (512 byte packets)

    while (1)
    {
        // Reset file descriptor set for select()
        FD_ZERO(&read_fds);
        max_fd = 0;
        struct sockaddr_in sender_addr;
        socklen_t snd_addr_len = sizeof(sender_addr);

        // Add all active sockets to the monitoring set
        sem_wait(Socket_sem);
        int i = 0;
        while (i < NUM_SOCKETS)
        {
            if (ktp_sm->sockets[i].available)
            {
                printf("[RECV] Socket %d: Active (fd=%d, pid=%d)\n",
                       i, ktp_sm->sockets[i].udp_socket_id, ktp_sm->sockets[i].process_id);
                FD_SET(ktp_sm->sockets[i].udp_socket_id, &read_fds);
                if (ktp_sm->sockets[i].udp_socket_id > max_fd)
                {
                    max_fd = ktp_sm->sockets[i].udp_socket_id;
                }
            }
            i++;
        }
        sem_post(Socket_sem);

        // Wait for data or timeout
        timer.tv_sec = T;
        timer.tv_usec = 0;
        int retval = select(max_fd + 1, &read_fds, NULL, NULL, &timer);
        if (retval == 0)
            continue; // Timeout, no data available
        else
        {
            // Check each socket for pending data
            i = 0;
            while (i < NUM_SOCKETS)
            {
                if (ktp_sm->sockets[i].available && FD_ISSET(ktp_sm->sockets[i].udp_socket_id, &read_fds))
                {
                    KtpPacket packet;
                    int udp_socket = ktp_sm->sockets[i].udp_socket_id;
                    ssize_t recv_len = recvfrom(udp_socket, &packet, sizeof(packet), 0,
                                                (struct sockaddr *)&sender_addr, &snd_addr_len);
                    if (recv_len < 0)
                    {
                        perror("[ERROR] recvfrom failed");
                        i++;
                        continue;
                    }

                    total++; // Count received messages for statistics

                    // Simulate random packet loss for testing reliability
                    if (drop_message() == 1)
                    {
                        printf("[DROP] %s packet dropped: seq=%d, expected=%d\n",
                               packet.type == MSG ? "DATA" : "ACK",
                               packet.seq_num,
                               packet.type == MSG ? ktp_sm->sockets[i].rwnd.next_expected_seq : -1);
                        i++;
                        continue;
                    }

                    // Handle data message (MSG) packets
                    if (packet.type == MSG)
                    {
                        printf("[DATA] Received seq=%d, expected=%d | window=%d-%d | content='%.20s%s'\n",
                               packet.seq_num,
                               ktp_sm->sockets[i].rwnd.next_expected_seq,
                               ktp_sm->sockets[i].rwnd.next_expected_seq,
                               (ktp_sm->sockets[i].rwnd.next_expected_seq + ktp_sm->sockets[i].rwnd.size - 1) % MX_MSGS,
                               packet.data,
                               strlen(packet.data) > 20 ? "...'" : "'");

                        // Check if this is the next expected sequence number (in-order delivery)
                        if (packet.seq_num == ktp_sm->sockets[i].rwnd.next_expected_seq)
                        {
                            // In-order message processing
                            printf("[IN-ORDER] Processing packet seq=%d (socket=%d)\n",
                                   packet.seq_num, i);
                            sem_wait(Socket_sem);

                            // Add to receive buffer at the next available position
                            strncpy(ktp_sm->sockets[i].recv_buffer[ktp_sm->sockets[i].rwnd.index_next], packet.data, 512);
                            ktp_sm->sockets[i].rwnd.received[ktp_sm->sockets[i].rwnd.index_next] = 1;
                            ktp_sm->sockets[i].rwnd.index_next++;
                            ktp_sm->sockets[i].rwnd.size++;
                            ktp_sm->sockets[i].rwnd.next_expected_seq = (ktp_sm->sockets[i].rwnd.next_expected_seq + 1) % MX_MSGS;

                            sem_post(Socket_sem);

                            // Send ACK for the received message
                            KtpPacket ack_pack;
                            ack_pack.type = ACK;
                            ack_pack.recv_buf_size = RECVING_BUFFER - ktp_sm->sockets[i].rwnd.size;
                            ack_pack.seq_num = (ktp_sm->sockets[i].rwnd.next_expected_seq - 1 + MX_MSGS) % MX_MSGS;
                            sendto(ktp_sm->sockets[i].udp_socket_id, &ack_pack, sizeof(ack_pack), 0,
                                   (struct sockaddr *)&sender_addr, snd_addr_len);
                            printf("[ACK-SENT] For seq=%d, window=%d\n",
                                   ack_pack.seq_num, ack_pack.recv_buf_size);
                        }
                        else
                        {
                            // Out-of-order message handling - store but don't ACK
                            printf("[OUT-OF-ORDER] seq=%d < expected=%d, gap=%d\n",
                                   packet.seq_num,
                                   ktp_sm->sockets[i].rwnd.next_expected_seq,
                                   (ktp_sm->sockets[i].rwnd.next_expected_seq - packet.seq_num + MX_MSGS) % MX_MSGS);
                            sem_wait(Socket_sem);

                            // Calculate buffer position for this sequence number
                            int offset = (packet.seq_num - ktp_sm->sockets[i].rwnd.next_expected_seq + MX_MSGS) % MX_MSGS;
                            int store_index = (ktp_sm->sockets[i].rwnd.index_next + offset) % RECVING_BUFFER;

                            // Store the message if we haven't already
                            if (!ktp_sm->sockets[i].rwnd.received[store_index])
                            {
                                strncpy(ktp_sm->sockets[i].recv_buffer[store_index], packet.data, 512);
                                ktp_sm->sockets[i].rwnd.received[store_index] = 1;
                                printf("[BUFFERED] Out-of-order seq=%d at buffer[%d]\n\n",
                                       packet.seq_num, store_index);
                            }
                            else
                            {
                                printf("[DUPLICATE] Packet seq=%d already stored\n",
                                       packet.seq_num);
                            }
                            sem_post(Socket_sem);
                            // No ACK is sent for out-of-order messages in KTP.
                        }
                    }
                    else
                    { // Handle acknowledgment (ACK) packets
                        int ack_seq_num = packet.seq_num;
                        sem_wait(Socket_sem);
                        printf("[ACK-RECV] seq=%d, remote buffer=%d | window: %d-%d\n",
                               ack_seq_num,
                               packet.recv_buf_size,
                               ktp_sm->sockets[i].swnd.start_seq_num,
                               (ktp_sm->sockets[i].swnd.next_seq_num - 1 + MX_MSGS) % MX_MSGS);

                        // Look for the acknowledged sequence number in our window
                        int found = 0;
                        int curr_seq_num = ktp_sm->sockets[i].swnd.start_seq_num;
                        int j = 0;
                        while (j < MX_MSGS)
                        {
                            int seq_ind = (curr_seq_num + j) % MX_MSGS;
                            if (seq_ind == ktp_sm->sockets[i].swnd.next_seq_num)
                                break; // Reached end of current window
                            if (seq_ind == ack_seq_num)
                            {
                                found = 1;
                                break;
                            }
                            j++;
                        }

                        // Update window size based on receiver's available buffer
                        ktp_sm->sockets[i].swnd.size = packet.recv_buf_size;

                        // If acknowledged sequence number is valid, update window
                        if (found == 1)
                        {
                            // Calculate window endpoint
                            int wd = ktp_sm->sockets[i].swnd.end_window;
                            if (wd < ktp_sm->sockets[i].swnd.start_seq_num)
                                wd += MX_MSGS;

                            // Mark all packets up to and including ACK'd packet as received
                            j = ktp_sm->sockets[i].swnd.start_seq_num;
                            while (j < wd)
                            {
                                ktp_sm->sockets[i].swnd.acked[j] = 1;
                                ktp_sm->sockets[i].swnd.start_seq_num = (ktp_sm->sockets[i].swnd.start_seq_num + 1) % MX_MSGS;
                                ktp_sm->sockets[i].swnd.start_window = (ktp_sm->sockets[i].swnd.start_window + 1) % MX_MSGS;
                                printf("[CONFIRMED] Packet seq=%d marked as received\n\n", j);
                                if (j == ack_seq_num)
                                    break;
                                j = (j + 1) % MX_MSGS;
                            }
                        }
                        else
                        {
                            printf("[WARNING] ACK seq=%d outside current window %d-%d\n",
                                   ack_seq_num,
                                   ktp_sm->sockets[i].swnd.start_seq_num,
                                   (ktp_sm->sockets[i].swnd.next_seq_num - 1 + MX_MSGS) % MX_MSGS);
                        }
                        sem_post(Socket_sem);
                    }
                }
                i++;
            }
        }
    }
    return NULL;
}

/**
 * Sender Thread
 *
 * Responsible for:
 * 1. Retransmitting unacknowledged packets after timeout
 * 2. Sending new packets from the window as space becomes available
 * 3. Managing the sliding window
 */
void *S_thread(void *arg)
{
    while (1)
    {
        sleep(T / 2); // Check half as frequently as timeout period
        sem_wait(Socket_sem);
        int i = 0;
        while (i < NUM_SOCKETS)
        {
            KtpSocket *socket_info = &ktp_sm->sockets[i];
            if (ktp_sm->sockets[i].available)
            {
                time_t curr_t;
                time(&curr_t);

                // First, scan window for packets that need retransmission
                int j = socket_info->swnd.start_window;
                while (j != socket_info->swnd.end_window)
                {
                    if (socket_info->swnd.acked[j] == 0)
                    { // Not yet acknowledged
                        double time_since_last_sent = difftime(curr_t, socket_info->last_sent_time[j]);
                        if (time_since_last_sent >= T)
                        {
                            // Timeout occurred - retransmit the packet
                            KtpPacket packet = {.type = MSG, .seq_num = j};
                            strncpy(packet.data, socket_info->send_buffer[j % MX_MSGS], sizeof(packet.data));
                            printf("[RETRY] seq=%d, age=%.1fs | content='%.20s%s'\n",
                                   packet.seq_num,
                                   time_since_last_sent,
                                   packet.data,
                                   strlen(packet.data) > 20 ? "...'" : "'");
                            sendto(socket_info->udp_socket_id, &packet, sizeof(packet), 0,
                                   (struct sockaddr *)&(socket_info->other_end_addr), sizeof(struct sockaddr_in));
                            time(&(socket_info->last_sent_time[j]));
                        }
                    }
                    j = (j + 1) % SENDING_BUFFER;
                }

                // Calculate available space in the window
                int window_space = (socket_info->swnd.end_window + SENDING_BUFFER - socket_info->swnd.start_window) % SENDING_BUFFER;
                if (window_space < socket_info->swnd.size)
                {
                    // Send new messages that fit in the window
                    int new_msg_to_send = socket_info->swnd.size - window_space;
                    printf("[WINDOW] Socket %d: size=%d, space=%d, new_msgs=%d\n",
                           i, socket_info->swnd.size, window_space, new_msg_to_send);

                    int k = 0;
                    while (k < new_msg_to_send)
                    {
                        // Check if new data is available to send
                        if (socket_info->swnd.next_seq_num != (socket_info->swnd.end_window + SENDING_BUFFER) % SENDING_BUFFER)
                        {
                            // Prepare and send new packet
                            KtpPacket packet = {.type = MSG, .seq_num = socket_info->swnd.end_window};
                            strncpy(packet.data, socket_info->send_buffer[socket_info->swnd.end_window], sizeof(packet.data));
                            printf("[SEND] New packet seq=%d | content='%.20s%s'\n",
                                   packet.seq_num,
                                   packet.data,
                                   strlen(packet.data) > 20 ? "...'" : "'");
                            sendto(socket_info->udp_socket_id, &packet, sizeof(packet), 0,
                                   (struct sockaddr *)&(socket_info->other_end_addr), sizeof(struct sockaddr_in));
                            time(&(socket_info->last_sent_time[socket_info->swnd.end_window]));
                            socket_info->swnd.end_window = (socket_info->swnd.end_window + 1) % SENDING_BUFFER;
                        }
                        else
                        {
                            printf("[BUFFER-EMPTY] No more data to send for socket %d\n\n", i);
                            break; // No more data to send
                        }
                        k++;
                    }
                }
            }
            i++;
        }
        sem_post(Socket_sem);
    }
    return NULL;
}

/**
 * Garbage Collection Thread
 *
 * Periodically checks for abandoned sockets by verifying process existence.
 * If a process no longer exists but its socket is marked as available,
 * the socket is cleaned up and released.
 */
void *G_thread(void *arg)
{
    while (1)
    {
        sleep(T);             // Check every T seconds (defined in ksocket.h)
        sem_wait(Socket_sem); // Lock socket structures
        int i = 0;
        while (i < NUM_SOCKETS)
        {
            // Check if socket is allocated and has a process assigned
            if (ktp_sm->sockets[i].available && ktp_sm->sockets[i].process_id != 0)
            {
                // Test if process still exists by sending null signal
                int result = kill(ktp_sm->sockets[i].process_id, 0);
                if (result == -1 && errno == ESRCH)
                {
                    // Process doesn't exist - clean up the socket
                    ktp_sm->sockets[i].available = 0;
                    close(ktp_sm->sockets[i].udp_socket_id);
                    ktp_sm->sockets[i].udp_socket_id = -1;
                    printf("[CLEANUP] Socket %d released (pid %d terminated)\n",
                           i, ktp_sm->sockets[i].process_id);
                }
            }
            i++;
        }
        sem_post(Socket_sem); // Release socket structures
    }
    return NULL;
}

/**
 * Initialize shared resources
 */
void init_shared_resources()
{
    // Initialize semaphores for KTP
    Sem1 = sem_open("/sem1", O_CREAT | O_EXCL, 0777, 0);
    if (Sem1 == SEM_FAILED)
    {
        perror("[ERROR] sem_open Sem1 failed");
        sem_unlink("/sem1");
        Sem1 = sem_open("/sem1", O_CREAT | O_EXCL, 0777, 0);
    }
    Sem2 = sem_open("/sem2", O_CREAT | O_EXCL, 0777, 0);
    if (Sem2 == SEM_FAILED)
    {
        perror("[ERROR] sem_open Sem2 failed");
        sem_unlink("/sem2");
        Sem2 = sem_open("/sem2", O_CREAT | O_EXCL, 0777, 0);
    }
    Socket_sem = sem_open("/semS", O_CREAT | O_EXCL, 0777, 1);
    if (Socket_sem == SEM_FAILED)
    {
        perror("[ERROR] sem_open Socket_sem failed");
        sem_unlink("/semS");
        Socket_sem = sem_open("/semS", O_CREAT | O_EXCL, 0777, 1);
    }
    update_sem = sem_open("/semV", O_CREAT | O_EXCL, 0777, 0);
    if (update_sem == SEM_FAILED)
    {
        perror("[ERROR] sem_open update_sem failed");
        sem_unlink("/semV");
        update_sem = sem_open("/semV", O_CREAT | O_EXCL, 0777, 0);
    }

    // Create shared memory for socket metadata
    key_t id1 = ftok("/home/", 'A');
    int shmid = shmget(id1, sizeof(ktp_meta), IPC_CREAT | 0666);
    ktp_meta = shmat(shmid, NULL, 0);
    ktp_meta->sock_id = 0;

    // Create shared memory for socket structures
    key_t id2 = ftok("/home/", 'B');
    int shmid2 = shmget(id2, sizeof(SharedKtp), IPC_CREAT | 0666);
    ktp_sm = shmat(shmid2, NULL, 0);

    // Initialize all socket slots as free
    printf("[INIT] Initializing %d socket slots\n", NUM_SOCKETS);
    int i = 0;
    while (i < NUM_SOCKETS)
    {
        ktp_sm->sockets[i].available = 0;
        ktp_sm->sockets[i].udp_socket_id = -1;
        ktp_sm->sockets[i].process_id = 0;
        ktp_sm->sockets[i].swnd.start_seq_num = 0;
        ktp_sm->sockets[i].swnd.next_seq_num = 0;
        ktp_sm->sockets[i].rwnd.read_seq_num = 0;
        ktp_sm->sockets[i].rwnd.next_expected_seq = 0;
        int j = 0;
        while (j < RECVING_BUFFER)
        {
            ktp_sm->sockets[i].rwnd.received[j] = 0;
            j++;
        }
        i++;
    }
    printf("[INIT] Socket initialization complete\n");
}

/**
 * Simulates random packet loss for protocol testing
 */
int drop_message()
{
    float randomf = (float)rand() / (float)RAND_MAX;
    return randomf < Prob; // Drop message if random number < probability
}

/**
 * Clean up resources on exit
 */
void cleanup_resources()
{
    printf("[STATS] Total packets processed: %d, Loss probability: %.1f%%\n",
           total, Prob * 100);

    // Detach shared memory segments
    if (ktp_meta != (void *)-1)
    {
        shmdt(ktp_meta);
    }
    if (ktp_sm != (void *)-1)
    {
        shmdt(ktp_sm);
    }

    // Close and unlink named semaphores
    if (Sem1 != SEM_FAILED)
    {
        sem_close(Sem1);
        sem_unlink("/sem1");
    }
    if (Sem2 != SEM_FAILED)
    {
        sem_close(Sem2);
        sem_unlink("/sem2");
    }
    if (Socket_sem != SEM_FAILED)
    {
        sem_close(Socket_sem);
        sem_unlink("/semS");
    }
    if (update_sem != SEM_FAILED)
    {
        sem_close(update_sem);
        sem_unlink("/semV");
    }
    printf("[EXIT] KTP daemon shutdown complete\n");
    exit(0);
}

/**
 * Main daemon process
 */
int main()
{
    // Initialize random number generator for packet drop simulation
    srand(time(NULL));

    // Set up signal handler for clean termination
    signal(SIGINT, cleanup_resources);

    // Set up shared resources
    init_shared_resources();

    // Create worker threads
    pthread_t thread_R, thread_S, thread_G;
    pthread_create(&thread_R, NULL, R_thread, (void *)ktp_sm); // Receiver thread
    pthread_create(&thread_S, NULL, S_thread, (void *)ktp_sm); // Sender thread
    pthread_create(&thread_G, NULL, G_thread, (void *)ktp_sm); // Garbage collector

    printf("[READY] KTP daemon initialized (packet loss: %.1f%%)\n", Prob * 100);

    // Main service loop - handle socket operations requested by clients
    while (1)
    {
        printf("[MAIN] Waiting for client requests\n");
        sem_wait(Sem1); // Wait for client request
        printf("[MAIN] Processing client request\n");

        if (ktp_meta->sock_id == 0)
        {
            // Request for socket creation
            ktp_meta->sock_id = socket(AF_INET, SOCK_DGRAM, 0);
            if (ktp_meta->sock_id == -1)
            {
                ktp_meta->errno_val = errno;
                printf("[ERROR] Socket creation failed: %s\n", strerror(errno));
            }
            else
            {
                printf("[SOCKET] Created UDP socket fd=%d\n", ktp_meta->sock_id);
            }
        }
        else if (ktp_meta->sock_id != 0)
        {
            // Request for socket binding
            printf("[BIND] Binding socket %d to port %d\n",
                   ktp_meta->sock_id, ktp_meta->port);
            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port = htons(ktp_meta->port);
            addr.sin_addr.s_addr = INADDR_ANY;
            int bind_result = bind(ktp_meta->sock_id, (struct sockaddr *)&addr, sizeof(addr));
            if (bind_result == -1)
            {
                printf("[ERROR] Bind failed: %s\n", strerror(errno));
                ktp_meta->errno_val = errno;
                ktp_meta->sock_id = -1; // Reset sock_id on failure
            }
            else
            {
                printf("[BIND] Success: socket %d bound to port %d\n",
                       ktp_meta->sock_id, ktp_meta->port);
            }
        }

        // Signal completion to client
        sem_post(Sem2);
        printf("[MAIN] Response sent to client\n");

        // Wait for client to acknowledge receipt
        sem_wait(update_sem);
    }

    // Clean up (normally unreachable - handled by signal handler)
    pthread_join(thread_R, NULL);
    pthread_join(thread_S, NULL);

    free(ktp_sm);
    free(ktp_meta);
    sem_destroy(Socket_sem);
    sem_destroy(Sem1);
    sem_destroy(Sem2);
    return 0;
}