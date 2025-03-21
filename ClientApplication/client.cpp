#define _CRT_SECURE_NO_WARNINGS 
#include <iostream>
#include <string>
#include <cstring>
#include <random>
#include <thread>
#include <chrono>

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
    RECEIVE_ERROR = 4,
    CONNECTION_REQUEST_DENIED = 5
};

struct ConnectionRequest {
    std::string clientId;

    std::string serialize() const {
        return "REQUEST_CONNECTION\nCLIENT_ID=" + clientId + "\n";
    }

    static bool parseResponse(const std::string& response) {
        return response.find("CONNECTION_ACCEPTED") != std::string::npos;
    }

    static std::string getRejectReason(const std::string& response) {
        std::string reason = "Unknown reason";
        size_t reasonPos = response.find("REASON=");

        if (reasonPos != std::string::npos) {
            reason = response.substr(reasonPos + 7); // +7 to skip "REASON="
        }

        return reason;
    }
};

// Flight number generator
class FlightNumberGenerator {
private:
    std::mt19937 rng;
    std::uniform_int_distribution<int> flightNumDist;

public:
    FlightNumberGenerator() {
        // Seed the random number generator
        rng.seed(static_cast<unsigned int>(time(nullptr)));
        // Flight numbers typically range from 1-9999
        flightNumDist = std::uniform_int_distribution<int>(100, 4999);
    }

    std::string generateFlightNumber() {
        // Always start with F followed by 3-4 digits
        return "F" + std::to_string(flightNumDist(rng));
    }
};

class NotamClient {
private:
    static const int BUFFER_SIZE = 4096;
    SOCKET clientSocket;
    bool initialized;
    std::string clientId;
    bool isConnected;
    bool isApproved;

public:
    NotamClient() :
        clientSocket(INVALID_SOCKET),
        initialized(false),
        isConnected(false),
        isApproved(false) {

        // Initialize Winsock
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "WSAStartup failed: " << result << std::endl;
            return;
        }
        initialized = true;

        FlightNumberGenerator generator;
        clientId = generator.generateFlightNumber();
    }

    ~NotamClient() {
        disconnect();
        if (initialized) {
            WSACleanup();
        }
    }

    std::string getClientId() const {
        return clientId;
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

        isConnected = true;
        return ErrorCode::SUCCESS;
    }

    // Request connection approval from the server
    ErrorCode requestConnection() {
        if (clientSocket == INVALID_SOCKET || !isConnected) {
            return ErrorCode::CONNECTION_ERROR;
        }

        // Create connection request
        ConnectionRequest request;
        request.clientId = clientId;
        std::string requestMessage = request.serialize();

        // Send connection request
        int sendResult = send(clientSocket, requestMessage.c_str(), static_cast<int>(requestMessage.length()), 0);
        if (sendResult == SOCKET_ERROR) {
            std::cerr << "Send connection request failed: " << WSAGetLastError() << std::endl;
            return ErrorCode::SEND_ERROR;
        }

        // Receive response
        std::string response = receiveResponse();

        // Parse response
        if (!ConnectionRequest::parseResponse(response)) {
            std::string reason = ConnectionRequest::getRejectReason(response);
            std::cerr << "Connection request denied by server: " << reason << std::endl;
            return ErrorCode::CONNECTION_REQUEST_DENIED;
        }

        isApproved = true;
        return ErrorCode::SUCCESS;
    }

    ErrorCode sendFlightPlan(const std::string& flightId, const std::string& departure,
        const std::string& arrival) {
        if (clientSocket == INVALID_SOCKET || !isConnected || !isApproved) {
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
        if (clientSocket == INVALID_SOCKET || !isConnected) {
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

    bool retryConnection(const std::string& serverIP, int port, int maxRetries = 3) {
        for (int retry = 0; retry < maxRetries; retry++) {
            // Disconnect if previously connected
            if (isConnected) {
                disconnect();
            }

            std::cout << "Attempting connection retry " << (retry + 1) << " of " << maxRetries << "..." << std::endl;

            // Try to connect
            if (connect(serverIP, port) == ErrorCode::SUCCESS) {
                // If connected, try to get approval
                if (requestConnection() == ErrorCode::SUCCESS) {
                    std::cout << "Connection and approval successful on retry " << (retry + 1) << std::endl;
                    return true;
                }

                // If server is full, wait before retrying
                std::cout << "Server is full. Waiting 30 seconds before retry..." << std::endl;
                disconnect();
                std::this_thread::sleep_for(std::chrono::seconds(30));
            }
            else {
                // If connection failed, wait a bit less before retrying
                std::cout << "Connection failed. Waiting 5 seconds before retry..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }

        std::cout << "Maximum retry attempts reached. Could not connect to server." << std::endl;
        return false;
    }

    void disconnect() {
        if (clientSocket != INVALID_SOCKET) {
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            isConnected = false;
            isApproved = false;
        }
    }

    bool isConnectionApproved() const {
        return isApproved;
    }
};

int main() {
    std::string serverIP = "127.0.0.1";  // Default to localhost
    int serverPort = 8080;               // Default port

    std::cout << "====================================\n";
    std::cout << "       NOTAM Client Application     \n";
    std::cout << "====================================\n\n";

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

    // Initialize client
    NotamClient client;
    std::string generatedClientId = client.getClientId();
    std::cout << "\nGenerated flight ID: " << generatedClientId << std::endl;

    // Connect to the server
    std::cout << "\nConnecting to NOTAM server at " << serverIP << ":" << serverPort << "..." << std::endl;
    ErrorCode result = client.connect(serverIP, serverPort);
    if (result != ErrorCode::SUCCESS) {
        std::cerr << "Failed to connect to server." << std::endl;

        std::cout << "Would you like to retry connecting? (y/n): ";
        std::cin >> choice;
        if (choice == 'y' || choice == 'Y') {
            if (!client.retryConnection(serverIP, serverPort, 3)) {
                std::cout << "Exiting application." << std::endl;
                return 1;
            }
        }
        else {
            std::cout << "Exiting application." << std::endl;
            return 1;
        }
    }
    else {
        std::cout << "Connected successfully!" << std::endl;

        // Request connection approval from the server
        std::cout << "Requesting connection approval from the server..." << std::endl;
        result = client.requestConnection();
        if (result != ErrorCode::SUCCESS) {
            std::cerr << "Failed to get connection approval from server." << std::endl;

            std::cout << "Would you like to retry after waiting? (y/n): ";
            std::cin >> choice;
            if (choice == 'y' || choice == 'Y') {
                if (!client.retryConnection(serverIP, serverPort, 3)) {
                    std::cout << "Exiting application." << std::endl;
                    client.disconnect();
                    return 1;
                }
            }
            else {
                std::cout << "Exiting application." << std::endl;
                client.disconnect();
                return 1;
            }
        }
        else {
            std::cout << "Connection approved by server!" << std::endl;
        }
    }

    // If we reached here, connection was successful and approved
    if (!client.isConnectionApproved()) {
        std::cerr << "Connection not approved. Exiting." << std::endl;
        return 1;
    }

    // Menu to select which flight to simulate
    int flightChoice = 0;
    std::string flightId, departure, arrival;

    std::cout << "\nSelect a flight to check NOTAMs:" << std::endl;
    std::cout << "1. Toronto (CYYZ) to New York (KJFK)" << std::endl;
    std::cout << "2. Waterloo (CYKF) to Montreal (CYUL)" << std::endl;
    std::cout << "3. Custom flight" << std::endl;
    std::cout << "Enter your choice (1-3): ";
    std::cin >> flightChoice;
    std::cin.ignore(); // Clear the newline character

    // Set flight details based on user selection
    if (flightChoice == 1) {
        flightId = generatedClientId;
        departure = "CYYZ";  // Toronto
        arrival = "KJFK";    // New York JFK
        std::cout << "\nSelected flight: Toronto (CYYZ) to New York (KJFK)" << std::endl;
    }
    else if (flightChoice == 2) {
        flightId = generatedClientId;
        departure = "CYKF";  // Waterloo
        arrival = "CYUL";    // Montreal
        std::cout << "\nSelected flight: Waterloo (CYKF) to Montreal (CYUL)" << std::endl;
    }
    else {
        flightId = generatedClientId;
        std::cout << "Enter departure airport code (e.g., CYYZ): ";
        std::getline(std::cin, departure);
        std::cout << "Enter arrival airport code (e.g., KJFK): ";
        std::getline(std::cin, arrival);
        std::cout << "\nSelected flight: " << departure << " to " << arrival << std::endl;
    }

    // Send flight plan to server
    std::cout << "Sending flight plan to NOTAM server..." << std::endl;
    result = client.sendFlightPlan(flightId, departure, arrival);
    if (result != ErrorCode::SUCCESS) {
        std::cerr << "Failed to send flight plan. Exiting." << std::endl;
        client.disconnect();
        return 1;
    }

    // Receive and display the server's response
    std::cout << "Waiting for server response..." << std::endl;
    std::string response = client.receiveResponse();

    std::cout << "\n========================================================\n";
    std::cout << "       NOTAM INFORMATION FOR FLIGHT " << flightId << "       \n";
    std::cout << "========================================================\n";
    std::cout << response << std::endl;
    std::cout << "========================================================\n";

    // Disconnect from the server
    client.disconnect();
    std::cout << "Disconnected from server." << std::endl;

    std::cout << "\nPress Enter to exit...";
    std::cin.get();    // Wait for Enter key

    return 0;
}