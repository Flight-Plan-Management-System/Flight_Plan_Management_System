#include <iostream>
#include <string>
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

static const int DEFAULT_SERVER_PORT = 8081;
static const int MAX_RETRY_ATTEMPTS = 3;
static const int RETRY_DELAY_SECONDS = 5;
static const int FULL_SERVER_RETRY_DELAY_SECONDS = 30;
static const int PROGRESS_BAR_WIDTH = 50;
static const int SPINNER_INTERVAL_MS = 250;

// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")


static const size_t BUFFER_SIZE = 4096U;

enum class ErrorCode {
    SUCCESS = 0,
    WINSOCK_ERROR = 1,
    CONNECTION_ERROR = 2,
    SEND_ERROR = MAX_RETRY_ATTEMPTS,
    RECEIVE_ERROR = 4,
    CONNECTION_REQUEST_DENIED = RETRY_DELAY_SECONDS
};


class PacketLogger {
private:
    std::ofstream logFile;
    bool isInitialized;

public:
    PacketLogger() : isInitialized(false) {
    }

    ~PacketLogger() {
        if (isInitialized) {
            logFile << "\n----- LOG SESSION ENDED -----\n\n";
            logFile.close();
        }
    }

    bool initialize(const std::string& filename = "packet_log.txt") {
        logFile.open(filename, std::ios::out | std::ios::app);
        if (!logFile.is_open()) {
            std::cerr << "Failed to open log file: " << filename << std::endl;
            return false;
        }

        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        struct tm timeInfo;
     
#ifdef _WIN32
        localtime_s(&timeInfo, &now_c);
#else
        localtime_r(&now_c, &timeInfo);
#endif

        logFile << "\n=================================================\n";
        logFile << "NOTAM CLIENT LOG SESSION: " << std::put_time(&timeInfo, "%Y-%m-%d %H:%M:%S") << "\n";
        logFile << "=================================================\n";

        isInitialized = true;
        return true;
    }

    void logSentPacket(const std::string& packet, const std::string& description = "") {
        if (!isInitialized) {
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        struct tm timeInfo;
#ifdef _WIN32
        localtime_s(&timeInfo, &now_c);
#else
        localtime_r(&now_c, &timeInfo);
#endif

        logFile << "\n----- SENT PACKET [" << std::put_time(&timeInfo, "%H:%M:%S") << "] -----\n";
        if (!description.empty()) {
            logFile << "Description: " << description << "\n";
        }
        logFile << packet << "\n";
        logFile << "----- END OF SENT PACKET -----\n";

      
        logFile.flush();
    }

    void logReceivedPacket(const std::string& packet, const std::string& description = "") {
        if (!isInitialized) {
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        struct tm timeInfo;
#ifdef _WIN32
        localtime_s(&timeInfo, &now_c);
#else
        localtime_r(&now_c, &timeInfo);
#endif

        logFile << "\n----- RECEIVED PACKET [" << std::put_time(&timeInfo, "%H:%M:%S") << "] -----\n";
        if (!description.empty()) {
            logFile << "Description: " << description << "\n";
        }
        logFile << packet << "\n";
        logFile << "----- END OF RECEIVED PACKET -----\n";

        
        logFile.flush();
    }

    void logEvent(const std::string& eventDescription) {
        if (!isInitialized) {
            return;
        }

        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        struct tm timeInfo;
#ifdef _WIN32
        localtime_s(&timeInfo, &now_c);
#else
        localtime_r(&now_c, &timeInfo);
#endif

        logFile << "[" << std::put_time(&timeInfo, "%H:%M:%S") << "] " << eventDescription << "\n";
        logFile.flush();
    }
};

class PacketHeader {
private:
    static std::atomic<uint64_t> sequenceCounter;
    static PacketLogger* logger;

public:
    static void setLogger(PacketLogger* loggerInstance) {
        logger = loggerInstance;
    }

    static std::string createHeader(const std::string& payload) {
      
        uint64_t seqNum = ++sequenceCounter;

        
        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        struct tm timeInfo;
#ifdef _WIN32
        localtime_s(&timeInfo, &now_c);
#else
        localtime_r(&now_c, &timeInfo);
#endif

        std::stringstream timestampSS;
        timestampSS << std::put_time(&timeInfo, "%Y-%m-%dT%H:%M:%S.%fZ");

        
        size_t payloadSize = payload.length();

      
        std::stringstream headerSS;
        headerSS << "HEADER\n"
            << "SEQ_NUM=" << seqNum << "\n"
            << "TIMESTAMP=" << timestampSS.str() << "\n"
            << "PAYLOAD_SIZE=" << payloadSize << "\n"
            << "END_HEADER\n";

      
        std::cout << "Sending Packet Header:" << std::endl;
        std::cout << headerSS.str();

        return headerSS.str();
    }
};

std::atomic<uint64_t> PacketHeader::sequenceCounter(0U);
PacketLogger* PacketHeader::logger = nullptr;

struct ConnectionRequest {
    std::string clientId;

    std::string serialize() const {
        std::string payload = "REQUEST_CONNECTION\nCLIENT_ID=" + clientId + "\n";
        std::string header = PacketHeader::createHeader(payload);
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


class FlightNumberGenerator {
private:
    std::mt19937 rng;
    std::uniform_int_distribution<int> flightNumDist;

public:
    FlightNumberGenerator() {

        std::random_device rd;
        rng.seed(rd());

        flightNumDist = std::uniform_int_distribution<int>(100, 4999);
    }

    std::string generateFlightNumber() {

        return "F" + std::to_string(flightNumDist(rng));
    }
};

class NotamClient {
private:
    SOCKET clientSocket;
    bool initialized;
    std::string clientId;
    bool isConnected;
    bool isApproved;
    PacketLogger logger;


    std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        struct tm timeInfo;
#ifdef _WIN32
        localtime_s(&timeInfo, &now_c);
#else
        localtime_r(&now_c, &timeInfo);
#endif

        std::stringstream ss;
        ss << std::put_time(&timeInfo, "%Y-%m-%dT%H:%M:%SZ");
        return ss.str();
    }

public:
    NotamClient() :
        clientSocket(INVALID_SOCKET),
        initialized(false),
        isConnected(false),
        isApproved(false) {


        if (logger.initialize("notam_client_log.txt")) {
            PacketHeader::setLogger(&logger);
            logger.logEvent("NotamClient initialized");
        }


        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "WSAStartup failed: " << result << std::endl;
            logger.logEvent("WSAStartup failed with error code: " + std::to_string(result));
            return;
        }
        initialized = true;
        logger.logEvent("Winsock initialized successfully");

        FlightNumberGenerator generator;
        clientId = generator.generateFlightNumber();
        logger.logEvent("Generated client ID: " + clientId);
    }

    ~NotamClient() {
        disconnect();
        if (initialized) {
            WSACleanup();
            logger.logEvent("Winsock cleaned up");
        }
    }

    std::string getClientId() const {
        return clientId;
    }

    ErrorCode connect(const std::string& serverIP, int port) {
        if (!initialized) {
            logger.logEvent("ERROR: Winsock not initialized");
            return ErrorCode::WINSOCK_ERROR;
        }


        if (port <= 0 || port > 65535) {
            logger.logEvent("ERROR: Invalid port number");
            return ErrorCode::CONNECTION_ERROR;
        }

        logger.logEvent("Attempting to connect to server at " + serverIP + ":" + std::to_string(port));


        clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (clientSocket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            std::cerr << "Socket creation failed: " << error << std::endl;
            logger.logEvent("Socket creation failed with error: " + std::to_string(error));
            return ErrorCode::CONNECTION_ERROR;
        }


        sockaddr_in serverAddr;
        ZeroMemory(&serverAddr, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        if (port > 0 && port <= UINT16_MAX) {
            serverAddr.sin_port = htons(static_cast<uint16_t>(port));
        }
        else {
            logger.logEvent("Port number out of valid range");
            return ErrorCode::CONNECTION_ERROR;
        }


        int convResult = inet_pton(AF_INET, serverIP.c_str(), &(serverAddr.sin_addr));
        if (convResult != 1) {
            std::cerr << "Invalid address format: " << serverIP << std::endl;
            logger.logEvent("Invalid address format: " + serverIP);
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            return ErrorCode::CONNECTION_ERROR;
        }


        if (::connect(clientSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
            int error = WSAGetLastError();
            std::cerr << "Connection failed: " << error << std::endl;
            logger.logEvent("Connection failed with error: " + std::to_string(error));
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            return ErrorCode::CONNECTION_ERROR;
        }

        isConnected = true;
        logger.logEvent("Connected to server successfully");
        return ErrorCode::SUCCESS;
    }


    ErrorCode requestConnection() {
        if (clientSocket == INVALID_SOCKET || !isConnected) {
            logger.logEvent("ERROR: Not connected to server");
            return ErrorCode::CONNECTION_ERROR;
        }


        ConnectionRequest request;
        request.clientId = clientId;
        std::string requestMessage = request.serialize();

        logger.logEvent("Sending connection request for client ID: " + clientId);
        logger.logSentPacket(requestMessage, "Connection Request");


        int sendResult = send(clientSocket, requestMessage.c_str(), static_cast<int>(requestMessage.length()), 0);
        if (sendResult == SOCKET_ERROR) {
            int error = WSAGetLastError();
            std::cerr << "Send connection request failed: " << error << std::endl;
            logger.logEvent("Send connection request failed with error: " + std::to_string(error));
            return ErrorCode::SEND_ERROR;
        }


        std::string response = receiveResponse();
        logger.logReceivedPacket(response, "Connection Response");


        if (!ConnectionRequest::parseResponse(response)) {
            std::string reason = ConnectionRequest::getRejectReason(response);
            std::cerr << "Connection request denied by server: " << reason << std::endl;
            logger.logEvent("Connection request denied: " + reason);
            return ErrorCode::CONNECTION_REQUEST_DENIED;
        }

        isApproved = true;
        logger.logEvent("Connection request approved by server");
        return ErrorCode::SUCCESS;
    }


    ErrorCode sendExtendedFlightInformation(const std::string& flightId, const std::string& departure, const std::string& arrival) {
        if (clientSocket == INVALID_SOCKET || !isConnected || !isApproved) {
            logger.logEvent("ERROR: Not connected or approved by server");
            return ErrorCode::CONNECTION_ERROR;
        }

        logger.logEvent("Sending extended flight information for flight: " + flightId +
            " from " + departure + " to " + arrival);


        std::string aircraftReg, aircraftType, operatorName, route, cruiseAlt, speed, eobt, etd, eta;
        std::cout << "Enter Aircraft Registration: ";
        std::getline(std::cin, aircraftReg);
        std::cout << "Enter Aircraft Type: ";
        std::getline(std::cin, aircraftType);
        std::cout << "Enter Operator Name: ";
        std::getline(std::cin, operatorName);
        std::cout << "Enter Route: ";
        std::getline(std::cin, route);
        std::cout << "Enter Cruise Altitude: ";
        std::getline(std::cin, cruiseAlt);
        std::cout << "Enter Speed: ";
        std::getline(std::cin, speed);
        std::cout << "Enter EOBT (Estimated Off-Block Time): ";
        std::getline(std::cin, eobt);
        std::cout << "Enter ETD (Estimated Time of Departure): ";
        std::getline(std::cin, etd);
        std::cout << "Enter ETA (Estimated Time of Arrival): ";
        std::getline(std::cin, eta);


        std::string flightPlan = std::string("FLIGHT_PLAN\n") +
            "FLIGHT_NUMBER=" + flightId + "\n" +
            "AIRCRAFT_REG=" + aircraftReg + "\n" +
            "AIRCRAFT_TYPE=" + aircraftType + "\n" +
            "OPERATOR=" + operatorName + "\n" +
            "DEP=" + departure + "\n" +
            "ARR=" + arrival + "\n" +
            "LAYOVER=CYUL\n" +
            "ROUTE=" + route + "\n" +
            "CRUISE_ALT=" + cruiseAlt + "\n" +
            "SPEED=" + speed + "\n" +
            "EOBT=" + eobt + "\n" +
            "ETD=" + etd + "\n" +
            "ETA=" + eta + "\n";

        std::string flightPlanWithHeader = PacketHeader::createHeader(flightPlan) + flightPlan;
        logger.logSentPacket(flightPlanWithHeader, "Flight Plan");

        int sendResult = send(clientSocket, flightPlanWithHeader.c_str(), static_cast<int>(flightPlanWithHeader.length()), 0);
        if (sendResult == SOCKET_ERROR) {
            int error = WSAGetLastError();
            std::cerr << "Send flight plan failed: " << error << std::endl;
            logger.logEvent("Send flight plan failed with error: " + std::to_string(error));
            return ErrorCode::SEND_ERROR;
        }


        std::this_thread::sleep_for(std::chrono::milliseconds(500));


        std::string totalFlightTime, fuelOnBoard, estimatedFuelBurn, totalWeight, pic, remarks;
        std::cout << "Enter Total Flight Time: ";
        std::getline(std::cin, totalFlightTime);
        std::cout << "Enter Fuel On Board: ";
        std::getline(std::cin, fuelOnBoard);
        std::cout << "Enter Estimated Fuel Burn: ";
        std::getline(std::cin, estimatedFuelBurn);
        std::cout << "Enter Total Weight: ";
        std::getline(std::cin, totalWeight);
        std::cout << "Enter PIC (Pilot in Command): ";
        std::getline(std::cin, pic);
        std::cout << "Enter Remarks: ";
        std::getline(std::cin, remarks);


        std::string flightLog = std::string("FLIGHT_LOG\n") +
            "FLIGHT_NUMBER=" + flightId + "\n" +
            "TOTAL_FLIGHT_TIME=" + totalFlightTime + "\n" +
            "FUEL_ON_BOARD=" + fuelOnBoard + "\n" +
            "ESTIMATED_FUEL_BURN=" + estimatedFuelBurn + "\n" +
            "TOTAL_WEIGHT=" + totalWeight + "\n" +
            "PIC=" + pic + "\n" +
            "REMARKS=" + remarks + "\n";

        std::string flightLogWithHeader = PacketHeader::createHeader(flightLog) + flightLog;
        logger.logSentPacket(flightLogWithHeader, "Flight Log");


        sendResult = send(clientSocket, flightLogWithHeader.c_str(), static_cast<int>(flightLogWithHeader.length()), 0);
        if (sendResult == SOCKET_ERROR) {
            int error = WSAGetLastError();
            std::cerr << "Send flight log failed: " << error << std::endl;
            logger.logEvent("Send flight log failed with error: " + std::to_string(error));
            return ErrorCode::SEND_ERROR;
        }

        logger.logEvent("Flight information sent successfully");
        return ErrorCode::SUCCESS;
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
            std::cerr << "Receive failed: " << error << std::endl;
            logger.logEvent("Receive failed with error: " + std::to_string(error));
            return "ERROR: Failed to receive response";
        }


        buffer[bytesReceived] = '\0';
        std::string response(buffer);

        logger.logReceivedPacket(response, "Server Response (" + std::to_string(bytesReceived) + " bytes)");
        return response;
    }

    bool retryConnection(const std::string& serverIP, int port, int maxRetries = 3) {

        if (maxRetries <= 0) {
            logger.logEvent("ERROR: Invalid maxRetries value");
            return false;
        }


        if (port <= 0 || port > 65535) {
            logger.logEvent("ERROR: Invalid port number");
            return false;
        }

        logger.logEvent("Beginning retry connection sequence. Max retries: " + std::to_string(maxRetries));

        for (int retry = 0; retry < maxRetries; ++retry) {

            if (isConnected) {
                disconnect();
            }

            std::cout << "Attempting connection retry " << (retry + 1) << " of " << maxRetries << "..." << std::endl;
            logger.logEvent("Attempting connection retry " + std::to_string(retry + 1) +
                " of " + std::to_string(maxRetries));


            if (connect(serverIP, port) == ErrorCode::SUCCESS) {

                if (requestConnection() == ErrorCode::SUCCESS) {
                    std::cout << "Connection and approval successful on retry " << (retry + 1) << std::endl;
                    logger.logEvent("Connection and approval successful on retry " + std::to_string(retry + 1));
                    return true;
                }


                std::cout << "Server is full. Waiting 30 seconds before retry..." << std::endl;
                logger.logEvent("Server is full. Waiting 30 seconds before retry...");
                disconnect();
                std::this_thread::sleep_for(std::chrono::seconds(FULL_SERVER_RETRY_DELAY_SECONDS));
            }
            else {

                std::cout << "Connection failed. Waiting 5 seconds before retry..." << std::endl;
                logger.logEvent("Connection failed. Waiting 5 seconds before retry...");
                std::this_thread::sleep_for(std::chrono::seconds(RETRY_DELAY_SECONDS));
            }
        }

        std::cout << "Maximum retry attempts reached. Could not connect to server." << std::endl;
        logger.logEvent("Maximum retry attempts reached. Could not connect to server.");
        return false;
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

    bool isConnectionApproved() const {
        return isApproved;
    }
};

