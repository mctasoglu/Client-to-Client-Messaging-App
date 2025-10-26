// Craft a client that connects to a server and sends a message

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>

#define PORT "3491"
#define HOST "127.0.0.1"
#define MAX_MESSAGE_LENGTH 256
#define MESSAGE_PROMPT "Type Message > "

// A flag to control the main while loop. It's declared 'volatile' 
// because it is modified asynchronously by the signal handler.
volatile int running = 1;

/**
 * @brief Signal handler for SIGINT (Ctrl+C). 
 * This function is called when the user presses Ctrl+C.
 * It changes the 'running' flag to 0 to stop the main loop gracefully.
 */
void sigint_handler(int sig) {
    printf("\n\n[INFO] SIGINT (Ctrl+C) received. Exiting loop...\n");
    running = 0; 
    exit(0);

}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main() {

    // Definitions for socket creation
    struct addrinfo hints, *res, *p;
    int activity;
    size_t numbytes;
    size_t bytes_received;
	int status;
	char ipstr[INET6_ADDRSTRLEN];
    int sockfd, cfd = -1;
    int max_fd;
    fd_set readfds;

    if(signal(SIGINT, sigint_handler) == SIG_ERR) {
        perror("Could not set up SIGINT handler");
        return EXIT_FAILURE;
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    

    
    char message_buffer[MAX_MESSAGE_LENGTH];


    // Now we got the message stored message_buffer, time to create sockets

    if((status = getaddrinfo(HOST, PORT, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
		return 2;
    }

    p = res;
    while(p != NULL) {
        void *addr;
        char *ipver;
        struct sockaddr_in *ipv4;
        struct sockaddr_in6 *ipv6;

        // get the pointer to the address itself,
        // different fields in IPv4 and IPv6:
        if(p->ai_family == AF_INET) {
            ipv4 = (struct sockaddr_in *)p->ai_addr;
            addr = &(ipv4->sin_addr);
            ipver = "IPv4";
        } else {
            ipv6 = (struct sockaddr_in6 *)p->ai_addr;
            addr = &(ipv6->sin6_addr);
            ipver = "IPv6";
        }

        // Now that we got the address, we want to create a socket and connect to it
        sockfd = socket(p->ai_family, p->ai_socktype, 0);

        if(sockfd  == -1) {
            perror("socket");
            p = p->ai_next;
            continue;
        }

        printf("Socket created: %d\n", sockfd);

        inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), ipstr, sizeof ipstr);
        printf("client: attempting connection to %s\n", ipstr);
        // As we loop through addrinfo structure, we want to create a socket, and a connection for each successful socket
        // As soon as we find the first successful connection, we break out of the loop
        cfd = connect(sockfd, p->ai_addr, p->ai_addrlen);

        if(cfd == -1) {
            perror("connect");
            close(sockfd);
            sockfd = -1;
            p = p->ai_next;
            continue;
        }

        break;
    }
    printf("Successfully connected: %d\n", cfd);

    freeaddrinfo(res);

    if(p == NULL) {
        fprintf(stderr, "client: failed to connect\n");
        return 2;
    }

    // Check if the loop finished without connecting (s is still -1)
    if (sockfd == -1) {
        fprintf(stderr, "ERROR: Failed to connect to any address.\n");
        return 3;
    }

    printf("--- Interactive Input Console ---\n");
    printf("Press Ctrl+C at any time to quit.\n\n");
    max_fd = sockfd;

    // 2. The core indefinite loop. It runs as long as the 'running' flag is 1.
    while (running) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        // 2. RE-POPULATE THE SET
        // Add the listener socket back
        FD_SET(sockfd, &readfds);

        printf("%s", MESSAGE_PROMPT);
        fflush(stdout); // Ensures the prompt appears immediately
        



        // --- B. WAITING (select() call) ---
        // Blocks here until activity occurs on ANY monitored socket
        //printf("\nWaiting for activity (max_fd + 1: %d)...\n", max_fd + 1);
        activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);

        if ((activity < 0) && (errno != EINTR)) {
            // A genuine, non-interrupt error occurred
            perror("select error");
            // Server should likely continue or attempt recovery, but for simplicity:
            continue; 
        }        

        if(FD_ISSET(STDIN_FILENO, &readfds)) {

            // 3. Safely read input from the keyboard (stdin).
            if (fgets(message_buffer, MAX_MESSAGE_LENGTH, stdin) == NULL) {
                // Handle stream closure or error if it happens unexpectedly
                if (running) {
                    printf("\n[ERROR] Input stream closed. Exiting.\n");
                }
                break; 
            }

            // 4. Clean up the input string by removing the newline character.
            size_t len = strlen(message_buffer);
            size_t bytes_to_send = len + 1;
            if (len > 0 && message_buffer[len - 1] == '\n') {
                message_buffer[len - 1] = '\0';
            }

            // ... send and receive data here ...
            if( (numbytes = send(sockfd, message_buffer, strlen(message_buffer), 0)) == -1) {
                perror("send");
            }

            else if (numbytes < bytes_to_send) {
                // WARNING: Partial send. Not all data was sent in one call.
                printf("[SEND WARNING] Sent only %zd of %zu bytes. You must loop to send remaining data.\n", numbytes, bytes_to_send);
            } else {
                // SUCCESS: All data was sent.
                printf("[SENT SUCCESS] Message: '%s' (%zd bytes sent)\n", message_buffer, numbytes);
            }
        }

        if(FD_ISSET(sockfd, &readfds)) {
            // --- START OF NEW RECV() LOGIC ---
            char recv_buffer[MAX_MESSAGE_LENGTH]; // Buffer for receiving data
            int bytes_received;

            printf("[INFO] Waiting for server response...\n");
            fflush(stdout); // Ensure message appears before blocking on recv

            // Receive data from the server
            if ((bytes_received = recv(sockfd, recv_buffer, MAX_MESSAGE_LENGTH - 1, 0)) == -1) {
                perror("recv error");
                // If recv fails, it might indicate a lost connection or server issue
                // You might want to set running = 0 here to exit gracefully
                running = 0;
            } else if (bytes_received == 0) {
                // Server gracefully closed the connection
                printf("[INFO] Server disconnected.\n");
                running = 0; // Exit the loop as server is gone
            } else {
                // Data successfully received
                recv_buffer[bytes_received] = '\0'; // Null-terminate the received data
                printf("[RECV SUCCESS] Server says: '%s' (%d bytes received)\n", recv_buffer, bytes_received);
            }
        }
    }

    close(sockfd);
    printf("Socket closed and program finished.\n");

    return 0;
}


