#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h> // Added for dynamic timestamp parsing and timezone calculation

HardwareSerial ec200(1);

#define EC200_TX      17
#define EC200_RX      18
#define EC200_PWRKEY  10

// WiFi Credentials
const char* ssid = "MOX";
const char* password = "20062006";

const char* apiURL = "https://b8vai0jyt6.execute-api.ap-southeast-2.amazonaws.com/upload";
const char* configURL = "https://b8vai0jyt6.execute-api.ap-southeast-2.amazonaws.com/getConfig";

uint8_t A_32[44];
char DEVICE_NAME[32] = "UFD-001";
char PHONE1[20] = "";
char PHONE2[20] = "";

unsigned long lastUploadTime = 0;
const unsigned long uploadInterval = 30000; // 30 seconds

struct MachineData {
    char operatorName[32];
    int chainage;
    int direction; 
    char statusMsg[16]; 
    float latitude;
    float longitude;
    char timestamp[24]; // Format: YYYY-MM-DD HH:MM:SS
    int encoder; 
    char machineId[16];
};

MachineData currentData;
int errorCount = 0; 

// Function prototypes
String sendAT(String cmd, uint32_t timeout = 5000);
bool sendSMS(const char* number, String message);
void parseGPSResponse(String resp);

void powerOnEC200() {
    pinMode(EC200_PWRKEY, OUTPUT);
    digitalWrite(EC200_PWRKEY, LOW);
    delay(100);
    digitalWrite(EC200_PWRKEY, HIGH);
    delay(2500);
    digitalWrite(EC200_PWRKEY, LOW);
    delay(10000);
}

bool initEC200() {
    sendAT("AT");
    sendAT("ATE0");
    sendAT("AT+CPIN?");
    sendAT("AT+CSQ");
    sendAT("AT+CREG?");
    sendAT("AT+QICSGP=1,1,\"airtelgprs.com\",\"\",\"\",1");
    sendAT("AT+QIACT=1", 15000);
    String ip = sendAT("AT+QIACT?", 5000);
    return ip.length() > 0;
}

String sendAT(String cmd, uint32_t timeout) {
    while (ec200.available()) ec200.read();
    ec200.println(cmd);
    String resp = "";
    uint32_t start = millis();
    
    while (millis() - start < timeout) {
        while (ec200.available()) {
            resp += (char)ec200.read();
        }
        if (resp.indexOf("OK\r\n") != -1 || resp.indexOf("ERROR\r\n") != -1) {
            break;
        }
    }
    Serial.println(resp);
    return resp;
}

bool sendSMS(const char* number, String message) {
    if (strlen(number) == 0) return false;
    Serial.print("Sending SMS to: ");
    Serial.println(number);
    sendAT("AT+CMGF=1");

    ec200.print("AT+CMGS=\"");
    ec200.print(number);
    ec200.println("\"");
    delay(3000);
    while (ec200.available()) Serial.write(ec200.read());

    ec200.print(message);
    delay(1000);
    ec200.write(26); 
    delay(10000);

    while (ec200.available()) Serial.write(ec200.read());
    return true;
}

bool fetchConfig() {
    Serial.println("Fetching Config via WiFi...");
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure(); 
    HTTPClient http;
    
    http.begin(client, configURL);
    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
        String body = http.getString();
        int jsonStart = body.indexOf('{');
        int jsonEnd   = body.lastIndexOf('}');

        if (jsonStart == -1 || jsonEnd == -1) {
            http.end();
            return false;
        }

        String json = body.substring(jsonStart, jsonEnd + 1);
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, json);

        if (err) {
            http.end();
            return false;
        }

        strlcpy(DEVICE_NAME, doc["device"] | "UFD-001", sizeof(DEVICE_NAME));
        strlcpy(PHONE1, doc["phone1"] | "", sizeof(PHONE1));
        strlcpy(PHONE2, doc["phone2"] | "", sizeof(PHONE2));

        http.end();
        return true;
    }
    http.end();
    return false;
}

void getInitialInputs() {
    Serial.println("\n--- Initial Setup ---");
    
    // 1. Get and Show Operator Name
    Serial.println("Enter Operator Name:");
    while (Serial.available() == 0) delay(10);
    String op = Serial.readStringUntil('\n');
    op.trim();
    op.toCharArray(currentData.operatorName, sizeof(currentData.operatorName));
    
    Serial.print("-> Received Operator Name: ");
    Serial.println(currentData.operatorName);
    Serial.println("--------------------------------");

    // 2. Get and Show Chainage
    Serial.println("Enter Chainage (integer):");
    while (Serial.available() == 0) delay(10);
    currentData.chainage = Serial.parseInt();
    Serial.readString(); 
    
    Serial.print("-> Received Chainage: ");
    Serial.println(currentData.chainage);
    Serial.println("--------------------------------");

    // 3. Get and Show Direction
    Serial.println("Enter Direction (1 for UP, 0 for DOWN):");
    while (Serial.available() == 0) delay(10);
    currentData.direction = Serial.parseInt();
    Serial.readString(); 
    
    Serial.print("-> Received Direction: ");
    if (currentData.direction == 1) {
        Serial.println("1 (UP)");
    } else if (currentData.direction == 0) {
        Serial.println("0 (DOWN)");
    } else {
        Serial.printf("%d (Invalid input, please use 1 or 0)\n", currentData.direction);
    }
    Serial.println("--------------------------------");
    
    // Set static parameters
    strcpy(currentData.machineId, DEVICE_NAME);
    currentData.encoder = 0; 
    
    // Removed static hardcoded timestamp assignment since it's handled live via GPS
    
    Serial.println("Setup Complete! Starting continuous loop...\n");
}

void generateA32Array() {
    memset(A_32, 0, sizeof(A_32));
    for (int i = 0; i <= 17; i++) {
        A_32[i] = random(0, 255);
    }
}

void appendA32ToTextFile() {
    File file = SPIFFS.open("/data.txt", FILE_APPEND);
    if (!file) return;
    
    for (int i = 0; i < 44; i++) {
        file.print(A_32[i]);
        if (i < 43) file.print(",");
    }
    file.println();
    file.close();
}

bool uploadFileToAWS(const char* filepath, String url) {
    File file = SPIFFS.open(filepath, FILE_READ);
    if (!file) return false;

    size_t fileSize = file.size();
    if (fileSize == 0) {
        file.close();
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure(); 
    HTTPClient http;
    
    http.begin(client, url);
    
    if (strstr(filepath, ".json") != NULL) {
        http.addHeader("Content-Type", "application/json");
    } else {
        http.addHeader("Content-Type", "text/plain");
    }

    Serial.printf("Uploading %s to AWS...\n", filepath);
    int httpResponseCode = http.sendRequest("POST", &file, fileSize);
    file.close();

    if (httpResponseCode == 200 || httpResponseCode == 201) {
        http.end();
        return true;
    } else {
        http.end();
        return false;
    }
}

void uploadTextData() {
    String url = String(apiURL) + "?device=" + String(DEVICE_NAME) + "&type=data";
    
    if (uploadFileToAWS("/data.txt", url)) {
        Serial.println("Upload Success! Buffer cleared.");
        SPIFFS.remove("/data.txt"); 
    } else {
        Serial.println("Upload Failed. Data stays in buffer...");
        File f = SPIFFS.open("/data.txt", FILE_READ);
        if (f && f.size() > 200000) {  
            f.close();
            SPIFFS.remove("/data.txt");
            Serial.println("Emergency: Buffer exceeded 200KB and was reset.");
        } else if (f) f.close();
    }
}

void uploadStructureData() {
    StaticJsonDocument<512> doc;
    doc["operatorName"] = currentData.operatorName;
    doc["chainage"] = currentData.chainage;
    doc["direction"] = currentData.direction;
    doc["statusMsg"] = currentData.statusMsg;
    doc["latitude"] = currentData.latitude;
    doc["longitude"] = currentData.longitude;
    doc["timestamp"] = currentData.timestamp;
    doc["encoder"] = currentData.encoder;
    doc["machineId"] = currentData.machineId;

    File file = SPIFFS.open("/struct.json", FILE_WRITE);
    if (!file) return;
    serializeJson(doc, file);
    file.close();

    String url = String(apiURL) + "?device=" + String(DEVICE_NAME) + "&type=struct";
    uploadFileToAWS("/struct.json", url);
}

void initGPS() {
    Serial.println("Turning on GPS Module...");
    sendAT("AT+QGPSCFG=\"gnssconfig\",5", 2000);
    delay(1000);
    sendAT("AT+QGPS=1", 2000); 
}

// Custom function to safely parse coordinates and timezone-corrected timestamp
void parseGPSResponse(String resp) {
    int idx = resp.indexOf("+QGPSLOC:");
    if (idx == -1) return;

    // Track the index placement of commas in the response string
    int commas[11];
    int currentComma = 0;
    int startFind = idx;
    
    while (currentComma < 11) {
        int nextComma = resp.indexOf(',', startFind);
        if (nextComma == -1) break;
        commas[currentComma++] = nextComma;
        startFind = nextComma + 1;
    }

    // Process if we have successfully parsed at least 10 fields
    if (currentComma >= 10) {
        // Extract Coordinates
        currentData.latitude = resp.substring(commas[0] + 1, commas[1]).toFloat();
        currentData.longitude = resp.substring(commas[1] + 1, commas[2]).toFloat();

        // Extract UTC Time (Format: hhmmss.sss)
        int colonIdx = resp.indexOf(':', idx);
        String utcStr = resp.substring(colonIdx + 1, commas[0]);
        utcStr.trim();

        // Extract Date (Format: ddmmyy)
        String dateStr = resp.substring(commas[8] + 1, commas[9]);
        dateStr.trim();

        if (utcStr.length() >= 6 && dateStr.length() == 6) {
            int hour   = utcStr.substring(0, 2).toInt();
            int minute = utcStr.substring(2, 4).toInt();
            int second = utcStr.substring(4, 6).toInt();

            int day   = dateStr.substring(0, 2).toInt();
            int month = dateStr.substring(2, 4).toInt();
            int year  = 2000 + dateStr.substring(4, 6).toInt();

            // Setup time struct for standard correction
            struct tm t = {0};
            t.tm_hour = hour;
            t.tm_min  = minute;
            t.tm_sec  = second;
            t.tm_mday = day;
            t.tm_mon  = month - 1;     // Months since January (0-11)
            t.tm_year = year - 1900;   // Years since 1900
            t.tm_isdst = -1;

            time_t epoch = mktime(&t);
            if (epoch != (time_t)-1) {
                // Apply Indian Standard Time (IST) shift: UTC + 5:30 (19800 seconds)
                epoch += 19800; 
                
                struct tm *localTime = gmtime(&epoch);
                if (localTime != NULL) {
                    snprintf(currentData.timestamp, sizeof(currentData.timestamp), 
                             "%04d-%02d-%02d %02d:%02d:%02d", 
                             localTime->tm_year + 1900, 
                             localTime->tm_mon + 1, 
                             localTime->tm_mday, 
                             localTime->tm_hour, 
                             localTime->tm_min, 
                             localTime->tm_sec);
                }
            } else {
                // Fallback directly to uncorrected GPS UTC format if conversion fails
                snprintf(currentData.timestamp, sizeof(currentData.timestamp), 
                         "%04d-%02d-%02d %02d:%02d:%02d", 
                         year, month, day, hour, minute, second);
            }
        }
    }
}

void waitForGPSFix() {
    Serial.println("\nGNSS Started");
    Serial.println("Move outdoors and wait for fix...");
    bool fixAquired = false;

    while (!fixAquired) {
        String resp = sendAT("AT+QGPSLOC=2", 5000);

        if (resp.indexOf("+QGPSLOC:") >= 0) {
            Serial.println("--------------------------------");
            Serial.println("GPS FIX RECEIVED! Proceeding...");
            Serial.println("--------------------------------");
            
            parseGPSResponse(resp); // Process location and timestamp dynamically
            fixAquired = true; 
        }
        else if (resp.indexOf("516") >= 0) {
            Serial.println("Searching satellites... Waiting 5 seconds");
            delay(5000);
        }
        else {
            Serial.println("No valid GPS response... Retrying.");
            delay(5000);
        }
    }
}

void updateGPS() {
    String resp = sendAT("AT+QGPSLOC=2", 1000);
    if (resp.indexOf("+QGPSLOC:") != -1) {
        parseGPSResponse(resp); // Live-update variables inside currentData struct
    }
}

void checkAndSendSMS() {
    if (strcmp(currentData.statusMsg, "imr") == 0 || strcmp(currentData.statusMsg, "obs") == 0) {
        errorCount++;

        if (errorCount >= 3) {
            int totalDistance = currentData.chainage + currentData.encoder;

            String smsBody = "FAULT ALERT:\n";
            smsBody += "MachineID: " + String(DEVICE_NAME) + "\n";
            smsBody += "Operator: " + String(currentData.operatorName) + "\n";
            smsBody += "Total Dist: " + String(totalDistance) + "\n";
            smsBody += "Status: " + String(currentData.statusMsg) + "\n";
            smsBody += "Loc: http://maps.google.com/maps?q=" + String(currentData.latitude, 6) + "," + String(currentData.longitude, 6) + "\n";
            smsBody += "Time: " + String(currentData.timestamp) + "\n";
            
            Serial.println("--- Sending SMS Alert ---");
            Serial.println(smsBody);
            
            sendSMS(PHONE1, smsBody);
            delay(3000);
            sendSMS(PHONE2, smsBody);
            
            errorCount = 0; 
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);

    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Failed");
        while (1);
    }

    SPIFFS.remove("/data.txt");
    SPIFFS.remove("/struct.json");

    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected!");

    ec200.begin(115200, SERIAL_8N1, EC200_RX, EC200_TX);
    powerOnEC200();
    initEC200();

    if (!fetchConfig()) {
        strcpy(DEVICE_NAME, "UFD-001");
        strcpy(PHONE1, "");
        strcpy(PHONE2, "");
    }
    
    initGPS(); 
    waitForGPSFix(); 
    
    randomSeed(micros());
    getInitialInputs(); 
    
    lastUploadTime = millis();
}

void loop() {
    generateA32Array();
    appendA32ToTextFile();
    
    currentData.encoder += 10; 
    
    int simStatus = random(0, 10); 
    if (simStatus == 1) strcpy(currentData.statusMsg, "imr");
    else if (simStatus == 2) strcpy(currentData.statusMsg, "obs");
    else strcpy(currentData.statusMsg, "ok");

    updateGPS(); 
    checkAndSendSMS(); 

    if (millis() - lastUploadTime >= uploadInterval) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi Disconnected! Data is buffering locally. Attempting reconnect...");
            WiFi.disconnect();
            WiFi.reconnect(); 
        } else {
            uploadTextData();
            uploadStructureData(); 
        }
        lastUploadTime = millis();
    }

    delay(2000);
}