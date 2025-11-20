#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <string>
#include <atomic> 

const int PORT = 8080;
const char* SERVER_IP = "127.0.0.1";

std::atomic<bool> keep_running(true);

/**
 * @brief Thread function dedicated to listening for messages from the server.
 * @param client_socket The connected socket file descriptor.
 */
void receive_handler(int client_socket) {
    char buffer[1024];
    ssize_t valread;
    
    std::cout << "[Receiver Thread] Started listening for partner messages." << std::endl;

    while (keep_running.load()) {
        memset(buffer, 0, sizeof(buffer));
        
        // If the server sends data (partner message or server error), it unblocks.
        valread = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

        if (valread > 0) {
            buffer[valread] = '\0';
            // Use cerr to print to avoid mixing with cin/cout buffering,
            std::cerr << "\n" << buffer << "\n> ";
            std::cerr.flush();
        } else if (valread == 0) {
            std::cerr << "\nServer disconnected. Conversation ended." << std::endl;
            keep_running.store(false);
            break;
        } else {
            if (keep_running.load()) {
                perror("\nReceive error on socket");
            }
            keep_running.store(false);
            break;
        }
    }
    std::cout << "[Receiver Thread] Exiting." << std::endl;
}

int main() {
    int client_fd;
    struct sockaddr_in serv_addr;
    char buffer[1024] = {0};
    ssize_t valread;
    
    // --- Connection Setup (Steps 1, 2, 3) ---
    if ((client_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Client socket creation failed");
        return EXIT_FAILURE;
    }
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address/Address not supported");
        close(client_fd);
        return EXIT_FAILURE;
    }

    if (connect(client_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
        close(client_fd);
        return EXIT_FAILURE;
    }

    std::cout << "Successfully connected to server." << std::endl;

    // --- Step 4a: Receive initial greeting/login prompt ---
    valread = recv(client_fd, buffer, 1024 - 1, 0);

    if (valread > 0) {
        buffer[valread] = '\0';
        std::cout << "\n--- SERVER MESSAGE ---\n" << buffer << "\n----------------------" << std::endl;
    } else {
        std::cout << "Server closed connection immediately." << std::endl;
        close(client_fd);
        return 0;
    }

    // --- Step 5: Start the Receiver Thread ---
    std::thread receiver_thread(receive_handler, client_fd);

    // --- Step 6: Input/Sender Loop (Main Thread) ---
    while (keep_running.load()) {
        std::cout << "> ";
        std::string input_message;
        
        // This blocks until the user presses Enter
        if (!std::getline(std::cin, input_message)) {
            // If getline fails (e.g., EOF, Ctrl+D), exit.
            keep_running.store(false); 
            break;
        }

        if (input_message == "QUIT") {
            std::cout << "Closing connection..." << std::endl;
            keep_running.store(false);
            break; 
        }

        if (input_message.empty()) {
            continue; 
        }
        
        if (send(client_fd, input_message.c_str(), input_message.length(), 0) < 0) {
            perror("Send failed");
            keep_running.store(false);
            break;
        }
    }
    
    // --- Step 7: Cleanup ---
    shutdown(client_fd, SHUT_RDWR); 
    
    // Wait for the receiver thread to finish its cleanup
    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }
    
    close(client_fd);
    std::cout << "\nClient socket closed. Program finished." << std::endl;

    return 0;
}