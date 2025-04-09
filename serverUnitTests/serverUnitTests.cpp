#define _CRT_SECURE_NO_WARNINGS 
#include "pch.h"
#include "CppUnitTest.h"
// Include minimal dependencies to avoid unresolved symbols
#include <string>
#include <sstream>
#include <vector>
#include <set>

#include <map>



using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Forward declarations of mock test objects
class MockSocket;

// Define necessary enums and structs locally so we don't need external dependencies
enum class ServerStateMachine {
    SUCCESS,
    FAILURE,
    INVALID_INPUT,
    TIMEOUT
};

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

// Mock implementation of the SafeString class
class SafeString {
public:
    static ServerStateMachine copy(char* dest, unsigned int destSize, const char* src) {
        // Check for invalid parameters
        if (dest == nullptr || src == nullptr || destSize == 0) {
            return ServerStateMachine::INVALID_INPUT;
        }

        // Perform safe copy
        size_t i;
        for (i = 0; i < destSize - 1 && src[i] != '\0'; i++) {
            dest[i] = src[i];
        }
        dest[i] = '\0';

        return ServerStateMachine::SUCCESS;
    }
};

// Mock implementation of the PacketHeaderParser class
class PacketHeaderParser {
public:
    struct ParsedHeader {
        bool isValid;
        uint64_t sequenceNumber;
        std::string timestamp;
        size_t payloadSize;
    };

    static ParsedHeader parseHeader(const std::string& headerData) {
        ParsedHeader header;
        header.isValid = false;
        header.sequenceNumber = 0;
        header.timestamp = "";
        header.payloadSize = 0;

        // Check if header starts with correct marker
        if (headerData.find("HEADER") != 0) {
            return header;
        }

        // Simple parsing logic
        if (headerData.find("SEQ_NUM=") != std::string::npos &&
            headerData.find("TIMESTAMP=") != std::string::npos &&
            headerData.find("PAYLOAD_SIZE=") != std::string::npos &&
            headerData.find("END_HEADER") != std::string::npos) {

            // Extract sequence number
            size_t seqPos = headerData.find("SEQ_NUM=") + 8;
            size_t seqEnd = headerData.find("\n", seqPos);
            std::string seqStr = headerData.substr(seqPos, seqEnd - seqPos);
            header.sequenceNumber = std::stoull(seqStr);

            // Extract timestamp
            size_t tsPos = headerData.find("TIMESTAMP=") + 10;
            size_t tsEnd = headerData.find("\n", tsPos);
            header.timestamp = headerData.substr(tsPos, tsEnd - tsPos);

            // Extract payload size
            size_t psPos = headerData.find("PAYLOAD_SIZE=") + 13;
            size_t psEnd = headerData.find("\n", psPos);
            std::string psStr = headerData.substr(psPos, psEnd - psPos);
            header.payloadSize = std::stoull(psStr);

            header.isValid = true;
        }

        return header;
    }
};

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

FlightPlan createTestFlightPlan(const char* flightId, const char* depAirport, const char* arrAirport) {
    FlightPlan plan;
    SafeString::copy(plan.flightId, sizeof(plan.flightId), flightId);
    SafeString::copy(plan.departureAirport, sizeof(plan.departureAirport), depAirport);
    SafeString::copy(plan.arrivalAirport, sizeof(plan.arrivalAirport), arrAirport);
    SafeString::copy(plan.aircraftReg, sizeof(plan.aircraftReg), "N12345");
    SafeString::copy(plan.aircraftType, sizeof(plan.aircraftType), "B737");
    SafeString::copy(plan.operator_, sizeof(plan.operator_), "TestAir");
    SafeString::copy(plan.route, sizeof(plan.route), "TEST1 TEST2 TEST3");
    SafeString::copy(plan.etdTime, sizeof(plan.etdTime), "2025-04-07T10:00Z");
    SafeString::copy(plan.etaTime, sizeof(plan.etaTime), "2025-04-07T12:00Z");

    plan.cruiseAlt = 35000;
    plan.speed = 450;

    // Add a route airspace for test
    AirspaceInfo airspace;
    SafeString::copy(airspace.identifier, sizeof(airspace.identifier), "TEST1");
    airspace.center = { 43.6777, -79.6248 };
    airspace.radius = 50.0;
    plan.routeAirspaces.push_back(airspace);

    return plan;
}

Notam createTestNotam(const char* id, const char* location, const char* affectedAirspaceId) {
    Notam notam;
    SafeString::copy(notam.identifier, sizeof(notam.identifier), id);
    SafeString::copy(notam.fir, sizeof(notam.fir), "CZYZ");  // FIR is not used in logic currently
    SafeString::copy(notam.location, sizeof(notam.location), location);
    SafeString::copy(notam.startTime, sizeof(notam.startTime), "2025-04-07T00:00Z");
    SafeString::copy(notam.endTime, sizeof(notam.endTime), "2025-04-08T00:00Z");

    SafeString::copy(notam.affectedAirspace.identifier, sizeof(notam.affectedAirspace.identifier), affectedAirspaceId);
    notam.affectedAirspace.center = { 43.6777, -79.6248 };
    notam.affectedAirspace.radius = 50.0;

    SafeString::copy(notam.description, sizeof(notam.description), "Sample NOTAM");

    return notam;
}


class NotamDatabase {
private:
    std::vector<Notam> notams_;

public:
    NotamDatabase() : notams_() {}

    void addNotam(const Notam& notam) {
        notams_.push_back(notam);
    }

    const std::vector<Notam>& getAllNotams() const {
        return notams_;
    }
};

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


// Add the mock class for WeatherProcessor
class MockWeatherProcessor {
public:
    MockWeatherProcessor(const std::string& apiKey) {}

    // Mocking fetchWeatherData to return a fixed JSON response
    std::string fetchWeatherData(double latitude, double longitude) {
        return R"({
            "weather": [{"id": 802, "description": "partly cloudy"}],
            "visibility": 5000,
            "main": {"temp": 25.0, "temp_min": 20.0, "temp_max": 30.0},
            "wind": {"speed": 15},
            "timezone": 3600
        })";
    }

    // Mocking parseWeatherData to simulate parsing the above JSON response
    WeatherConditions parseWeatherData(const std::string& jsonData) {
        WeatherConditions weatherConditions;
        // Hardcode parsed values to simulate real parsing
        weatherConditions.conditionCode = 802;
        strncpy_s(weatherConditions.description, "partly cloudy", sizeof(weatherConditions.description) - 1);
        weatherConditions.description[sizeof(weatherConditions.description) - 1] = '\0';
        weatherConditions.arrVisibility = 5000;
        weatherConditions.avgTemp = 25.0;
        weatherConditions.tempMin = 20.0;
        weatherConditions.tempMax = 30.0;
        weatherConditions.windSpeed = 15;
        weatherConditions.timezone = 3600;
        return weatherConditions;
    }

    // Directly return updated weather conditions based on mock data
    WeatherConditions updateWeather(double latitude, double longitude) {
        return parseWeatherData(fetchWeatherData(latitude, longitude));
    }

    // Return a mocked safe weather status
    WeatherStatus isWeatherGood(const WeatherConditions& weatherConditions) {
        WeatherStatus status;
        if (weatherConditions.windSpeed > 20 || weatherConditions.arrVisibility < 1000) {
            status.weatherGood = false;
            status.weatherMessage = "Bad weather conditions detected.";
        }
        else {
            status.weatherGood = true;
            status.weatherMessage.clear();
        }
        return status;
    }
};

namespace ServerUnitTests
{
    // Mock socket class
    class MockSocket {
    private:
        static int nextSocketId;
        int socketId;

    public:
        MockSocket() : socketId(nextSocketId++) {}

        int getSocketId() const { return socketId; }

        static void resetSocketIds() { nextSocketId = 1000; }
    };

    int MockSocket::nextSocketId = 1000;

    // Test class
    TEST_CLASS(ServerUnitTests)
    {
    private:
        // Helper methods
        std::string createConnectionRequest(const std::string& clientId) {
            std::stringstream ss;
            ss << "REQUEST_CONNECTION\n";
            ss << "CLIENT_ID=" << clientId << "\n";
            return ss.str();
        }

        std::string createPacketWithHeader(const std::string& payload) {
            std::stringstream ss;
            ss << "HEADER\n";
            ss << "SEQ_NUM=1\n";
            ss << "TIMESTAMP=20250407120000\n";
            ss << "PAYLOAD_SIZE=" << payload.size() << "\n";
            ss << "END_HEADER\n";
            ss << payload;
            return ss.str();
        }

    public:
        TEST_METHOD_INITIALIZE(Initialize)
        {
            MockSocket::resetSocketIds();
        }

        // SafeString tests
        TEST_METHOD(TestSafeStringCopySuccess)
        {
            char dest[16] = { 0 };
            const char* src = "Test";
            auto result = SafeString::copy(dest, sizeof(dest), src);

            Assert::AreEqual(static_cast<int>(ServerStateMachine::SUCCESS), static_cast<int>(result));
            Assert::AreEqual(src, dest);
        }

        TEST_METHOD(TestSafeStringCopyNullDest)
        {
            const char* src = "Test";
            auto result = SafeString::copy(nullptr, 16, src);

            Assert::AreEqual(static_cast<int>(ServerStateMachine::INVALID_INPUT), static_cast<int>(result));
        }

        TEST_METHOD(TestSafeStringCopyNullSrc)
        {
            char dest[16] = { 0 };
            auto result = SafeString::copy(dest, sizeof(dest), nullptr);

            Assert::AreEqual(static_cast<int>(ServerStateMachine::INVALID_INPUT), static_cast<int>(result));
        }

        TEST_METHOD(TestSafeStringCopyZeroSize)
        {
            char dest[16] = { 0 };
            const char* src = "Test";
            auto result = SafeString::copy(dest, 0, src);

            Assert::AreEqual(static_cast<int>(ServerStateMachine::INVALID_INPUT), static_cast<int>(result));
        }

        TEST_METHOD(TestSafeStringCopyTruncate)
        {
            char dest[5] = { 0 };
            const char* src = "TestLong";
            auto result = SafeString::copy(dest, sizeof(dest), src);

            Assert::AreEqual(static_cast<int>(ServerStateMachine::SUCCESS), static_cast<int>(result));
            Assert::AreEqual("Test", dest); // Should be truncated with null terminator
        }

        // PacketHeaderParser tests
        TEST_METHOD(TestPacketHeaderParserValid)
        {
            std::string headerStr = "HEADER\nSEQ_NUM=123\nTIMESTAMP=20250407120000\nPAYLOAD_SIZE=456\nEND_HEADER\n";
            auto parsedHeader = PacketHeaderParser::parseHeader(headerStr);

            Assert::IsTrue(parsedHeader.isValid);
            Assert::AreEqual(static_cast<uint64_t>(123), parsedHeader.sequenceNumber);
            Assert::AreEqual(std::string("20250407120000"), parsedHeader.timestamp);
            Assert::AreEqual(static_cast<size_t>(456), parsedHeader.payloadSize);
        }

        TEST_METHOD(TestPacketHeaderParserInvalid)
        {
            std::string headerStr = "INVALID\nSEQ_NUM=123\nTIMESTAMP=20250407120000\nPAYLOAD_SIZE=456\n";
            auto parsedHeader = PacketHeaderParser::parseHeader(headerStr);

            Assert::IsFalse(parsedHeader.isValid);
        }

        // Mock Socket tests
        TEST_METHOD(TestMockSocketIds)
        {
            MockSocket::resetSocketIds();
            MockSocket socket1;
            MockSocket socket2;

            Assert::AreEqual(1000, socket1.getSocketId());
            Assert::AreEqual(1001, socket2.getSocketId());
        }

        // Test helper methods
        TEST_METHOD(TestCreateConnectionRequest)
        {
            std::string request = createConnectionRequest("TESTCLIENT");
            Assert::IsTrue(request.find("REQUEST_CONNECTION") != std::string::npos);
            Assert::IsTrue(request.find("CLIENT_ID=TESTCLIENT") != std::string::npos);
        }

        TEST_METHOD(TestCreatePacketWithHeader)
        {
            std::string payload = "TEST_PAYLOAD";
            std::string packet = createPacketWithHeader(payload);

            Assert::IsTrue(packet.find("HEADER") != std::string::npos);
            Assert::IsTrue(packet.find("SEQ_NUM=1") != std::string::npos);
            Assert::IsTrue(packet.find("PAYLOAD_SIZE=" + std::to_string(payload.size())) != std::string::npos);
            Assert::IsTrue(packet.find(payload) != std::string::npos);
        }


        // ConnectionRequest tests
        TEST_METHOD(TestConnectionRequestParse_Valid)
        {
            ConnectionRequest connectionRequest;
            std::string requestData = "REQUEST_CONNECTION\nCLIENT_ID=TESTCLIENT\n";
            std::string clientId = connectionRequest.parseFromData(requestData);

            Assert::AreEqual(std::string("TESTCLIENT"), clientId);
        }

        TEST_METHOD(TestConnectionRequestParse_Invalid)
        {
            ConnectionRequest connectionRequest;
            std::string requestData = "INVALID_REQUEST\nCLIENT_ID=TESTCLIENT\n";
            std::string clientId = connectionRequest.parseFromData(requestData);

            Assert::AreEqual(std::string(""), clientId);
        }

        TEST_METHOD(TestConnectionRequestParse_NoClientId)
        {
            ConnectionRequest connectionRequest;
            std::string requestData = "REQUEST_CONNECTION\nNO_CLIENT_ID_HERE\n";
            std::string clientId = connectionRequest.parseFromData(requestData);

            Assert::AreEqual(std::string(""), clientId);
        }

        TEST_METHOD(TestConnectionRequestCreateResponses)
        {
            std::string acceptResponse = ConnectionRequest::createAcceptResponse();
            std::string rejectResponse = ConnectionRequest::createRejectResponse();

            Assert::IsTrue(acceptResponse.find("CONNECTION_ACCEPTED") != std::string::npos);
            Assert::IsTrue(rejectResponse.find("CONNECTION_REJECTED") != std::string::npos);
            Assert::IsTrue(rejectResponse.find("REASON=") != std::string::npos);
        }

        TEST_METHOD(TestNotamProcessorRelevantNotams_DepartureAirport)
        {
            NotamDatabase notamDb;
            notamDb.addNotam(createTestNotam("N1", "CYYZ", "CYYZ"));
            notamDb.addNotam(createTestNotam("N2", "KJFK", "KJFK"));
            notamDb.addNotam(createTestNotam("N3", "EGLL", "EGLL"));

            NotamProcessor processor(notamDb);
            FlightPlan plan = createTestFlightPlan("TST123", "CYYZ", "KJFK");

            auto relevantNotams = processor.getRelevantNotams(plan);

            // Should find 2 relevant NOTAMs (departure and arrival airports)
            Assert::AreEqual(size_t(2), relevantNotams.size());
            bool foundN1 = false, foundN2 = false;
            for (const auto* notam : relevantNotams) {
                if (strcmp(notam->identifier, "N1") == 0) foundN1 = true;
                if (strcmp(notam->identifier, "N2") == 0) foundN2 = true;
            }
            Assert::IsTrue(foundN1 && foundN2);
        }

        TEST_METHOD(TestNotamProcessorRelevantNotams_RouteAirspace)
        {
            NotamDatabase notamDb;
            notamDb.addNotam(createTestNotam("N1", "EGLL", "TEST1"));  // Matches route airspace
            notamDb.addNotam(createTestNotam("N2", "EGLL", "TEST2"));  // Doesn't match

            NotamProcessor processor(notamDb);
            FlightPlan plan = createTestFlightPlan("TST123", "CYYZ", "KJFK");
            // plan already has TEST1 airspace from createTestFlightPlan

            auto relevantNotams = processor.getRelevantNotams(plan);

            // Should find 1 relevant NOTAM (route airspace)
            Assert::AreEqual(size_t(1), relevantNotams.size());
            Assert::AreEqual("N1", relevantNotams[0]->identifier);
        }

        TEST_METHOD(TestNotamProcessorRelevantNotams_None)
        {
            NotamDatabase notamDb;
            notamDb.addNotam(createTestNotam("N1", "EGLL", "EGLL"));  // Different airport
            notamDb.addNotam(createTestNotam("N2", "EGLL", "TEST2"));  // Different airspace

            NotamProcessor processor(notamDb);
            FlightPlan plan = createTestFlightPlan("TST123", "CYYZ", "KJFK");

            auto relevantNotams = processor.getRelevantNotams(plan);

            // Should not find any relevant NOTAMs
            Assert::AreEqual(size_t(0), relevantNotams.size());
        }

        TEST_METHOD(TestUpdateWeather) {
            MockWeatherProcessor weatherProcessor("dummyApiKey");
            WeatherConditions conditions = weatherProcessor.updateWeather(43.6777, -79.6248);

            Assert::AreEqual(802, conditions.conditionCode);
            Assert::AreEqual(std::string("partly cloudy"), std::string(conditions.description));
            Assert::AreEqual(5000, conditions.arrVisibility);
            Assert::AreEqual(25, conditions.avgTemp);
            Assert::AreEqual(20, conditions.tempMin);
            Assert::AreEqual(30, conditions.tempMax);
            Assert::AreEqual(15, conditions.windSpeed);
            Assert::AreEqual(3600, conditions.timezone);
        }

        // Test method for WeatherProcessor::isWeatherGood
        TEST_METHOD(TestIsWeatherGood) {
            MockWeatherProcessor weatherProcessor("dummyApiKey");
            WeatherConditions conditions = weatherProcessor.updateWeather(43.6777, -79.6248);
            WeatherStatus status = weatherProcessor.isWeatherGood(conditions);

            Assert::IsTrue(status.weatherGood);
            Assert::IsTrue(status.weatherMessage.empty());
        }

        // Test method for bad weather conditions
        TEST_METHOD(TestBadWeatherConditions) {
            MockWeatherProcessor weatherProcessor("dummyApiKey");

            // Simulate bad weather conditions
            WeatherConditions badConditions;
            badConditions.windSpeed = 25; // High wind speed
            badConditions.arrVisibility = 800; // Low visibility

            WeatherStatus status = weatherProcessor.isWeatherGood(badConditions);

            Assert::IsFalse(status.weatherGood);
            Assert::AreEqual("Bad weather conditions detected.", status.weatherMessage.c_str());
        }
 
    };
}