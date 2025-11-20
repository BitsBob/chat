#include <iostream>
#include <map>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sstream>
#include <mutex>
#include <cstring>
#include <thread>
#include <fstream>
#include <vector>
#include <chrono>

const int PORT = 8080;
const int BACKLOG = 5; 
std::map<std::string, std::string> user_credentials;
std::map<int, std::string> logged_in_users;
// Key: Client FD, Value: Partner FD (-1 for waiting)
std::map<int, int> active_pairs; 
std::mutex user_data_mutex; 


bool authenticate_user(const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(user_data_mutex);
    if (user_credentials.find(username) == user_credentials.end()) return false;
    return user_credentials[username] == "hashed_" + password + "_pass"; 
}

bool register_user(const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(user_data_mutex);
    if (user_credentials.count(username)) return false;
    user_credentials[username] = "hashed_" + password + "_pass";
    std::cout << "SERVER: New user registered: " << username << std::endl;
    return true;
}


/**
 * @brief Cleans up shared data structures and closes the client socket
 * @param client_socket The file descriptor of the disconnecting client.
 * @param partner_socket The file descriptor of the partner, or -1.
 * @param current_user The username of the disconnecting client.
 */
void perform_cleanup(int client_socket, int partner_socket, const std::string& current_user) {
    std::cout << "SERVER CLEANUP: Starting for user " << current_user << " (FD: " << client_socket << ")" << std::endl;
    
    // Notify the partner
    if (partner_socket != -1) {
        // Send a notification message to the partner. We rely on the partner's 
        // thread to detect the next failure or /quit and run its own cleanup.
        std::string disconnect_message = "Your partner (" + current_user + ") has disconnected. Conversation ended.";
        send(partner_socket, disconnect_message.c_str(), disconnect_message.length(), 0);
        
        // IMPORTANT: DO NOT close(partner_socket);
    }

    {
        std::lock_guard<std::mutex> lock(user_data_mutex);
        
        logged_in_users.erase(client_socket); 
        active_pairs.erase(client_socket);

        if (partner_socket != -1) {
            active_pairs.erase(partner_socket);
        }
    }

    // 3. Close the client socket
    close(client_socket);
    std::cout << "[Thread " << std::this_thread::get_id() << "] Handler for user " << current_user << " finished." << std::endl;
}

void handle_client(int client_socket) {
    char buffer[1024];
    ssize_t valread;
    std::string current_user = "";
    bool isAuthenticated = false;
    int partner_socket = -1; 
    
    std::cout << "[Thread " << std::this_thread::get_id() << "] started for client FD: " << client_socket << std::endl;
    const char *login_prompt = "Welcome! Type REGISTER <user> <pass> OR LOGIN <user> <pass>\n";
    send(client_socket, login_prompt, strlen(login_prompt), 0);

    while (!isAuthenticated) {
        memset(buffer, 0, sizeof(buffer));
        valread = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        // Check for immediate disconnect or error during login
        if (valread <= 0) { 
            perform_cleanup(client_socket, partner_socket, current_user.empty() ? "(unauthenticated)" : current_user);
            return; 
        }

        std::stringstream ss(buffer);
        std::string command, username, password;
        ss >> command >> username >> password;

        if (command == "REGISTER") {
            if (register_user(username, password)) {
                send(client_socket, "SUCCESS: Registered! Please LOGIN now.\n", 39, 0);
            } else {
                send(client_socket, "ERROR: User already exists.\n", 28, 0);
            }
        } else if (command == "LOGIN") { 
            if (authenticate_user(username, password)) {
                
                bool conflict = false;
                {
                    std::lock_guard<std::mutex> lock(user_data_mutex);
                    // Check if the user is already logged in on a different socket
                    for(const auto& pair : logged_in_users) {
                        if (pair.second == username) {
                            conflict = true;
                            break;
                        }
                    }

                    if (!conflict) {
                        isAuthenticated = true;
                        current_user = username;
                        logged_in_users[client_socket] = current_user;
                        send(client_socket, "SUCCESS: Logged in.\n", 20, 0);
                    }
                } 

                if (conflict) {
                    send(client_socket, "ERROR: User already logged in elsewhere.\n", 40, 0);
                    // Continue the loop to allow another login attempt
                }
            } else {
                 send(client_socket, "ERROR: Invalid username or password.\n", 36, 0);
            }
        } else {
             send(client_socket, "ERROR: Invalid command. Use REGISTER or LOGIN.\n", 47, 0);
        }
    } // End of auth loop
        
    std::cout << "SERVER: " << current_user << " searching for partner (FD: " << client_socket << ")." << std::endl;
    bool paired = false;

    { // Pairing lock block
        std::lock_guard<std::mutex> lock(user_data_mutex);
        
        // Look for any existing waiting partner (-1)
        for (auto& pair : active_pairs) {
            if (pair.second == -1 && pair.first != client_socket) {
                partner_socket = pair.first;
                
                active_pairs[partner_socket] = client_socket;
                active_pairs[client_socket] = partner_socket; 
                paired = true;
                break;
            }
        }

        if (!paired) {
            // Client is now waiting
            active_pairs[client_socket] = -1;
            send(client_socket, "Waiting for a partner to connect. Please hold...\n", 49, 0);
        }
    }

    if (!paired) {
        for(int i = 0; i < 30; ++i) { // 30 iterations * 500ms = 15 seconds max wait
            std::this_thread::sleep_for(std::chrono::milliseconds(500)); 
            
            std::lock_guard<std::mutex> lock(user_data_mutex);
            if (active_pairs.count(client_socket) && active_pairs[client_socket] != -1) {
                partner_socket = active_pairs[client_socket];
                paired = true;
                break;
            }
        }
    }
    
    // Check if pairing failed after waiting
    if (!paired) {
        send(client_socket, "Timed out waiting for partner. Disconnecting.\n", 45, 0);
        perform_cleanup(client_socket, partner_socket, current_user);
        return; 
    }

    std::string partner_name;
    {
        std::lock_guard<std::mutex> lock(user_data_mutex);
        auto it = logged_in_users.find(partner_socket);
        if (it != logged_in_users.end()) {
            partner_name = it->second;
        } else {
            // This should not happen if logic is correct, but safe check
            partner_name = "Unknown Partner (FD: " + std::to_string(partner_socket) + ")"; 
        }
    }
    
    std::string start_msg = "Partner " + partner_name + " found! Start chatting.\n";
    send(client_socket, start_msg.c_str(), start_msg.length(), 0);
    std::cout << "SERVER: Paired " << current_user << " (FD: " << client_socket << ") with " 
              << partner_name << " (FD: " << partner_socket << ")" << std::endl;

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        valread = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (valread > 0) {
            buffer[valread] = '\0';
            
            std::cout << "CHAT [" << current_user << " -> " << partner_name << "]: " << buffer << std::endl;

            std::string received_msg(buffer);
            
            if (received_msg.rfind("/", 0) == 0) {
                if (received_msg == "/quit") {
                    send(client_socket, "Disconnecting...", 16, 0);
                    break;
                } else if (received_msg == "/who") {
                    std::string who_msg = "You are paired with " + partner_name + ".";
                    send(client_socket, who_msg.c_str(), who_msg.length(), 0);
                } else {
                    send(client_socket, "Unknown command.", 16, 0);
                }
            } else {
                std::string full_msg = "[" + current_user + "]: " + std::string(buffer);
                if (send(partner_socket, full_msg.c_str(), full_msg.length(), 0) < 0) {
                    perror("Send to partner failed (Partner dead)");
                    break;
                }
            }
        } else if (valread == 0) {
            std::cout << "SERVER: User " << current_user << " disconnected gracefully." << std::endl;
            break; 
        } else {
            perror("Receive error in chat loop");
            break; 
        }
    }

    perform_cleanup(client_socket, partner_socket, current_user);
}


int main() {
    int server_fd;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        return EXIT_FAILURE;
    }
    
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt failed");
    }
    
    // Define Address
    memset(&address, 0, addrlen);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY); 
    address.sin_port = htons(PORT);             
    
    if (bind(server_fd, (struct sockaddr *)&address, addrlen) < 0) {
        perror("Bind failed");
        return EXIT_FAILURE;
    }

    if (listen(server_fd, BACKLOG) < 0) {
        perror("Listen failed");
        return EXIT_FAILURE;
    }

    std::cout << "Server is listening on port " << PORT << "..." << std::endl;
        
    while (true) {
        int new_socket_fd; 

        std::cout << "\nServer waiting for a new connection..." << std::endl;
        
        if ((new_socket_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Accept failed");
            continue; // Go back and try to accept again
        }

        std::cout << "Connection accepted. Dispatching to thread (New FD: " << new_socket_fd << ")." << std::endl;
        
        try {
            std::thread client_thread(handle_client, new_socket_fd);
            client_thread.detach(); 
        } catch (const std::system_error& e) {
            std::cerr << "Could not create thread: " << e.what() << std::endl;
            close(new_socket_fd);
        }
    }

    //  Theoretically unreachable
    close(server_fd); 
    
    return 0;
}