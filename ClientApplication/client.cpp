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
#include <cstdint>  // For fixed-width integer types (MISRA compliance)
#include <limits>   // For numeric limits

// For TCP/IP sockets (Windows)
#include <WinSock2.h>
#include <WS2tcpip.h>

namespace Constants {
    // Define buffer size as a constant for safety and maintainability
    static const size_t BUFFER_SIZE = 4096U;

    static const int DEFAULT_SERVER_PORT = 8081;
    static const int MAX_RETRY_ATTEMPTS = 3;
    static const int RETRY_DELAY_SECONDS = 5;
    static const int FULL_SERVER_RETRY_DELAY_SECONDS = 30;
    static const int PROGRESS_BAR_WIDTH = 50;
    static const int SPINNER_INTERVAL_MS = 250;

}

// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

// UI Helper Functions

namespace UI_helper {

    void printSuccess(const std::string& message) {
        std::cout << "> " << message << std::endl;
    }

    void printError(const std::string& message) {
        std::cout << "X " << message << std::endl;
    }

    void clearScreen() {
#ifdef _WIN32
        HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hStdOut != INVALID_HANDLE_VALUE) {
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            DWORD count;
            DWORD cellCount;

            if (GetConsoleScreenBufferInfo(hStdOut, &csbi)) {

                COORD homeCoords = { 0, 0 };

                cellCount = csbi.dwSize.X * csbi.dwSize.Y;
                if (FillConsoleOutputCharacter(hStdOut, static_cast<TCHAR>(' '), cellCount, homeCoords, &count) == 0) {
                    printError("Failed to fill console output character buffer.");
                }
                if (FillConsoleOutputAttribute(hStdOut, csbi.wAttributes, cellCount, homeCoords, &count) == 0) {
                    printError("Failed to fill console output attribute buffer.");
                }
                if (SetConsoleCursorPosition(hStdOut, homeCoords) == 0) {
                    printError("Failed to set console cursor position.");
                }
            }
        }
#else
        // For non-Windows platforms, use ANSI escape codes
        std::cout << "\033[2J\033[1;1H";
#endif
    }

    void printHeader() {
        clearScreen(); // Clear screen in a platform-independent way
        std::cout << "-------------------------------------------------------------\n";
        std::cout << "|                 NOTAM CLIENT APPLICATION                   |\n";
        std::cout << "-------------------------------------------------------------\n\n";
    }

    void printProgressBar(int percentage) {
        // Validate percentage range
        if (percentage < 0) {
            percentage = 0;
        }
        if (percentage > 100) {
            percentage = 100;
        }

        int barWidth = Constants::PROGRESS_BAR_WIDTH;
        std::cout << "[";
        int pos = barWidth * percentage / 100;
        for (int i = 0; i < barWidth; ++i) {
            if (i < pos) {
                std::cout << "=";
            }
            else if (i == pos) {
                std::cout << ">";
            }
            else {
                std::cout << " ";
            }
        }
        std::cout << "] " << percentage << " %\r";
        if (!std::cout.flush()) {
            printError("Failed to flush std::cout.");
        }
    }

    void showSpinner(const std::string& message, int seconds) {
        if (seconds <= 0) {
            return; // Avoid negative or zero seconds
        }
        else {
            const char spinner[] = { '|', '/', '-', '\\' };
            int i = 0;

            std::cout << message;

            for (int j = 0; j < seconds * 4; ++j) {
                std::cout << " " << spinner[i % 4] << "\r";
                std::cout << message;
                if (!std::cout.flush()) {
                    printError("Failed to flush std::cout.");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(Constants::SPINNER_INTERVAL_MS));
                ++i;
            }
            std::cout << std::endl;
        }
    }

    void printSection(const std::string& title) {
        std::cout << "\n-------------------------------------------------------------\n";
        std::cout << "|" << std::left << std::setw(51) << title << "        |\n";
        std::cout << "-------------------------------------------------------------\n";
    }

    void printInfo(const std::string& message) {
        std::cout << "> " << message << std::endl;
    }

    std::string getInput(const std::string& prompt) {
        std::string input;
        std::cout << prompt;
        if (!std::getline(std::cin, input)) {
            printError("Failed to read input.");
        }
        return input;
    }

    char getCharInput(const std::string& prompt) {
        std::string input;
        std::cout << prompt;
        if (!std::getline(std::cin, input)) {
            printError("Failed to read input.");
        }
        return input.empty() ? '\0' : input[0];
    }

    int getIntInput(const std::string& prompt) {
        std::string input;
        int value = 0;
        bool validInput = false;

        while (!validInput) {
            std::cout << prompt;
            if (!std::getline(std::cin, input)) {
                printError("Failed to read input.");
            }

            try {
                size_t pos = 0;
                // Use std::stoi with error checking
                value = std::stoi(input, &pos);

                // Check if the entire string was consumed
                if (pos == input.size()) {
                    validInput = true;
                }
                else {
                    printError("Invalid input. Please enter a valid number.");
                }
            }
            catch (const std::invalid_argument&) {
                printError("Invalid input. Please enter a number.");
            }
            catch (const std::out_of_range&) {
                printError("Number out of range. Please enter a smaller number.");
            }
        }

        return value;
    }

    void displayNotamInfo(const std::string& response, const std::string& flightId)
    {
        UI_helper::printSection("NOTAM INFORMATION");

        std::cout << "-------------------------------------------------------------\n";
        std::cout << "|             NOTAM INFO FOR FLIGHT " << std::left << std::setw(17) << flightId << "        |\n";
        std::cout << "-------------------------------------------------------------\n";

        // Process response to display in a more formatted way
        std::istringstream responseStream(response);
        std::string line;

        while (std::getline(responseStream, line))
        {
            if (0 < line.length())
            {
                std::cout << " " << std::left << std::setw(52) << line << "\n";
            }
        }
    }
} 

namespace ServerUtilities {
    enum class ServerStateMachine {
        SUCCESS = 0,
        WINSOCK_ERROR = 1,
        CONNECTION_ERROR = 2,
        SEND_ERROR = Constants::MAX_RETRY_ATTEMPTS,
        RECEIVE_ERROR = 4,
        CONNECTION_REQUEST_DENIED = Constants::RETRY_DELAY_SECONDS
    };
}


namespace NotamClient
{

    // File logger class for packet data
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
            bool success = false;  // Track success status

            logFile.open(filename, std::ios::out | std::ios::app);
            if (!logFile.is_open()) {
                std::cerr << "Failed to open log file: " << filename << std::endl;
            }
            else {
                // Write header with timestamp
                auto now = std::chrono::system_clock::now();
                auto now_c = std::chrono::system_clock::to_time_t(now);
                struct tm timeInfo;

#ifdef _WIN32
                if (localtime_s(&timeInfo, &now_c) != 0) {
                    UI_helper::printError("Failed to convert time using localtime_s.");
                }
                else {
                    logFile << "\n=================================================\n";
                    logFile << "NOTAM CLIENT LOG SESSION: " << std::put_time(&timeInfo, static_cast<const char*>("%Y-%m-%d %H:%M:%S")) << "\n";
                    logFile << "=================================================\n";
                    success = true;  // Mark as successful
                }
#else
                if (localtime_r(&now_c, &timeInfo) == nullptr) {
                    printError("Failed to convert time using localtime_r.");
                }
                else {
                    logFile << "\n=================================================\n";
                    logFile << "NOTAM CLIENT LOG SESSION: " << std::put_time(&timeInfo, "%Y-%m-%d %H:%M:%S") << "\n";
                    logFile << "=================================================\n";
                    success = true;  // Mark as successful
                }
#endif
            }

            isInitialized = success;  // Set initialization status
            return success;  // Single return point
        }

        void logSentPacket(const std::string& packet, const std::string& description = "") {
            bool success = false;  // Track success status

            bool initializedFlag = isInitialized;  // Store the initialized state

            if (!initializedFlag) {
                UI_helper::printError("Log system not initialized.");
            }

            auto now = std::chrono::system_clock::now();
            auto now_c = std::chrono::system_clock::to_time_t(now);
            struct tm timeInfo;

#ifdef _WIN32
            if (localtime_s(&timeInfo, &now_c) != 0) {
                UI_helper::printError("Failed to convert time using localtime_s.");
            }
            else {
                success = true;
            }
#else
            if (localtime_r(&now_c, &timeInfo) == nullptr) {
                printError("Failed to convert time using localtime_r.");
            }
            else {
                success = true;
            }
#endif
            // Only proceed if time conversion was successful and system is initialized
            if (success && initializedFlag) {
                logFile << "\n----- SENT PACKET [" << std::put_time(&timeInfo, static_cast<const char*>("%H:%M:%S")) << "] -----\n";
                if (!description.empty()) {
                    logFile << "Description: " << description << "\n";
                }
                logFile << packet << "\n";
                logFile << "----- END OF SENT PACKET -----\n";

                if (!logFile.flush()) {
                    UI_helper::printError("Failed to flush logFile to disk.");
                }
            }
        }

        void logReceivedPacket(const std::string& packet, const std::string& description = "") {
            bool success = false;  // Track success status
            bool initializedFlag = isInitialized;  // Store the initialized state

            if (!initializedFlag) {
                UI_helper::printError("Log system not initialized.");
            }

            auto now = std::chrono::system_clock::now();
            auto now_c = std::chrono::system_clock::to_time_t(now);
            struct tm timeInfo;

#ifdef _WIN32
            if (localtime_s(&timeInfo, &now_c) != 0) {
                UI_helper::printError("Failed to convert time using localtime_s.");
            }
            else {
                success = true;
            }
#else
            if (localtime_r(&now_c, &timeInfo) == nullptr) {
                printError("Failed to convert time using localtime_r.");
            }
            else {
                success = true;
            }
#endif
            // Only proceed if time conversion was successful and system is initialized
            if (success && initializedFlag) {
                logFile << "\n----- RECEIVED PACKET [" << std::put_time(&timeInfo, static_cast<const char*>("%H:%M:%S")) << "] -----\n";
                if (!description.empty()) {
                    logFile << "Description: " << description << "\n";
                }
                logFile << packet << "\n";
                logFile << "----- END OF RECEIVED PACKET -----\n";

                if (!logFile.flush()) {
                    UI_helper::printError("Failed to flush logFile to disk.");
                }
            }
        }

        void logEvent(const std::string& eventDescription) {
            // Check if the logging system is initialized
            if (!isInitialized) {
                UI_helper::printError("Log system not initialized.");
            }
            else {
                auto now = std::chrono::system_clock::now();
                auto now_c = std::chrono::system_clock::to_time_t(now);
                struct tm timeInfo;

                bool success = false;

#ifdef _WIN32
                if (localtime_s(&timeInfo, &now_c) != 0) {
                    UI_helper::printError("Failed to convert time using localtime_s.");
                }
                else {
                    success = true;
                }
#else
                if (localtime_r(&now_c, &timeInfo) == nullptr) {
                    printError("Failed to convert time using localtime_r.");
                }
                else {
                    success = true;
                }
#endif
                // Only log if time conversion is successful
                if (success) {
                    logFile << "[" << std::put_time(&timeInfo, static_cast<const char*>("%H:%M:%S")) << "] " << eventDescription << "\n";

                    if (!logFile.flush()) {
                        UI_helper::printError("Failed to flush logFile to disk.");
                    }
                }
            }
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
            std::string result = "";  // Empty string in case of failure

            // Get current timestamp
            auto now = std::chrono::system_clock::now();
            auto now_c = std::chrono::system_clock::to_time_t(now);
            struct tm timeInfo;

#ifdef _WIN32
            if (localtime_s(&timeInfo, &now_c) != 0) {
                UI_helper::printError("Failed to convert time using localtime_s.");
                result = "Error: Failed to convert time";  // Set error string
            }
#else
            if (localtime_r(&now_c, &timeInfo) == nullptr) {
                printError("Failed to convert time using localtime_r.");
                result = "Error: Failed to convert time";  // Set error string
            }
#endif

            if (result.empty()) {  // Proceed only if there was no error
                uint64_t seqNum = ++sequenceCounter;

                std::stringstream timestampSS;
                timestampSS << std::put_time(&timeInfo, static_cast<const char*>("%Y-%m-%dT%H:%M:%SZ"));

                // Calculate payload size
                size_t payloadSize = payload.length();

                // Create header string
                std::stringstream headerSS;
                headerSS << "HEADER\n"
                    << "SEQ_NUM=" << seqNum << "\n"
                    << "TIMESTAMP=" << timestampSS.str() << "\n"
                    << "PAYLOAD_SIZE=" << payloadSize << "\n"
                    << "END_HEADER\n";

                // Print header information
                std::cout << "Sending Packet Header:" << std::endl;
                std::cout << headerSS.str();

                result = headerSS.str();
            }

            return result;
        }
    };

    // Initialize the static atomic sequence counter and logger
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
            return response.find(static_cast<const char*>("CONNECTION_ACCEPTED")) != std::string::npos;
        }

        static std::string getRejectReason(const std::string& response) {
            std::string reason = "Unknown reason";
            size_t reasonPos = response.find(static_cast<const char*>("REASON="));

            if (reasonPos != std::string::npos) {
                reason = response.substr(reasonPos + 7U); // +7 to skip "REASON="
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
            std::random_device rd;
            rng.seed(rd());
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
        SOCKET clientSocket;
        bool initialized;
        std::string clientId;
        bool isConnected;
        bool isApproved;
        PacketLogger logger;

        // Helper method to get current timestamp
        std::string getCurrentTimestamp() {
            std::string result = "";  //empty string in case of failure

            auto now = std::chrono::system_clock::now();
            auto now_c = std::chrono::system_clock::to_time_t(now);
            struct tm timeInfo;
#ifdef _WIN32
            if (localtime_s(&timeInfo, &now_c) != 0)
            {
                UI_helper::printError("Failed to convert time using localtime_s.");
                result = "Error: Failed to convert time";
            }
#else
            if (localtime_r(&now_c, &timeInfo) == nullptr)
            {
                printError("Failed to convert time using localtime_r.");
                result = "Error: Failed to convert time";
            }
#endif
            if (result.empty()) {  // Proceed only if there was no error

                std::stringstream ss;
                ss << std::put_time(&timeInfo, static_cast<const char*>("%Y-%m-%dT%H:%M:%SZ"));
                result = ss.str();
            }

            return result;
        }

    public:
        NotamClient() :
            clientSocket(INVALID_SOCKET),
            initialized(false),
            isConnected(false),
            isApproved(false) {

            // Initialize logger
            if (logger.initialize("notam_client_log.txt")) {
                PacketHeader::setLogger(&logger);
                logger.logEvent("NotamClient initialized");
            }

            // Initialize Winsock
            WSADATA wsaData;
            int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
            if (result != 0) {
                std::cerr << "WSAStartup failed: " << result << std::endl;
                logger.logEvent("WSAStartup failed with error code: " + std::to_string(result));
            }
            else {
                initialized = true;
                logger.logEvent("Winsock initialized successfully");
            }

            // Proceed with client ID generation if initialization was successful
            if (initialized) {
                FlightNumberGenerator generator;
                clientId = generator.generateFlightNumber();
                logger.logEvent("Generated client ID: " + clientId);
            }
        }

        ~NotamClient() {
            disconnect();

            if (initialized) {
                const int result = WSACleanup();
                if (result == SOCKET_ERROR) {
                    logger.logEvent("WSACleanup failed: " + std::to_string(WSAGetLastError()));
                }
                else {
                    logger.logEvent("Winsock cleaned up");
                }
            }
        }

        std::string getClientId() const {
            return clientId;
        }

        ServerUtilities::ServerStateMachine connect(const std::string& serverIP, int port) {
            ServerUtilities::ServerStateMachine result = ServerUtilities::ServerStateMachine::SUCCESS;  // Default result

            if (!initialized) {
                logger.logEvent("ERROR: Winsock not initialized");
                result = ServerUtilities::ServerStateMachine::WINSOCK_ERROR;
            }
            else if (port <= 0 || port > 65535) {
                logger.logEvent("ERROR: Invalid port number");
                result = ServerUtilities::ServerStateMachine::CONNECTION_ERROR;
            }
            else {
                logger.logEvent("Attempting to connect to server at " + serverIP + ":" + std::to_string(port));

                // Create a socket
                clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (clientSocket == INVALID_SOCKET) {
                    int error = WSAGetLastError();
                    std::cerr << "Socket creation failed: " << error << std::endl;
                    logger.logEvent("Socket creation failed with error: " + std::to_string(error));
                    result = ServerUtilities::ServerStateMachine::CONNECTION_ERROR;
                }
                else {
                    // Set up the server address
                    sockaddr_in serverAddr;
                    (void)ZeroMemory(&serverAddr, sizeof(serverAddr)); // Initialize structure to zero
                    serverAddr.sin_family = AF_INET;
                    if (port > 0 && port <= 65535) {
                        serverAddr.sin_port = htons(static_cast<uint16_t>(port));
                    }
                    else {
                        logger.logEvent("Port number out of valid range");
                        result = ServerUtilities::ServerStateMachine::CONNECTION_ERROR;
                    }

                    if (result == ServerUtilities::ServerStateMachine::SUCCESS) {
                        // Use inet_pton for safer IP address conversion
                        int convResult = inet_pton(AF_INET, serverIP.c_str(), &(serverAddr.sin_addr));
                        if (convResult != 1) {
                            std::cerr << "Invalid address format: " << serverIP << std::endl;
                            logger.logEvent("Invalid address format: " + serverIP);
                            if (closesocket(clientSocket) == SOCKET_ERROR) {
                                logger.logEvent("closesocket failed: " + std::to_string(WSAGetLastError()));
                            }
                            clientSocket = INVALID_SOCKET;
                            result = ServerUtilities::ServerStateMachine::CONNECTION_ERROR;
                        }
                        else {
                            // Connect to server
                            if (::connect(clientSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
                                int error = WSAGetLastError();
                                std::cerr << "Connection failed: " << error << std::endl;
                                logger.logEvent("Connection failed with error: " + std::to_string(error));
                                if (closesocket(clientSocket) == SOCKET_ERROR) {
                                    logger.logEvent("closesocket failed: " + std::to_string(WSAGetLastError()));
                                }
                                clientSocket = INVALID_SOCKET;
                                result = ServerUtilities::ServerStateMachine::CONNECTION_ERROR;
                            }
                            else {
                                isConnected = true;
                                logger.logEvent("Connected to server successfully");
                            }
                        }
                    }
                }
            }

            return result;
        }

        // Request connection approval from the server
        ServerUtilities::ServerStateMachine requestConnection() {
            ServerUtilities::ServerStateMachine result = ServerUtilities::ServerStateMachine::SUCCESS;  // Default result

            if (clientSocket == INVALID_SOCKET || !isConnected) {
                logger.logEvent("ERROR: Not connected to server");
                result = ServerUtilities::ServerStateMachine::CONNECTION_ERROR;
            }
            else {
                // Create connection request
                ConnectionRequest request;
                request.clientId = clientId;
                std::string requestMessage = request.serialize();

                logger.logEvent("Sending connection request for client ID: " + clientId);
                logger.logSentPacket(requestMessage, "Connection Request");

                // Send connection request
                int sendResult = send(clientSocket, requestMessage.c_str(), static_cast<int>(requestMessage.length()), 0);
                if (sendResult == SOCKET_ERROR) {
                    int error = WSAGetLastError();
                    std::cerr << "Send connection request failed: " << error << std::endl;
                    logger.logEvent("Send connection request failed with error: " + std::to_string(error));
                    result = ServerUtilities::ServerStateMachine::SEND_ERROR;
                }
                else {
                    // Receive response
                    std::string response = receiveResponse();
                    logger.logReceivedPacket(response, "Connection Response");

                    // Parse response
                    if (!ConnectionRequest::parseResponse(response)) {
                        std::string reason = ConnectionRequest::getRejectReason(response);
                        std::cerr << "Connection request denied by server: " << reason << std::endl;
                        logger.logEvent("Connection request denied: " + reason);
                        result = ServerUtilities::ServerStateMachine::CONNECTION_REQUEST_DENIED;
                    }
                    else {
                        isApproved = true;
                        logger.logEvent("Connection request approved by server");
                    }
                }
            }

            return result;
        }

        // Send extended flight information
        ServerUtilities::ServerStateMachine sendExtendedFlightInformation(const std::string& flightId, const std::string& departure, const std::string& arrival) {
            ServerUtilities::ServerStateMachine result = ServerUtilities::ServerStateMachine::SUCCESS;  // Default result

            if (clientSocket == INVALID_SOCKET || !isConnected || !isApproved) {
                logger.logEvent("ERROR: Not connected or approved by server");
                result = ServerUtilities::ServerStateMachine::CONNECTION_ERROR;
            }
            else {
                logger.logEvent("Sending extended flight information for flight: " + flightId +
                    " from " + departure + " to " + arrival);

                // Prompt user for additional flight details
                std::string aircraftReg, aircraftType, operatorName, route, cruiseAlt, speed, eobt, etd, eta;
                std::cout << "Enter Aircraft Registration: ";
                if (!std::getline(std::cin, aircraftReg)) {
                    UI_helper::printError("Failed to read Aircraft Registration.");
                }

                std::cout << "Enter Aircraft Type: ";
                if (!std::getline(std::cin, aircraftType)) {
                    UI_helper::printError("Failed to read Aircraft Type.");
                }

                std::cout << "Enter Operator Name: ";
                if (!std::getline(std::cin, operatorName)) {
                    UI_helper::printError("Failed to read Operator Name.");
                }

                std::cout << "Enter Route: ";
                if (!std::getline(std::cin, route)) {
                    UI_helper::printError("Failed to read Route.");
                }

                std::cout << "Enter Cruise Altitude: ";
                if (!std::getline(std::cin, cruiseAlt)) {
                    UI_helper::printError("Failed to read Cruise Altitude.");
                }

                std::cout << "Enter Speed: ";
                if (!std::getline(std::cin, speed)) {
                    UI_helper::printError("Failed to read Speed.");
                }

                std::cout << "Enter EOBT (Estimated Off-Block Time): ";
                if (!std::getline(std::cin, eobt)) {
                    UI_helper::printError("Failed to read EOBT.");
                }

                std::cout << "Enter ETD (Estimated Time of Departure): ";
                if (!std::getline(std::cin, etd)) {
                    UI_helper::printError("Failed to read ETD.");
                }

                std::cout << "Enter ETA (Estimated Time of Arrival): ";
                if (!std::getline(std::cin, eta)) {
                    UI_helper::printError("Failed to read ETA.");
                }

                // Flight Plan Packet
                std::string flightPlan = std::string(static_cast<const char*>("FLIGHT_PLAN\n")) +
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
                    result = ServerUtilities::ServerStateMachine::SEND_ERROR;
                }
                else {
                    // Add a small delay to simulate packet separation
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));

                    // Prompt user for additional flight log details
                    std::string totalFlightTime, fuelOnBoard, estimatedFuelBurn, totalWeight, pic, remarks;
                    std::cout << "Enter Total Flight Time (HH:MM): ";
                    if (!std::getline(std::cin, totalFlightTime)) {
                        UI_helper::printError("Failed to read Total Flight Time.");
                    }

                    std::cout << "Enter Fuel On Board (L): ";
                    if (!std::getline(std::cin, fuelOnBoard)) {
                        UI_helper::printError("Failed to read Fuel On Board.");
                    }

                    std::cout << "Enter Estimated Fuel Burn (L/min): ";
                    if (!std::getline(std::cin, estimatedFuelBurn)) {
                        UI_helper::printError("Failed to read Estimated Fuel Burn.");
                    }

                    std::cout << "Enter Total Weight (KG): ";
                    if (!std::getline(std::cin, totalWeight)) {
                        UI_helper::printError("Failed to read Total Weight.");
                    }

                    std::cout << "Enter PIC (Pilot in Command): ";
                    if (!std::getline(std::cin, pic)) {
                        UI_helper::printError("Failed to read Pilot in Command.");
                    }

                    std::cout << "Enter Remarks: ";
                    if (!std::getline(std::cin, remarks)) {
                        UI_helper::printError("Failed to read Remarks.");
                    }

                    // Flight Log Packet
                    std::string flightLog = std::string(static_cast<const char*>("FLIGHT_LOG\n")) +
                        "FLIGHT_NUMBER=" + flightId + "\n" +
                        "TOTAL_FLIGHT_TIME=" + totalFlightTime + "\n" +
                        "FUEL_ON_BOARD=" + fuelOnBoard + "\n" +
                        "ESTIMATED_FUEL_BURN=" + estimatedFuelBurn + "\n" +
                        "TOTAL_WEIGHT=" + totalWeight + "\n" +
                        "PIC=" + pic + "\n" +
                        "REMARKS=" + remarks + "\n";

                    std::string flightLogWithHeader = PacketHeader::createHeader(flightLog) + flightLog;
                    logger.logSentPacket(flightLogWithHeader, "Flight Log");

                    // Send Flight Log
                    sendResult = send(clientSocket, flightLogWithHeader.c_str(), static_cast<int>(flightLogWithHeader.length()), 0);
                    if (sendResult == SOCKET_ERROR) {
                        int error = WSAGetLastError();
                        std::cerr << "Send flight log failed: " << error << std::endl;
                        logger.logEvent("Send flight log failed with error: " + std::to_string(error));
                        result = ServerUtilities::ServerStateMachine::SEND_ERROR;
                    }
                    else {
                        logger.logEvent("Flight information sent successfully");
                    }
                }
            }

            return result;
        }

        std::string receiveResponse() {
            std::string response = "ERROR: Failed to receive response"; // Default response in case of failure

            if (clientSocket == INVALID_SOCKET || !isConnected) {
                logger.logEvent("ERROR: Not connected to server");
                response = "ERROR: Not connected to server";
            }
            else {
                char buffer[Constants::BUFFER_SIZE];
                (void)ZeroMemory(static_cast<void*>(buffer), sizeof(buffer)); // Initialize buffer to zero

                logger.logEvent("Waiting to receive response from server");

                // Receive response from server
                int bytesReceived = recv(clientSocket, static_cast<char*>(buffer), Constants::BUFFER_SIZE - 1, 0);
                if (bytesReceived == SOCKET_ERROR) {
                    int error = WSAGetLastError();
                    std::cerr << "Receive failed: " << error << std::endl;
                    logger.logEvent("Receive failed with error: " + std::to_string(error));
                }
                else {
                    // Null terminate the received data (safe because we allocated BUFFER_SIZE bytes and read at most BUFFER_SIZE-1)
                    buffer[bytesReceived] = '\0';
                    response = static_cast<char*>(buffer);
                    logger.logReceivedPacket(response, "Server Response (" + std::to_string(bytesReceived) + " bytes)");
                }
            }

            return response;
        }

        bool retryConnection(const std::string& serverIP, int port, int maxRetries = 3) {
            bool isSuccess = false; // Default result is failure

            // Validate input parameters
            if (maxRetries <= 0) {
                logger.logEvent("ERROR: Invalid maxRetries value");
                isSuccess = false;
            }

            // Validate port range
            if (port <= 0 || port > 65535) {
                logger.logEvent("ERROR: Invalid port number");
                isSuccess = false;
            }

            logger.logEvent("Beginning retry connection sequence. Max retries: " + std::to_string(maxRetries));

            for (int retry = 0; retry < maxRetries; ++retry) {
                // Disconnect if previously connected
                if (isConnected) {
                    disconnect();
                }

                std::cout << "Attempting connection retry " << (retry + 1) << " of " << maxRetries << "..." << std::endl;
                logger.logEvent("Attempting connection retry " + std::to_string(retry + 1) +
                    " of " + std::to_string(maxRetries));

                // Try to connect
                if (connect(serverIP, port) == ServerUtilities::ServerStateMachine::SUCCESS) {
                    // If connected, try to get approval
                    if (requestConnection() == ServerUtilities::ServerStateMachine::SUCCESS) {
                        std::cout << "Connection and approval successful on retry " << (retry + 1) << std::endl;
                        logger.logEvent("Connection and approval successful on retry " + std::to_string(retry + 1));
                        isSuccess = true;
                        break; // Connection and approval successful, exit loop
                    }

                    // If server is full, wait before retrying
                    std::cout << "Server is full. Waiting 30 seconds before retry..." << std::endl;
                    logger.logEvent("Server is full. Waiting 30 seconds before retry...");
                    disconnect();
                    std::this_thread::sleep_for(std::chrono::seconds(Constants::FULL_SERVER_RETRY_DELAY_SECONDS));
                }
                else {
                    // If connection failed, wait a bit less before retrying
                    std::cout << "Connection failed. Waiting 5 seconds before retry..." << std::endl;
                    logger.logEvent("Connection failed. Waiting 5 seconds before retry...");
                    std::this_thread::sleep_for(std::chrono::seconds(Constants::RETRY_DELAY_SECONDS));
                }
            }

            if (!isSuccess) {
                std::cout << "Maximum retry attempts reached. Could not connect to server." << std::endl;
                logger.logEvent("Maximum retry attempts reached. Could not connect to server.");
            }

            return isSuccess;
        }

        void disconnect() {
            if (clientSocket != INVALID_SOCKET) {
                logger.logEvent("Disconnecting from server");
                if (closesocket(clientSocket) == SOCKET_ERROR) {
                    logger.logEvent("closesocket failed: " + std::to_string(WSAGetLastError()));
                }
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



    // Function to handle server connection setup
    bool setupServerConnection(NotamClient& client, std::string& serverIP, int& serverPort) {
        UI_helper::printSection("SERVER CONNECTION SETUP");

        char choice = UI_helper::getCharInput("Use default server settings (127.0.0.1:8081)? (y/n): ");
        bool isConnected = false; // Flag to track connection success

        if (choice == 'n' || choice == 'N') {
            serverIP = UI_helper::getInput("Enter server IP address: ");
            serverPort = UI_helper::getIntInput("Enter server port: ");
        }

        UI_helper::printInfo("Connecting to NOTAM server at " + serverIP + ":" + std::to_string(serverPort));

        // Show progress during connection attempt
        for (int i = 0; i <= 100; i += 10) {
            UI_helper::printProgressBar(i);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << std::endl;

        ServerUtilities::ServerStateMachine result = client.connect(serverIP, serverPort);
        if (result != ServerUtilities::ServerStateMachine::SUCCESS) {
            UI_helper::printError("Failed to connect to server.");

            choice = UI_helper::getCharInput("Would you like to retry connecting? (y/n): ");
            if (choice == 'y' || choice == 'Y') {
                UI_helper::showSpinner("Attempting to reconnect", 2);
                isConnected = client.retryConnection(serverIP, serverPort, Constants::MAX_RETRY_ATTEMPTS);
            }
        }
        else {
            UI_helper::printSuccess("Connected successfully!");

            // Request connection approval
            UI_helper::printInfo("Requesting connection approval from the server...");
            UI_helper::showSpinner("Waiting for server approval", 1);

            result = client.requestConnection();
            if (result != ServerUtilities::ServerStateMachine::SUCCESS) {
                UI_helper::printError("Failed to get connection approval from server.");

                choice = UI_helper::getCharInput("Would you like to retry after waiting? (y/n): ");
                if (choice == 'y' || choice == 'Y') {
                    UI_helper::showSpinner("Preparing to retry connection", 2);
                    isConnected = client.retryConnection(serverIP, serverPort, Constants::MAX_RETRY_ATTEMPTS);
                }
                else {
                    client.disconnect();
                }
            }
            else {
                UI_helper::printSuccess("Connection approved by server!");
                isConnected = true;
            }
        }

        return isConnected;
    }

    // Function to select flight details
    bool selectFlight(std::string& departure, std::string& arrival)
    {
        UI_helper::printSection("FLIGHT SELECTION");

        std::cout << "Select a flight to check NOTAMs:\n\n";
        std::cout << "  1. Toronto (CYYZ) to New York (KJFK)\n";
        std::cout << "  2. Waterloo (CYKF) to Montreal (CYUL)\n";
        std::cout << "  3. Custom flight\n\n";

        int flightChoice = UI_helper::getIntInput("Enter your choice (1-3): ");

        bool isValidSelection = true;

        // Set flight details based on user selection
        if (1 == flightChoice)
        {
            departure = "CYYZ";  // Toronto
            arrival = "KJFK";    // New York JFK
            UI_helper::printSuccess("Selected flight: Toronto (CYYZ) to New York (KJFK)");
        }
        else if (2 == flightChoice)
        {
            departure = "CYKF";  // Waterloo
            arrival = "CYUL";    // Montreal
            UI_helper::printSuccess("Selected flight: Waterloo (CYKF) to Montreal (CYUL)");
        }
        else if (3 == flightChoice)
        {
            departure = UI_helper::getInput("Enter departure airport code (e.g., CYYZ): ");
            arrival = UI_helper::getInput("Enter arrival airport code (e.g., KJFK): ");
            UI_helper::printSuccess("Selected flight: " + departure + " to " + arrival);
        }
        else
        {
            UI_helper::printError("Invalid selection. Please try again.");
            isValidSelection = false;
        }

        return isValidSelection;
    }
}

int main(void)
{
    // Initialize variables
    std::string serverIP = "127.0.0.1";  // Default server IP
    int serverPort = Constants::DEFAULT_SERVER_PORT;              // Default server port
    std::string flightId;
    std::string departure;
    std::string arrival;
    int returnCode = 0;

    // Create NOTAM client
    NotamClient::NotamClient client;

    // Display application header
    UI_helper::printHeader();

    // Display client ID
    UI_helper::printInfo("Your client ID is: " + client.getClientId());

    // Connect to server
    if (!setupServerConnection(client, serverIP, serverPort))
    {
        UI_helper::printError("Failed to establish connection with the NOTAM server.");
        returnCode = 1;
    }
    else
    {
        // Select flight for NOTAM information
        flightId = client.getClientId(); // Use client ID as flight ID

        if (!NotamClient::selectFlight(departure, arrival))
        {
            UI_helper::printError("Flight selection failed.");
            client.disconnect();
            returnCode = 1;
        }
        else
        {
            // Send flight information
            UI_helper::printSection("SENDING FLIGHT INFORMATION");
            UI_helper::printInfo("Preparing to send extended flight information to server...");

            ServerUtilities::ServerStateMachine result = client.sendExtendedFlightInformation(flightId, departure, arrival);

            if (ServerUtilities::ServerStateMachine::SUCCESS != result)
            {
                UI_helper::printError("Failed to send flight information.");
                client.disconnect();
                returnCode = 1;
            }
            else
            {
                UI_helper::printSuccess("Flight information sent successfully!");

                // Receive and display NOTAM information
                UI_helper::printInfo("Waiting for NOTAM information from server...");
                UI_helper::showSpinner("Processing NOTAM data", 2);

                std::string response = client.receiveResponse();

                if(std::string::npos != response.find(static_cast<const char*>("ERROR")))
                {
                    UI_helper::printError("Failed to receive NOTAM information.");
                    client.disconnect();
                    returnCode = 1;
                }
                else
                {
                    // Display NOTAM information
                    UI_helper::displayNotamInfo(response, flightId);

                    // Clean up and exit
                    UI_helper::printSection("CONNECTION COMPLETE");
                    UI_helper::printInfo("Press Enter to disconnect and exit...");
                    if (std::cin.get() == EOF) {
                        UI_helper::printError("Failed to get input.");
                    }
                }
            }
        }
    }

    client.disconnect();
    UI_helper::printSuccess("Disconnected from server. Goodbye!");

    return returnCode;
}


