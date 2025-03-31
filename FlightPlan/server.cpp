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
#include <unordered_map>
#include <chrono>
#include <iomanip>
#include <atomic>

#include <nlohmann/json.hpp>

#define CURL_STATICLIB

#include <curl/curl.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

// Error handling utilities
enum class ServerStateMachine
{
    SUCCESS = 0,
    INVALID_INPUT = 1,
    CONNECTION_ERROR = 2,
    PROCESSING_ERROR = 3,
    DATABASE_ERROR = 4
};

class FlightDataHandler;
class ConnectionManager;

// Function declarations
ServerStateMachine initializeWinsock();
ServerStateMachine createServerSocket(SOCKET& serverSocket);
ServerStateMachine bindSocketToPort(SOCKET serverSocket, uint16_t port);
ServerStateMachine startListening(SOCKET serverSocket);
void cleanupWinsock(SOCKET serverSocket);
void acceptAndHandleConnections(SOCKET serverSocket, FlightDataHandler& dataHandler, ConnectionManager& connectionManager);
void handleClientConnection(SOCKET clientSocket, FlightDataHandler& dataHandler, ConnectionManager& connectionManager);
std::string processConnectionRequest(const std::string& clientData, ConnectionManager& connectionManager);
std::string processFlightData(const std::string& clientData, const std::string& clientId, FlightDataHandler& dataHandler);

// Packet header parser
class PacketHeaderParser {
public:
    struct ParsedHeader {
        uint64_t sequenceNumber;
        std::string timestamp;
        size_t payloadSize;
        bool isValid;
    };

    static ParsedHeader parseHeader(const std::string& headerData) {
        ParsedHeader header;
        header.isValid = false;

        std::istringstream iss(headerData);
        std::string line;
        std::string currentSection;

        while (std::getline(iss, line)) {
            if (line == "HEADER") {
                currentSection = "HEADER";
                continue;
            }

            if (line == "END_HEADER") {
                header.isValid = true;
                break;
            }

            if (currentSection == "HEADER") {
                size_t delimPos = line.find('=');
                if (delimPos != std::string::npos) {
                    std::string key = line.substr(0, delimPos);
                    std::string value = line.substr(delimPos + 1);

                    if (key == "SEQ_NUM") {
                        header.sequenceNumber = std::stoull(value);
                    }
                    else if (key == "TIMESTAMP") {
                        header.timestamp = value;
                    }
                    else if (key == "PAYLOAD_SIZE") {
                        header.payloadSize = std::stoull(value);
                    }
                }
            }
        }

        return header;
    }
};

// Safe string operations
class SafeString {
private:
    static const uint32_t MAX_STRING_LENGTH = 256U;

public:
    static ServerStateMachine copy(char* dest, uint32_t destSize, const char* src) {
        if ((NULL == dest) || (NULL == src) || (0U == destSize)) {
            return ServerStateMachine::INVALID_INPUT;
        }

        const uint32_t srcLen = static_cast<uint32_t>(strlen(src));
        const uint32_t copyLen = (srcLen < (destSize - 1U)) ? srcLen : (destSize - 1U);

        (void)memcpy(dest, src, copyLen);
        dest[copyLen] = '\0';

        return ServerStateMachine::SUCCESS;
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
    char aircraftReg[16];
    char aircraftType[32];
    char operator_[32];
    char route[64];
    int cruiseAlt;
    int speed;
    char etdTime[20];
    char etaTime[20];
    std::vector<AirspaceInfo> routeAirspaces;
};

struct FlightLog {
    char flightId[16];
    char totalFlightTime[16];
    int fuelOnBoard;
    int estimatedFuelBurn;
    int totalWeight;
    char picName[32];
    char remarks[256];
    struct {
        char depVisibility[16];
        char arrVisibility[16];
        int avgTemp;
        int tempMin;
        int tempMax;
        int windSpeed;
        int windDir;
        int windGust;
        char cloudInfo[64];
        char precipitation[32];
        char timezone[8];
        char airspace[32];
    } weatherInfo;
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

