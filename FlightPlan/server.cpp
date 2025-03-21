#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// For TCP/IP sockets
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

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
       // Simple check if airspace identifiers match
       // In a real system, this would check for actual overlap
       return (0 == strcmp(spaceId1, spaceId2));
   }

public:
   explicit NotamProcessor(const NotamDatabase& notamDb) : notamDb_(notamDb) {
       // No initialization needed
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

   // Known airspace routes - in a real system, this would be from a database
   std::vector<AirspaceInfo> getRouteAirspaces(const char* departure, const char* arrival) const {
       std::vector<AirspaceInfo> airspaces;

       // Toronto to New York route
       if ((0 == strcmp(departure, "CYYZ")) && (0 == strcmp(arrival, "KJFK"))) {
           const char* routePoints[] = { "CYYZ", "KBUF", "KJFK" };
           for (const char* point : routePoints) {
               AirspaceInfo space;
               (void)SafeString::copy(space.identifier, sizeof(space.identifier), point);
               space.center.latitude = 0.0;   // These would be real coordinates
               space.center.longitude = 0.0;  // in a production system
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
   explicit FlightDataHandler(NotamProcessor& processor) : notamProcessor_(processor) {
       // No initialization needed
   }

   std::string processClientData(const std::string& clientData) {
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

// TCP Server for client connections
class TcpServer {
private:
   int32_t serverSocket_;
   FlightDataHandler& dataHandler_;
   bool isRunning_;
   static const uint32_t BUFFER_SIZE = 4096U;

   void handleClient(int32_t clientSocket) {
       std::array<char, BUFFER_SIZE> buffer;
       ssize_t bytesRead = read(clientSocket, buffer.data(), buffer.size() - 1);

       if (bytesRead > 0) {
           // Null-terminate the received data
           buffer[static_cast<size_t>(bytesRead)] = '\0';

           // Process the client data
           std::string request(buffer.data());
           std::string response = dataHandler_.processClientData(request);

           // Send the response back to the client
           write(clientSocket, response.c_str(), response.length());

           // Log the transaction
           std::cout << "\n=== Client Request ===\n" << request << std::endl;
           std::cout << "=== Server Response ===\n" << response << std::endl;
       }

       // Close the client socket
       close(clientSocket);
   }

public:
   explicit TcpServer(FlightDataHandler& handler) :
       serverSocket_(-1),
       dataHandler_(handler),
       isRunning_(false) {
       // No initialization needed
   }

   ErrorCode start(uint16_t port) {
       // Create the server socket
       serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
       if (serverSocket_ < 0) {
           std::cerr << "Failed to create socket" << std::endl;
           return ErrorCode::CONNECTION_ERROR;
       }

       // Set socket options to allow address reuse
       int32_t opt = 1;
       if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
           std::cerr << "Failed to set socket options" << std::endl;
           close(serverSocket_);
           return ErrorCode::CONNECTION_ERROR;
       }

       // Bind the socket to the specified port
       struct sockaddr_in serverAddr;
       memset(&serverAddr, 0, sizeof(serverAddr));
       serverAddr.sin_family = AF_INET;
       serverAddr.sin_addr.s_addr = INADDR_ANY;
       serverAddr.sin_port = htons(port);

       if (bind(serverSocket_, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
           std::cerr << "Failed to bind socket to port " << port << std::endl;
           close(serverSocket_);
           return ErrorCode::CONNECTION_ERROR;
       }

       // Start listening for connections
       if (listen(serverSocket_, 5) < 0) {
           std::cerr << "Failed to listen on socket" << std::endl;
           close(serverSocket_);
           return ErrorCode::CONNECTION_ERROR;
       }

       isRunning_ = true;
       std::cout << "NOTAM Server started on port " << port << std::endl;

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
           socklen_t clientAddrLen = sizeof(clientAddr);

           int32_t clientSocket = accept(serverSocket_, (struct sockaddr*)&clientAddr, &clientAddrLen);
           if (clientSocket < 0) {
               std::cerr << "Failed to accept connection" << std::endl;
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
       if (serverSocket_ >= 0) {
           close(serverSocket_);
           serverSocket_ = -1;
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
   uint16_t port = 8080;

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

   // Initialize NOTAM database
   NotamDatabase notamDb;
   if (!notamDb.loadFromFile(notamFile)) {
       std::cerr << "Failed to load NOTAM database from: " << notamFile << std::endl;
       return 1;
   }

   std::cout << "Loaded NOTAM database from: " << notamFile << std::endl;

   // Initialize NOTAM processor
   NotamProcessor processor(notamDb);

   // Initialize flight data handler
   FlightDataHandler handler(processor);

   // Start the TCP server
   TcpServer server(handler);
   ErrorCode startResult = server.start(port);

   if (startResult != ErrorCode::SUCCESS) {
       return static_cast<int32_t>(startResult);
   }

   // Run the server until stopped
   server.run();

   return 0;
}



