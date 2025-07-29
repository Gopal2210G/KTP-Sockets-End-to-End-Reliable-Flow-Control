/*
 * KTP Receiver Application
 * Reliable transport protocol message receiver
 * =====================================
 * Assignment 4 Submission
 * Name: Gopal Ji
 * Roll number: 22CS10026
 * =====================================
 */
#include "ksocket.h"

int main(){
    // Protocol initialization
    Variable_Initialization();

    // Create KTP transport socket
    int ktp_fd = k_socket(1, 2, 3);
    printf("[KTP-RCVR] Transport socket created with ID: %d\n", ktp_fd);
    
    // Configure socket endpoints
    printf("Enter a number to proceed with binding (preventing conflicts):\n");
    int dummy_input;
    scanf("%d", &dummy_input);
    k_bind(ktp_fd, "127.0.0.1", 8002, "127.0.0.1", 8001);

    // Prepare output file
    FILE *output_file = fopen("recv.txt", "w");
    if(!output_file){
        fprintf(stderr, "[KTP-RCVR] ERROR: Cannot create output file\n");
        return 1;
    }

    // Data reception variables
    char message_buffer[500];
    int messages_received = 0;
    int end_of_transfer = 0;
    
    printf("[KTP-RCVR] Listening for incoming data...\n");
    
    // Reception loop
    while(!end_of_transfer){
        int bytes_read;
        
        // Poll for incoming messages with retry
        do {
            bytes_read = k_recvfrom(ktp_fd, message_buffer, sizeof(message_buffer));
            if(bytes_read == -1) {
                usleep(600);  // 10ms pause between retries
            }
        } while(bytes_read == -1);
        
        // Process received message
        message_buffer[bytes_read] = '\0';
        messages_received++;
        
        // Check for termination marker
        if(strchr(message_buffer, '%') != NULL){
            printf("[KTP-RCVR] Transfer complete marker detected\n");
            end_of_transfer = 1;
            
            // Write everything before the '%' character
            char *term_pos = strchr(message_buffer, '%');
            if(term_pos > message_buffer) {
                *term_pos = '\0'; // Terminate string at marker
                fprintf(output_file, "%s", message_buffer);
            }
        } else {
            // Write normal message to file and flush
            fprintf(output_file, "%s", message_buffer);
            fflush(output_file); // Force buffer write to disk
            printf("[KTP-RCVR] Message #%d processed (%d bytes)\n", 
                   messages_received, bytes_read);
        }
    }
    
    // Finalize file operations
    fclose(output_file);
    printf("[KTP-RCVR] Transfer complete - %d messages received\n", messages_received);
    
    // Cleanup
    int exit_input;
    printf("[KTP-RCVR] Enter any number to close connection: ");
    scanf("%d", &exit_input);
    
    k_close(ktp_fd);
    return 0;
}