#include "ksocket.h"
/*
 * KTP Sender Application
 * Reliable transport protocol message sender
 * =====================================
 * Assignment 4 Submission
 * Name: Gopal Ji
 * Roll number: 22CS10026
 * =====================================
 */

int main() {
    // Initialize protocol
    Variable_Initialization();

    // Establish transport endpoint
    int ktp_fd = k_socket(1, 2, 3);
    printf("[KTP-SNDR] Transport socket created with ID: %d\n", ktp_fd);
    
    // Configure connection endpoints
    printf("Enter a number to proceed with binding (preventing conflicts):\n");
    int dummy_input;
    scanf("%d", &dummy_input);
    k_bind(ktp_fd, "127.0.0.1", 8001, "127.0.0.1", 8002);
    
    // Access source data
    FILE *input_file = fopen("send.txt", "r");
    if (!input_file) {
        fprintf(stderr, "[KTP-SNDR] ERROR: Source file not found\n");
        return 1;
    }
    
    // Transmission variables
    char message_buffer[500];
    int message_count = 0;
    
    printf("[KTP-SNDR] Beginning data transmission...\n");
    
    // Process input file line by line
    while (fgets(message_buffer, sizeof(message_buffer), input_file)) {
        // Prepare message
        size_t msg_len = strlen(message_buffer);
        
        // Ensure transmission with retry on failure
        int transmission_result;
        do {
            transmission_result = k_sendto(ktp_fd, message_buffer, msg_len);
            if (transmission_result == -1) {
                usleep(1000);  // 20ms backoff before retry
            }
        } while (transmission_result == -1);
        
        message_count++;
        printf("[KTP-SNDR] Message #%d transmitted (%zu bytes)\n", 
               message_count, msg_len);
               
        // Add brief delay between messages to prevent overloading
        usleep(250);  // 5ms pause
    }
    
    printf("[KTP-SNDR] File transmission complete - %d messages sent\n", message_count);
    fclose(input_file);
    
    // Send termination signal
    printf("[KTP-SNDR] Sending transmission completion marker\n");
    strcpy(message_buffer, "%");
    
    // Ensure termination marker is delivered
    while (k_sendto(ktp_fd, message_buffer, strlen(message_buffer)) == -1) {
        usleep(2500);  // 50ms delay between retries for final message
    }
    
    // Allow time for final acknowledgments
    sleep(2);
    
    // Clean exit
    printf("[KTP-SNDR] Enter any number to close connection: ");
    scanf("%d", &dummy_input);
    
    k_close(ktp_fd);
    printf("[KTP-SNDR] Connection closed successfully\n");
    
    return 0;
}