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




