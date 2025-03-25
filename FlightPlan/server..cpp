#define _CRT_SECURE_NO_WARNINGS 
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <mutex>

#include <nlohmann/json.hpp>
#define CURL_STATICLIB
#include "curl/curl.h"

// Windows-specific headers for networking
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// Error handling utilities
enum class ErrorCode {
    SUCCESS = 0,
    INVALID_INPUT = 1,
    CONNECTION_ERROR = 2,
    PROCESSING_ERROR = 3,
    DATABASE_ERROR = 4
};

// Safe string operations
class SafeString {
private:
    static const uint32_t MAX_STRING_LENGTH = 256U;

public:
    static ErrorCode copy(char* dest, uint32_t destSize, const char* src) {
        if ((NULL == dest) || (NULL == src) || (0U == destSize)) {
            return ErrorCode::INVALID_INPUT;
        }

        const uint32_t srcLen = static_cast<uint32_t>(strlen(src));
        const uint32_t copyLen = (srcLen < (destSize - 1U)) ? srcLen : (destSize - 1U);

        (void)memcpy(dest, src, copyLen);
        dest[copyLen] = '\0';

        return ErrorCode::SUCCESS;
    }
};

// Flight data structures
struct Coordinate {
    double latitude;
    double longitude;
};

struct AirspaceInfo {
    char identifier[8];
    Coordinate center;
    double radius;
};

struct FlightPlan {
    char flightId[16];
    char departureAirport[8];
    char arrivalAirport[8];
    std::vector<AirspaceInfo> routeAirspaces;
};

struct Notam {
    char identifier[16];
    char fir[8];
    char location[8];
    char startTime[20];
    char endTime[20];
    AirspaceInfo affectedAirspace;
    char description[256];
};

// Connection request handling
struct ConnectionRequest {
    std::string parseFromData(const std::string& data) {
        std::istringstream iss(data);
        std::string line;
        std::string clientId;

        // Check if it's a connection request
        if (std::getline(iss, line) && line == "REQUEST_CONNECTION") {
            // Parse the CLIENT_ID= line
            while (std::getline(iss, line)) {
                if (line.find("CLIENT_ID=") == 0) {
                    clientId = line.substr(10); // Length of "CLIENT_ID="
                    break;
                }
            }
        }
        return clientId;
    }

    static std::string createAcceptResponse() {
        return "CONNECTION_ACCEPTED\n";
    }

    static std::string createRejectResponse() {
        return "CONNECTION_REJECTED\nREASON=Maximum connections reached. Please hover for 30 more minutes.\n";
    }
};

// Client connection manager
class ConnectionManager {
public:
    static const size_t MAX_CONNECTIONS = 5;
    std::set<std::string> activeClients;
    std::mutex mutex;

public:
    bool canAcceptConnection() {
        std::lock_guard<std::mutex> lock(mutex);
        return activeClients.size() < MAX_CONNECTIONS;
    }

    bool addClient(const std::string& clientId) {
        std::lock_guard<std::mutex> lock(mutex);
        if (activeClients.size() >= MAX_CONNECTIONS) {
            return false;
        }
        activeClients.insert(clientId);
        return true;
    }

    void removeClient(const std::string& clientId) {
        std::lock_guard<std::mutex> lock(mutex);
        activeClients.erase(clientId);
    }

    size_t getActiveClientCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return activeClients.size();
    }
};

// NOTAM database
class NotamDatabase {
private:
    std::vector<Notam> notams_;

    bool parseNotamLine(const std::string& line, Notam& notam) {
        std::istringstream iss(line);
        std::string token;

        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') {
            return false;
        }

        // Parse pipe-delimited fields
        // Format: NOTAM_ID|FIR|LOCATION|START_TIME|END_TIME|AFFECTED_AIRSPACE|LAT|LON|RADIUS|DESCRIPTION

        // NOTAM ID
        if (!std::getline(iss, token, '|')) return false;
        strncpy(notam.identifier, token.c_str(), sizeof(notam.identifier) - 1);
        notam.identifier[sizeof(notam.identifier) - 1] = '\0';

        // FIR
        if (!std::getline(iss, token, '|')) return false;
        strncpy(notam.fir, token.c_str(), sizeof(notam.fir) - 1);
        notam.fir[sizeof(notam.fir) - 1] = '\0';

        // Location
        if (!std::getline(iss, token, '|')) return false;
        strncpy(notam.location, token.c_str(), sizeof(notam.location) - 1);
        notam.location[sizeof(notam.location) - 1] = '\0';

        // Start time
        if (!std::getline(iss, token, '|')) return false;
        strncpy(notam.startTime, token.c_str(), sizeof(notam.startTime) - 1);
        notam.startTime[sizeof(notam.startTime) - 1] = '\0';

        // End time
        if (!std::getline(iss, token, '|')) return false;
        strncpy(notam.endTime, token.c_str(), sizeof(notam.endTime) - 1);
        notam.endTime[sizeof(notam.endTime) - 1] = '\0';

        // Affected airspace
        if (!std::getline(iss, token, '|')) return false;
        strncpy(notam.affectedAirspace.identifier, token.c_str(),
            sizeof(notam.affectedAirspace.identifier) - 1);
        notam.affectedAirspace.identifier[sizeof(notam.affectedAirspace.identifier) - 1] = '\0';

        // Latitude
        if (!std::getline(iss, token, '|')) return false;
        notam.affectedAirspace.center.latitude = std::stod(token);

        // Longitude
        if (!std::getline(iss, token, '|')) return false;
        notam.affectedAirspace.center.longitude = std::stod(token);

        // Radius
        if (!std::getline(iss, token, '|')) return false;
        notam.affectedAirspace.radius = std::stod(token);

        // Description (rest of line)
        if (!std::getline(iss, token)) return false;
        strncpy(notam.description, token.c_str(), sizeof(notam.description) - 1);
        notam.description[sizeof(notam.description) - 1] = '\0';

        return true;
    }

public:
    NotamDatabase(void) : notams_() {
        // No initialization needed
    }

    bool loadFromFile(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return false;
        }

        std::string line;
        while (std::getline(file, line)) {
            Notam notam;
            if (parseNotamLine(line, notam)) {
                notams_.push_back(notam);
            }
        }

        return true;
    }

    const std::vector<Notam>& getAllNotams(void) const {
        return notams_;
    }
};

// Process NOTAMs to determine if they affect a flight
class NotamProcessor {
private:
    const NotamDatabase& notamDb_;

    static bool isAirspaceAffected(const char* spaceId1, const char* spaceId2) {

        return (0 == strcmp(spaceId1, spaceId2));
    }

public:
    explicit NotamProcessor(const NotamDatabase& notamDb) : notamDb_(notamDb) {
    }

    std::vector<const Notam*> getRelevantNotams(const FlightPlan& flightPlan) const {
        std::vector<const Notam*> relevantNotams;
        const std::vector<Notam>& allNotams = notamDb_.getAllNotams();

        for (const Notam& notam : allNotams) {
            // Check if NOTAM affects departure or arrival airport
            if ((0 == strcmp(notam.location, flightPlan.departureAirport)) ||
                (0 == strcmp(notam.location, flightPlan.arrivalAirport))) {
                relevantNotams.push_back(&notam);
                continue;
            }

            // Check if NOTAM affects any airspace in the route
            for (const AirspaceInfo& routeSpace : flightPlan.routeAirspaces) {
                if (isAirspaceAffected(routeSpace.identifier, notam.affectedAirspace.identifier)) {
                    relevantNotams.push_back(&notam);
                    break;
                }
            }
        }

        return relevantNotams;
    }
};

// Handle client message processing
class FlightDataHandler {
private:
    NotamProcessor& notamProcessor_;
    ConnectionManager& connectionManager_;

    std::vector<AirspaceInfo> getRouteAirspaces(const char* departure, const char* arrival) const {
        std::vector<AirspaceInfo> airspaces;

        // Toronto to New York route
        if ((0 == strcmp(departure, "CYYZ")) && (0 == strcmp(arrival, "KJFK"))) {
            const char* routePoints[] = { "CYYZ", "KBUF", "KJFK" };
            for (const char* point : routePoints) {
                AirspaceInfo space;
                (void)SafeString::copy(space.identifier, sizeof(space.identifier), point);
                space.center.latitude = 0.0;
                space.center.longitude = 0.0;
                space.radius = 15.0;
                airspaces.push_back(space);
            }
        }
        // Waterloo to Montreal route
        else if ((0 == strcmp(departure, "CYKF")) && (0 == strcmp(arrival, "CYUL"))) {
            const char* routePoints[] = { "CYKF", "CYOW", "CYUL" };
            for (const char* point : routePoints) {
                AirspaceInfo space;
                (void)SafeString::copy(space.identifier, sizeof(space.identifier), point);
                space.center.latitude = 0.0;
                space.center.longitude = 0.0;
                space.radius = 15.0;
                airspaces.push_back(space);
            }
        }
        // Generic fallback - just use departure and arrival
        else {
            AirspaceInfo depSpace;
            (void)SafeString::copy(depSpace.identifier, sizeof(depSpace.identifier), departure);
            depSpace.center.latitude = 0.0;
            depSpace.center.longitude = 0.0;
            depSpace.radius = 15.0;
            airspaces.push_back(depSpace);

            AirspaceInfo arrSpace;
            (void)SafeString::copy(arrSpace.identifier, sizeof(arrSpace.identifier), arrival);
            arrSpace.center.latitude = 0.0;
            arrSpace.center.longitude = 0.0;
            arrSpace.radius = 15.0;
            airspaces.push_back(arrSpace);
        }

        return airspaces;
    }

    FlightPlan parseFlightPlan(const std::string& data) const {
        FlightPlan plan;
        std::memset(&plan, 0, sizeof(FlightPlan));

        std::istringstream iss(data);
        std::string line;

        // Parse basic format: "FLIGHT=ABC123 DEP=CYYZ ARR=KJFK"
        std::string flightId;
        std::string departure;
        std::string arrival;

        while (std::getline(iss, line)) {
            if (line.find("FLIGHT=") == 0) {
                flightId = line.substr(7);
            }
            else if (line.find("DEP=") == 0) {
                departure = line.substr(4);
            }
            else if (line.find("ARR=") == 0) {
                arrival = line.substr(4);
            }
        }

        // Copy to flight plan
        (void)SafeString::copy(plan.flightId, sizeof(plan.flightId), flightId.c_str());
        (void)SafeString::copy(plan.departureAirport, sizeof(plan.departureAirport), departure.c_str());
        (void)SafeString::copy(plan.arrivalAirport, sizeof(plan.arrivalAirport), arrival.c_str());

        // Get route airspaces
        plan.routeAirspaces = getRouteAirspaces(plan.departureAirport, plan.arrivalAirport);

        return plan;
    }

public:
    explicit FlightDataHandler(NotamProcessor& processor, ConnectionManager& connManager)
        : notamProcessor_(processor), connectionManager_(connManager) {
        // No initialization needed
    }

    std::string processConnectionRequest(const std::string& clientData) {
        // Parse connection request
        ConnectionRequest request;
        std::string clientId = request.parseFromData(clientData);

        if (clientId.empty()) {
            return "ERROR: Invalid connection request format";
        }

        // Check if we can accept connection
        bool accepted = connectionManager_.canAcceptConnection();
        if (accepted) {
            connectionManager_.addClient(clientId);
            std::cout << "Connection accepted for client: " << clientId
                << " (Active clients: " << connectionManager_.getActiveClientCount() << ")" << std::endl;
            return ConnectionRequest::createAcceptResponse();
        }
        else {
            std::cout << "Connection rejected for client: " << clientId
                << " (Maximum connections reached: " << connectionManager_.getActiveClientCount() << ")" << std::endl;
            return ConnectionRequest::createRejectResponse();
        }
    }

    std::string processFlightPlan(const std::string& clientData, const std::string& clientId) {
        if (clientData.empty()) {
            return "ERROR: Invalid flight data";
        }

        // Parse the flight plan from client data
        FlightPlan flightPlan = parseFlightPlan(clientData);

        if (flightPlan.departureAirport[0] == '\0' || flightPlan.arrivalAirport[0] == '\0') {
            return "ERROR: Missing departure or arrival airport";
        }

        // Get relevant NOTAMs
        std::vector<const Notam*> relevantNotams = notamProcessor_.getRelevantNotams(flightPlan);

        if (relevantNotams.empty()) {
            return "REQUEST: Please send flight log and flight plan";
        }

        // Create response with relevant NOTAMs
        std::ostringstream response;
        response << "NOTAMS AFFECTING YOUR FLIGHT:\n";
        for (const Notam* notam : relevantNotams) {
            response << "NOTAM: " << notam->identifier << " for " << notam->location;
            response << " - " << notam->description << "\n";
        }

        return response.str();
    }
};

// TCP Server for client connections - adapted for Windows
class TcpServer {
private:
    SOCKET serverSocket_;
    FlightDataHandler& dataHandler_;
    ConnectionManager& connectionManager_;
    bool isRunning_;
    static const uint32_t BUFFER_SIZE = 4096U;

    void handleClient(SOCKET clientSocket) {
        std::array<char, BUFFER_SIZE> buffer;
        int bytesRead = recv(clientSocket, buffer.data(), buffer.size() - 1, 0);

        if (bytesRead > 0) {
            // Null-terminate the received data
            buffer[static_cast<size_t>(bytesRead)] = '\0';
            std::string request(buffer.data());
            std::string response;
            std::string clientId;

            // Check if this is a connection request
            if (request.find("REQUEST_CONNECTION") == 0) {
                // Handle connection request
                response = dataHandler_.processConnectionRequest(request);

                // Extract client ID for logging
                ConnectionRequest req;
                clientId = req.parseFromData(request);

                // Send the response back to the client
                send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);

                // If connection was accepted, wait for flight plan data
                if (response.find("CONNECTION_ACCEPTED") == 0) {
                    memset(buffer.data(), 0, buffer.size());
                    bytesRead = recv(clientSocket, buffer.data(), buffer.size() - 1, 0);

                    if (bytesRead > 0) {
                        buffer[static_cast<size_t>(bytesRead)] = '\0';
                        std::string flightPlanRequest(buffer.data());

                        // Process flight plan
                        std::string flightPlanResponse = dataHandler_.processFlightPlan(flightPlanRequest, clientId);

                        // Send flight plan response
                        send(clientSocket, flightPlanResponse.c_str(),
                            static_cast<int>(flightPlanResponse.length()), 0);

                        // Log the transaction
                        std::cout << "\n=== Flight Plan Request from " << clientId << " ===\n"
                            << flightPlanRequest << std::endl;
                        std::cout << "=== Server Response ===\n" << flightPlanResponse << std::endl;
                    }

                    // Remove client when done
                    connectionManager_.removeClient(clientId);
                    std::cout << "Client disconnected: " << clientId
                        << " (Active clients: " << connectionManager_.getActiveClientCount() << ")" << std::endl;
                }
            }
            else {
                // If not a connection request, this is an error
                response = "ERROR: Connection request required before sending flight data";
                send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
            }

            // Log the transaction
            std::cout << "\n=== Client Request ===\n" << request << std::endl;
            std::cout << "=== Server Response ===\n" << response << std::endl;
        }

        // Close the client socket
        closesocket(clientSocket);
    }

public:
    explicit TcpServer(FlightDataHandler& handler, ConnectionManager& connManager) :
        serverSocket_(INVALID_SOCKET),
        dataHandler_(handler),
        connectionManager_(connManager),
        isRunning_(false) {
        // No initialization needed
    }

    ErrorCode start(uint16_t port) {
        // Initialize Winsock
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "WSAStartup failed: " << result << std::endl;
            return ErrorCode::CONNECTION_ERROR;
        }

        // Create the server socket
        serverSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (serverSocket_ == INVALID_SOCKET) {
            std::cerr << "Failed to create socket: " << WSAGetLastError() << std::endl;
            WSACleanup();
            return ErrorCode::CONNECTION_ERROR;
        }

        // Set socket options to allow address reuse
        BOOL opt = TRUE;
        if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&opt), sizeof(opt)) == SOCKET_ERROR) {
            std::cerr << "Failed to set socket options: " << WSAGetLastError() << std::endl;
            closesocket(serverSocket_);
            WSACleanup();
            return ErrorCode::CONNECTION_ERROR;
        }

        // Bind the socket to the specified port
        struct sockaddr_in serverAddr;
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(port);

        if (bind(serverSocket_, reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "Failed to bind socket to port " << port << ": " << WSAGetLastError() << std::endl;
            closesocket(serverSocket_);
            WSACleanup();
            return ErrorCode::CONNECTION_ERROR;
        }

        // Start listening for connections
        if (listen(serverSocket_, 5) == SOCKET_ERROR) {
            std::cerr << "Failed to listen on socket: " << WSAGetLastError() << std::endl;
            closesocket(serverSocket_);
            WSACleanup();
            return ErrorCode::CONNECTION_ERROR;
        }

        isRunning_ = true;
        std::cout << "NOTAM Server started on port " << port << std::endl;
        std::cout << "Maximum concurrent connections: " << ConnectionManager::MAX_CONNECTIONS << std::endl;

        return ErrorCode::SUCCESS;
    }

    void run(void) {
        if (!isRunning_) {
            std::cerr << "Server not started" << std::endl;
            return;
        }

        std::cout << "Waiting for connections..." << std::endl;

        while (isRunning_) {
            // Accept a new client connection
            struct sockaddr_in clientAddr;
            int clientAddrLen = sizeof(clientAddr);

            SOCKET clientSocket = accept(serverSocket_, reinterpret_cast<SOCKADDR*>(&clientAddr), &clientAddrLen);
            if (clientSocket == INVALID_SOCKET) {
                std::cerr << "Failed to accept connection: " << WSAGetLastError() << std::endl;
                continue;
            }

            // Get client information
            char clientIP[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIP, INET_ADDRSTRLEN);
            std::cout << "Client connected: " << clientIP << std::endl;

            // Handle the client in the current thread
            // In a production system, this would use a thread pool
            handleClient(clientSocket);
        }
    }

    void stop(void) {
        isRunning_ = false;
        if (serverSocket_ != INVALID_SOCKET) {
            closesocket(serverSocket_);
            serverSocket_ = INVALID_SOCKET;
            WSACleanup();
        }
        std::cout << "Server stopped" << std::endl;
    }

    ~TcpServer(void) {
        if (isRunning_) {
            stop();
        }
    }
};

// Main application
int32_t main(int argc, char* argv[]) {
    // Default parameters
    std::string notamFile = "notam_database.txt";
    uint16_t port = 8081;

    // Parse command line arguments
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            break;
        }

        if (strcmp(argv[i], "-f") == 0) {
            notamFile = argv[i + 1];
        }
        else if (strcmp(argv[i], "-p") == 0) {
            port = static_cast<uint16_t>(atoi(argv[i + 1]));
        }
    }

    std::cout << "NOTAM Server\n";
    std::cout << "============\n\n";

    // Initialize NOTAM database
    NotamDatabase notamDb;
    if (!notamDb.loadFromFile(notamFile)) {
        std::cerr << "Failed to load NOTAM database from: " << notamFile << std::endl;
        std::cerr << "Creating an empty database...\n";
    }
    else {
        std::cout << "Loaded NOTAM database from: " << notamFile << std::endl;
    }

    // Initialize connection manager
    ConnectionManager connectionManager;

    // Initialize NOTAM processor
    NotamProcessor processor(notamDb);

    // Initialize flight data handler
    FlightDataHandler handler(processor, connectionManager);

    // Start the TCP server
    TcpServer server(handler, connectionManager);
    ErrorCode startResult = server.start(port);

    if (startResult != ErrorCode::SUCCESS) {
        return static_cast<int32_t>(startResult);
    }

    // Run the server until stopped
    server.run();

    return 0;
}