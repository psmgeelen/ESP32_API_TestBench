#include <WiFi.h>
#include <WebServer.h>
#include "driver/gpio.h" // For raw ESP32 GPIO configuration

// --- 1. CONFIGURATION ---

// Configure your Wi-Fi credentials here
const char* ssid = "YourSSID";
const char* password = "YourPassword";

// GPIO Pin Definitions
// GPIO 17 is generally safe, though often the default TX for UART2.
const int CHARGE_PIN = 17;

/*
 * PROJECT CONTEXT: Project Scrooge - Zero-Leakage Switching Test Bench
 * This API is part of a larger project (Scrooge) designed to test the charge 
 * and discharge characteristics of various electrolyte capacitors under reduced
 * loads from an ESP32 GPIO. The goal is to validate using the capacitor/relay 
 * combination for robust, near zero-leakage high-current switching, which 
 * is crucial for optimizing deep-sleep power consumption.
 * * Repository: https://github.com/psmgeelen/ESP32_API_TestBench
 */

// --- 2. GLOBAL VARIABLES ---

WebServer server(80);

// Non-blocking charge state management
// We use volatile because these variables are modified in the main loop and potentially within an ISR/timer context, 
// though here they are only used in the main loop and the HTTP handler.
volatile bool isCharging = false;
unsigned long chargeStartTime = 0;
unsigned long chargeDurationMs = 0;

// --- 3. SWAGGER / OPENAPI DEFINITION ---

// OpenAPI 3.0 specification for the API. Reverted to standard C-string literal 
// with escaped quotes to guarantee no trailing characters (like \n) are included.
const char* swaggerJson = "{\"openapi\":\"3.0.0\",\"info\":{\"title\":\"ESP32 Capacitor Charger API (Project Scrooge)\",\"version\":\"1.0.1\",\"description\":\"API to control the charge duration of an external capacitor connected to GPIO 17. Part of Project Scrooge: a zero-leakage switching test bench.\",\"contact\":{\"url\":\"https://github.com/psmgeelen/ESP32_API_TestBench\"}},\"servers\":[{\"url\":\"/\",\"description\":\"Local ESP32 Server\"}],\"paths\":{\"/charge\":{\"get\":{\"tags\":[\"Control\"],\"summary\":\"Start Capacitor Charging\",\"parameters\":[{\"name\":\"time\",\"in\":\"query\",\"required\":true,\"schema\":{\"type\":\"integer\",\"format\":\"int32\",\"minimum\":100,\"maximum\":60000},\"description\":\"Duration to hold GPIO 17 HIGH, in milliseconds (100ms to 60000ms).\"}],\"responses\":{\"200\":{\"description\":\"Charging cycle initiated successfully.\"},\"400\":{\"description\":\"Invalid or missing 'time' parameter.\"},\"409\":{\"description\":\"A charging cycle is already in progress.\"}}}},\"/state\":{\"get\":{\"tags\":[\"Status\"],\"summary\":\"Get Current GPIO Charge State\",\"description\":\"Reports if the GPIO is currently HIGH (charging) or LOW (idle), and the remaining time if charging.\",\"responses\":{\"200\":{\"description\":\"Current state information.\",\"content\":{\"application/json\":{\"example\":{\"status\":\"charging\",\"gpio_level\":\"HIGH\",\"duration_ms\":5000,\"time_remaining_ms\":1500}}}}}}},\"/stop\":{\"post\":{\"tags\":[\"Control\"],\"summary\":\"Emergency Stop\",\"description\":\"Immediately stops any active charging cycle by setting GPIO 17 LOW.\",\"responses\":{\"200\":{\"description\":\"Charge stopped or confirmed idle.\"}}}},\"/health\":{\"get\":{\"tags\":[\"System\"],\"summary\":\"Health Check\",\"description\":\"Simple check to ensure the server is running.\",\"responses\":{\"200\":{\"description\":\"System operational.\"}}}},\"/info\":{\"get\":{\"tags\":[\"System\"],\"summary\":\"Get Project Information\",\"description\":\"Provides details about the project context and configuration.\",\"responses\":{\"200\":{\"description\":\"Project details.\"}}}}}}";

// HTML for the Swagger UI page, loading assets from a CDN
const char* swaggerHtml = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <title>ESP32 Capacitor Charger API</title>
  <link rel="stylesheet" type="text/css" href="https://cdnjs.cloudflare.com/ajax/libs/swagger-ui/3.52.0/swagger-ui.css" >
  <style>
    body { font-family: 'Inter', sans-serif; background-color: #f0f0f0; }
    .topbar a span { content: "Capacitor Charger (Project Scrooge)"; }
  </style>
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/swagger-ui/3.52.0/swagger-ui-bundle.js"></script>
  <script>
    window.onload = function() {
      // Build a system
      const ui = SwaggerUIBundle({
        url: window.location.origin + "/swagger.json", // Load the OpenAPI spec from our ESP32 endpoint
        dom_id: '#swagger-ui',
        deepLinking: true,
        presets: [
          SwaggerUIBundle.presets.apis,
          SwaggerUIBundle.SwaggerUIStandalonePreset
        ],
        layout: "BaseLayout"
      });
      window.ui = ui;
    };
  </script>
</body>
</html>
)rawliteral";

// --- 4. API HANDLERS ---

/**
 * @brief Serves the OpenAPI specification in JSON format.
 */
void handleSwaggerJson() {
  server.sendHeader("Content-Type", "application/json");
  server.send(200, "application/json", swaggerJson);
}

/**
 * @brief Serves the Swagger UI HTML page.
 */
void handleSwaggerUi() {
  server.send(200, "text/html", swaggerHtml);
}

/**
 * @brief Handles the root path and redirects to Swagger UI.
 */
void handleRoot() {
  server.sendHeader("Location", "/swagger");
  server.send(302, "text/plain", "Redirecting to Swagger UI...");
}

/**
 * @brief Handles the main /charge API call.
 * * Takes 'time' parameter and starts the non-blocking charge cycle.
 * URL format: /charge?time=500
 */
void handleCharge() {
  if (isCharging) {
    // Conflict: already busy
    server.send(409, "application/json", "{\"status\":\"error\", \"message\":\"Charging in progress. Please wait.\"}");
    return;
  }

  if (!server.hasArg("time")) {
    // Bad request: missing parameter
    server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"Missing 'time' parameter (ms).\"}");
    return;
  }

  // Parse and validate the time parameter
  long requestedTime = server.arg("time").toInt();

  // Enforce a reasonable range (100ms to 60s)
  if (requestedTime < 100 || requestedTime > 60000) {
    server.send(400, "application/json", "{\"status\":\"error\", \"message\":\"'time' must be between 100 and 60000 ms.\"}");
    return;
  }

  // Start the non-blocking charge cycle
  isCharging = true;
  chargeStartTime = millis();
  chargeDurationMs = requestedTime;
  
  // Immediately set pin HIGH
  digitalWrite(CHARGE_PIN, HIGH);
  
  String response = "{\"status\":\"success\", \"message\":\"Charge cycle initiated for " + String(requestedTime) + "ms.\"}";
  server.send(200, "application/json", response);

  Serial.printf("Charge initiated for %d ms.\n", (int)requestedTime);
}

/**
 * @brief Handles the /state API call to report charge status.
 */
void handleState() {
  String response;
  if (isCharging) {
    unsigned long timeElapsed = millis() - chargeStartTime;
    // Calculate time remaining. Use ternary to prevent underflow if monitorChargeState hasn't run yet.
    unsigned long timeRemaining = chargeDurationMs > timeElapsed ? chargeDurationMs - timeElapsed : 0;
    
    response = "{\"status\":\"charging\", ";
    response += "\"gpio_level\":\"HIGH\", ";
    response += "\"duration_ms\":" + String(chargeDurationMs) + ", ";
    response += "\"time_remaining_ms\":" + String(timeRemaining) + "}";
  } else {
    // We check the actual digital read of the pin for the real state, 
    // especially after an emergency stop or if the pin was manipulated externally.
    int pinState = digitalRead(CHARGE_PIN);
    
    response = "{\"status\":\"idle\", \"gpio_level\":\"" + (pinState == HIGH ? String("HIGH") : String("LOW")) + "\"}";
  }
  server.send(200, "application/json", response);
}

/**
 * @brief Handles the /stop API call to immediately halt charging (POST method).
 */
void handleStop() {
  if (isCharging) {
    digitalWrite(CHARGE_PIN, LOW); // Turn off the charge immediately
    isCharging = false;
    Serial.println("Emergency stop requested. Charge pin set LOW.");
    server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Charging stopped immediately.\"}");
  } else {
    // Just ensure the pin is low and report success if it was already low/idle
    digitalWrite(CHARGE_PIN, LOW);
    server.send(200, "application/json", "{\"status\":\"success\", \"message\":\"Not currently charging. Pin confirmed LOW.\"}");
  }
}

/**
 * @brief Handles the /health API call.
 */
void handleHealth() {
  String response = "{\"status\":\"ok\", \"device\":\"ESP32\", \"uptime_ms\":" + String(millis()) + "}";
  server.send(200, "application/json", response);
}

/**
 * @brief Handles the /info API call, providing project context.
 */
void handleInfo() {
  String response = "{\"project\":\"Scrooge Capacitor Test Bench\", ";
  response += "\"description\":\"Tests capacitor charge/discharge for zero-leakage switching using relays (no transistors/MOSFETs).\", ";
  response += "\"repository\":\"https://github.com/psmgeelen/ESP32_API_TestBench\", ";
  response += "\"charge_pin\":" + String(CHARGE_PIN) + ", ";
  response += "\"api_version\":\"1.0.1\"}";
  server.send(200, "application/json", response);
}

/**
 * @brief Handles any 404 not found errors.
 */
void handleNotFound() {
  String message = "Resource Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : (server.method() == HTTP_POST ? "POST" : "OTHER");
  server.send(404, "text/plain", message);
}

// --- 5. CORE FUNCTIONS ---

/**
 * @brief Checks the non-blocking charge state and turns off the pin when time is up.
 */
void monitorChargeState() {
  if (isCharging) {
    // Check if the duration has passed
    // This is the non-blocking way to check time elapsed, safely handling millis() overflow.
    if (millis() - chargeStartTime >= chargeDurationMs) {
      digitalWrite(CHARGE_PIN, LOW); // Turn off the charge
      isCharging = false;
      Serial.printf("Charge complete after %d ms. Pin set LOW.\n", (int)chargeDurationMs);
    }
  }
}

/**
 * @brief Connects to Wi-Fi.
 */
void connectWifi() {
  Serial.print("Connecting to Wi-Fi...");
  WiFi.begin(ssid, password);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    attempt++;
    if (attempt > 20) {
      Serial.println("\nFailed to connect. Restarting...");
      ESP.restart(); // Restart if connection fails
    }
  }

  Serial.println("\nWiFi connected.");
  Serial.print("Access API at: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/swagger");
}

void setup() {
  Serial.begin(9600);
  delay(100);

  // Set the pin to output mode and LOW initially
  pinMode(CHARGE_PIN, OUTPUT);
  digitalWrite(CHARGE_PIN, LOW);

  connectWifi();

  // Define API routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/swagger", HTTP_GET, handleSwaggerUi);
  server.on("/swagger.json", HTTP_GET, handleSwaggerJson);
  
  // Control Endpoints
  server.on("/charge", HTTP_GET, handleCharge);
  server.on("/stop", HTTP_POST, handleStop); 
  
  // Status/Info Endpoints
  server.on("/state", HTTP_GET, handleState);
  server.on("/health", HTTP_GET, handleHealth);
  server.on("/info", HTTP_GET, handleInfo);

  // Fallback for 404
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP Server started.");
}

void loop() {
  // Handle incoming HTTP requests
  server.handleClient();

  // Non-blocking check for the charge state
  monitorChargeState();
}
