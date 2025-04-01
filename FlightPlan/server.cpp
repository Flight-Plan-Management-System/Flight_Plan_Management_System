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
#include "curl/curl.h"

/*Windows Specific Additional Depenedencies*/
#pragma comment (lib,"Normaliz.lib")
#pragma comment (lib,"Ws2_32.lib")
#pragma comment (lib,"Wldap32.lib")
#pragma comment (lib,"Crypt32.lib")

#include <curl/curl.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

using json = nlohmann::json;

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

struct WeatherConditions {
    int conditionCode;
    char description[20];
    int depVisibility;
    int arrVisibility;
    int avgTemp;
    int tempMin;
    int tempMax;
    int windSpeed;
    int timezone;
    char airspace[32];
};

struct FlightLog {
    char flightId[16];
    char totalFlightTime[16];
    int fuelOnBoard;
    int estimatedFuelBurn;
    int totalWeight;
    char picName[32];
    char remarks[256];
	WeatherConditions weatherInfo;
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

struct WeatherStatus {
    bool weatherGood;
    std::string weatherMessage;
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
    NotamDatabase(void) : notams_() {}

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
    explicit NotamProcessor(const NotamDatabase& notamDb) : notamDb_(notamDb) {}

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

// Process Weather  to determine if they affect a flight
class WeatherProcessor {
private:
    std::string apiKey_;

    // Callback function to handle API response
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
        size_t total_size = size * nmemb;
        output->append((char*)contents, total_size);
        return total_size;
    }

public:
    WeatherProcessor(const std::string& apiKey)
        : apiKey_(apiKey) {}

    // Fetch weather data from API
    std::string fetchWeatherData(double latitude, double longitude) {
        CURL* curl;
        CURLcode res;
        std::string response_data;

        std::string url = "https://api.openweathermap.org/data/2.5/weather?lat="
            + std::to_string(latitude) + "&lon=" + std::to_string(longitude)
            + "&units=metric&appid=" + apiKey_;

        curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

            res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                std::cerr << "Request failed: " << curl_easy_strerror(res) << std::endl;
            }

            curl_easy_cleanup(curl);
        }
        else {
            std::cerr << "Failed to initialize cURL" << std::endl;
        }

        return response_data;
    }

    // Parse JSON and return a WeatherConditions struct
    WeatherConditions parseWeatherData(const std::string& jsonData) {
        WeatherConditions weatherConditions;

        try {
            json parsedJson = json::parse(jsonData);

            // Extract values
            weatherConditions.conditionCode = parsedJson["weather"][0]["id"];

            std::string desc = parsedJson["weather"][0]["description"];
            strncpy(weatherConditions.description, desc.c_str(), sizeof(weatherConditions.description) - 1);
            weatherConditions.description[sizeof(weatherConditions.description) - 1] = '\0';

            weatherConditions.arrVisibility = parsedJson["visibility"];

            weatherConditions.avgTemp = parsedJson["main"]["temp"];
            weatherConditions.tempMin = parsedJson["main"]["temp_min"];
            weatherConditions.tempMax = parsedJson["main"]["temp_max"];

            weatherConditions.windSpeed = parsedJson["wind"]["speed"];

            weatherConditions.timezone = parsedJson["timezone"];
        }
        catch (const std::exception& e) {
            std::cerr << "Error parsing weather data: " << e.what() << std::endl;
        }

        return weatherConditions;
    }

    // Update weather conditions and return the new conditions
    WeatherConditions updateWeather(double latitude, double longitude) {
        std::string jsonData = fetchWeatherData(latitude, longitude);
        if (!jsonData.empty()) {
            return parseWeatherData(jsonData);
        }
        else {
            std::cerr << "Error: Received empty weather data" << std::endl;
            return WeatherConditions(); // Return default/empty weather conditions
        }
    }

    // Determine if weather conditions are safe
    WeatherStatus isWeatherGood(const WeatherConditions& weatherConditions) {
        WeatherStatus status;
        status.weatherGood = true;
        status.weatherMessage.clear();

        // Condition codes that are not safe
        std::set<int> badConditionCodes = { 202, 212, 221, 232, 302, 312, 314, 503, 504, 522, 531, 602, 622, 781 };

        if (badConditionCodes.find(weatherConditions.conditionCode) != badConditionCodes.end()) {
            status.weatherMessage += "Dangerous weather conditions detected: " + std::string(weatherConditions.description) + "\n";
            status.weatherGood = false;
        }

        // Check visibility
        if (weatherConditions.arrVisibility <= 4828) {
            status.weatherMessage += "Reduced visibility detected: " + std::to_string(weatherConditions.arrVisibility) + " meters\n";
            status.weatherGood = false;
        }

        // Check temperature
        if (weatherConditions.tempMin < -40) {
            status.weatherMessage += "Extreme low temperature detected: " + std::to_string(weatherConditions.tempMin) + "°C\n";
            status.weatherGood = false;
        }

        // Check wind speed
        if (weatherConditions.windSpeed < 22) {
            status.weatherMessage += "High wind speed detected: " + std::to_string(weatherConditions.tempMin) + "km/h\n";
            status.weatherGood = false;
        }

        return status;
    }
};

class FlightDataHandler {
private:
    NotamProcessor& notamProcessor_;
    WeatherProcessor& weatherProcessor_;
    ConnectionManager& connectionManager_;

    // Cached state for multi-packet handling
    std::unordered_map<std::string, std::string> assembledMessages;
    std::unordered_map<std::string, std::set<uint64_t>> receivedSequences;

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

        while (std::getline(iss, line)) {
            if (line.find("FLIGHT_NUMBER=") == 0) {
                (void)SafeString::copy(plan.flightId, sizeof(plan.flightId), line.substr(14).c_str());
            }
            else if (line.find("AIRCRAFT_REG=") == 0) {
                (void)SafeString::copy(plan.aircraftReg, sizeof(plan.aircraftReg), line.substr(13).c_str());
            }
            else if (line.find("AIRCRAFT_TYPE=") == 0) {
                (void)SafeString::copy(plan.aircraftType, sizeof(plan.aircraftType), line.substr(14).c_str());
            }
            else if (line.find("OPERATOR=") == 0) {
                (void)SafeString::copy(plan.operator_, sizeof(plan.operator_), line.substr(9).c_str());
            }
            else if (line.find("DEP=") == 0) {
                (void)SafeString::copy(plan.departureAirport, sizeof(plan.departureAirport), line.substr(4).c_str());
            }
            else if (line.find("ARR=") == 0) {
                (void)SafeString::copy(plan.arrivalAirport, sizeof(plan.arrivalAirport), line.substr(4).c_str());
            }
            else if (line.find("ROUTE=") == 0) {
                (void)SafeString::copy(plan.route, sizeof(plan.route), line.substr(6).c_str());
            }
            else if (line.find("CRUISE_ALT=") == 0) {
                plan.cruiseAlt = std::stoi(line.substr(11));
            }
            else if (line.find("SPEED=") == 0) {
                plan.speed = std::stoi(line.substr(6));
            }
            else if (line.find("EOBT=") == 0) {
                (void)SafeString::copy(plan.etdTime, sizeof(plan.etdTime), line.substr(5).c_str());
            }
            else if (line.find("ETA=") == 0) {
                (void)SafeString::copy(plan.etaTime, sizeof(plan.etaTime), line.substr(4).c_str());
            }
        }

        // Get route airspaces
        plan.routeAirspaces = getRouteAirspaces(plan.departureAirport, plan.arrivalAirport);

        return plan;
    }

    FlightLog parseFlightLog(const std::string& data) const {
        FlightLog log;
        std::memset(&log, 0, sizeof(FlightLog));

        std::istringstream iss(data);
        std::string line;

        while (std::getline(iss, line)) {
            if (line.find("FLIGHT_NUMBER=") == 0) {
                (void)SafeString::copy(log.flightId, sizeof(log.flightId), line.substr(14).c_str());
            }
            else if (line.find("TOTAL_FLIGHT_TIME=") == 0) {
                (void)SafeString::copy(log.totalFlightTime, sizeof(log.totalFlightTime), line.substr(18).c_str());
            }
            else if (line.find("FUEL_ON_BOARD=") == 0) {
                log.fuelOnBoard = std::stoi(line.substr(14));
            }
            else if (line.find("ESTIMATED_FUEL_BURN=") == 0) {
                log.estimatedFuelBurn = std::stoi(line.substr(20));
            }
            else if (line.find("TOTAL_WEIGHT=") == 0) {
                log.totalWeight = std::stoi(line.substr(13));
            }
            else if (line.find("PIC=") == 0) {
                (void)SafeString::copy(log.picName, sizeof(log.picName), line.substr(4).c_str());
            }
            else if (line.find("REMARKS=") == 0) {
                (void)SafeString::copy(log.remarks, sizeof(log.remarks), line.substr(8).c_str());
            }
            // Weather info parsing
            else if (line.find("WEATHER_CONDITION_CODE=") == 0) {
                log.weatherInfo.conditionCode = std::stoi(line.substr(22));
            }
            else if (line.find("WEATHER_DESCRIPTION=") == 0) {
                (void)SafeString::copy(log.weatherInfo.description, sizeof(log.weatherInfo.description), line.substr(20).c_str());
            }
            else if (line.find("DEP_VISIBILITY=") == 0) {
                log.weatherInfo.depVisibility = std::stoi(line.substr(15));
            }
            else if (line.find("ARR_VISIBILITY=") == 0) {
                log.weatherInfo.arrVisibility = std::stoi(line.substr(15));
            }
            else if (line.find("AVG_TEMP=") == 0) {
                log.weatherInfo.avgTemp = std::stoi(line.substr(9));
            }
            else if (line.find("TEMP_MIN=") == 0) {
                log.weatherInfo.tempMin = std::stoi(line.substr(9));
            }
            else if (line.find("TEMP_MAX=") == 0) {
                log.weatherInfo.tempMax = std::stoi(line.substr(9));
            }
            else if (line.find("WIND_SPEED=") == 0) {
                log.weatherInfo.windSpeed = std::stoi(line.substr(11));
            }
            else if (line.find("TIMEZONE=") == 0) {
                log.weatherInfo.timezone = std::stoi(line.substr(9));
            }
            else if (line.find("AIRSPACE=") == 0) {
                (void)SafeString::copy(log.weatherInfo.airspace, sizeof(log.weatherInfo.airspace), line.substr(9).c_str());
            }
        }

        return log;
    }

public:
    explicit FlightDataHandler(NotamProcessor& processor, WeatherProcessor& weatherProcessor, ConnectionManager& connManager)
        : notamProcessor_(processor), weatherProcessor_(weatherProcessor), connectionManager_(connManager) {
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

    std::string processFlightData(const std::string& clientData, const std::string& clientId) {
        // First, parse the header
        PacketHeaderParser::ParsedHeader header = PacketHeaderParser::parseHeader(clientData);

        if (!header.isValid) {
            std::cerr << "Invalid packet header for client " << clientId << std::endl;
            return "ERROR: Invalid packet header";
        }

        // Find the payload (everything after END_HEADER)
        size_t payloadStart = clientData.find("END_HEADER\n");
        if (payloadStart == std::string::npos) {
            std::cerr << "No payload found for client " << clientId << std::endl;
            return "ERROR: No payload found";
        }
        payloadStart += 11; // Length of "END_HEADER\n"
        std::string payload = clientData.substr(payloadStart);

        // Debug print
        std::cout << "Received Packet - Client: " << clientId
            << ", Seq: " << header.sequenceNumber
            << ", Payload Size: " << payload.length()
            << ", Payload Type: "
            << (payload.find("FLIGHT_PLAN") != std::string::npos ? "FLIGHT_PLAN" :
                (payload.find("FLIGHT_LOG") != std::string::npos ? "FLIGHT_LOG" : "UNKNOWN"))
            << std::endl;

        // Validate payload size
        if (payload.length() != header.payloadSize) {
            std::cerr << "Payload size mismatch. Expected: " << header.payloadSize
                << ", Actual: " << payload.length() << std::endl;
            return "ERROR: Payload size mismatch";
        }

        // Track received sequences for this client
        auto& clientSequences = receivedSequences[clientId];

        // Check for duplicate sequence number
        if (clientSequences.count(header.sequenceNumber) > 0) {
            std::cerr << "Duplicate sequence number for client " << clientId
                << ": " << header.sequenceNumber << std::endl;
            return "ERROR: Duplicate sequence number";
        }
        clientSequences.insert(header.sequenceNumber);

        // Assemble multi-packet message
        assembledMessages[clientId] += payload;

        // Detect if this is the final packet
        if (clientSequences.size() >= 2) { // Assuming 2 packets for this example
            std::string fullMessage = assembledMessages[clientId];

            // Reset the assembled messages and sequences
            assembledMessages.erase(clientId);
            receivedSequences.erase(clientId);

            // Debug print full message
            std::cout << "Full Message Assembled for Client " << clientId << ":" << std::endl;
            std::cout << fullMessage << std::endl;

            // Process the full message
            if (fullMessage.find("FLIGHT_PLAN") != std::string::npos) {
                // Parse and store flight plan
                FlightPlan flightPlan = parseFlightPlan(fullMessage);

				FlightLog flightLog = parseFlightLog(fullMessage);

                // Get relevant NOTAMs
                std::vector<const Notam*> relevantNotams = notamProcessor_.getRelevantNotams(flightPlan);

                if (relevantNotams.empty()) {
                    return "NO_NOTAMS_FOUND\n";
                }

                // Create response with relevant NOTAMs
                std::ostringstream response;
                response << "NOTAMS AFFECTING YOUR FLIGHT:\n";
                for (const Notam* notam : relevantNotams) {
                    response << "NOTAM: " << notam->identifier << " for " << notam->location;
                    response << " - " << notam->description << "\n";
                }

                WeatherConditions conditions = weatherProcessor_.updateWeather(
                    flightPlan.routeAirspaces[0].center.latitude,
                    flightPlan.routeAirspaces[0].center.longitude
                );

                // Add to response with weather updates
                response << "WEATHER UPDATES AFFECTING YOUR FLIGHT::\n";

                // Pass the weather conditions to isWeatherGood
                WeatherStatus status = weatherProcessor_.isWeatherGood(conditions);

                if (status.weatherGood) {
                    response << "WEATHER UPDATE: Current conditions are favorable for flight operations.\n";
                }
                else {
                    response << "WEATHER WARNING:\n" << status.weatherMessage;
                }




                return response.str();
            }
        }

        // Partial message or waiting for more
        return "RECEIVED: Partial Message (Seq: " + std::to_string(header.sequenceNumber) + ")\n";
    }
};

// Server implementation functions
ServerStateMachine initializeWinsock() {
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "WSAStartup failed: " << result << std::endl;
        return ServerStateMachine::CONNECTION_ERROR;
    }
    return ServerStateMachine::SUCCESS;
}

ServerStateMachine createServerSocket(SOCKET& serverSocket) {
    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "Failed to create socket: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return ServerStateMachine::CONNECTION_ERROR;
    }

    // Set socket options to allow address reuse
    BOOL opt = TRUE;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&opt), sizeof(opt)) == SOCKET_ERROR) {
        std::cerr << "Failed to set socket options: " << WSAGetLastError() << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return ServerStateMachine::CONNECTION_ERROR;
    }

    return ServerStateMachine::SUCCESS;
}

ServerStateMachine bindSocketToPort(SOCKET serverSocket, uint16_t port) {
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(serverSocket, reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind socket to port " << port << ": " << WSAGetLastError() << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return ServerStateMachine::CONNECTION_ERROR;
    }

    return ServerStateMachine::SUCCESS;
}

ServerStateMachine startListening(SOCKET serverSocket) {
    if (listen(serverSocket, 5) == SOCKET_ERROR) {
        std::cerr << "Failed to listen on socket: " << WSAGetLastError() << std::endl;
        closesocket(serverSocket);
        WSACleanup();
        return ServerStateMachine::CONNECTION_ERROR;
    }

    return ServerStateMachine::SUCCESS;
}

void cleanupWinsock(SOCKET serverSocket) {
    if (serverSocket != INVALID_SOCKET) {
        closesocket(serverSocket);
    }
    WSACleanup();
}

void handleClientConnection(SOCKET clientSocket, FlightDataHandler& dataHandler, ConnectionManager& connectionManager) {
    std::string clientId; // Store the client ID for the entire connection
    bool isConnectionEstablished = false;
    const uint32_t BUFFER_SIZE = 4096U;

    while (true) {
        std::array<char, BUFFER_SIZE> buffer;
        int bytesRead = recv(clientSocket, buffer.data(), buffer.size() - 1, 0);

        if (bytesRead <= 0) {
            // Connection closed or error
            break;
        }

        // Null-terminate the received data
        buffer[static_cast<size_t>(bytesRead)] = '\0';
        std::string fullRequest(buffer.data());
        std::string response;

        // First, parse the header
        PacketHeaderParser::ParsedHeader header = PacketHeaderParser::parseHeader(fullRequest);

        if (!header.isValid) {
            response = "ERROR: Invalid packet header";
            send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
            break;
        }

        // Find the actual payload (after END_HEADER)
        size_t payloadStart = fullRequest.find("END_HEADER\n");
        if (payloadStart == std::string::npos) {
            response = "ERROR: Invalid packet format - no END_HEADER found";
            send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
            break;
        }
        payloadStart += 11; // Length of "END_HEADER\n"
        std::string payload = fullRequest.substr(payloadStart);

        // Verify payload size matches header
        if (payload.length() != header.payloadSize) {
            response = "ERROR: Payload size mismatch. Expected " +
                std::to_string(header.payloadSize) +
                ", got " + std::to_string(payload.length());
            send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
            break;
        }

        // Process different types of packets
        if (!isConnectionEstablished) {
            // Handle connection request
            if (payload.find("REQUEST_CONNECTION") != std::string::npos) {
                ConnectionRequest request;
                clientId = request.parseFromData(payload);

                if (clientId.empty()) {
                    response = "ERROR: Invalid connection request format";
                    send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                    break;
                }

                // Check if we can accept connection
                bool accepted = connectionManager.canAcceptConnection();
                if (accepted) {
                    connectionManager.addClient(clientId);
                    std::cout << "Connection accepted for client: " << clientId
                        << " (Active clients: " << connectionManager.getActiveClientCount() << ")" << std::endl;
                    response = ConnectionRequest::createAcceptResponse();
                    isConnectionEstablished = true;
                }
                else {
                    std::cout << "Connection rejected for client: " << clientId
                        << " (Maximum connections reached: " << connectionManager.getActiveClientCount() << ")" << std::endl;
                    response = ConnectionRequest::createRejectResponse();
                    break;
                }

                // Send the response back to the client
                send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);

                // Log the transaction
                std::cout << "\n=== Client Request ===\n" << fullRequest << std::endl;
                std::cout << "=== Server Response ===\n" << response << std::endl;
            }
            else {
                response = "ERROR: Connection request required first";
                send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                break;
            }
        }
        else {
            // Process flight data after connection is established
            try {
                response = dataHandler.processFlightData(fullRequest, clientId);

                // Only send response if it's not a partial message
                if (response.find("Partial Message") == std::string::npos) {
                    send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);

                    // Break the loop if we've processed the full flight data
                    if (response.find("NOTAMS AFFECTING YOUR FLIGHT") != std::string::npos) {
                        break;
                    }
                }
            }
            catch (const std::exception& e) {
                response = "ERROR: Processing flight data failed - " + std::string(e.what());
                send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                break;
            }
        }
    }

    // Close the client socket
    closesocket(clientSocket);

    // Remove the client from connection manager
    if (!clientId.empty()) {
        connectionManager.removeClient(clientId);
    }
}

class TcpServer {
private:
    SOCKET serverSocket_;
    FlightDataHandler& dataHandler_;
    ConnectionManager& connectionManager_;
    bool isRunning_;
    static const uint32_t BUFFER_SIZE = 4096U;

public:
    explicit TcpServer(FlightDataHandler& handler, ConnectionManager& connManager) :
        serverSocket_(INVALID_SOCKET),
        dataHandler_(handler),
        connectionManager_(connManager),
        isRunning_(false) {
        // No initialization needed
    }

    ServerStateMachine start(uint16_t port) {
        // Initialize Winsock
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "WSAStartup failed: " << result << std::endl;
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Create the server socket
        serverSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (serverSocket_ == INVALID_SOCKET) {
            std::cerr << "Failed to create socket: " << WSAGetLastError() << std::endl;
            WSACleanup();
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Set socket options to allow address reuse
        BOOL opt = TRUE;
        if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&opt), sizeof(opt)) == SOCKET_ERROR) {
            std::cerr << "Failed to set socket options: " << WSAGetLastError() << std::endl;
            closesocket(serverSocket_);
            WSACleanup();
            return ServerStateMachine::CONNECTION_ERROR;
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
            return ServerStateMachine::CONNECTION_ERROR;
        }

        // Start listening for connections
        if (listen(serverSocket_, 5) == SOCKET_ERROR) {
            std::cerr << "Failed to listen on socket: " << WSAGetLastError() << std::endl;
            closesocket(serverSocket_);
            WSACleanup();
            return ServerStateMachine::CONNECTION_ERROR;
        }

        isRunning_ = true;
        std::cout << "NOTAM Server started on port " << port << std::endl;
        std::cout << "Maximum concurrent connections: " << ConnectionManager::MAX_CONNECTIONS << std::endl;

        return ServerStateMachine::SUCCESS;
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
            handleClientConnection(clientSocket, dataHandler_, connectionManager_);
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

    // 	// api key for openweathermap
    std::string apiKey = "445b28699592a8c90c07b345dd4de9cd";

    // Initialize connection manager
    ConnectionManager connectionManager;

    // Initialize NOTAM processor
    NotamProcessor processor(notamDb);

    // Initialize weather conditions
    WeatherConditions conditions;

    // Initialize Weather processor
    WeatherProcessor weatherProcessor(apiKey);

    // Initialize flight data handler
    FlightDataHandler handler(processor, weatherProcessor, connectionManager);

    // Start the TCP server
    TcpServer server(handler, connectionManager);
    ServerStateMachine startResult = server.start(port);

    if (startResult != ServerStateMachine::SUCCESS) {
        return static_cast<int32_t>(startResult);
    }

    // Run the server until stopped
    server.run();

    return 0;
}