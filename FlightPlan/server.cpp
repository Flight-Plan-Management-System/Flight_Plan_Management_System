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
#include <map>

#include <nlohmann/json.hpp>

#define CURL_STATICLIB
#include "curl/curl.h"

/*Windows Specific Additional Depenedencies*/
#pragma comment (lib,"Normaliz.lib")
#pragma comment (lib,"Ws2_32.lib")
#pragma comment (lib,"Wldap32.lib")
#pragma comment (lib,"Crypt32.lib")


#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

namespace ServerFlightManagement {



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
                    else
                    {
						//"Unknown header field
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
        ServerStateMachine result = ServerStateMachine::SUCCESS;

        if ((nullptr == dest) || (nullptr == src) || (0U == destSize)) {
            result = ServerStateMachine::INVALID_INPUT;
        }
        else {
            uint32_t index = 0U;

            while ((index < (destSize - 1U)) && (src[index] != '\0')) {
                dest[index] = src[index];
                index++;
            }

            dest[index] = '\0';
        }

        return result;
    }

    static int compare(const char* lhs, const char* rhs) {
        int result = 0;

        if ((nullptr == lhs) || (nullptr == rhs)) {
            result = -1;
        }
        else {
            const size_t lhsLen = strnlen(lhs, MAX_STRING_LENGTH);
            const size_t rhsLen = strnlen(rhs, MAX_STRING_LENGTH);

            if (lhsLen != rhsLen) {
                if (lhsLen > rhsLen) {
                    result = 1;
                }
                else {
                    result = -1;
                }
            }
            else {
                result = std::strncmp(lhs, rhs, lhsLen);
            }
        }

        return result;
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

        static const std::string requestConnection = "REQUEST_CONNECTION";
        static const std::string clientIdPrefix = "CLIENT_ID=";

        if (std::getline(iss, line) && (line == requestConnection)) {
            while (std::getline(iss, line)) {
                if (line.find(clientIdPrefix) == 0U) {
                    clientId = line.substr(clientIdPrefix.length());
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
    std::map<std::string, SOCKET> activeClients; // Store client sockets
    std::mutex mutex;

public:
    bool canAcceptConnection() {
        std::lock_guard<std::mutex> lock(mutex);
        return activeClients.size() < MAX_CONNECTIONS;
    }

    bool addClient(const std::string& clientId, SOCKET clientSocket) {
        bool result = true;
        std::lock_guard<std::mutex> lock(mutex);

        if (activeClients.size() >= MAX_CONNECTIONS) {
            result = false;
        }
        else {
            activeClients[clientId] = clientSocket;
        }

        return result;
    }

    void removeClient(const std::string& clientId) {
        std::lock_guard<std::mutex> lock(mutex);
        const size_t erased = activeClients.erase(clientId);
        (void)erased;
    }

    size_t getActiveClientCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return activeClients.size();
    }

    std::vector<SOCKET> getATCsClients() {
        std::vector<SOCKET> matchingClients;
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& client : activeClients) {
            if (!client.first.empty() && client.first[0] == 'A') {
                matchingClients.push_back(client.second);
            }
        }
        return matchingClients;
    }

    void broadcastFlightPlan(const FlightPlan& flightPlan) {
        // Get the list of ATC clients
        std::vector<SOCKET> atcClients = getATCsClients();

  
        std::string flightPlanData = serializeFlightPlan(flightPlan);

        // Generate the current timestamp
        std::time_t now = std::time(nullptr);
        std::tm* tmNow = std::localtime(&now);
        std::stringstream timestampSS;
        timestampSS << (1900 + tmNow->tm_year) << (tmNow->tm_mon + 1) << tmNow->tm_mday
            << tmNow->tm_hour << tmNow->tm_min << tmNow->tm_sec;

        // Generate sequence number (this can be incremented for each new packet)
        static int seqNum = 1;

        // Create the header
        size_t payloadSize = flightPlanData.length();
        std::stringstream headerSS;
        headerSS << "HEADER\n"
            << "SEQ_NUM=" << seqNum++ << "\n"
            << "TIMESTAMP=" << timestampSS.str() << "\n"
            << "PAYLOAD_SIZE=" << payloadSize << "\n"
            << "END_HEADER\n";

        // Combine header and payload into the final packet
        std::string packet = headerSS.str() + flightPlanData;

        // Send the packet to each ATC client
        for (SOCKET clientSocket : atcClients) {
            int bytesSent = send(clientSocket, packet.c_str(), static_cast<int>(packet.length()), 0);
            if (bytesSent == SOCKET_ERROR) {
                std::cerr << "Failed to send packet to client with socket " << clientSocket << "\n";
            }
        }
    }

    std::string serializeFlightPlan(const FlightPlan& flightPlan) {
        std::ostringstream flightPlanStream;

        flightPlanStream << "FLIGHT_ID=" << flightPlan.flightId << "\n"
            << "DEPARTURE_AIRPORT=" << flightPlan.departureAirport << "\n"
            << "ARRIVAL_AIRPORT=" << flightPlan.arrivalAirport << "\n"
            << "AIRCRAFT_REG=" << flightPlan.aircraftReg << "\n"
            << "AIRCRAFT_TYPE=" << flightPlan.aircraftType << "\n"
            << "OPERATOR=" << flightPlan.operator_ << "\n"
            << "ROUTE=" << flightPlan.route << "\n"
            << "CRUISE_ALT=" << flightPlan.cruiseAlt << "\n"
            << "SPEED=" << flightPlan.speed << "\n"
            << "ETD_TIME=" << flightPlan.etdTime << "\n"
            << "ETA_TIME=" << flightPlan.etaTime << "\n";

        // Serialize route airspaces
        for (const auto& airspace : flightPlan.routeAirspaces) {
            flightPlanStream << "AIRSPACE_ID=" << airspace.identifier << "\n"
                << "AIRSPACE_CENTER_LAT=" << airspace.center.latitude << "\n"
                << "AIRSPACE_CENTER_LON=" << airspace.center.longitude << "\n"
                << "AIRSPACE_RADIUS=" << airspace.radius << "\n";
        }

        return flightPlanStream.str();
    }
};

// NOTAM database
class NotamDatabase {
private:
    std::vector<Notam> notams_;

    bool parseNotamLine(const std::string& line, Notam& notam) {
        bool result = true;

        std::istringstream iss(line);
        std::string token;

        if (line.empty() || line[0] == '#') {
            result = false;
        }

        if (result && !std::getline(iss, token, '|')) {
            result = false;
        }
        if (result) {
            (void)strncpy(&notam.identifier[0], token.c_str(), sizeof(notam.identifier) - 1U);
            notam.identifier[sizeof(notam.identifier) - 1U] = '\0';
        }

        if (result && !std::getline(iss, token, '|')) {
            result = false;
        }
        if (result) {
            (void)strncpy(&notam.fir[0], token.c_str(), sizeof(notam.fir) - 1U);
            notam.fir[sizeof(notam.fir) - 1U] = '\0';
        }

        if (result && !std::getline(iss, token, '|')) {
            result = false;
        }
        if (result) {
            (void)strncpy(&notam.location[0], token.c_str(), sizeof(notam.location) - 1U);
            notam.location[sizeof(notam.location) - 1U] = '\0';
        }

        if (result && !std::getline(iss, token, '|')) {
            result = false;
        }
        if (result) {
            (void)strncpy(&notam.startTime[0], token.c_str(), sizeof(notam.startTime) - 1U);
            notam.startTime[sizeof(notam.startTime) - 1U] = '\0';
        }

        if (result && !std::getline(iss, token, '|')) {
            result = false;
        }
        if (result) {
            (void)strncpy(&notam.endTime[0], token.c_str(), sizeof(notam.endTime) - 1U);
            notam.endTime[sizeof(notam.endTime) - 1U] = '\0';
        }

        if (result && !std::getline(iss, token, '|')) {
            result = false;
        }
        if (result) {
            (void)strncpy(&notam.affectedAirspace.identifier[0], token.c_str(), sizeof(notam.affectedAirspace.identifier) - 1U);
            notam.affectedAirspace.identifier[sizeof(notam.affectedAirspace.identifier) - 1U] = '\0';
        }

        if (result && !std::getline(iss, token, '|')) {
            result = false;
        }
        if (result) {
            notam.affectedAirspace.center.latitude = std::stod(token);
        }

        if (result && !std::getline(iss, token, '|')) {
            result = false;
        }
        if (result) {
            notam.affectedAirspace.center.longitude = std::stod(token);
        }

        if (result && !std::getline(iss, token, '|')) {
            result = false;
        }
        if (result) {
            notam.affectedAirspace.radius = std::stod(token);
        }

        if (result && !std::getline(iss, token)) {
            result = false;
        }
        if (result) {
            (void)strncpy(&notam.description[0], token.c_str(), sizeof(notam.description) - 1U);
            notam.description[sizeof(notam.description) - 1U] = '\0';
        }

        return result;
    }

public:
    NotamDatabase(void) : notams_() {}

    bool loadFromFile(const std::string& filename) {
        bool result = true;

        std::ifstream file(filename);
        if (!file.is_open()) {
            result = false;
        }
        else {
            std::string line;
            while (std::getline(file, line)) {
                Notam notam;
                if (parseNotamLine(line, notam)) {
                    notams_.push_back(notam);
                }
            }
        }

        return result;
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
        return (SafeString::compare(spaceId1, spaceId2) == 0);
    }

public:
    explicit NotamProcessor(const NotamDatabase& notamDb) : notamDb_(notamDb) {}

    std::vector<const Notam*> getRelevantNotams(const FlightPlan& flightPlan) const {
        std::vector<const Notam*> relevantNotams;
        const std::vector<Notam>& allNotams = notamDb_.getAllNotams();

        for (const Notam& notam : allNotams) {
            const bool affectsDeparture = (SafeString::compare(&notam.location[0], &flightPlan.departureAirport[0]) == 0);
            const bool affectsArrival = (SafeString::compare(&notam.location[0], &flightPlan.arrivalAirport[0]) == 0);

            if (affectsDeparture || affectsArrival) {
                relevantNotams.push_back(&notam);
                continue;
            }

            for (const AirspaceInfo& routeSpace : flightPlan.routeAirspaces) {
                if (isAirspaceAffected(&routeSpace.identifier[0], &notam.affectedAirspace.identifier[0])) {
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
    static size_t WriteCallback(const void* contents, size_t size, size_t nmemb, std::string* output) {
        const size_t total_size = size * nmemb;

        const char* charData = static_cast<const char*>(contents);
        (void)output->append(charData, total_size);

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

        curl = curl_easy_init();
        if (curl != nullptr) {
            const std::string url = "https://api.openweathermap.org/data/2.5/weather?lat="
                + std::to_string(latitude) + "&lon=" + std::to_string(longitude)
                + "&units=metric&appid=" + apiKey_;

            CURLcode setoptResult = curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            if (setoptResult != CURLE_OK) {
                std::cerr << "Failed to set CURLOPT_URL: " << curl_easy_strerror(setoptResult) << std::endl;
            }

            setoptResult = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            if (setoptResult != CURLE_OK) {
                std::cerr << "Failed to set CURLOPT_WRITEFUNCTION: " << curl_easy_strerror(setoptResult) << std::endl;
            }

            setoptResult = curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
            if (setoptResult != CURLE_OK) {
                std::cerr << "Failed to set CURLOPT_WRITEDATA: " << curl_easy_strerror(setoptResult) << std::endl;
            }

            setoptResult = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            if (setoptResult != CURLE_OK) {
                std::cerr << "Failed to set CURLOPT_FOLLOWLOCATION: " << curl_easy_strerror(setoptResult) << std::endl;
            }

            setoptResult = curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            if (setoptResult != CURLE_OK) {
                std::cerr << "Failed to set CURLOPT_TIMEOUT: " << curl_easy_strerror(setoptResult) << std::endl;
            }

            res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                std::cerr << "Request failed: " << curl_easy_strerror(res) << std::endl;
            }

            (void)curl_easy_cleanup(curl);
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
            (void)SafeString::copy(&weatherConditions.description[0], sizeof(weatherConditions.description), desc.c_str());
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
        WeatherConditions weatherConditions;

        const std::string jsonData = fetchWeatherData(latitude, longitude);

        if (!jsonData.empty()) {
            weatherConditions = parseWeatherData(jsonData);
        }
        else {
            std::cerr << "Error: Received empty weather data" << std::endl;
            // weatherConditions is already default-initialized
        }

        return weatherConditions;
    }

    // Determine if weather conditions are safe
    WeatherStatus isWeatherGood(const WeatherConditions& weatherConditions) {
        WeatherStatus status;
        status.weatherGood = true;
        status.weatherMessage.clear();

        // Condition codes that are not safe
        std::set<int> badConditionCodes = { 202, 212, 221, 232, 302, 312, 314, 503, 504, 522, 531, 602, 622, 781 };

        if (badConditionCodes.find(weatherConditions.conditionCode) != badConditionCodes.end()) {
            status.weatherMessage += "Dangerous weather conditions detected: " + std::string(&weatherConditions.description[0]) + "\n";
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
        if (weatherConditions.windSpeed > 22) {
            status.weatherMessage += "High wind speed detected: " + std::to_string(weatherConditions.windSpeed) + "km/h\n";
            status.weatherGood = false;
        }

        return status;
    }
};

// Class for Fuel Checking
class FuelChecker {
private:

    std::map<std::string, int> fuelBurnRates = {
        {"Boeing 737", 50},
        {"Airbus A320", 45},
        {"Embraer E175", 30},
        {"Boeing 787", 100}
    };
    const int MINIMUM_RESERVE_FUEL = 1000; // minimum reserve fuel

    // Convert HH:MM format flight time to total minutes
    int convertFlightTimeToMinutes(const char* flightTime) {
        int totalMinutes = 0;

        if (flightTime != nullptr) {
            int hours = ((static_cast<int>(flightTime[0]) - static_cast<int>('0')) * 10)
                + (static_cast<int>(flightTime[1]) - static_cast<int>('0'));

            int minutes = ((static_cast<int>(flightTime[3]) - static_cast<int>('0')) * 10)
                + (static_cast<int>(flightTime[4]) - static_cast<int>('0'));

            totalMinutes = (hours * 60) + minutes;
        }

        return totalMinutes;
    }

public:
    // Check if the aircraft has enough fuel for the flight
    bool hasSufficientFuel(const FlightLog& log) {
        return log.fuelOnBoard >= log.estimatedFuelBurn;
    }

    // Check if reserve fuel is within safe limits
    bool meetsReserveFuelRequirement(const FlightLog& log) {
        int remainingFuel = log.fuelOnBoard - log.estimatedFuelBurn;
        return remainingFuel >= MINIMUM_RESERVE_FUEL;
    }

    // Detects unusual fuel burn rates
    bool isUnusualFuelBurn(const FlightLog& log, const std::string& aircraftType) {
        bool returnValue = false;

        auto burnRateIt = fuelBurnRates.find(aircraftType);
        if (burnRateIt != fuelBurnRates.end()) {
            int expectedBurnRate = burnRateIt->second; // Liters per minute
            int flightDuration = convertFlightTimeToMinutes(&log.totalFlightTime[0]);
            int expectedFuelBurn = flightDuration * expectedBurnRate;

            float expectedFuelBurnF = static_cast<float>(expectedFuelBurn);
            float estimatedFuelBurnF = static_cast<float>(log.estimatedFuelBurn);

            float lowerLimit = expectedFuelBurnF * 0.8F;
            float upperLimit = expectedFuelBurnF * 1.2F;

            if ((estimatedFuelBurnF < lowerLimit) || (estimatedFuelBurnF > upperLimit)) {
                returnValue = true;
            }
        }

        return returnValue;
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

        static const char cyyzMsg[] = "CYYZ";
        static const char kjfkMsg[] = "KJFK";
        static const char cykfMsg[] = "CYKF";
        static const char cyulMsg[] = "CYUL";
        static const char cyowMsg[] = "CYOW";
        static const char kbufMsg[] = "KBUF";

        // Toronto to New York route
        if ((0 == SafeString::compare(departure, &cyyzMsg[0])) &&
            (0 == SafeString::compare(arrival, &kjfkMsg[0]))) {

            const char* routePoints[] = { &cyyzMsg[0], &kbufMsg[0], &kjfkMsg[0] };
            for (const char* point : routePoints) {
                AirspaceInfo space;
                (void)SafeString::copy(&space.identifier[0], sizeof(space.identifier), point);
                space.center.latitude = 0.0;
                space.center.longitude = 0.0;
                space.radius = 15.0;
                airspaces.push_back(space);
            }
        }
        // Waterloo to Montreal route
        else if ((0 == SafeString::compare(departure, &cykfMsg[0])) &&
            (0 == SafeString::compare(arrival, &cyulMsg[0]))) {

            const char* routePoints[] = { &cykfMsg[0], &cyowMsg[0], &cyulMsg[0] };
            for (const char* point : routePoints) {
                AirspaceInfo space;
                (void)SafeString::copy(&space.identifier[0], sizeof(space.identifier), point);
                space.center.latitude = 0.0;
                space.center.longitude = 0.0;
                space.radius = 15.0;
                airspaces.push_back(space);
            }
        }
        // Generic fallback - just use departure and arrival
        else {
            AirspaceInfo depSpace;
            (void)SafeString::copy(&depSpace.identifier[0], sizeof(depSpace.identifier), departure);
            depSpace.center.latitude = 0.0;
            depSpace.center.longitude = 0.0;
            depSpace.radius = 15.0;
            airspaces.push_back(depSpace);

            AirspaceInfo arrSpace;
            (void)SafeString::copy(&arrSpace.identifier[0], sizeof(arrSpace.identifier), arrival);
            arrSpace.center.latitude = 0.0;
            arrSpace.center.longitude = 0.0;
            arrSpace.radius = 15.0;
            airspaces.push_back(arrSpace);
        }

        return airspaces;
    }

    FlightPlan parseFlightPlan(const std::string& data) const {
        FlightPlan plan;
        (void)std::memset(&plan, 0, sizeof(FlightPlan));

        static const char flightNumberMsg[] = "FLIGHT_NUMBER=";
        static const char aircraftRegMsg[] = "AIRCRAFT_REG=";
        static const char aircraftTypeMsg[] = "AIRCRAFT_TYPE=";
        static const char operatorMsg[] = "OPERATOR=";
        static const char depMsg[] = "DEP=";
        static const char arrMsg[] = "ARR=";
        static const char routeMsg[] = "ROUTE=";
        static const char cruiseAltMsg[] = "CRUISE_ALT=";
        static const char speedMsg[] = "SPEED=";
        static const char eobtMsg[] = "EOBT=";
        static const char etaMsg[] = "ETA=";

        std::istringstream iss(data);
        std::string line;

        while (std::getline(iss, line)) {
            if (line.find(&flightNumberMsg[0]) == 0U) {
                (void)SafeString::copy(&plan.flightId[0], sizeof(plan.flightId), line.substr(14).c_str());
            }
            else if (line.find(&aircraftRegMsg[0]) == 0U) {
                (void)SafeString::copy(&plan.aircraftReg[0], sizeof(plan.aircraftReg), line.substr(13).c_str());
            }
            else if (line.find(&aircraftTypeMsg[0]) == 0U) {
                (void)SafeString::copy(&plan.aircraftType[0], sizeof(plan.aircraftType), line.substr(14).c_str());
            }
            else if (line.find(&operatorMsg[0]) == 0U) {
                (void)SafeString::copy(&plan.operator_[0], sizeof(plan.operator_), line.substr(9).c_str());
            }
            else if (line.find(&depMsg[0]) == 0U) {
                (void)SafeString::copy(&plan.departureAirport[0], sizeof(plan.departureAirport), line.substr(4).c_str());
            }
            else if (line.find(&arrMsg[0]) == 0U) {
                (void)SafeString::copy(&plan.arrivalAirport[0], sizeof(plan.arrivalAirport), line.substr(4).c_str());
            }
            else if (line.find(&routeMsg[0]) == 0U) {
                (void)SafeString::copy(&plan.route[0], sizeof(plan.route), line.substr(6).c_str());
            }
            else if (line.find(&cruiseAltMsg[0]) == 0U) {
                plan.cruiseAlt = std::stoi(line.substr(11));
            }
            else if (line.find(&speedMsg[0]) == 0U) {
                plan.speed = std::stoi(line.substr(6));
            }
            else if (line.find(&eobtMsg[0]) == 0U) {
                (void)SafeString::copy(&plan.etdTime[0], sizeof(plan.etdTime), line.substr(5).c_str());
            }
            else if (line.find(&etaMsg[0]) == 0U) {
                (void)SafeString::copy(&plan.etaTime[0], sizeof(plan.etaTime), line.substr(4).c_str());
            }
            else {
                //Unknown log field
            }
        }

        // Get route airspaces
        plan.routeAirspaces = getRouteAirspaces(&plan.departureAirport[0], &plan.arrivalAirport[0]);

        return plan;
    }

    FlightLog parseFlightLog(const std::string& data) const {
        FlightLog log;
        (void)std::memset(&log, 0, sizeof(FlightLog));

        static const char flightNumberMsg[] = "FLIGHT_NUMBER=";
        static const char totalFlightTimeMsg[] = "TOTAL_FLIGHT_TIME=";
        static const char fuelOnBoardMsg[] = "FUEL_ON_BOARD=";
        static const char estimatedFuelBurnMsg[] = "ESTIMATED_FUEL_BURN=";
        static const char totalWeightMsg[] = "TOTAL_WEIGHT=";
        static const char picMsg[] = "PIC=";
        static const char remarksMsg[] = "REMARKS=";
        static const char weatherConditionCodeMsg[] = "WEATHER_CONDITION_CODE=";
        static const char weatherDescriptionMsg[] = "WEATHER_DESCRIPTION=";
        static const char depVisibilityMsg[] = "DEP_VISIBILITY=";
        static const char arrVisibilityMsg[] = "ARR_VISIBILITY=";
        static const char avgTempMsg[] = "AVG_TEMP=";
        static const char tempMinMsg[] = "TEMP_MIN=";
        static const char tempMaxMsg[] = "TEMP_MAX=";
        static const char windSpeedMsg[] = "WIND_SPEED=";
        static const char timezoneMsg[] = "TIMEZONE=";
        static const char airspaceMsg[] = "AIRSPACE=";

        std::istringstream iss(data);
        std::string line;

        while (std::getline(iss, line)) {
            if (line.find(&flightNumberMsg[0]) == 0U) {
                (void)SafeString::copy(&log.flightId[0], sizeof(log.flightId), line.substr(14).c_str());
            }
            else if (line.find(&totalFlightTimeMsg[0]) == 0U) {
                (void)SafeString::copy(&log.totalFlightTime[0], sizeof(log.totalFlightTime), line.substr(18).c_str());
            }
            else if (line.find(&fuelOnBoardMsg[0]) == 0U) {
                log.fuelOnBoard = std::stoi(line.substr(14));
            }
            else if (line.find(&estimatedFuelBurnMsg[0]) == 0U) {
                log.estimatedFuelBurn = std::stoi(line.substr(20));
            }
            else if (line.find(&totalWeightMsg[0]) == 0U) {
                log.totalWeight = std::stoi(line.substr(13));
            }
            else if (line.find(&picMsg[0]) == 0U) {
                (void)SafeString::copy(&log.picName[0], sizeof(log.picName), line.substr(4).c_str());
            }
            else if (line.find(&remarksMsg[0]) == 0U) {
                (void)SafeString::copy(&log.remarks[0], sizeof(log.remarks), line.substr(8).c_str());
            }
            else if (line.find(&weatherConditionCodeMsg[0]) == 0U) {
                log.weatherInfo.conditionCode = std::stoi(line.substr(22));
            }
            else if (line.find(&weatherDescriptionMsg[0]) == 0U) {
                (void)SafeString::copy(&log.weatherInfo.description[0], sizeof(log.weatherInfo.description), line.substr(20).c_str());
            }
            else if (line.find(&depVisibilityMsg[0]) == 0U) {
                log.weatherInfo.depVisibility = std::stoi(line.substr(15));
            }
            else if (line.find(&arrVisibilityMsg[0]) == 0U) {
                log.weatherInfo.arrVisibility = std::stoi(line.substr(15));
            }
            else if (line.find(&avgTempMsg[0]) == 0U) {
                log.weatherInfo.avgTemp = std::stoi(line.substr(9));
            }
            else if (line.find(&tempMinMsg[0]) == 0U) {
                log.weatherInfo.tempMin = std::stoi(line.substr(9));
            }
            else if (line.find(&tempMaxMsg[0]) == 0U) {
                log.weatherInfo.tempMax = std::stoi(line.substr(9));
            }
            else if (line.find(&windSpeedMsg[0]) == 0U) {
                log.weatherInfo.windSpeed = std::stoi(line.substr(11));
            }
            else if (line.find(&timezoneMsg[0]) == 0U) {
                log.weatherInfo.timezone = std::stoi(line.substr(9));
            }
            else if (line.find(&airspaceMsg[0]) == 0U) {
                (void)SafeString::copy(&log.weatherInfo.airspace[0], sizeof(log.weatherInfo.airspace), line.substr(9).c_str());
            }
            else {
                //Unknown log field
            }
        }

        return log;
    }

public:
    explicit FlightDataHandler(NotamProcessor& processor, WeatherProcessor& weatherProcessor, ConnectionManager& connManager)
        : notamProcessor_(processor), weatherProcessor_(weatherProcessor), connectionManager_(connManager) {
    }

    std::string processConnectionRequest(const std::string& clientData, SOCKET clientSocket) {
        std::string returnValue; 

        // Parse connection request
        ConnectionRequest request;
        std::string clientId = request.parseFromData(clientData);

        if (clientId.empty()) {
            returnValue = "ERROR: Invalid connection request format";
        }
        else {
            bool clientAdded = connectionManager_.addClient(clientId, clientSocket);
            if (!clientAdded) {
                std::cerr << "Failed to add client: " << clientId << std::endl;
                returnValue = ConnectionRequest::createRejectResponse();
            }
            else {
                std::cout << "Connection accepted for client: " << clientId
                    << " (Active clients: " << connectionManager_.getActiveClientCount() << ")" << std::endl;

                returnValue = ConnectionRequest::createAcceptResponse();
            }
        }

        return returnValue;
    }

    std::string processFlightData(const std::string& clientData, const std::string& clientId) {
        static const char endHeaderMsg[] = "END_HEADER\n";
        static const char flightPlanMsg[] = "FLIGHT_PLAN";
        static const char flightLogMsg[] = "FLIGHT_LOG";
        static const char unknownMsg[] = "UNKNOWN";

        std::string returnValue; // Final return value
        std::ostringstream response;

        PacketHeaderParser::ParsedHeader header = PacketHeaderParser::parseHeader(clientData);

        if (!header.isValid) {
            std::cerr << "Invalid packet header for client " << clientId << std::endl;
            returnValue = "ERROR: Invalid packet header";
        }
        else {
            size_t payloadStart = clientData.find(&endHeaderMsg[0]);
            if (payloadStart == std::string::npos) {
                std::cerr << "No payload found for client " << clientId << std::endl;
                returnValue = "ERROR: No payload found";
            }
            else {
                payloadStart += 11U; // Length of "END_HEADER\n"
                std::string payload = clientData.substr(payloadStart);

                const char* payloadType = &unknownMsg[0];
                if (payload.find(&flightPlanMsg[0]) != std::string::npos) {
                    payloadType = &flightPlanMsg[0];
                }
                else if (payload.find(&flightLogMsg[0]) != std::string::npos) {
                    payloadType = &flightLogMsg[0];
                }
                else {
                    std::cerr << "Unknown payload type detected for client: " << clientId << std::endl;
                }

                std::cout << "Received Packet - Client: " << clientId
                    << ", Seq: " << header.sequenceNumber
                    << ", Payload Size: " << payload.length()
                    << ", Payload Type: " << payloadType
                    << std::endl;

                if (payload.length() != header.payloadSize) {
                    std::cerr << "Payload size mismatch. Expected: " << header.payloadSize
                        << ", Actual: " << payload.length() << std::endl;
                    returnValue = "ERROR: Payload size mismatch";
                }
                else {
                    auto& clientSequences = receivedSequences[clientId];

                    if (clientSequences.count(header.sequenceNumber) > 0U) {
                        std::cerr << "Duplicate sequence number for client " << clientId
                            << ": " << header.sequenceNumber << std::endl;
                        returnValue = "ERROR: Duplicate sequence number";
                    }
                    else {
                        (void)clientSequences.insert(header.sequenceNumber);
                        assembledMessages[clientId] += payload;

                        if (clientSequences.size() >= 2U) { // Assuming 2 packets
                            std::string fullMessage = assembledMessages[clientId];
                            (void)assembledMessages.erase(clientId);
                            (void)receivedSequences.erase(clientId);

                            std::cout << "Full Message Assembled for Client " << clientId << ":" << std::endl;
                            std::cout << fullMessage << std::endl;

                            if (fullMessage.find(&flightPlanMsg[0]) != std::string::npos) {
                                FlightPlan flightPlan = parseFlightPlan(fullMessage);
                                FlightLog flightLog = parseFlightLog(fullMessage);

                                std::vector<const Notam*> relevantNotams = notamProcessor_.getRelevantNotams(flightPlan);

                                if (relevantNotams.empty()) {
                                    returnValue = "NO_NOTAMS_FOUND\n";
                                }
                                else {
                                    response << "NOTAMS AFFECTING YOUR FLIGHT:\n";
                                    for (const Notam* notam : relevantNotams) {
                                        response << "NOTAM: " << notam->identifier
                                            << " for " << notam->location
                                            << " - " << notam->description << "\n";
                                    }

                                    WeatherConditions conditions = weatherProcessor_.updateWeather(
                                        flightPlan.routeAirspaces[0].center.latitude,
                                        flightPlan.routeAirspaces[0].center.longitude
                                    );

                                    response << "WEATHER UPDATES AFFECTING YOUR FLIGHT::\n";
                                    WeatherStatus status = weatherProcessor_.isWeatherGood(conditions);

                                    if (status.weatherGood) {
                                        response << "WEATHER UPDATE: Current conditions are favorable for flight operations.\n";
                                    }
                                    else {
                                        response << "WEATHER WARNING:\n" << status.weatherMessage;
                                        response << "*** FLIGHT PLAN REJECTED DUE TO ADVERSE WEATHER CONDITIONS. ***\n";
                                        returnValue = response.str();
                                    }

                                    if (returnValue.empty()) {
                                        response << "FUEL CHECK RESULTS:\n";
                                        FuelChecker fuelChecker;

                                        if (!fuelChecker.hasSufficientFuel(flightLog)) {
                                            response << "WARNING: Insufficient fuel for estimated flight duration.\n";
                                            response << "*** FLIGHT PLAN REJECTED DUE TO INSUFFICIENT FUEL. ***\n";
                                            returnValue = response.str();
                                        }
                                        else {
                                            if (!fuelChecker.meetsReserveFuelRequirement(flightLog)) {
                                                response << "WARNING: Fuel reserves are below required minimum.\n";
                                                response << "RECOMMENDATION: Increase fuel load to meet safety requirements.\n";
                                            }
                                            if (fuelChecker.isUnusualFuelBurn(flightLog, flightPlan.aircraftType)) {
                                                response << "WARNING: Unusual fuel burn rate detected.\n";
                                                response << "*** FLIGHT PLAN REJECTED DUE TO UNUSUAL FUEL BURN RATE. ***\n";
                                                returnValue = response.str();
                                            }
                                        }

                                        if (returnValue.empty()) {
                                            response << "*** FLIGHT PLAN ACCEPTED. NOTIFYNG TO RELEVANT ATCs. HAVE A SAFE FLIGHT. ***\n";
                                            connectionManager_.broadcastFlightPlan(flightPlan);
                                            returnValue = response.str();
                                        }
                                    }
                                }
                            }
                            else {
                                returnValue = "ERROR: Unknown message type";
                            }
                        }
                        else {
                            returnValue = "RECEIVED: Partial Message (Seq: " + std::to_string(header.sequenceNumber) + ")\n";
                        }
                    }
                }
            }
        }

        return returnValue;
    }
};

// Server implementation functions
ServerStateMachine initializeWinsock() {
    WSADATA wsaData;
    ServerStateMachine returnCode = ServerStateMachine::SUCCESS;

    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cerr << "WSAStartup failed: " << result << std::endl;
        returnCode = ServerStateMachine::CONNECTION_ERROR;
    }

    return returnCode;
}

ServerStateMachine createServerSocket(SOCKET& serverSocket) {
    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ServerStateMachine result = ServerStateMachine::SUCCESS;

    if (serverSocket == INVALID_SOCKET) {
        std::cerr << "Failed to create socket: " << WSAGetLastError() << std::endl;

        int cleanupResult = WSACleanup();
        if (cleanupResult != 0) {
            std::cerr << "WSACleanup failed: " << WSAGetLastError() << std::endl;
        }

        result = ServerStateMachine::CONNECTION_ERROR;
    }
    else {
        BOOL opt = TRUE;
        if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<char*>(&opt), sizeof(opt)) == SOCKET_ERROR) {
            std::cerr << "Failed to set socket options: " << WSAGetLastError() << std::endl;

            int closeResult = closesocket(serverSocket);
            if (closeResult == SOCKET_ERROR) {
                std::cerr << "closesocket failed: " << WSAGetLastError() << std::endl;
            }

            int cleanupResult = WSACleanup();
            if (cleanupResult != 0) {
                std::cerr << "WSACleanup failed: " << WSAGetLastError() << std::endl;
            }

            result = ServerStateMachine::CONNECTION_ERROR;
        }
    }

    return result;
}

ServerStateMachine bindSocketToPort(SOCKET serverSocket, uint16_t port) {
    struct sockaddr_in serverAddr;
    (void)memset(&serverAddr, 0, sizeof(serverAddr));  // Explicit discard of return value

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    ServerStateMachine result = ServerStateMachine::SUCCESS;

    if (bind(serverSocket, reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind socket to port " << port << ": " << WSAGetLastError() << std::endl;

        int closeResult = closesocket(serverSocket);
        if (closeResult == SOCKET_ERROR) {
            std::cerr << "closesocket failed: " << WSAGetLastError() << std::endl;
        }

        int cleanupResult = WSACleanup();
        if (cleanupResult != 0) {
            std::cerr << "WSACleanup failed: " << WSAGetLastError() << std::endl;
        }

        result = ServerStateMachine::CONNECTION_ERROR;
    }

    return result;
}

ServerStateMachine startListening(SOCKET serverSocket) {
    ServerStateMachine result = ServerStateMachine::SUCCESS;

    if (listen(serverSocket, 5) == SOCKET_ERROR) {
        std::cerr << "Failed to listen on socket: " << WSAGetLastError() << std::endl;

        int closeResult = closesocket(serverSocket);
        if (closeResult == SOCKET_ERROR) {
            std::cerr << "closesocket failed: " << WSAGetLastError() << std::endl;
        }

        int cleanupResult = WSACleanup();
        if (cleanupResult != 0) {
            std::cerr << "WSACleanup failed: " << WSAGetLastError() << std::endl;
        }

        result = ServerStateMachine::CONNECTION_ERROR;
    }

    return result;
}

void cleanupWinsock(SOCKET serverSocket) {
    if (serverSocket != INVALID_SOCKET) {
        int closeResult = closesocket(serverSocket);
        if (closeResult == SOCKET_ERROR) {
            std::cerr << "closesocket failed: " << WSAGetLastError() << std::endl;
        }
    }

    int cleanupResult = WSACleanup();
    if (cleanupResult != 0) {
        std::cerr << "WSACleanup failed: " << WSAGetLastError() << std::endl;
    }
}

void handleClientConnection(SOCKET clientSocket, FlightDataHandler& dataHandler, ConnectionManager& connectionManager) {
    static const char endHeaderMsg[] = "END_HEADER\n";
    static const char requestConnectionMsg[] = "REQUEST_CONNECTION";
    static const char partialMsg[] = "Partial Message";
    static const char notamsMsg[] = "NOTAMS AFFECTING YOUR FLIGHT";
    static const char errorMsg[] = "ERROR: Connection request required first";

    std::string clientId;
    bool isConnectionEstablished = false;
    bool shouldExitLoop = false;
    const uint32_t BUFFER_SIZE = 4096U;

    while (!shouldExitLoop) {
        std::array<char, BUFFER_SIZE> buffer;
        int bytesRead = recv(clientSocket, buffer.data(), buffer.size() - 1, 0);

        if (bytesRead <= 0) {
            shouldExitLoop = true;
        }
        else {
            buffer[static_cast<size_t>(bytesRead)] = '\0';
            std::string fullRequest(buffer.data());
            std::string response;

            PacketHeaderParser::ParsedHeader header = PacketHeaderParser::parseHeader(fullRequest);

            if (!header.isValid) {
                response = "ERROR: Invalid packet header";
                int sendResult = send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                if (sendResult == SOCKET_ERROR) {
                    std::cerr << "send failed: " << WSAGetLastError() << std::endl;
                }
                shouldExitLoop = true;
            }
            else {
                size_t payloadStart = fullRequest.find(&endHeaderMsg[0]);
                if (payloadStart == std::string::npos) {
                    response = "ERROR: Invalid packet format - no END_HEADER found";
                    int sendResult = send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                    if (sendResult == SOCKET_ERROR) {
                        std::cerr << "send failed: " << WSAGetLastError() << std::endl;
                    }
                    shouldExitLoop = true;
                }
                else {
                    payloadStart += 11U;
                    std::string payload = fullRequest.substr(payloadStart);

                    if (payload.length() != header.payloadSize) {
                        response = "ERROR: Payload size mismatch. Expected " +
                            std::to_string(header.payloadSize) +
                            ", got " + std::to_string(payload.length());
                        int sendResult = send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                        if (sendResult == SOCKET_ERROR) {
                            std::cerr << "send failed: " << WSAGetLastError() << std::endl;
                        }
                        shouldExitLoop = true;
                    }
                    else {
                        if (!isConnectionEstablished) {
                            if (payload.find(&requestConnectionMsg[0]) != std::string::npos) {
                                ConnectionRequest request;
                                clientId = request.parseFromData(payload);

                                if (clientId.empty()) {
                                    response = "ERROR: Invalid connection request format";
                                    int sendResult = send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                                    if (sendResult == SOCKET_ERROR) {
                                        std::cerr << "send failed: " << WSAGetLastError() << std::endl;
                                    }
                                    shouldExitLoop = true;
                                }
                                else {
                                    bool accepted = connectionManager.canAcceptConnection();
                                    if (accepted) {
                                        (void)connectionManager.addClient(clientId, clientSocket);
                                        std::cout << "Connection accepted for client: " << clientId
                                            << " (Active clients: " << connectionManager.getActiveClientCount() << ")" << std::endl;

                                        response = ConnectionRequest::createAcceptResponse();
                                        isConnectionEstablished = true;
                                    }
                                    else {
                                        std::cout << "Connection rejected for client: " << clientId
                                            << " (Maximum connections reached: " << connectionManager.getActiveClientCount() << ")" << std::endl;

                                        response = ConnectionRequest::createRejectResponse();
                                        int sendResult = send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                                        if (sendResult == SOCKET_ERROR) {
                                            std::cerr << "send failed: " << WSAGetLastError() << std::endl;
                                        }
                                        shouldExitLoop = true;
                                    }

                                    int sendResult = send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                                    if (sendResult == SOCKET_ERROR) {
                                        std::cerr << "send failed: " << WSAGetLastError() << std::endl;
                                    }

                                    std::cout << "\n=== Client Request ===\n" << fullRequest << std::endl;
                                    std::cout << "=== Server Response ===\n" << response << std::endl;
                                }
                            }
                            else {
                                int sendResult = send(clientSocket, &errorMsg[0], static_cast<int>(sizeof(errorMsg) - 1U), 0);
                                if (sendResult == SOCKET_ERROR) {
                                    std::cerr << "send failed: " << WSAGetLastError() << std::endl;
                                }
                                shouldExitLoop = true;
                            }
                        }
                        else {
                            try {
                                response = dataHandler.processFlightData(fullRequest, clientId);

                                if (response.find(&partialMsg[0]) == std::string::npos) {
                                    int sendResult = send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                                    if (sendResult == SOCKET_ERROR) {
                                        std::cerr << "send failed: " << WSAGetLastError() << std::endl;
                                    }

                                    if (response.find(&notamsMsg[0]) != std::string::npos) {
                                        shouldExitLoop = true;
                                    }
                                }
                            }
                            catch (const std::exception& e) {
                                response = "ERROR: Processing flight data failed - " + std::string(e.what());
                                int sendResult = send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
                                if (sendResult == SOCKET_ERROR) {
                                    std::cerr << "send failed: " << WSAGetLastError() << std::endl;
                                }
                                shouldExitLoop = true;
                            }
                        }
                    }
                }
            }
        }
    }

    int closeResult = closesocket(clientSocket);
    if (closeResult == SOCKET_ERROR) {
        std::cerr << "closesocket failed: " << WSAGetLastError() << std::endl;
    }

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
        ServerStateMachine returnCode = ServerStateMachine::SUCCESS;

        // Initialize Winsock
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "WSAStartup failed: " << result << std::endl;
            returnCode = ServerStateMachine::CONNECTION_ERROR;
        }

        if (returnCode == ServerStateMachine::SUCCESS) {
            // Create the server socket
            serverSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (serverSocket_ == INVALID_SOCKET) {
                std::cerr << "Failed to create socket: " << WSAGetLastError() << std::endl;

                int cleanupResult = WSACleanup();
                if (cleanupResult != 0) {
                    std::cerr << "WSACleanup failed: " << WSAGetLastError() << std::endl;
                }

                returnCode = ServerStateMachine::CONNECTION_ERROR;
            }
        }

        if (returnCode == ServerStateMachine::SUCCESS) {
            // Set socket options to allow address reuse
            BOOL opt = TRUE;
            if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR,
                reinterpret_cast<char*>(&opt), sizeof(opt)) == SOCKET_ERROR) {
                std::cerr << "Failed to set socket options: " << WSAGetLastError() << std::endl;

                int closeResult = closesocket(serverSocket_);
                if (closeResult == SOCKET_ERROR) {
                    std::cerr << "closesocket failed: " << WSAGetLastError() << std::endl;
                }

                int cleanupResult = WSACleanup();
                if (cleanupResult != 0) {
                    std::cerr << "WSACleanup failed: " << WSAGetLastError() << std::endl;
                }

                returnCode = ServerStateMachine::CONNECTION_ERROR;
            }
        }

        if (returnCode == ServerStateMachine::SUCCESS) {
            // Bind the socket to the specified port
            struct sockaddr_in serverAddr;
            (void)memset(&serverAddr, 0, sizeof(serverAddr));
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_addr.s_addr = INADDR_ANY;
            serverAddr.sin_port = htons(port);

            if (bind(serverSocket_, reinterpret_cast<SOCKADDR*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
                std::cerr << "Failed to bind socket to port " << port << ": " << WSAGetLastError() << std::endl;

                int closeResult = closesocket(serverSocket_);
                if (closeResult == SOCKET_ERROR) {
                    std::cerr << "closesocket failed: " << WSAGetLastError() << std::endl;
                }

                int cleanupResult = WSACleanup();
                if (cleanupResult != 0) {
                    std::cerr << "WSACleanup failed: " << WSAGetLastError() << std::endl;
                }

                returnCode = ServerStateMachine::CONNECTION_ERROR;
            }
        }

        if (returnCode == ServerStateMachine::SUCCESS) {
            // Start listening for connections
            if (listen(serverSocket_, 5) == SOCKET_ERROR) {
                std::cerr << "Failed to listen on socket: " << WSAGetLastError() << std::endl;

                int closeResult = closesocket(serverSocket_);
                if (closeResult == SOCKET_ERROR) {
                    std::cerr << "closesocket failed: " << WSAGetLastError() << std::endl;
                }

                int cleanupResult = WSACleanup();
                if (cleanupResult != 0) {
                    std::cerr << "WSACleanup failed: " << WSAGetLastError() << std::endl;
                }

                returnCode = ServerStateMachine::CONNECTION_ERROR;
            }
        }

        if (returnCode == ServerStateMachine::SUCCESS) {
            isRunning_ = true;
            std::cout << "NOTAM Server started on port " << port << std::endl;
            std::cout << "Maximum concurrent connections: " << ConnectionManager::MAX_CONNECTIONS << std::endl;
        }

        return returnCode;
    }

    void run(void) {
        bool canRun = true;

        if (!isRunning_) {
            std::cerr << "Server not started" << std::endl;
            canRun = false;
        }

        if (canRun) {
            std::cout << "Waiting for connections..." << std::endl;

            while (isRunning_) {
                struct sockaddr_in clientAddr;
                int clientAddrLen = sizeof(clientAddr);

                SOCKET clientSocket = accept(serverSocket_, reinterpret_cast<SOCKADDR*>(&clientAddr), &clientAddrLen);
                if (clientSocket == INVALID_SOCKET) {
                    std::cerr << "Failed to accept connection: " << WSAGetLastError() << std::endl;
                    continue;
                }

                char clientIP[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &(clientAddr.sin_addr), &clientIP[0], static_cast<socklen_t>(INET_ADDRSTRLEN)) == nullptr) {
                    std::cerr << "Failed to convert address: " << WSAGetLastError() << std::endl;
                    continue;
                }

                std::cout << "Client connected: " << clientIP << std::endl;

                handleClientConnection(clientSocket, dataHandler_, connectionManager_);
            }
        }

        return;
    }

    void stop(void) {
        isRunning_ = false;

        // Check if the server socket is valid
        if (serverSocket_ != INVALID_SOCKET) {
            // Close the socket and check the return value
            int closeResult = closesocket(serverSocket_);
            if (closeResult == SOCKET_ERROR) {
                std::cerr << "Failed to close socket, error: " << WSAGetLastError() << std::endl;
            }
            serverSocket_ = INVALID_SOCKET;

            // Cleanup and check the return value
            int wsacleanupResult = WSACleanup();
            if (wsacleanupResult != 0) {
                std::cerr << "WSACleanup failed, error: " << WSAGetLastError() << std::endl;
            }
        }

        std::cout << "Server stopped" << std::endl;
    }

    ~TcpServer(void) {
        if (isRunning_) {
            stop();
        }
    }
};
}

int32_t main(int argc, char* argv[]) {
    int32_t returnCode = 0; // Default success code

    // Default parameters
    std::string notamFile = "notam_database.txt";
    uint16_t port = 8081;

    // Parse command line arguments
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            break;
        }

        std::string arg = argv[i];

        if (arg == "-f") {
            notamFile = argv[i + 1];
        }
        else if (arg == "-p") {
            try {
                port = static_cast<uint16_t>(std::stoi(argv[i + 1]));
            }
            catch (const std::invalid_argument& e) {
                std::cerr << "Invalid port number: " << argv[i + 1] << std::endl;
            }
            catch (const std::out_of_range& e) {
                std::cerr << "Port number out of range: " << argv[i + 1] << std::endl;
            }
        }
        else {
            std::cerr << "Unknown argument: " << arg << std::endl;
        }
    }

    std::cout << "NOTAM Server\n";
    std::cout << "============\n\n";

    // Initialize NOTAM database
    ServerFlightManagement::NotamDatabase notamDb;
    if (!notamDb.loadFromFile(notamFile)) {
        std::cerr << "Failed to load NOTAM database from: " << notamFile << std::endl;
        std::cerr << "Creating an empty database...\n";
    }
    else {
        std::cout << "Loaded NOTAM database from: " << notamFile << std::endl;
    }

    // API key for openweathermap
    std::string apiKey = "445b28699592a8c90c07b345dd4de9cd";

    // Initialize components
    ServerFlightManagement::ConnectionManager connectionManager;
    ServerFlightManagement::NotamProcessor processor(notamDb);
    ServerFlightManagement::WeatherConditions conditions;
    ServerFlightManagement::WeatherProcessor weatherProcessor(apiKey);
    ServerFlightManagement::FlightDataHandler handler(processor, weatherProcessor, connectionManager);
    ServerFlightManagement::TcpServer server(handler, connectionManager);

    // Start the TCP server
    ServerFlightManagement::ServerStateMachine startResult = server.start(port);

    if (startResult != ServerFlightManagement::ServerStateMachine::SUCCESS) {
        returnCode = static_cast<int32_t>(startResult);
    }
    else {
        // Run the server until stopped
        server.run();
    }

    return returnCode;
}