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

// Add these constant definitions at an appropriate scope:
static const int DEFAULT_SERVER_PORT = 8081;
static const int MAX_RETRY_ATTEMPTS = 3;
static const int RETRY_DELAY_SECONDS = 5;
static const int FULL_SERVER_RETRY_DELAY_SECONDS = 30;
static const int PROGRESS_BAR_WIDTH = 50;
static const int SPINNER_INTERVAL_MS = 250;

// Link with Ws2_32.lib
#pragma comment(lib, "Ws2_32.lib")

// Define buffer size as a constant for safety and maintainability
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
        logFile.open(filename, std::ios::out | std::ios::app);
        if (!logFile.is_open()) {
            std::cerr << "Failed to open log file: " << filename << std::endl;
            return false;
        }

        // Write header with timestamp
        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        struct tm timeInfo;
        // Use localtime_s instead of std::localtime for safety (MISRA)
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

        // Make sure it's written to disk
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
        // Increment and get the sequence number
        uint64_t seqNum = ++sequenceCounter;

        // Get current timestamp
        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        struct tm timeInfo;
#ifdef _WIN32
        localtime_s(&timeInfo, &now_c);
#else
        localtime_r(&now_c, &timeInfo);
#endif

        std::stringstream timestampSS;
        //timestampSS << std::put_time(&timeInfo, "%Y-%m-%dT%H:%M:%S.%fZ");
        timestampSS << std::put_time(&timeInfo, "%Y-%m-%dT%H:%M:%SZ");

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

        return headerSS.str();
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
        return response.find("CONNECTION_ACCEPTED") != std::string::npos;
    }

    static std::string getRejectReason(const std::string& response) {
        std::string reason = "Unknown reason";
        size_t reasonPos = response.find("REASON=");

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

    std::string generateATCNumber() {
        // Always start with A followed by 3-4 digits
        return "A" + std::to_string(flightNumDist(rng));
    }
};

class ATC_client {
private:
    SOCKET clientSocket;
    bool initialized;
    std::string clientId;
    bool isConnected;
    bool isApproved;
    PacketLogger logger;

    // Helper method to get current timestamp
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
    ATC_client() :
        clientSocket(INVALID_SOCKET),
        initialized(false),
        isConnected(false),
        isApproved(false) {

        // Initialize logger
        if (logger.initialize("atc_client_log.txt")) {
            PacketHeader::setLogger(&logger);
            logger.logEvent("NotamClient initialized");
        }

        // Initialize Winsock
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
        clientId = generator.generateATCNumber();
        logger.logEvent("Generated client ID: " + clientId);
    }

    ~ATC_client() {
        disconnect();
        if (initialized) {
            WSACleanup();
            logger.logEvent("Winsock cleaned up");
        }
    }

    std::string getClientId() const {
        return clientId;
    }

    ServerStateMachine connect(const std::string& serverIP, int port) {
        if (!initialized) {
            logger.logEvent("ERROR: Winsock not initialized");
            return ServerStateMachine::WINSOCK_ERROR;
        }

        // Validate port range
        if (port <= 0 || port > 65535) {
            logger.logEvent("ERROR: Invalid port number");
            return ServerStateMachine::CONNECTION_ERROR;
        }

        logger.logEvent("Attempting to connect to server at " + serverIP + ":" + std::to_string(port));

        // Create a socket
        clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (clientSocket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            std::cerr << "Socket creation failed: " << error << std::endl;
            logger.logEvent("Socket creation failed with error: " + std::to_string(error));
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Set up the server address
        sockaddr_in serverAddr;
        ZeroMemory(&serverAddr, sizeof(serverAddr)); // Initialize structure to zero
        serverAddr.sin_family = AF_INET;
        if (port > 0 && port <= UINT16_MAX) {
            serverAddr.sin_port = htons(static_cast<uint16_t>(port));
        }
        else {
            logger.logEvent("Port number out of valid range");
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Use inet_pton for safer IP address conversion
        int convResult = inet_pton(AF_INET, serverIP.c_str(), &(serverAddr.sin_addr));
        if (convResult != 1) {
            std::cerr << "Invalid address format: " << serverIP << std::endl;
            logger.logEvent("Invalid address format: " + serverIP);
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Connect to server
        if (::connect(clientSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
            int error = WSAGetLastError();
            std::cerr << "Connection failed: " << error << std::endl;
            logger.logEvent("Connection failed with error: " + std::to_string(error));
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
            return ServerStateMachine::CONNECTION_ERROR;
        }

        isConnected = true;
        logger.logEvent("Connected to server successfully");
        return ServerStateMachine::SUCCESS;
    }

    // Request connection approval from the server
    ServerStateMachine requestConnection() {
        if (clientSocket == INVALID_SOCKET || !isConnected) {
            logger.logEvent("ERROR: Not connected to server");
            return ServerStateMachine::CONNECTION_ERROR;
        }

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
            return ServerStateMachine::SEND_ERROR;
        }

        // Receive response
        std::string response = receiveResponse();
        logger.logReceivedPacket(response, "Connection Response");

        // Parse response
        if (!ConnectionRequest::parseResponse(response)) {
            std::string reason = ConnectionRequest::getRejectReason(response);
            std::cerr << "Connection request denied by server: " << reason << std::endl;
            logger.logEvent("Connection request denied: " + reason);
            return ServerStateMachine::CONNECTION_REQUEST_DENIED;
        }

        isApproved = true;
        logger.logEvent("Connection request approved by server");
        return ServerStateMachine::SUCCESS;
    }

    std::string receiveResponse() {
        if (clientSocket == INVALID_SOCKET || !isConnected) {
            logger.logEvent("ERROR: Not connected to server");
            return "ERROR: Not connected to server";
        }

        char buffer[BUFFER_SIZE];
        ZeroMemory(buffer, sizeof(buffer)); // Initialize buffer to zero

        logger.logEvent("Waiting to receive response from server");

        // Receive response from server
        int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived == SOCKET_ERROR) {
            int error = WSAGetLastError();
            std::cerr << "Receive failed: " << error << std::endl;
            logger.logEvent("Receive failed with error: " + std::to_string(error));
            return "ERROR: Failed to receive response";
        }

        // Null terminate the received data (safe because we allocated BUFFER_SIZE bytes and read at most BUFFER_SIZE-1)
        buffer[bytesReceived] = '\0';
        std::string response(buffer);

        logger.logReceivedPacket(response, "Server Response (" + std::to_string(bytesReceived) + " bytes)");
        return response;
    }

    bool retryConnection(const std::string& serverIP, int port, int maxRetries = 3) {
        // Validate input parameters
        if (maxRetries <= 0) {
            logger.logEvent("ERROR: Invalid maxRetries value");
            return false;
        }

        // Validate port range
        if (port <= 0 || port > 65535) {
            logger.logEvent("ERROR: Invalid port number");
            return false;
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
            if (connect(serverIP, port) == ServerStateMachine::SUCCESS) {
                // If connected, try to get approval
                if (requestConnection() == ServerStateMachine::SUCCESS) {
                    std::cout << "Connection and approval successful on retry " << (retry + 1) << std::endl;
                    logger.logEvent("Connection and approval successful on retry " + std::to_string(retry + 1));
                    return true;
                }

                // If server is full, wait before retrying
                std::cout << "Server is full. Waiting 30 seconds before retry..." << std::endl;
                logger.logEvent("Server is full. Waiting 30 seconds before retry...");
                disconnect();
                std::this_thread::sleep_for(std::chrono::seconds(FULL_SERVER_RETRY_DELAY_SECONDS));
            }
            else {
                // If connection failed, wait a bit less before retrying
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

// UI Helper Functions
// With this safer alternative:
void clearScreen() {
#ifdef _WIN32
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdOut != INVALID_HANDLE_VALUE) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        DWORD count;
        DWORD cellCount;
        COORD homeCoords = { 0, 0 };

        if (GetConsoleScreenBufferInfo(hStdOut, &csbi)) {
            cellCount = csbi.dwSize.X * csbi.dwSize.Y;
            FillConsoleOutputCharacter(hStdOut, (TCHAR)' ', cellCount, homeCoords, &count);
            FillConsoleOutputAttribute(hStdOut, csbi.wAttributes, cellCount, homeCoords, &count);
            SetConsoleCursorPosition(hStdOut, homeCoords);
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
    std::cout << "|                 ATC CLIENT APPLICATION                   |\n";
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

    int barWidth = PROGRESS_BAR_WIDTH;
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
    std::cout.flush();
}

void showSpinner(const std::string& message, int seconds) {
    if (seconds <= 0) {
        return; // Avoid negative or zero seconds
    }

    const char spinner[] = { '|', '/', '-', '\\' };
    int i = 0;

    std::cout << message;

    for (int j = 0; j < seconds * 4; ++j) {
        std::cout << " " << spinner[i % 4] << "\r";
        std::cout << message;
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(SPINNER_INTERVAL_MS));
        ++i;
    }
    std::cout << std::endl;
}

void printSection(const std::string& title) {
    std::cout << "\n-------------------------------------------------------------\n";
    std::cout << "|" << std::left << std::setw(51) << title << "        |\n";
    std::cout << "-------------------------------------------------------------\n";
}

void printSuccess(const std::string& message) {
    std::cout << "> " << message << std::endl;
}

void printError(const std::string& message) {
    std::cout << "X " << message << std::endl;
}

void printInfo(const std::string& message) {
    std::cout << "> " << message << std::endl;
}

std::string getInput(const std::string& prompt) {
    std::string input;
    std::cout << prompt;
    std::getline(std::cin, input);
    return input;
}

char getCharInput(const std::string& prompt) {
    std::string input;
    std::cout << prompt;
    std::getline(std::cin, input);
    return input.empty() ? '\0' : input[0];
}

int getIntInput(const std::string& prompt) {
    std::string input;
    int value = 0;
    bool validInput = false;

    while (!validInput) {
        std::cout << prompt;
        std::getline(std::cin, input);

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

// Function to handle server connection setup
bool setupServerConnection(ATC_client& client, std::string& serverIP, int& serverPort) {
    printSection("SERVER CONNECTION SETUP");

    char choice = getCharInput("Use default server settings (127.0.0.1:8081)? (y/n): ");

    if (choice == 'n' || choice == 'N') {
        serverIP = getInput("Enter server IP address: ");
        serverPort = getIntInput("Enter server port: ");
    }

    printInfo("Connecting to NOTAM server at " + serverIP + ":" + std::to_string(serverPort));

    // Show progress during connection attempt
    for (int i = 0; i <= 100; i += 10) {
        printProgressBar(i);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << std::endl;

    ServerStateMachine result = client.connect(serverIP, serverPort);
    if (result != ServerStateMachine::SUCCESS) {
        printError("Failed to connect to server.");

        choice = getCharInput("Would you like to retry connecting? (y/n): ");
        if (choice == 'y' || choice == 'Y') {
            showSpinner("Attempting to reconnect", 2);
            return client.retryConnection(serverIP, serverPort, MAX_RETRY_ATTEMPTS);
        }
        else {
            return false;
        }
    }

    printSuccess("Connected successfully!");

    // Request connection approval
    printInfo("Requesting connection approval from the server...");
    showSpinner("Waiting for server approval", 1);

    result = client.requestConnection();
    if (result != ServerStateMachine::SUCCESS) {
        printError("Failed to get connection approval from server.");

        choice = getCharInput("Would you like to retry after waiting? (y/n): ");
        if (choice == 'y' || choice == 'Y') {
            showSpinner("Preparing to retry connection", 2);
            return client.retryConnection(serverIP, serverPort, MAX_RETRY_ATTEMPTS);
        }
        else {
            client.disconnect();
            return false;
        }
    }

    printSuccess("Connection approved by server!");
    return true;
}

// Function to display NOTAM information
void displayFlightPlanInfo(const std::string& response, const std::string& ATCid)
{
    printSection("FLIGHT PLAN INFORMATION");

    std::cout << "-------------------------------------------------------------\n";
    std::cout << "|             FLIGHT PLAN INFO FOR ATC " << std::left << std::setw(17) << ATCid << "        |\n";
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
};

int main(void)
{
    // Initialize variables
    std::string serverIP = "127.0.0.1";  // Default server IP
    int serverPort = DEFAULT_SERVER_PORT;              // Default server port
    std::string ATCid;
    std::string departure;
    std::string arrival;
    int returnCode = 0;

    // Create NOTAM client
    ATC_client client;

    // Display application header
    printHeader();

    // Display client ID
    printInfo("Your client ID is: " + client.getClientId());

    // Connect to server
    if (!setupServerConnection(client, serverIP, serverPort))
    {
        printError("Failed to establish connection with the NOTAM server.");
        returnCode = 1;
    }
    else
    {
        // Select flight for NOTAM information
        ATCid = client.getClientId(); // Use client ID as flight ID


        printInfo("Waiting for  information from server...");
        showSpinner("Processing Flight Plan data", 2);

        std::string response = client.receiveResponse();

        if (std::string::npos != response.find("ERROR"))
        {
            printError("Failed to receive Flight Plan information.");
            client.disconnect();
            returnCode = 1;
        }
        else
        {
            // Display Flight Plan information
            displayFlightPlanInfo(response, ATCid);

            // Clean up and exit
            printSection("CONNECTION COMPLETE");
            printInfo("Press Enter to disconnect and exit...");
            std::cin.get();
        }
    }

    client.disconnect();
    printSuccess("Disconnected from server. Goodbye!");

    return returnCode;
}


