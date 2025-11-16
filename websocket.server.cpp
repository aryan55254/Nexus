#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cstring>
#include <cstdint>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <set>

#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#include <algorithm>
#include <cctype>

#define PORT 8080
#define BUFFER_size 4096
#define MAX_PAYLOAD_SIZE 10485760

std::set<int> g_client_sockets;
std::mutex g_sockets_mutex;

void safe_client_socket_addition(int client__socket)
{
    std::lock_guard<std::mutex> gaurd(g_sockets_mutex);
    g_client_sockets.insert(client__socket);
};
void safe_client_socket_removal(int client__socket)
{
    std::lock_guard<std::mutex> gaurd(g_sockets_mutex);
    g_client_sockets.erase(client__socket);
};

// helper functions
// encodes raw bytes into a base64 string
std::string base64_encode(const unsigned char *input, int length)
{
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, input, length);
    BIO_flush(b64);

    BUF_MEM *mem_ptr;
    BIO_get_mem_ptr(b64, &mem_ptr);
    std::string encoded(mem_ptr->data, mem_ptr->length);

    BIO_free_all(b64);
    return encoded;
}

// add all key value pairs in a hashmap
std::map<std::string, std::string> parse_http_request(char *buffer)
{
    std::map<std::string, std::string> headers;
    std::istringstream request(buffer);
    std::string line;

    std::getline(request, line);

    while (std::getline(request, line) && line != "\r")
    {
        std::size_t colon_pos = line.find(":");
        if (colon_pos != std::string::npos)
        {
            std::string header_name = line.substr(0, colon_pos);
            std::transform(header_name.begin(), header_name.end(), header_name.begin(), ::tolower);
            std::size_t value_start = colon_pos + 1;
            while (value_start < line.length() && (line[value_start] == ' ' || line[value_start] == '\t'))
            {
                value_start++;
            }
            std::string header_value = line.substr(value_start);
            if (!header_value.empty() && header_value.back() == '\r')
            {
                header_value.pop_back();
            }
            headers[header_name] = header_value;
        }
    }
    return headers;
}

// function to add the parsed client key to the magic key and create the websocket protocol accepting key
std::string generate_websocket_accept_key(const std::string &client_key)
{
    const std::string magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = client_key + magic_string;

    unsigned char sha1_hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char *>(combined.c_str()), combined.length(), sha1_hash);
    return base64_encode(sha1_hash, SHA_DIGEST_LENGTH);
}

void send_websocket_frame(int client_socket, const std::string &payload, uint8_t opcode = 0x1) // opcode 0x1 meaans we wanna send text frame type of messages
{
    std::vector<uint8_t> frame_header;
    frame_header.push_back(0x80 | opcode);

    size_t len = payload.length();
    if (len <= 125)
    { // byte 2
        // mask bit in this is 0 as frames from server to client aren't masked
        frame_header.push_back(static_cast<uint8_t>(len));
    }
    else if (len <= 65535)
    {
        // medium size messages tells that lenght isnt 126 the real is in next 2 bytes
        frame_header.push_back(126);
        uint16_t len16 = htons(static_cast<uint16_t>(len));
        frame_header.insert(frame_header.end(), (uint8_t *)&len16, (uint8_t *)&len16 + 2);
    }
    else
    { // tells real lenght is in next 8 bytes
        frame_header.push_back(127);
        uint64_t len64 = htobe64(static_cast<uint64_t>(len));
        frame_header.insert(frame_header.end(), (uint8_t *)&len64, (uint8_t *)&len64 + 8);
    }
    ssize_t header_sent = send(client_socket, frame_header.data(), frame_header.size(), 0);
    if (header_sent < 0)
    {
        std::cerr << "Failed to send frame header" << std::endl;
        return;
    }

    // Send the payload (it's not masked)
    ssize_t payload_sent = send(client_socket, payload.c_str(), payload.length(), 0);
    if (payload_sent < 0)
    {
        std::cerr << "Failed to send payload" << std::endl;
        return;
    }

    std::cout << "SENT: " << payload << std::endl;
}

void handle_websocket_frame(int client_socket)
{
    std::vector<uint8_t> persistent_buffer;
    char temp_buffer[BUFFER_size];

    while (true)
    {
        memset(temp_buffer, 0, BUFFER_size);

        ssize_t bytes_received = recv(client_socket, temp_buffer, BUFFER_size, 0);

        if (bytes_received <= 0)
        {
            std::cout << "Client disconnected." << std::endl;
            break;
        }
        persistent_buffer.insert(persistent_buffer.end(),
                                 temp_buffer,
                                 temp_buffer + bytes_received);

        bool fatal_protocol_error = false;
        while (true)
        {
            const uint8_t *frame = persistent_buffer.data();
            if (persistent_buffer.size() < 2)
            {
                break;
            }

            bool fin = (frame[0] & 0x80) != 0;
            bool rsv = (frame[0] & 0x70) != 0;

            if (rsv)
            {
                std::cerr << "Error: RSV bits set without negotiated extension. Closing connection." << std::endl;
                fatal_protocol_error = true;
                break;
            }

            if (!fin)
            {
                std::cerr << "Error: Fragmented frames not supported. Closing connection." << std::endl;
                fatal_protocol_error = true;
                break;
            }

            uint8_t opcode = frame[0] & 0x0F;

            // Byte 2: MASK bit and Payload Len
            bool is_masked = (frame[1] & 0x80) != 0;
            uint64_t payload_len = frame[1] & 0x7F;

            // Check for required masking
            if (!is_masked)
            {
                std::cerr << "Error: Client frame not masked. Closing connection." << std::endl;
                fatal_protocol_error = true;
                break;
            }

            size_t header_offset = 2;

            // Handle extended payload lengths
            if (payload_len == 126)
            {
                if (persistent_buffer.size() < header_offset + 2)
                {
                    std::cerr << "Incomplete frame" << std::endl;
                    break;
                }

                uint16_t len16;
                memcpy(&len16, &frame[header_offset], 2);
                payload_len = ntohs(len16);
                header_offset += 2;
            }
            else if (payload_len == 127)
            {
                if (persistent_buffer.size() < header_offset + 8)
                {
                    std::cerr << "Incomplete frame" << std::endl;
                    break;
                }
                uint64_t len64;
                memcpy(&len64, &frame[header_offset], 8);
                payload_len = be64toh(len64);
                header_offset += 8;
            }
            if (payload_len > MAX_PAYLOAD_SIZE)
            {
                std::cerr << "Error: Payload size exceeds limit. Closing connection." << std::endl;
                send_websocket_frame(client_socket, "", 0x8);
                goto connection_close;
            }

            if (persistent_buffer.size() < header_offset + 4)
            {
                std::cerr << "Incomplete frame (missing masking key)" << std::endl;
                break;
            }
            uint8_t masking_key[4];
            memcpy(masking_key, &frame[header_offset], 4);
            header_offset += 4;

            if ((size_t)persistent_buffer.size() < header_offset + payload_len)
            {
                std::cerr << "Incomplete frame (missing payload data)" << std::endl;
                break;
            }

            std::vector<char> unmasked_payload;
            unmasked_payload.resize(payload_len);

            const uint8_t *masked_payload = &frame[header_offset];
            uint64_t total_frame_size = header_offset + payload_len;
            if (persistent_buffer.size() < total_frame_size)
            {
                break;
            }

            for (size_t i = 0; i < payload_len; ++i)
            {
                unmasked_payload[i] = masked_payload[i] ^ masking_key[i % 4];
            }
            std::string payload_text(unmasked_payload.begin(), unmasked_payload.end());

            switch (opcode)
            {
            case 0x1:
            {
                std::cout << "RECEIVED" << client_socket << ": " << payload_text << std::endl;

                std::set<int> clients_to_send_to;
                {
                    std::lock_guard<std::mutex> guard(g_sockets_mutex);
                    clients_to_send_to = g_client_sockets;
                }
                clients_to_send_to.erase(client_socket);
                for (int socket_fd : clients_to_send_to)
                {
                    std::string message_for_recipient = "Someone: " + payload_text;
                    send_websocket_frame(socket_fd, message_for_recipient, 0x1);
                }
                break;
            }

            case 0x8:
                std::cout << "RECEIVED: Client Close Frame" << std::endl;
                send_websocket_frame(client_socket, "", 0x8);
                goto connection_close;

            case 0x9:
                std::cout << "RECEIVED: Ping Frame" << std::endl;
                send_websocket_frame(client_socket, payload_text, 0xA);
                break;

            default:
                std::cout << "RECEIVED: Unhandled Opcode " << (int)opcode << std::endl;
                fatal_protocol_error = true;
                break;
            }

            persistent_buffer.erase(persistent_buffer.begin(),
                                    persistent_buffer.begin() + total_frame_size);
        }
        if (fatal_protocol_error)
            break;
    }

connection_close:
    safe_client_socket_removal(client_socket);
    close(client_socket);
    std::cout << "Connection closed." << std::endl;
}

void handle_new_connection(int client_socket)
{
    // Handshake Logic
    char buffer[BUFFER_size] = {0};
    ssize_t bytes_received = recv(client_socket, buffer, BUFFER_size - 1, 0);

    if (bytes_received <= 0)
    {
        perror("recv failed or client disconnected before handshake");
        close(client_socket);
        return;
    }

    std::cout << "--- Client Handshake Request (Socket " << client_socket << ") ---" << std::endl;
    std::cout << buffer << std::endl;
    std::cout << "--------------------------------" << std::endl;

    std::map<std::string, std::string> headers = parse_http_request(buffer);

    std::string connection_value;
    if (headers.find("connection") != headers.end())
    {
        connection_value = headers["connection"];
        std::transform(connection_value.begin(), connection_value.end(), connection_value.begin(), ::tolower);
    }
    std::string upgrade_value;
    if (headers.find("upgrade") != headers.end())
    {
        upgrade_value = headers["upgrade"];
        std::transform(upgrade_value.begin(), upgrade_value.end(), upgrade_value.begin(), ::tolower);
    }
    // check for ws headers
    if (headers.find("upgrade") == headers.end() || upgrade_value != "websocket" || headers.find("sec-websocket-key") == headers.end() ||
        headers.find("sec-websocket-version") == headers.end() || headers["sec-websocket-version"] != "13" || headers.find("connection") == headers.end() || connection_value.find("upgrade") == std::string::npos)
    {
        std::cerr << "Invalid HTTP request (not a WebSocket upgrade) on Socket " << client_socket << std::endl;
        const char *http_response = "HTTP/1.1 400 Bad Request\r\n\r\n";
        send(client_socket, http_response, strlen(http_response), 0);
        close(client_socket);
        return;
    }
    std::string client_key = headers["sec-websocket-key"];
    std::string accept_key = generate_websocket_accept_key(client_key);
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        accept_key + "\r\n"
                     "\r\n";

    ssize_t bytes_sent = send(client_socket, response.c_str(), response.length(), 0);
    if (bytes_sent < 0)
    {
        perror("Handshake send failed");
        close(client_socket);
        return; // This ends the thread
    }
    std::cout << "Handshake successful for Socket " << client_socket << "." << std::endl;
    safe_client_socket_addition(client_socket);
    handle_websocket_frame(client_socket);
}

int main()
{
    // initializing variables
    int server_fd, client_socket;
    struct sockaddr_in address;
    struct sockaddr_in client_addr;
    int opt = 1;
    int addrlen = sizeof(address);
    int client_addrlen = sizeof(client_addr);

    // asking os to create a socket with tcp connection
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    // asks os to enable reuse address and reuse port on the socket server_fd we created
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)))
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    // assign address family port and ip address on which our computer will listen to
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // bind the address to our socket
    // struct sockaddr * thing is because bind func can't directly accept the pointer to address variable
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    // start listening on our socket
    if (listen(server_fd, 3) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    std::cout << "Server listening on port " << PORT << "..." << std::endl;

    while (true)
    {
        // we wait for a client to connect after it connect we return a new socket the client_socket while our server_fd keep listening for new connections
        if ((client_socket = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addrlen)) < 0)
        {
            perror("accept");
            continue;
        }
        // basically storing the ip of client in human readable format;
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "new connection from " << client_ip << std::endl;

        std::thread client_thread(handle_new_connection, client_socket);
        client_thread.detach();
    }
    close(server_fd);
    return 0;
}