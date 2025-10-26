#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h> // Used for non-blocking I/O multiplexing
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define SERVER_HOST "127.0.0.1" // Use localhost for testing
#define SERVER_PORT "3491"      // *** UPDATED: Matches the provided server.c port ***
#define MAX_MESSAGE_LENGTH 256
#define BUF_SIZE 1024
#define MESSAGE_PROMPT "You > "

// Global file descriptor for the connected socket
int g_sockfd = -1;
volatile int running = 1;

// --- Helper Functions ---

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

// --- Signal Handler and Cleanup ---

/**
 * @brief Performs graceful client shutdown.
 */
void cleanup_and_exit() {
    printf("\n[INFO] Client is gracefully shutting down...\n");
    if (g_sockfd != -1) {
        // Close the connected socket. This sends a FIN packet to the server.
        close(g_sockfd);
        printf("[INFO] Closed socket FD %d.\n", g_sockfd);
    }
    printf("Client exited. Goodbye!\n");
    exit(0);
}

/**
 * @brief Signal handler function for SIGINT (Ctrl+C).
 */
void sigint_handler(int sig) {
    // Call cleanup directly to ensure all sockets are closed before exit.
    cleanup_and_exit();
}

// --- Connection Logic (Refactored from user's main) ---

/**
 * @brief Initializes the socket and connects to the server.
 * Uses getaddrinfo and loops through results for robust connection.
 * @return The connected socket file descriptor, or -1 on failure.
 */
int setup_connection() {
    struct addrinfo hints, *res, *p;
    int sockfd = -1;
    int status;
    char ipstr[INET6_ADDRSTRLEN];

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    if((status = getaddrinfo(SERVER_HOST, SERVER_PORT, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return -1;
    }

    // Loop through all results and connect to the first we can
    for(p = res; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            perror("client: socket");
            continue;
        }

        inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr), ipstr, sizeof ipstr);
        printf("[INFO] Attempting connection to %s:%s\n", ipstr, SERVER_PORT);
        
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            perror("client: connect failed");
            close(sockfd);
            sockfd = -1;
            continue;
        }

        printf("[SUCCESS] Connected to server! Socket FD: %d\n", sockfd);
        break; // Success! Break out of the loop
    }

    freeaddrinfo(res); // All done with this structure

    if (p == NULL) {
        fprintf(stderr, "[ERROR] Client failed to connect to server.\n");
        return -1;
    }

    return sockfd;
}

// --- Main Select Loop ---

int main(void) {
    // 1. Setup Signal Handler
    if (signal(SIGINT, sigint_handler) == SIG_ERR) {
        perror("Could not set up SIGINT handler");
        return EXIT_FAILURE;
    }

    printf("--- Chat Client Console ---\n");
    
    // 2. Establish the connection
    g_sockfd = setup_connection();
    if (g_sockfd == -1) {
        return EXIT_FAILURE;
    }

    // 3. Main loop variables
    fd_set read_fds;
    int max_fd = (g_sockfd > STDIN_FILENO) ? g_sockfd : STDIN_FILENO;
    int activity;
    char buffer[BUF_SIZE];
    
    // Welcome message
    printf("Type messages below. Type '/quit' or press Ctrl+C to exit.\n\n");
    printf("%s", MESSAGE_PROMPT); 
    fflush(stdout);

    // 4. Main event loop using select()
    while (running) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds); // For keyboard input
        FD_SET(g_sockfd, &read_fds);     // For incoming data from server

        // Wait for activity on either FD (NULL timeout = infinite block)
        activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);

        if ((activity < 0) && (errno != EINTR)) {
            perror("select error");
            break;
        }
        
        // --- A. Handle Server Input (Incoming Messages) ---
        if (FD_ISSET(g_sockfd, &read_fds)) {
            int valread = recv(g_sockfd, buffer, BUF_SIZE - 1, 0);

            if (valread == 0) {
                // Server gracefully closed connection
                printf("\n[ALERT] Server closed the connection. Exiting.\n");
                running = 0;
            } else if (valread < 0) {
                // Error receiving data
                perror("recv error");
                running = 0;
            } else {
                // Message received successfully
                buffer[valread] = '\0';
                printf("\n[SERVER] %s\n", buffer);
                printf("%s", MESSAGE_PROMPT); // Re-print prompt after receiving message
                fflush(stdout);
            }
        }

        // --- B. Handle STDIN Input (User Messages to Send) ---
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char send_buffer[MAX_MESSAGE_LENGTH];
            if (fgets(send_buffer, MAX_MESSAGE_LENGTH, stdin) != NULL) {
                
                // Remove newline
                size_t len = strlen(send_buffer);
                if (len > 0 && send_buffer[len - 1] == '\n') {
                    send_buffer[len - 1] = '\0';
                    len--;
                }
                
                // Check for client-side quit command
                if (strcmp(send_buffer, "/quit") == 0) {
                    printf("[INFO] '/quit' command received. Disconnecting.\n");
                    running = 0;
                    break;
                }

                // Send the message to the server
                if (len > 0) {
                    ssize_t bytes_sent = send(g_sockfd, send_buffer, len, 0);
                    if (bytes_sent == -1) {
                        perror("send error");
                        running = 0;
                        break;
                    } else {
                         printf("[SENT] %zd bytes.\n", bytes_sent);
                    }
                }
                printf("%s", MESSAGE_PROMPT); // Re-print prompt for next input
                fflush(stdout);
            }
        }
    }
    
    cleanup_and_exit(); 
    return EXIT_SUCCESS;
}
