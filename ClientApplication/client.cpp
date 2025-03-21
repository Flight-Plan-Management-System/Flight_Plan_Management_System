#define _CRT_SECURE_NO_WARNINGS 
#include <iostream>
#include <string>
#include <cstring>

// For TCP/IP sockets (Windows)
#include <WinSock2.h>
#include <WS2tcpip.h>

// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

// Error handling utilities
enum class ErrorCode {
    SUCCESS = 0,
    WINSOCK_ERROR = 1,
    CONNECTION_ERROR = 2,
    SEND_ERROR = 3,
    RECEIVE_ERROR = 4
};

class NotamClient {
private:
    static const int BUFFER_SIZE = 4096;
    SOCKET clientSocket;
    bool initialized;

public:
    NotamClient() : clientSocket(INVALID_SOCKET), initialized(false) {
        // Initialize Winsock
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "WSAStartup failed: " << result << std::endl;
            return;
        }
        initialized = true;
    }

    ~NotamClient() {
        disconnect();
        if (initialized) {
            WSACleanup();
        }
    }

    ErrorCode connect(const std::string& serverIP, int port) {
        if (!initialized) {
            return ErrorCode::WINSOCK_ERROR;
        }

        // Create a socket
        clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (clientSocket == INVALID_SOCKET) {
            std::cerr << "Socket creation failed: " << WSAGetLastError() << std::endl;
            return ErrorCode::CONNECTION_ERROR;
        }

        // Set up the server address
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, serverIP.c_str(), &serverAddr.sin_addr);

        // Connect to server
        if (::connect(clientSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "Connection failed: " << WSAGetLastError() << std::endl;
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            return ErrorCode::CONNECTION_ERROR;
        }

        return ErrorCode::SUCCESS;
    }

    ErrorCode sendFlightPlan(const std::string& flightId, const std::string& departure,
        const std::string& arrival) {
        if (clientSocket == INVALID_SOCKET) {
            return ErrorCode::CONNECTION_ERROR;
        }

        // Construct the flight plan message in the format expected by the server
        std::string flightPlan = "FLIGHT=" + flightId + "\n" +
            "DEP=" + departure + "\n" +
            "ARR=" + arrival + "\n";

        // Send the flight plan
        int sendResult = send(clientSocket, flightPlan.c_str(), static_cast<int>(flightPlan.length()), 0);
        if (sendResult == SOCKET_ERROR) {
            std::cerr << "Send failed: " << WSAGetLastError() << std::endl;
            return ErrorCode::SEND_ERROR;
        }

        return ErrorCode::SUCCESS;
    }

    std::string receiveResponse() {
        if (clientSocket == INVALID_SOCKET) {
            return "ERROR: Not connected to server";
        }

        char buffer[BUFFER_SIZE];
        memset(buffer, 0, sizeof(buffer));

        // Receive response from server
        int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived == SOCKET_ERROR) {
            std::cerr << "Receive failed: " << WSAGetLastError() << std::endl;
            return "ERROR: Failed to receive response";
        }

        // Null terminate the received data
        buffer[bytesReceived] = '\0';
        return std::string(buffer);
    }

    void disconnect() {
        if (clientSocket != INVALID_SOCKET) {
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
        }
    }
};

int main() {
    std::string serverIP = "127.0.0.1";  // Default to localhost
    int serverPort = 8080;               // Default port

    std::cout << "NOTAM Client Application\n" << std::endl;

    // Ask user if they want to customize server settings
    std::cout << "Use default server settings (127.0.0.1:8080)? (y/n): ";
    char choice;
    std::cin >> choice;

    if (choice == 'n' || choice == 'N') {
        std::cout << "Enter server IP address: ";
        std::cin >> serverIP;

        std::cout << "Enter server port: ";
        std::cin >> serverPort;
    }

    std::cin.ignore(); // Clear the newline character

    // Menu to select which flight to simulate
    int flightChoice = 0;

    std::cout << "\nSelect a flight to check NOTAMs:" << std::endl;
    std::cout << "1. Toronto (CYYZ) to New York (KJFK)" << std::endl;
    std::cout << "2. Waterloo (CYKF) to Montreal (CYUL)" << std::endl;
    std::cout << "Enter your choice (1-2): ";
    std::cin >> flightChoice;

    NotamClient client;
    std::string flightId, departure, arrival;

    // Connect to the server
    std::cout << "\nConnecting to NOTAM server at " << serverIP << ":" << serverPort << "..." << std::endl;
    ErrorCode result = client.connect(serverIP, serverPort);
    if (result != ErrorCode::SUCCESS) {
        std::cerr << "Failed to connect to server. Exiting." << std::endl;
        return 1;
    }
    std::cout << "Connected successfully!" << std::endl;

    // Set flight details based on user selection
    if (flightChoice == 1) {
        flightId = "AC101";
        departure = "CYYZ";  // Toronto
        arrival = "KJFK";    // New York JFK
        std::cout << "\nSelected flight: Toronto (CYYZ) to New York (KJFK)" << std::endl;
    }
    else {
        flightId = "WJ202";
        departure = "CYKF";  // Waterloo
        arrival = "CYUL";    // Montreal
        std::cout << "\nSelected flight: Waterloo (CYKF) to Montreal (CYUL)" << std::endl;
    }

    // Send flight plan to server
    std::cout << "Sending flight plan to NOTAM server..." << std::endl;
    result = client.sendFlightPlan(flightId, departure, arrival);
    if (result != ErrorCode::SUCCESS) {
        std::cerr << "Failed to send flight plan. Exiting." << std::endl;
        return 1;
    }

    // Receive and display the server's response
    std::cout << "Waiting for server response..." << std::endl;
    std::string response = client.receiveResponse();
    std::cout << "\n===== NOTAM INFORMATION FOR FLIGHT " << flightId << " =====\n" << response << std::endl;

    // Disconnect from the server
    client.disconnect();

    std::cout << "\nPress Enter to exit...";
    std::cin.ignore(); // Clear any remaining input
    std::cin.get();    // Wait for Enter key

    return 0;
}


