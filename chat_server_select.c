/**
 * @file chat_server_select.c
 * @brief A single-process chat server that uses I/O multiplexing (select()) 
 * to handle multiple clients and forward (broadcast) messages between them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>     // Often contains the select definition
#include <sys/select.h> // Most systems define fd_set and macros here
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h> // For struct timeval (optional, but good practice)
#include <errno.h>

// Define some macros 
#define PORT "3491"
#define MAX_CLIENTS 10 // Maximum number of clients the server will manage
#define BUF_SIZE 256   // Maximum message length
#define BACKLOG 10 //How many pending connections queue will hold

// Global array to store the file descriptors of all connected clients
// A value of 0 indicates the slot is free.
int client_socket[MAX_CLIENTS]; 
//int max_sd = 0; // Highest file descriptor number (used by select)
int listener_sfd = -1; // Global variable that tracks the listener socket


void cleanup_and_exit() {
    printf("\n--- Graceful Shutdown Initiated ---\n");
    
    // 1. Close the listener socket first
    if (listener_sfd != -1) {
        close(listener_sfd);
        printf("Closed listener socket (FD %d).\n", listener_sfd);
    }

    // 2. Close all active client sockets to signal them to disconnect
    for (int i = 0; i < MAX_CLIENTS; i++) {
        int sd = client_socket[i];
        if (sd > 0) {
            close(sd);
            client_socket[i] = 0; // Mark as closed
            printf("Closed client socket (FD %d).\n", sd);
        }
    }
    
    printf("Cleanup complete. Exiting server process.\n");
    exit(0); 
}

// --- Signal Handler for Ctrl+C ---

/**
 * @brief Signal handler function for SIGINT (Ctrl+C).
 * Calls the cleanup function to shut down the server gracefully.
 * @param sig The signal number caught (always SIGINT, which is 2).
 */
void sigint_handler(int sig) {
    // Note: Calling non-reentrant functions like printf/close/exit in a signal handler 
    // is often discouraged, but it is common practice for immediate, fatal process cleanup
    // in simple server applications.
    cleanup_and_exit();
}

/**
 * @brief Get sockaddr, IPv4 or IPv6.
 */
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


int setup_listener() {

    int listen_fd;
    struct addrinfo hints, *servinfo, *p;
    int rv;
    int yes;

    memset(&hints, 0, sizeof hints);

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    // "Give me an address structure for a TCP server listening on port 3490"
    if(getaddrinfo(NULL, PORT, &hints, &servinfo) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 0;
    }

    p = servinfo;

    for(p = servinfo; p != NULL; p = p->ai_next) {
        
        if ((rv = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("listener socket creation issue");
            // If you can't create a socket, then move on to the next connection
            continue;
        }

        printf("Socket %d created\n", rv);
        // Once the socket is created, then we can set options
        if (setsockopt(rv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            perror("setsockopt");
            close(rv);
            // This is a FATAL, non-recoverable error for the program.
            return 0;
        }
        // Then bind the socket 
        if(bind(rv, p->ai_addr, p->ai_addrlen) == -1) {
            perror("bind");
            close(rv);
            continue;
        }
        // Once we have a socket that you can bind, then break out of the loop
        printf("Socket %d is binded\n", rv);
        break;
    }

    freeaddrinfo(servinfo);

    if(p == NULL) { // Couldn't find a single addrinfo from which we can create a socket
       perror("No addrinfo found");
       return 0;
    }

    if(listen(rv, BACKLOG) != 0) {
        perror("listen");
        return 0;
    }

    printf("Socket is now ready to listen\n");
    return rv;
}

// Function to handle broadcasting a received message to all other clients
void broadcast_message(int sender_fd, const char *message, size_t len) {
    printf("broadcast message called\n");
    ssize_t send_bytes;
    
    for(int i = 0; i < MAX_CLIENTS; i++) {
        int sfd = client_socket[i];
        printf("%s has %d bytes\n", message, len);
        if(sfd <= 0) continue; // Non active socket
        if(sfd == sender_fd) continue; //We don't broadcast message back to sender
        send_bytes = send(sfd, message, len, 0);
        printf("send_bytes is %d bytes\n", send_bytes);
        if(send_bytes == -1) {
            perror("send");
            close(sfd);
            client_socket[i] = 0;
            return;
        }
        if (send_bytes < len) {
            // WARNING: Partial send. Not all data was sent in one call.
            printf("[SEND WARNING] Sent only %zd of %zu bytes. You must loop to send remaining data.\n", send_bytes, len);
        } else {
            // SUCCESS: All data was sent.
            printf("[SENT SUCCESS] Message: '%s' (%zd bytes sent)\n", message, len);
        }

    }
}

int main() {
    //printf("[DIAGNOSTIC] Server execution started.\n");
    int running = 1;
    socklen_t sin_size;
    int afd = -1;
    fd_set readfds;
    int max_fd = 0; //Highest file descriptor + 1
    //int client_socket[MAX_CLIENTS]; // This list holds the client sockets that are attempting connection to the server
    int activity;
    char buffer[BUF_SIZE];
    ssize_t recv_bytes;

    // Clear out all the client sockets before proceeding further
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_socket[i] = 0;
    }

    //printf("Before running setup_listener\n");

    if((listener_sfd = setup_listener()) == 0) {
        printf("listener socket is %d\n", listener_sfd);
        perror("Couldn't set up a listening socket");
        exit(1);
    }

    max_fd = listener_sfd;
    // Infinite loop that allows the socket to listen forever
    while(running) {
        // 1. CLEAR THE SET
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        // 2. RE-POPULATE THE SET
        // Add the listener socket back
        FD_SET(listener_sfd, &readfds);

        for(int i = 0; i < MAX_CLIENTS; i++) {
            if(client_socket[i] > 0) {
                printf("Index %d is populated by socket %d\n", i, client_socket[i]);
                FD_SET(client_socket[i], &readfds);
            }
            if(client_socket[i] > max_fd) {
                max_fd = client_socket[i];
            }
        }
        // --- B. WAITING (select() call) ---
        // Blocks here until activity occurs on ANY monitored socket
        printf("\nWaiting for activity (max_fd + 1: %d)...\n", max_fd + 1);
        activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);

        if ((activity < 0) && (errno != EINTR)) {
            // A genuine, non-interrupt error occurred
            perror("select error");
            // Server should likely continue or attempt recovery, but for simplicity:
            continue; 
        }
        // --- C. EXECUTION (Handling the ready sockets) ---
        // 1. Check STDIN_FILENO (Server Command)
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char cmd_buffer[16];
            // Read 15 chars plus null terminator
            if (fgets(cmd_buffer, 16, stdin) != NULL) {
                if (strncmp(cmd_buffer, "quit", 4) == 0) {
                    printf("Server received 'quit' command. Shutting down...\n");
                    // Implement cleanup here: close listener_sd and all client FDs
                    // exit(0);
                } else {
                    printf("Command ignored.\n");
                }
            }
        }
        printf("About to accept client messages\n");
        // Now time for the listener socket to accept and accept client messages
        if(FD_ISSET(listener_sfd, &readfds)) {
            struct sockaddr_storage their_addr; // connector's address info
            sin_size = sizeof their_addr;
            char remote_ip[INET6_ADDRSTRLEN];

            printf("Now inside the accepting function\n");

            if( (afd = accept(listener_sfd, (struct sockaddr *)&their_addr, &sin_size)) == -1) {
                perror("Couldn't accept");
                continue;
            }
            // Successfully accepted, print client details
            inet_ntop(their_addr.ss_family, get_in_addr((struct sockaddr *)&their_addr), remote_ip, sizeof(remote_ip));
            printf("New connection accepted on socket %d from IP: %s\n", afd, remote_ip);

            
            //afd is the new client connection, loop through client sockets
            for(int i = 0; i < MAX_CLIENTS; i++) {
                int client_fd = client_socket[i];
                if(client_fd == 0) {
                    client_socket[i] = afd;
                    printf("Client assigned to array slot [%d]\n", i);
                    // Quickly update max_fd
                    if(client_socket[i] > max_fd) {
                        max_fd = client_socket[i];
                    }
                    break;
                }
            }
        }


        for(int i = 0; i < MAX_CLIENTS; i++) {
            int avail_cfd = client_socket[i];
            // This is an accepted client socket from which we can receive
            if(avail_cfd > 0 && FD_ISSET(avail_cfd, &readfds)) {
                char buffer_test[BUF_SIZE];
                recv_bytes = recv(avail_cfd, buffer_test, BUF_SIZE, 0);
                if (recv_bytes <= 0) {
                    // Client Disconnected/Error: Clean up sender_fd
                    close(avail_cfd);
                    client_socket[i] = 0;
                    continue; // Go to the next client slot
                }
                buffer_test[recv_bytes] = '\0'; // Null-terminate the received data
                printf("[RECV SUCCESS] Server says: '%s' (%d bytes received)\n", buffer_test, recv_bytes);
                // B. Send acknowledgement (optional but good practice)
                send(avail_cfd, "ACK", 3, 0);
                // C. BROADCAST to others (Go to Step 3)
                // Pass the actual sender's FD to the broadcast function
                broadcast_message(avail_cfd, buffer_test, recv_bytes);
            }
        }
    // End of infinite while loop
    }
    close(listener_sfd);

    for(int k = 0; k < MAX_CLIENTS; k++) {
        client_socket[k] = 0;
    }
    printf("Socket closed and program finished.\n");
    return 0;
}