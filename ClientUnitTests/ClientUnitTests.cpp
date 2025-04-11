#include "pch.h"
#include "CppUnitTest.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <string>
#include <sstream>
#include <fstream>
#include <thread>
#include <chrono>
#include <vector>

// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

// Mock server for testing
class MockServer {
private:
    SOCKET serverSocket;
    SOCKET clientSocket;
    std::thread serverThread;
    bool running;
    std::vector<std::string> receivedMessages;
    std::vector<std::string> responsesToSend;
    int port;

public:
    MockServer(int serverPort = 8081) : serverSocket(INVALID_SOCKET), clientSocket(INVALID_SOCKET), running(false), port(serverPort) {
        responsesToSend.push_back("CONNECTION_ACCEPTED\nSERVER_ID=MOCK_SERVER\n");
    }

    ~MockServer() {
        stop();
    }

    bool start() {
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            return false;
        }

        serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (serverSocket == INVALID_SOCKET) {
            WSACleanup();
            return false;
        }

        sockaddr_in serverAddr;
        ZeroMemory(&serverAddr, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(static_cast<u_short>(port));

        if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
            closesocket(serverSocket);
            WSACleanup();
            return false;
        }

        if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(serverSocket);
            WSACleanup();
            return false;
        }

        running = true;
        serverThread = std::thread(&MockServer::run, this);

        // Give the server thread a moment to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
    }

    void stop() {
        running = false;
        if (serverThread.joinable()) {
            serverThread.join();
        }

        if (clientSocket != INVALID_SOCKET) {
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
        }

        if (serverSocket != INVALID_SOCKET) {
            closesocket(serverSocket);
            serverSocket = INVALID_SOCKET;
        }

        WSACleanup();
    }

    void addResponse(const std::string& response) {
        responsesToSend.push_back(response);
    }

    void clearResponses() {
        responsesToSend.clear();
    }

    std::vector<std::string> getReceivedMessages() const {
        return receivedMessages;
    }

    bool waitForConnection(int timeoutMs = 5000) {
        int elapsedTime = 0;
        int sleepInterval = 100;

        while (clientSocket == INVALID_SOCKET && elapsedTime < timeoutMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepInterval));
            elapsedTime += sleepInterval;
        }

        return clientSocket != INVALID_SOCKET;
    }

private:
    void run() {
        const int BUFFER_SIZE = 4096;
        char recvBuffer[BUFFER_SIZE];
        int responseIndex = 0;

        while (running) {
            if (clientSocket == INVALID_SOCKET) {
                // Accept a client socket
                clientSocket = accept(serverSocket, NULL, NULL);
                if (clientSocket == INVALID_SOCKET) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
            }

            // Clear the buffer
            ZeroMemory(recvBuffer, BUFFER_SIZE);

            // Receive message from client
            int bytesReceived = recv(clientSocket, recvBuffer, BUFFER_SIZE - 1, 0);
            if (bytesReceived > 0) {
                // Null-terminate the received data
                recvBuffer[bytesReceived] = '\0';
                std::string receivedMessage(recvBuffer);
                receivedMessages.push_back(receivedMessage);

                // Send a response if available
                if (responseIndex < responsesToSend.size()) {
                    std::string& response = responsesToSend[responseIndex++];
                    send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                }
            }
            else if (bytesReceived == 0) {
                // Connection closed by client
                closesocket(clientSocket);
                clientSocket = INVALID_SOCKET;
            }
            else {
                // Error in recv()
                closesocket(clientSocket);
                clientSocket = INVALID_SOCKET;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};



// Beginning of imported NotamClient code (modified for testing)
static const int DEFAULT_SERVER_PORT = 8081;
static const int MAX_RETRY_ATTEMPTS = 3;
static const int RETRY_DELAY_SECONDS = 5;
static const int FULL_SERVER_RETRY_DELAY_SECONDS = 30;
static const size_t BUFFER_SIZE = 4096U;

enum class ServerStateMachine {
    SUCCESS = 0,
    WINSOCK_ERROR = 1,
    CONNECTION_ERROR = 2,
    SEND_ERROR = MAX_RETRY_ATTEMPTS,
    RECEIVE_ERROR = 4,
    CONNECTION_REQUEST_DENIED = RETRY_DELAY_SECONDS
};

class TestablePacketLogger {
private:
    std::stringstream logStream;
    bool isInitialized;

public:
    TestablePacketLogger() : isInitialized(false) {}

    bool initialize() {
        isInitialized = true;
        return true;
    }

    void logSentPacket(const std::string& packet, const std::string& description = "") {
        if (!isInitialized) return;
        logStream << "SENT_PACKET: " << description << "\n" << packet << "\n";
    }

    void logReceivedPacket(const std::string& packet, const std::string& description = "") {
        if (!isInitialized) return;
        logStream << "RECEIVED_PACKET: " << description << "\n" << packet << "\n";
    }

    void logEvent(const std::string& eventDescription) {
        if (!isInitialized) return;
        logStream << "EVENT: " << eventDescription << "\n";
    }

    std::string getLogContent() const {
        return logStream.str();
    }

    void clear() {
        logStream.str("");
        logStream.clear();
    }
};

class TestablePacketHeader {
private:
    static uint64_t sequenceCounter;
    static TestablePacketLogger* logger;

public:
    static void setLogger(TestablePacketLogger* loggerInstance) {
        logger = loggerInstance;
    }

    static std::string createHeader(const std::string& payload) {
        uint64_t seqNum = ++sequenceCounter;

        std::stringstream headerSS;
        headerSS << "HEADER\n"
            << "SEQ_NUM=" << seqNum << "\n"
            << "TIMESTAMP=2025-04-05T12:00:00Z\n"  // Fixed timestamp for testing
            << "PAYLOAD_SIZE=" << payload.length() << "\n"
            << "END_HEADER\n";

        return headerSS.str();
    }

    static void resetSequenceCounter() {
        sequenceCounter = 0;
    }
};
uint64_t TestablePacketHeader::sequenceCounter = 0;
TestablePacketLogger* TestablePacketHeader::logger = nullptr;

struct TestableConnectionRequest {
    std::string clientId;

    std::string serialize() const {
        std::string payload = "REQUEST_CONNECTION\nCLIENT_ID=" + clientId + "\n";
        std::string header = TestablePacketHeader::createHeader(payload);
        return header + payload;
    }

    static bool parseResponse(const std::string& response) {
        return response.find("CONNECTION_ACCEPTED") != std::string::npos;
    }

    static std::string getRejectReason(const std::string& response) {
        std::string reason = "Unknown reason";
        size_t reasonPos = response.find("REASON=");

        if (reasonPos != std::string::npos) {
            reason = response.substr(reasonPos + 7U);
        }

        return reason;
    }
};

class TestableNotamClient {
private:
    SOCKET clientSocket;
    bool initialized;
    std::string clientId;
    bool isConnected;
    bool isApproved;
    TestablePacketLogger logger;

public:
    TestableNotamClient(const std::string& id = "F1234") :
        clientSocket(INVALID_SOCKET),
        initialized(false),
        clientId(id),
        isConnected(false),
        isApproved(false) {

        logger.initialize();
        TestablePacketHeader::setLogger(&logger);
        logger.logEvent("TestableNotamClient initialized");

        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            logger.logEvent("WSAStartup failed with error code: " + std::to_string(result));
            return;
        }
        initialized = true;
        logger.logEvent("Winsock initialized successfully");
    }

    ~TestableNotamClient() {
        disconnect();
        if (initialized) {
            WSACleanup();
            logger.logEvent("Winsock cleaned up");
        }
    }

    std::string getClientId() const {
        return clientId;
    }

    void setClientId(const std::string& id) {
        clientId = id;
    }

    ServerStateMachine connect(const std::string& serverIP, int port) {
        if (!initialized) {
            logger.logEvent("ERROR: Winsock not initialized");
            return ServerStateMachine::WINSOCK_ERROR;
        }

        if (port <= 0 || port > 65535) {
            logger.logEvent("ERROR: Invalid port number");
            return ServerStateMachine::CONNECTION_ERROR;
        }

        logger.logEvent("Attempting to connect to server at " + serverIP + ":" + std::to_string(port));

        clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (clientSocket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            logger.logEvent("Socket creation failed with error: " + std::to_string(error));
            return ServerStateMachine::CONNECTION_ERROR;
        }

        sockaddr_in serverAddr;
        ZeroMemory(&serverAddr, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        if (port > 0 && port <= UINT16_MAX) {
            serverAddr.sin_port = htons(static_cast<uint16_t>(port));
        }
        else {
            logger.logEvent("Port number out of valid range");
            return ServerStateMachine::CONNECTION_ERROR;
        }

        int convResult = inet_pton(AF_INET, serverIP.c_str(), &(serverAddr.sin_addr));
        if (convResult != 1) {
            logger.logEvent("Invalid address format: " + serverIP);
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            return ServerStateMachine::CONNECTION_ERROR;
        }

        if (::connect(clientSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
            int error = WSAGetLastError();
            logger.logEvent("Connection failed with error: " + std::to_string(error));
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            return ServerStateMachine::CONNECTION_ERROR;
        }

        isConnected = true;
        logger.logEvent("Connected to server successfully");
        return ServerStateMachine::SUCCESS;
    }

    ServerStateMachine requestConnection() {
        if (clientSocket == INVALID_SOCKET || !isConnected) {
            logger.logEvent("ERROR: Not connected to server");
            return ServerStateMachine::CONNECTION_ERROR;
        }

        TestableConnectionRequest request;
        request.clientId = clientId;
        std::string requestMessage = request.serialize();

        logger.logEvent("Sending connection request for client ID: " + clientId);
        logger.logSentPacket(requestMessage, "Connection Request");

        int sendResult = send(clientSocket, requestMessage.c_str(), static_cast<int>(requestMessage.length()), 0);
        if (sendResult == SOCKET_ERROR) {
            int error = WSAGetLastError();
            logger.logEvent("Send connection request failed with error: " + std::to_string(error));
            return ServerStateMachine::SEND_ERROR;
        }

        std::string response = receiveResponse();
        logger.logReceivedPacket(response, "Connection Response");

        if (!TestableConnectionRequest::parseResponse(response)) {
            std::string reason = TestableConnectionRequest::getRejectReason(response);
            logger.logEvent("Connection request denied: " + reason);
            return ServerStateMachine::CONNECTION_REQUEST_DENIED;
        }

        isApproved = true;
        logger.logEvent("Connection request approved by server");
        return ServerStateMachine::SUCCESS;
    }

    ServerStateMachine sendFlightInformation(const std::string& flightId, const std::string& departure, const std::string& arrival) {
        if (clientSocket == INVALID_SOCKET || !isConnected || !isApproved) {
            logger.logEvent("ERROR: Not connected or approved by server");
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Create a simplified flight plan packet for testing
        std::string flightPlan = std::string("FLIGHT_PLAN\n") +
            "FLIGHT_NUMBER=" + flightId + "\n" +
            "AIRCRAFT_REG=TEST-REG\n" +
            "AIRCRAFT_TYPE=TEST-TYPE\n" +
            "OPERATOR=TEST-OPERATOR\n" +
            "DEP=" + departure + "\n" +
            "ARR=" + arrival + "\n";

        std::string flightPlanWithHeader = TestablePacketHeader::createHeader(flightPlan) + flightPlan;
        logger.logSentPacket(flightPlanWithHeader, "Flight Plan");

        int sendResult = send(clientSocket, flightPlanWithHeader.c_str(), static_cast<int>(flightPlanWithHeader.length()), 0);
        if (sendResult == SOCKET_ERROR) {
            int error = WSAGetLastError();
            logger.logEvent("Send flight plan failed with error: " + std::to_string(error));
            return ServerStateMachine::SEND_ERROR;
        }

        return ServerStateMachine::SUCCESS;
    }

    std::string receiveResponse() {
        if (clientSocket == INVALID_SOCKET || !isConnected) {
            logger.logEvent("ERROR: Not connected to server");
            return "ERROR: Not connected to server";
        }

        char buffer[BUFFER_SIZE];
        ZeroMemory(buffer, sizeof(buffer));

        logger.logEvent("Waiting to receive response from server");

        int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived == SOCKET_ERROR) {
            int error = WSAGetLastError();
            logger.logEvent("Receive failed with error: " + std::to_string(error));
            return "ERROR: Failed to receive response";
        }

        buffer[bytesReceived] = '\0';
        std::string response(buffer);

        logger.logReceivedPacket(response, "Server Response (" + std::to_string(bytesReceived) + " bytes)");
        return response;
    }

    void disconnect() {
        if (clientSocket != INVALID_SOCKET) {
            logger.logEvent("Disconnecting from server");
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            isConnected = false;
            isApproved = false;
            logger.logEvent("Disconnected from server");
        }
    }

    bool isInitialized() const {
        return initialized;
    }

    bool isConnectionApproved() const {
        return isApproved;
    }

    std::string getLogContent() const {
        return logger.getLogContent();
    }

    void clearLogs() {
        logger.clear();
    }
};

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ClientUnitTests
{
    TEST_CLASS(ClientUnitTests)
    {
    public:
        TEST_METHOD_INITIALIZE(SetUp)
        {
            // Reset for each test
            TestablePacketHeader::resetSequenceCounter();
        }

        TEST_METHOD(TestClientInitialization)
        {
            // Test that client initializes properly
            TestableNotamClient client;
            Assert::IsTrue(client.isInitialized(), L"Client should initialize successfully");
            Assert::IsFalse(client.isConnectionApproved(), L"Client should not be approved after initialization");
        }

        TEST_METHOD(TestGetClientId)
        {
            // Test getClientId method
            TestableNotamClient client("TEST1234");
            Assert::AreEqual(std::string("TEST1234"), client.getClientId(), L"Client ID should match what was set");
        }

        TEST_METHOD(TestPacketHeaderCreation)
{
    // Test packet header creation
    std::string payload = "TEST_PAYLOAD";
    std::string header = TestablePacketHeader::createHeader(payload);

    // Dynamic payload size calculation
    std::string expectedSizeStr = "PAYLOAD_SIZE=" + std::to_string(payload.size());

    // Check header format
    Assert::IsTrue(header.find("HEADER") != std::string::npos, L"Header should contain HEADER marker");
    Assert::IsTrue(header.find("SEQ_NUM=1") != std::string::npos, L"Header should contain sequence number");
    Assert::IsTrue(header.find("TIMESTAMP=") != std::string::npos, L"Header should contain timestamp");
    Assert::IsTrue(header.find(expectedSizeStr) != std::string::npos, L"Header should contain correct payload size");
    Assert::IsTrue(header.find("END_HEADER") != std::string::npos, L"Header should contain END_HEADER marker");
}

        TEST_METHOD(TestConnectionRequestSerialization)
        {
            // Test connection request serialization
            TestableConnectionRequest request;
            request.clientId = "TEST1234";
            std::string serialized = request.serialize();

            // Check serialized format
            Assert::IsTrue(serialized.find("REQUEST_CONNECTION") != std::string::npos, L"Request should contain REQUEST_CONNECTION");
            Assert::IsTrue(serialized.find("CLIENT_ID=TEST1234") != std::string::npos, L"Request should contain correct client ID");
        }

        TEST_METHOD(TestConnectionResponseParsing)
        {
            // Test accepted response parsing
            std::string acceptedResponse = "CONNECTION_ACCEPTED\nSERVER_ID=TEST_SERVER\n";
            bool accepted = TestableConnectionRequest::parseResponse(acceptedResponse);
            Assert::IsTrue(accepted, L"Should parse accepted response correctly");

            // Test rejected response parsing
            std::string rejectedResponse = "CONNECTION_REJECTED\nREASON=Server full\n";
            bool rejected = TestableConnectionRequest::parseResponse(rejectedResponse);
            Assert::IsFalse(rejected, L"Should parse rejected response correctly");

            // Test reason extraction
            std::string reason = TestableConnectionRequest::getRejectReason(rejectedResponse);
            Assert::AreEqual(std::string("Server full\n"), reason, L"Should extract rejection reason correctly");
        }

        TEST_METHOD(TestInvalidPortConnection)
        {
            // Test connection with invalid port
            TestableNotamClient client;
            ServerStateMachine result = client.connect("127.0.0.1", 0);
            Assert::AreEqual((int)ServerStateMachine::CONNECTION_ERROR, (int)result, L"Should reject invalid port number");

            result = client.connect("127.0.0.1", 65536);
            Assert::AreEqual((int)ServerStateMachine::CONNECTION_ERROR, (int)result, L"Should reject port number > 65535");
        }

        TEST_METHOD(TestInvalidAddressConnection)
        {
            // Test connection with invalid address
            TestableNotamClient client;
            ServerStateMachine result = client.connect("invalid_ip", 8081);
            Assert::AreEqual((int)ServerStateMachine::CONNECTION_ERROR, (int)result, L"Should reject invalid IP address");
        }

       

       

       

      

        TEST_METHOD(TestSequenceNumberIncrement)
        {
            // Reset sequence counter
            TestablePacketHeader::resetSequenceCounter();

            // Create three headers and verify sequence numbers
            std::string header1 = TestablePacketHeader::createHeader("TEST1");
            std::string header2 = TestablePacketHeader::createHeader("TEST2");
            std::string header3 = TestablePacketHeader::createHeader("TEST3");

            Assert::IsTrue(header1.find("SEQ_NUM=1") != std::string::npos, L"First header should have sequence number 1");
            Assert::IsTrue(header2.find("SEQ_NUM=2") != std::string::npos, L"Second header should have sequence number 2");
            Assert::IsTrue(header3.find("SEQ_NUM=3") != std::string::npos, L"Third header should have sequence number 3");
        }
        TEST_METHOD(TestReceiveResponseWithoutConnection)
        {
            // Test receiving response when not connected
            TestableNotamClient client;
            std::string response = client.receiveResponse();
            Assert::IsTrue(response.find("ERROR: Not connected to server") != std::string::npos,
                L"Should report error when receiving without connection");
        }
        TEST_METHOD(TestSendFlightInfoWithoutApproval)
        {
            // Test sending flight info without prior approval
            TestableNotamClient client;
            client.connect("127.0.0.1", 8087);

            ServerStateMachine result = client.sendFlightInformation("FL123", "KLAX", "KSFO");
            Assert::AreEqual((int)ServerStateMachine::CONNECTION_ERROR, (int)result,
                L"Should reject sending flight info without approval");

            client.disconnect();
        }
       
        TEST_METHOD(TestLogging)
        {
            // Test logging functionality
            TestablePacketLogger logger;
            logger.initialize();

            logger.logEvent("Test Event");
            logger.logSentPacket("TEST_PACKET", "Test Packet");
            logger.logReceivedPacket("RESPONSE_PACKET", "Test Response");

            std::string logs = logger.getLogContent();
            Assert::IsTrue(logs.find("Test Event") != std::string::npos, L"Log should contain event");
            Assert::IsTrue(logs.find("SENT_PACKET") != std::string::npos, L"Log should contain sent packet");
            Assert::IsTrue(logs.find("TEST_PACKET") != std::string::npos, L"Log should contain packet content");
            Assert::IsTrue(logs.find("RECEIVED_PACKET") != std::string::npos, L"Log should contain received packet");
            Assert::IsTrue(logs.find("RESPONSE_PACKET") != std::string::npos, L"Log should contain response content");
        }
    };
}