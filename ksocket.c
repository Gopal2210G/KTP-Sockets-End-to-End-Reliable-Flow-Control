
/**
 * ksocket.c - Implementation of KGP Transport Protocol (KTP)
 * 
 * This file implements a reliable transport protocol built over UDP sockets
 * with flow control and reliable data transfer mechanisms.
 * 
 * KTP protocol features include:
 * - Socket creation with automatic resource allocation
 * - Connection establishment with source/destination addressing
 * - Reliable data transmission with sequence numbers
 * - Flow control using sender and receiver windows
 * - Automatic retransmission handled by daemon process 
 * 
 * Name: Gopal Ji, 22CS10026
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <fcntl.h>
#include "ksocket.h" // Updated header for KTP


// Constants for semaphore signaling
#define SEM1_SIGNAL 1
#define SEM2_SIGNAL 2


// Global synchronization semaphores
sem_t *Socket_sem, *Sem1, *Sem2, *update_sem;
//Socket_sem---> Protects access to socket structures
//Sem1---> Signals daemon process for socket creation
//Sem2---> Signals user process for socket creation
//update_sem---> Signals daemon process for metadata update


// Shared memory pointers for KTP protocol
SocketInfo *ktp_meta;
SharedKtp *ktp_sm;



// Function prototypes
sem_t *create_semaphore(const char *name, unsigned int initial_value)
{
    sem_t *sem = sem_open(name, O_CREAT, 0666, initial_value);
    if (sem == SEM_FAILED)
    {
        perror("sem_open failed");
        exit(EXIT_FAILURE);
    }
    return sem;
}




/**
 * Initialize protocol variables and shared memory
 * 
 * Must be called before any other KTP functions to establish
 * communication with the daemon process and set up shared resources
 */
void Variable_Initialization()
{
    // Create named semaphores for synchronization
    Sem1 = create_semaphore("/sem1", 0);
    Sem2 = create_semaphore("/sem2", 0);
    Socket_sem = create_semaphore("/semS", 1);
    update_sem = create_semaphore("/semV", 0);

    // Create shared memory for socket metadata
    key_t id1 = ftok("/home/", 'A');
    int shmid = shmget(id1, sizeof(ktp_meta), IPC_CREAT | 0666);
    ktp_meta = shmat(shmid, NULL, 0);

    // Create shared memory for socket structures
    key_t id2 = ftok("/home/", 'B');
    int shmid2 = shmget(id2, sizeof(SharedKtp), IPC_CREAT | 0666);
    ktp_sm = shmat(shmid2, NULL, 0);
}


/**
 * Create a new KTP socket
 * 
 * Requests a UDP socket from daemon process and initializes KTP structures.
 * The socket is not bound to any address until k_bind() is called.
 */

int k_socket(int domain, int type, int protocol)
{
    // Synchronization to prevent resource conflicts
    int user_input;
    printf("Enter any number to continue with socket setup (avoiding simultaneous creations):\n");
    scanf("%d", &user_input);

    // Request socket creation from daemon
    sem_post(Sem1);
    sem_wait(Sem2);
    
    // Get assigned UDP socket and reset metadata
    int udp_fd = ktp_meta->sock_id;
    ktp_meta->sock_id = 0;
    sem_post(update_sem);
    
    printf("UDP_SOCKET ID is %d\n", udp_fd);
    
    // Find available KTP socket slot
    int socket_index = -1;
    sem_wait(Socket_sem);
    
    for (int i = 0; i < NUM_SOCKETS; i++) {
        if (!ktp_sm->sockets[i].available) {
            socket_index = i;
            break;
        }
    }
    
    sem_post(Socket_sem);
    
    // Handle no available slots
    if (socket_index == -1) {
        errno = ENOBUFS;
        return -1;
    }
    
    // Initialize KTP socket data
    sem_wait(Socket_sem);
    
    KtpSocket *socket = &ktp_sm->sockets[socket_index];
    socket->available = 1;
    socket->udp_socket_id = udp_fd;
    socket->process_id = getpid();
    socket->swnd.size = 10;         // Initial sender window size
    socket->swnd.start_seq_num = 0; // Initial sequence number
    socket->swnd.next_seq_num = 0;  // Next sequence number to use
    socket->swnd.start_window = 0;  // Window start index
    socket->swnd.end_window = 0;    // Window end index
    
    sem_post(Socket_sem);
    
    return socket_index;
}


/**
 * Bind KTP socket to local address and set remote endpoint
 * 
 * This function configures both the local binding for the UDP socket
 * (via daemon) and the remote endpoint for communication.
 * 
 */
int k_bind(int ktp_socket, char *src_ip, int src_port, char *dest_ip, int dest_port)
{
    // Validation
    if (ktp_socket < 0 || ktp_socket >= NUM_SOCKETS) {
        errno = EINVAL;
        return -1;
    }
    
    // User synchronization
    int user_input;
    printf("Enter a number to proceed with binding (preventing conflicts):\n");
    scanf("%d", &user_input);
    
    // Get socket descriptor and set up binding
    int udp_fd = ktp_sm->sockets[ktp_socket].udp_socket_id;
    if (udp_fd < 0) {
        errno = EBADF;
        return -1;
    }
    
    // Request binding from daemon process
    ktp_meta->sock_id = udp_fd;
    ktp_meta->IP = src_ip;
    ktp_meta->port = src_port;
    
    sem_post(Sem1);
    sem_wait(Sem2);
    
    // Reset metadata after binding
    ktp_meta->sock_id = 0;
    sem_post(update_sem);
    
    // Setup destination address
    struct sockaddr_in *dest_addr = &ktp_sm->sockets[ktp_socket].other_end_addr;
    dest_addr->sin_addr.s_addr = inet_addr(dest_ip);
    dest_addr->sin_port = htons(dest_port);
    dest_addr->sin_family = AF_INET;
    
    return 0;
}


/**
 * Send data through a KTP socket
 * 
 * Implements reliable message transmission with sequence numbers.
 * Places message in send buffer and triggers transmission by the daemon.
 * 
 */
int k_sendto(int ktp_socket, char *message, size_t message_len)
{
    // Parameter validation
    if (ktp_socket < 0 || ktp_socket >= NUM_SOCKETS || 
        message == NULL || message_len <= 0 ||
        message_len > sizeof(((KtpPacket *)0)->data)) {
        errno = EINVAL;
        return -1;
    }

    KtpSocket *socket = NULL;
    sem_wait(Socket_sem);
    
    // Socket availability check
    socket = &ktp_sm->sockets[ktp_socket];
    if (!socket->available) {
        errno = EBADF;
        sem_post(Socket_sem);
        return -1;
    }
    
    // Check buffer space
    if ((socket->swnd.next_seq_num + 1) % MX_MSGS == socket->swnd.start_seq_num) {
        sem_post(Socket_sem);
        return -1;  // Buffer full
    }
    
    sem_post(Socket_sem);
    
    // Process message in chunks
    sem_wait(Socket_sem);
    
    int seq_index = socket->swnd.next_seq_num;
    int bytes_processed = 0;
    
    while (bytes_processed < message_len) {
        // Check for buffer space again (might have changed)
        if ((socket->swnd.next_seq_num + 1) % MX_MSGS == socket->swnd.start_seq_num) {
            sem_post(Socket_sem);
            return -1;  // No space available
        }
        
        size_t chunk_size;
        if (message_len - bytes_processed > 510) {
            chunk_size = 510;  // Max chunk size
        } else {
            chunk_size = message_len - bytes_processed;
        }
        
        // Copy chunk to send buffer
        strncpy(socket->send_buffer[seq_index % MX_MSGS], 
                message + bytes_processed, chunk_size);
        
        // Ensure null termination
        socket->send_buffer[seq_index % MX_MSGS][chunk_size] = '\0';
        
        // Mark as unacknowledged
        socket->swnd.acked[seq_index] = 0;
        
        // Update counters
        bytes_processed += chunk_size;
        seq_index = (seq_index + 1) % MX_MSGS;
        socket->swnd.next_seq_num = seq_index;
        
        // If we've copied the whole message, no need to continue
        if (bytes_processed >= message_len) {
            break;
        }
    }
    
    // Prepare packet for transmission
    KtpPacket packet;
    packet.type = MSG;
    packet.seq_num = seq_index;
    packet.recv_buf_size = RECVING_BUFFER - socket->rwnd.size;
    memcpy(packet.data, socket->send_buffer[seq_index % MX_MSGS], message_len);
    
    // Record send time for retransmission
    socket->last_sent_time[seq_index % MX_MSGS] = time(NULL);
    
    // Send the packet
    sendto(socket->udp_socket_id, &packet, sizeof(KtpPacket), 0, 
           (struct sockaddr*)&socket->other_end_addr, sizeof(struct sockaddr_in));
    
    sem_post(Socket_sem);
    
    return 1;  // Success
}


/**
 * Receive data from a KTP socket
 * 
 * Retrieves data from the receive buffer with in-order delivery.
 * If no data is available, returns -1 with errno=EAGAIN. * 
 */
int k_recvfrom(int ktp_socket, char *buffer, size_t buffer_len)
{
    // Parameter validation
    if (ktp_socket < 0 || ktp_socket >= NUM_SOCKETS || 
        buffer == NULL || buffer_len == 0) {
        errno = EINVAL;
        return -1;
    }
    
    KtpSocket *socket = NULL;
    
    // Check socket availability
    sem_wait(Socket_sem);
    socket = &ktp_sm->sockets[ktp_socket];
    
    if (!socket->available) {
        errno = EBADF;
        sem_post(Socket_sem);
        return -1;
    }
    
    // Check if data is available
    if (socket->rwnd.received[0] == 0) {
        errno = EAGAIN;  // No message available
        sem_post(Socket_sem);
        return -1;
    }
    
    // Copy data to user buffer
    strncpy(buffer, socket->recv_buffer[0], buffer_len);
    buffer[buffer_len - 1] = '\0';  // Ensure null-termination
    
    // Shift the receiver buffer contents
    for (int i = 1; i < RECVING_BUFFER; i++) {
        strncpy(socket->recv_buffer[i - 1],
                socket->recv_buffer[i],
                sizeof(socket->recv_buffer[i - 1]) - 1);
                
        socket->recv_buffer[i - 1][511] = '\0';  // Ensure null-termination
        socket->rwnd.received[i - 1] = socket->rwnd.received[i];
    }
    
    // Clear the last slot
    socket->recv_buffer[socket->rwnd.size - 1][0] = '\0';
    socket->rwnd.received[socket->rwnd.size - 1] = 0;
    
    // Update indices
    socket->rwnd.index_next--;
    
    if (socket->rwnd.size > 0) {
        socket->rwnd.size--;
    }
    
    sem_post(Socket_sem);
    
    return strlen(buffer);
}




/**
 * Close a KTP socket and release resources
 * 
 * Marks the socket as available for reuse.
 * Note: Underlying UDP socket is managed by the daemon process.
 * 
 */
int k_close(int ktp_socket)
{
    // Validate socket descriptor
    if (ktp_socket < 0 || ktp_socket >= NUM_SOCKETS) {
        errno = EINVAL;
        return -1;
    }
    
    sem_wait(Socket_sem);
    
    // Check if socket is valid
    KtpSocket *socket = &ktp_sm->sockets[ktp_socket];
    if (!socket->available) {
        errno = EBADF;
        sem_post(Socket_sem);
        return -1;
    }
    
    // Mark socket as free
    socket->available = 0;
    
    sem_post(Socket_sem);
    
    return 0;
}

