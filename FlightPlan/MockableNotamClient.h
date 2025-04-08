#pragma once
#pragma once

#include <string>
#include <iostream>
#include <cstring>
#include <random>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <atomic>
#include <fstream>
#include <vector>
#include <cstdint>
#include <limits>

// For TCP/IP sockets (Windows)
#include <WinSock2.h>
#include <WS2tcpip.h>


// This is a modified version of the NotamClient class that can be used for testing
// It has the same functionality but allows for mocking socket operations

// Constants
static const int DEFAULT_SERVER_PORT = 8081;
static const int MAX_RETRY_ATTEMPTS = 3;
static const int RETRY_DELAY_SECONDS = 5;
static const int FULL_SERVER_RETRY_DELAY_SECONDS = 30;
static const int PROGRESS_BAR_WIDTH = 50;
static const int SPINNER_INTERVAL_MS = 250;
static const size_t BUFFER_SIZE = 4096U;

// Error handling utilities
enum class ServerStateMachine {
    SUCCESS = 0,
    WINSOCK_ERROR = 1,
    CONNECTION_ERROR = 2,
    SEND_ERROR = MAX_RETRY_ATTEMPTS,
    RECEIVE_ERROR = 4,
    CONNECTION_REQUEST_DENIED = RETRY_DELAY_SECONDS
};

// Simple logger class for testing
class TestLogger {
public:
    TestLogger() {}
    ~TestLogger() {}

    bool initialize(const std::string& filename = "test_log.txt") {
        return true;
    }

    void logSentPacket(const std::string& packet, const std::string& description = "") {}
    void logReceivedPacket(const std::string& packet, const std::string& description = "") {}
    void logEvent(const std::string& eventDescription) {}
};

// Simple header class for testing
class TestHeader {
public:
    static std::string createHeader(const std::string& payload) {
        return "HEADER\nSEQ_NUM=1\nTIMESTAMP=2023-01-01T00:00:00Z\nPAYLOAD_SIZE=" +
            std::to_string(payload.length()) + "\nEND_HEADER\n";
    }

    static void setLogger(TestLogger* logger) {}
};

// Test client for unit testing
class MockableNotamClient {
private:
    SOCKET clientSocket;
    bool initialized;
    std::string clientId;
    bool isConnected;
    bool isApproved;
    TestLogger logger;

public:
    MockableNotamClient() :
        clientSocket(INVALID_SOCKET),
        initialized(false),
        isConnected(false),
        isApproved(false),
        clientId("TEST1234") {

        // Initialize logger
        logger.initialize("test_log.txt");
        TestHeader::setLogger(&logger);

        // Initialize Winsock
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            return;
        }
        initialized = true;
    }

    ~MockableNotamClient() {
        disconnect();
        if (initialized) {
            WSACleanup();
        }
    }

    std::string getClientId() const {
        return clientId;
    }

    ServerStateMachine connect(const std::string& serverIP, int port) {
        if (!initialized) {
            return ServerStateMachine::WINSOCK_ERROR;
        }

        // Validate port range
        if (port <= 0 || port > 65535) {
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Create a socket
        clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (clientSocket == INVALID_SOCKET) {
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Set up the server address
        sockaddr_in serverAddr;
        ZeroMemory(&serverAddr, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(static_cast<uint16_t>(port));

        // Use inet_pton for safer IP address conversion
        int convResult = inet_pton(AF_INET, serverIP.c_str(), &(serverAddr.sin_addr));
        if (convResult != 1) {
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Connect to server
        if (::connect(clientSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            return ServerStateMachine::CONNECTION_ERROR;
        }

        isConnected = true;
        return ServerStateMachine::SUCCESS;
    }

    ServerStateMachine requestConnection() {
        if (clientSocket == INVALID_SOCKET || !isConnected) {
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Create a simple connection request
        std::string requestMessage = TestHeader::createHeader("REQUEST_CONNECTION\nCLIENT_ID=" + clientId + "\n") +
            "REQUEST_CONNECTION\nCLIENT_ID=" + clientId + "\n";

        // Send connection request
        int sendResult = send(clientSocket, requestMessage.c_str(), static_cast<int>(requestMessage.length()), 0);
        if (sendResult == SOCKET_ERROR) {
            return ServerStateMachine::SEND_ERROR;
        }

        // Receive response
        std::string response = receiveResponse();

        // Check if connection was accepted
        if (response.find("CONNECTION_ACCEPTED") == std::string::npos) {
            return ServerStateMachine::CONNECTION_REQUEST_DENIED;
        }

        isApproved = true;
        return ServerStateMachine::SUCCESS;
    }

    ServerStateMachine sendExtendedFlightInformation(const std::string& flightId, const std::string& departure, const std::string& arrival) {
        if (clientSocket == INVALID_SOCKET || !isConnected || !isApproved) {
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Use a stringstream to concatenate and format the flight plan
        std::ostringstream flightPlanStream;

        flightPlanStream << "FLIGHT_PLAN\n"
            << "FLIGHT_NUMBER=" << flightId << "\n"
            << "AIRCRAFT_REG=TEST-REG\n"
            << "AIRCRAFT_TYPE=B737\n"
            << "OPERATOR=TEST-OP\n"
            << "DEP=" << departure << "\n"
            << "ARR=" << arrival << "\n"
            << "ROUTE=TEST-ROUTE\n";

        // Convert the stringstream to a string
        std::string flightPlan = flightPlanStream.str();

        // Assuming TestHeader::createHeader() is correctly defined
        std::string flightPlanWithHeader = TestHeader::createHeader(flightPlan) + flightPlan;

        // Sending the data over the socket
        int sendResult = send(clientSocket, flightPlanWithHeader.c_str(), static_cast<int>(flightPlanWithHeader.length()), 0);
        if (sendResult == SOCKET_ERROR) {
            return ServerStateMachine::SEND_ERROR;
        }

        return ServerStateMachine::SUCCESS;
    }

    std::string receiveResponse() {
        if (clientSocket == INVALID_SOCKET || !isConnected) {
            return "ERROR: Not connected to server";
        }

        char buffer[BUFFER_SIZE];
        ZeroMemory(buffer, sizeof(buffer));

        // Receive response from server
        int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived == SOCKET_ERROR) {
            return "ERROR: Failed to receive response";
        }

        buffer[bytesReceived] = '\0';
        return std::string(buffer);
    }

    bool retryConnection(const std::string& serverIP, int port, int maxRetries = 3) {
        // For testing, simplify retry logic
        for (int retry = 0; retry < maxRetries; ++retry) {
            if (isConnected) {
                disconnect();
            }

            if (connect(serverIP, port) == ServerStateMachine::SUCCESS) {
                if (requestConnection() == ServerStateMachine::SUCCESS) {
                    return true;
                }
            }
        }
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