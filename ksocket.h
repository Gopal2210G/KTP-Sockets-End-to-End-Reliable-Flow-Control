/*
 * KGP Transport Protocol (KTP) - Header File
 * Reliable transport layer over UDP sockets
 * Implements flow control with sender/receiver windows
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
#include <time.h>
#include <semaphore.h>

/* Message types for KTP protocol */
#define MSG 1    // Data message
#define ACK 0    // Acknowledgment message

/* KTP configuration parameters */
#define SENDING_BUFFER 10  // Size of sender buffer (messages)
#define RECVING_BUFFER 10  // Size of receiver buffer (messages)
#define NUM_SOCKETS 25     // Maximum concurrent sockets
#define T 5                // Timeout in seconds
#define Prob 0.02              // Artificial packet drop probability
#define MX_MSGS 10         // Maximum messages in flight
#define MSG_SIZE 512       // Total message size in bytes

/**
 * KtpSocket - Core data structure for KTP protocol endpoints
 * Maintains both sender and receiver state
 */
typedef struct {
    int available;         // Socket allocation status (0=free, 1=allocated)
    pid_t process_id;      // Owner process ID
    int udp_socket_id;     // Underlying UDP socket descriptor
    struct sockaddr_in other_end_addr; // Remote endpoint address

    /* Message buffers (512 bytes per message) */
    char send_buffer[SENDING_BUFFER][MSG_SIZE];
    char recv_buffer[RECVING_BUFFER][MSG_SIZE];
    time_t last_sent_time[SENDING_BUFFER]; // Timestamps for retransmission

    /* Sender window state */
    struct {
        int size;          // Current window size
        int sequence_num[SENDING_BUFFER]; // Sequence numbers in buffer
        int start_seq_num; // First sequence number in window
        int next_seq_num;  // Next sequence number to use
        int start_window;  // Window start index
        int end_window;    // Window end index
        int acked[SENDING_BUFFER]; // Acknowledgment status (0=unacked, 1=acked)
    } swnd;

    /* Receiver window state */
    struct {
        int size;          // Number of messages in buffer
        int index_next;      // Next free index in buffer
        int next_expected_seq; // Next expected sequence number
        int read_seq_num;  // Next sequence number for app delivery
        int received[RECVING_BUFFER]; // Receipt status (0=empty, 1=received)
        int recv_seq_num[RECVING_BUFFER]; // Received message sequence numbers
    } rwnd;

} KtpSocket;

/**
 * SharedKtp - Container for all active KTP sockets
 * Stored in shared memory for access across processes
 */
typedef struct {
    KtpSocket sockets[NUM_SOCKETS];
} SharedKtp;

/**
 * KtpPacket - Network message format for KTP protocol
 * Header + payload, total 512 bytes
 */
typedef struct {
    int type;       // Message type (ACK=0, MSG=1)
    int seq_num;    // Sequence number
    int recv_buf_size; // Available buffer space at receiver (for flow control)
    char data[MSG_SIZE - sizeof(int)*3]; // Payload (size adjusted for header)
} KtpPacket;

/**
 * SocketInfo - Helper structure for socket creation
 */
typedef struct {
    int sock_id;    // Socket identifier
    char *IP;       // IP address
    int port;       // Port number
    int errno_val;  // Error code
} SocketInfo;

/* KTP API functions */
int k_socket(int domain, int type, int protocol);
void Variable_Initialization(void);
int k_bind(int sock_id, char *src_IP, int src_port, char *dst_IP, int dst_port);
int k_sendto(int sock_id, char *msg, size_t len);
int k_recvfrom(int sock_id, char *buf, size_t len);
int k_close(int sock_id);