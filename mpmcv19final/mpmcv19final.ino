#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <map>
#include <time.h>

// WiFi credentials
const char* ssid = "Your_WiFI_SSID";
const char* password = "Your_WiFi_Password";

#define RST_PIN         0          
#define SS_PIN          5          

MFRC522 mfrc522(SS_PIN, RST_PIN);   
AsyncWebServer server(80);
AsyncEventSource events("/events");  // Server-sent events instance

MFRC522::MIFARE_Key key;
const uint16_t LOW_BALANCE_THRESHOLD = 500;
const uint16_t TOLL_AMOUNT = 200;
const uint8_t MAX_VIOLATIONS = 3;
const unsigned long RETURN_TIME_WINDOW = 10000; // 10 seconds in milliseconds
const uint8_t RETURN_DISCOUNT_PERCENT = 20;

uint8_t Z = 3;    // Higher byte for a balance of 1000
uint8_t Y = 232;  // Lower byte for a balance of 1000
uint16_t M = 0;

// Structure to hold card data
struct CardData {
    uint16_t balance;
    uint8_t violations;
    bool isBlacklisted;
    unsigned long lastTransactionTime;
    uint16_t pendingPayment;
    bool isReturnJourney;
    uint16_t blacklistFine;  // New field to track accumulated fines
};

// Map to store card data
std::map<String, CardData> cardDatabase;

// Function declarations
void loadCardDatabase();
void saveCardDatabase();
CardData& getCardData(const String& uid);
void saveTransaction(const String& cardUID, uint16_t amount, uint16_t remainingBalance, bool isRecharge = false, bool isPending = false);
void rechargeCard(uint16_t amount);
void hexToDecimal(uint8_t Z, uint8_t Y, uint16_t &M);
void decimalToHex(uint16_t M, uint8_t &Z, uint8_t &Y);

// Function to load card database from storage
void loadCardDatabase() {
    if (LittleFS.exists("/cards.json")) {
        File file = LittleFS.open("/cards.json", "r");
        DynamicJsonDocument doc(16384);
        deserializeJson(doc, file);
        file.close();

        cardDatabase.clear();
        JsonObject cards = doc.as<JsonObject>();
        for (JsonPair card : cards) {
            String uid = card.key().c_str();
            JsonObject cardData = card.value().as<JsonObject>();
            
            CardData data;
            data.balance = cardData["balance"] | 1000;
            data.violations = cardData["violations"] | 0;
            data.isBlacklisted = cardData["isBlacklisted"] | false;
            data.lastTransactionTime = cardData["lastTransactionTime"] | 0;
            data.pendingPayment = cardData["pendingPayment"] | 0;
            data.blacklistFine = cardData["blacklistFine"] | 0;  // Load blacklistFine
            
            cardDatabase[uid] = data;
        }
    }
}


// Function to save card database to storage
void saveCardDatabase() {
    DynamicJsonDocument doc(16384);
    JsonObject cards = doc.to<JsonObject>();
    
    for (const auto& pair : cardDatabase) {
        JsonObject cardData = cards.createNestedObject(pair.first);
        cardData["balance"] = pair.second.balance;
        cardData["violations"] = pair.second.violations;
        cardData["isBlacklisted"] = pair.second.isBlacklisted;
        cardData["lastTransactionTime"] = pair.second.lastTransactionTime;
        cardData["pendingPayment"] = pair.second.pendingPayment;
        cardData["blacklistFine"] = pair.second.blacklistFine;  // Save blacklistFine
    }
    
    File file = LittleFS.open("/cards.json", "w");
    serializeJson(doc, file);
    file.close();
}

// Function to get or create card data
CardData& getCardData(const String& uid) {
    if (cardDatabase.find(uid) == cardDatabase.end()) {
        CardData newCard;
        newCard.balance = 1000;
        newCard.violations = 0;
        newCard.isBlacklisted = false;
        newCard.lastTransactionTime = 0;
        newCard.pendingPayment = 0;
        newCard.blacklistFine = 0;  // Initialize blacklistFine
        cardDatabase[uid] = newCard;
        saveCardDatabase();
    }
    return cardDatabase[uid];
}

// Modified saveTransaction function with timestamp
void saveTransaction(const String& cardUID, uint16_t amount, uint16_t remainingBalance, bool isRecharge, bool isPending) {
    if(!LittleFS.exists("/transactions.json")) {
        File file = LittleFS.open("/transactions.json", "w");
        file.println("[]");
        file.close();
    }
    
    File readFile = LittleFS.open("/transactions.json", "r");
    DynamicJsonDocument doc(16384);
    deserializeJson(doc, readFile);
    readFile.close();
    
    JsonObject newTrans = doc.createNestedObject();
    newTrans["cardUID"] = cardUID;
    newTrans["amount"] = amount;
    newTrans["balance"] = remainingBalance;
    
    struct tm timeinfo;
    if(getLocalTime(&timeinfo)) {
        time_t now = time(nullptr);
        newTrans["timestamp"] = now * 1000; // Convert to milliseconds for JavaScript
    } else {
        newTrans["timestamp"] = millis();
    }
    
    newTrans["type"] = isRecharge ? "recharge" : (isPending ? "pending" : "deduct");
    
    File writeFile = LittleFS.open("/transactions.json", "w");
    serializeJson(doc, writeFile);
    writeFile.close();

    DynamicJsonDocument eventDoc(1024);
    eventDoc["type"] = "transaction";
    eventDoc["data"] = newTrans;
    String eventString;
    serializeJson(eventDoc, eventString);
    events.send(eventString.c_str(), "transaction", millis());
}

// Function to recharge the card
void rechargeCard(uint16_t amount) {
    hexToDecimal(Z, Y, M);
    M = M + amount;
    decimalToHex(M, Z, Y);
}

// Function to convert hex to decimal
void hexToDecimal(uint8_t Z, uint8_t Y, uint16_t &M) {
    M = (Z << 8) | Y;
}

// Function to convert decimal to hex
void decimalToHex(uint16_t M, uint8_t &Z, uint8_t &Y) {
    Z = (M >> 8) & 0xFF;
    Y = M & 0xFF;
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    
    // Initialize RFID
    SPI.begin();
    mfrc522.PCD_Init();
    for (byte i = 0; i < 6; i++) {
        key.keyByte[i] = 0xFF;
    }
    
    // Initialize LittleFS
    if(!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed");
        return;
    }
    
    // Load card database
    loadCardDatabase();
    
    // Connect to WiFi
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("Connected to WiFi");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // Configure time
    configTime(19800, 0, "pool.ntp.org");  // UTC +5:30 for IST
    
    // CORS headers
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

    // Web Server Routes
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(LittleFS, "/index.html", "text/html");
    });

    server.on("/transactions", HTTP_GET, [](AsyncWebServerRequest *request){
        if(!LittleFS.exists("/transactions.json")) {
            request->send(200, "application/json", "[]");
            return;
        }
        request->send(LittleFS, "/transactions.json", "application/json");
    });
    
    server.on("/cardInfo", HTTP_GET, [](AsyncWebServerRequest *request){
        String uid = request->arg("uid");
        if (uid.isEmpty()) {
          request->send(400, "application/json", "{\"error\":\"No UID provided\"}");
          return;
        }
    
        CardData& card = getCardData(uid);
        DynamicJsonDocument doc(1024);
        doc["balance"] = card.balance;
        doc["violations"] = card.violations;
        doc["isBlacklisted"] = card.isBlacklisted;
        doc["pendingPayment"] = card.pendingPayment;
        doc["blacklistFine"] = card.blacklistFine;  // Include blacklistFine in response
        doc["hasLowBalance"] = (card.balance < TOLL_AMOUNT);  // Added this line to check for low balance
    
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    
    server.on("/recharge", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(400, "application/json", "{\"error\":\"No data provided\"}");
    }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, (char*)data);
        
        String uid = doc["cardUID"].as<String>();
        uint16_t amount = doc["amount"].as<uint16_t>();
        
        if (uid.isEmpty()) {
            request->send(400, "application/json", "{\"error\":\"No UID provided\"}");
            return;
        }
        
        CardData& card = getCardData(uid);
        card.balance += amount;
        
        if (card.pendingPayment > 0 && card.balance >= card.pendingPayment) {
            card.balance -= card.pendingPayment;
            card.pendingPayment = 0;
            if (card.violations > 0) card.violations--;
        }
        
        saveCardDatabase();
        saveTransaction(uid, amount, card.balance, true);
        
        DynamicJsonDocument response(1024);
        response["success"] = true;
        response["balance"] = card.balance;
        response["violations"] = card.violations;
        response["pendingPayment"] = card.pendingPayment;
        
        String responseStr;
        serializeJson(response, responseStr);
        request->send(200, "application/json", responseStr);
    });

    // Add unblacklist route
    server.on("/unblacklist", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(400, "application/json", "{\"error\":\"No data provided\"}");
    }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, (char*)data);
    
        String uid = doc["cardUID"].as<String>();
        if (uid.isEmpty()) {
            request->send(400, "application/json", "{\"error\":\"No UID provided\"}");
            return;
        }
    
        CardData& card = getCardData(uid);
        if (card.isBlacklisted) {
            // Unblacklist the card and reset fine
            card.isBlacklisted = false;
            card.violations = 0;
            card.blacklistFine = 0;  // Reset fine
            saveCardDatabase();
        
            DynamicJsonDocument response(1024);
            response["success"] = true;
            response["newBalance"] = card.balance;
        
            String responseStr;
            serializeJson(response, responseStr);
            request->send(200, "application/json", responseStr);
        } else {
            request->send(400, "application/json", "{\"error\":\"Card is not blacklisted\"}");
        }
    });

    // Add SSE handler
    events.onConnect([](AsyncEventSourceClient *client){
        client->send("hello!", NULL, millis(), 1000);
    });
    server.addHandler(&events);
    
    server.begin();
}

void loop() {
    if (!mfrc522.PICC_IsNewCardPresent()) {
        return;
    }

    if (!mfrc522.PICC_ReadCardSerial()) {
        return;
    }

    String uid = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
        uid += String(mfrc522.uid.uidByte[i], HEX);
    }

    Serial.print("Scanned UID: ");
    Serial.println(uid);

    CardData& card = getCardData(uid);

    if (card.isBlacklisted) {
        // Initialize blacklistFine to 1000
        if (card.blacklistFine == 0) {
            card.blacklistFine = 1000;
            card.lastTransactionTime = millis();
            saveCardDatabase();
        }
        // Add 1000 to blacklistFine if enough time has passed since last read
        else if (millis() - card.lastTransactionTime >= 5000) {  // Use same 5-second delay
            card.blacklistFine += 1000;  // Add 1000 to fine
            card.lastTransactionTime = millis();  // Update last transaction time
            saveCardDatabase();
        }
        
        Serial.println("Blacklisted card detected! Fine increased.");
        Serial.print("Current total fine: ");
        Serial.println(card.blacklistFine);
        
        DynamicJsonDocument eventDoc(1024);
        eventDoc["type"] = "blacklisted";
        eventDoc["cardUID"] = uid;
        eventDoc["blacklistFine"] = card.blacklistFine;
        String eventString;
        serializeJson(eventDoc, eventString);
        events.send(eventString.c_str(), "blacklisted", millis());
        return;
    }


    // Check for return journey (within 10 seconds)
    bool isReturnJourney = (millis() - card.lastTransactionTime <= RETURN_TIME_WINDOW);
    uint16_t currentTollAmount = isReturnJourney ? 
        (TOLL_AMOUNT * (100 - RETURN_DISCOUNT_PERCENT) / 100) : 
        TOLL_AMOUNT;

    // Prevent multiple scans within 5 seconds
    if (millis() - card.lastTransactionTime < 5000) {
        Serial.println("Please wait before next transaction");
        return;
    }

    if (card.balance < currentTollAmount) {
        Serial.println("Insufficient balance.");
        card.violations++;
        card.pendingPayment += currentTollAmount;
        card.lastTransactionTime = millis();
        
        if (card.violations >= MAX_VIOLATIONS) {
            card.isBlacklisted = true;
            card.blacklistFine = 1000; // Initialize blacklistFine to 1000 when first blacklisted
            saveCardDatabase();
            Serial.println("Card blacklisted due to violations");
            
            DynamicJsonDocument eventDoc(1024);
            eventDoc["type"] = "blacklisted";
            eventDoc["cardUID"] = uid;
            String eventString;
            serializeJson(eventDoc, eventString);
            events.send(eventString.c_str(), "blacklisted", millis());
        } else {
            saveCardDatabase();
            Serial.println("Insufficient balance, violation recorded.");
            
            DynamicJsonDocument eventDoc(1024);
            eventDoc["type"] = "violation";
            eventDoc["cardUID"] = uid;
            eventDoc["violations"] = card.violations;
            eventDoc["pendingPayment"] = card.pendingPayment;
            String eventString;
            serializeJson(eventDoc, eventString);
            events.send(eventString.c_str(), "violation", millis());
        }
    } else {
        card.balance -= currentTollAmount;
        card.lastTransactionTime = millis();
        saveCardDatabase();
        
        // Modified saveTransaction call to include return journey info
        DynamicJsonDocument transDoc(1024);
        transDoc["cardUID"] = uid;
        transDoc["amount"] = currentTollAmount;
        transDoc["balance"] = card.balance;
        transDoc["isReturnJourney"] = isReturnJourney;
        String transString;
        serializeJson(transDoc, transString);
        
        saveTransaction(uid, currentTollAmount, card.balance);
        Serial.println(isReturnJourney ? "Return journey toll deducted with discount" : "Toll deducted");
    }

    DynamicJsonDocument eventDoc(1024);
    eventDoc["type"] = "cardUpdate";
    eventDoc["cardUID"] = uid;
    eventDoc["balance"] = card.balance;
    eventDoc["violations"] = card.violations;
    eventDoc["isBlacklisted"] = card.isBlacklisted;
    eventDoc["pendingPayment"] = card.pendingPayment;
    eventDoc["isReturnJourney"] = isReturnJourney;
    String eventString;
    serializeJson(eventDoc, eventString);
    events.send(eventString.c_str(), "cardUpdate", millis());
}
