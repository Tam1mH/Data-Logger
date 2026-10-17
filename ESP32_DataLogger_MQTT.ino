#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <BH1750.h>
#include <RTClib.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>

#define WDT_TIMEOUT_DETIK 60

// --- PIN Declaration ---
#define DHTPIN 4
#define DHTTYPE DHT22
#define ONE_WIRE_BUS 16
#define SOIL_PIN 34
#define SD_CS_PIN 5

// --- TARGET INTERVAL SETTING (15 Minute = 900 Second) ---
#define TOTAL_INTERVAL_DETIK 900.0    // Interval for measurment 

// --- WIFI NETWORK CREDENTIALS ---
const char* ssid     = "";      // Insert the SSID
const char* password = "";  // Insert the Password

// --- MQTT THINGSPEAK CREDENTIALS ---
const char* mqttBroker   = "mqtt3.thingspeak.com";
const int mqttPort       = 1883;
const char* mqttClientID = ""; // Adjust based on the MQTT Device Configuration
const char* mqttUserName = "";  // Adjust based on the MQTT Device Configuration
const char* mqttPassWord = "";  // Adjust based on the MQTT Device Configuration
const char* channelID    = "";// Your ThingSpeak Channel ID

// --- CALIBRATION VARIABLES ---
float offsetSuhuUdara = 0 ; // Change the Value based on your measurement
float offsetSuhuTanah = 0 ; // Change the Value based on your measurement
int soilDryValue = 0 ; // Change the Value based on your measurement
int soilWetValue = 0 ; // Change the Value based on your measurement

// Device Initiation
DHT dht(DHTPIN, DHTTYPE);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
BH1750 lightMeter;
RTC_DS3231 rtc;

// Network Object Initialization
WiFiClient espClient;
PubSubClient mqttClient(espClient);

void setup() {
  // 1. MARK START TIME (Start ESP32 Internal Stopwatch)
  unsigned long waktuMulai = millis();

  Serial.begin(115200);
  Wire.begin();

  esp_task_wdt_init(WDT_TIMEOUT_DETIK, true); 
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();

  Serial.println("\n=== ESP32 Wakes Up! Initiating the Data Collection and Transmission Cycle ===");

  // 2. Local Hardware Initiation
  if (!rtc.begin()) Serial.println("Failed to find RTC DS3231!");
  else if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  if (!lightMeter.begin()) Serial.println("Failed to find BH1750 sensor!");

  bool sdSiap = SD.begin(SD_CS_PIN);
  if (!sdSiap) {
    Serial.println("SD Card Initiation Failed!");
  } else {
    const char* namaFile = "/dataset.csv";
    File dataFile = SD.open(namaFile, FILE_READ);
    bool butuhHeader = (!dataFile || dataFile.size() == 0);
    if (dataFile) dataFile.close();

    if (butuhHeader) {
      dataFile = SD.open(namaFile, FILE_WRITE);
      if (dataFile) {
        dataFile.println("Time,TempU,HumU,TempT,Soil,Lux");
        dataFile.close();
      }
    }
  }

  dht.begin();
  sensors.begin();

  // Mandatory sensor stabilization pause
  delay(2000);

  // Watchdog checkpoint: hardware initialization completed normally; reporting in.
  esp_task_wdt_reset();

  // 3. LOCAL DATA ACQUISITION & CALIBRATION
  DateTime now = rtc.now();
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  sensors.requestTemperatures();
  float tempTanah = sensors.getTempCByIndex(0);
  float lux = lightMeter.readLightLevel();
  int soilRaw = analogRead(SOIL_PIN);

  float t_cal = t + offsetSuhuUdara;
  float tempTanah_cal = tempTanah + offsetSuhuTanah;
  int soilMoisturePercent = map(soilRaw, soilDryValue, soilWetValue, 0, 100);
  soilMoisturePercent = constrain(soilMoisturePercent, 0, 100);

  // 4. SAVING TO SD CARD
  char timeBuffer[25];
  sprintf(timeBuffer, "%d/%02d/%04d %02d:%02d", now.day(), now.month(), now.year(), now.hour(), now.minute());

  String dataString = String(timeBuffer) + "," + String(t_cal, 1) + "," + String(h, 1) + "," +
                      String(tempTanah_cal, 1) + "," + String(soilMoisturePercent) + "," + String(lux, 1);

  if (sdSiap) {
    File dataFile = SD.open("/dataset.csv", FILE_APPEND);
    if (dataFile) {
      dataFile.println(dataString);
      dataFile.close();
      Serial.println("[Local] Data successfully saved to SD card.");
    } else {
      Serial.println("[Local] SD card is ready upon boot but fails to open for writing.");
    }
  } else {
    Serial.println("[Local] Skipped: SD card failed to mount at the start of the cycle (see initialization log).");
  }

  // Checkpoint watchdog: data is safely stored on the SD card before entering the riskier WiFi phase.
  esp_task_wdt_reset();

  // 5. IoT MISSION: DATA TRANSMISSION VIA WI-FI & MQTT
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  // Protection feature: limit the Wi-Fi connection timeout to a maximum of 10 seconds
  unsigned long startWifiAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startWifiAttempt < 10000) {
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[Wi-Fi] Connected! Setting up MQTT configuration...");
    mqttClient.setServer(mqttBroker, mqttPort);

    // Attempting to connect to the ThingSpeak MQTT broker
    Serial.print("Connecting to Broker MQTT...");
    if (mqttClient.connect(mqttClientID, mqttUserName, mqttPassWord)) {
      Serial.println(" Connected!");

      DateTime nowUtc = now - TimeSpan(0, 7, 0, 0);
      char isoCreatedAt[26];
      sprintf(isoCreatedAt, "%04d-%02d-%02dT%02d:%02d:%02dZ",
              nowUtc.year(), nowUtc.month(), nowUtc.day(),
              nowUtc.hour(), nowUtc.minute(), nowUtc.second());

      String payload = "field1=" + String(t_cal, 1) +
                       "&field2=" + String(h, 1) +
                       "&field3=" + String(tempTanah_cal, 1) +
                       "&field4=" + String(soilMoisturePercent) +
                       "&field5=" + String(lux, 1) +
                       "&created_at=" + String(isoCreatedAt);

      // Structuring MQTT publication topics for ThingSpeak
      String topic = "channels/" + String(channelID) + "/publish";

      // sending data to the internet
      if (mqttClient.publish(topic.c_str(), payload.c_str())) {
        Serial.println("[Online] Data successfully sent to the ThingSpeak Dashboard via MQTT!");
      } else {
        Serial.println("[Online] Failed to publish data via MQTT.");
      }

      // --- IMPORTANT CODE ADDITION --- 
      // Allow the MQTT library to complete processing in the background
      mqttClient.loop();
      // Wait for 1 second to allow the Wi-Fi antenna to fully transmit data to the router.
      delay(1000);
      // -----------------------------

      mqttClient.disconnect();
    } else {
      Serial.print(" MQTT connection failed, Error Status: ");
      Serial.println(mqttClient.state());
    }

    // Matikan modul radio Wi-Fi
    WiFi.disconnect(true);
    Serial.println("[Wi-Fi] Radio turned off.");
  } else {
    Serial.println("\n[Warning] Wi-Fi not found/timed out. Data is stored locally only..");
  }

// Watchdog checkpoint: the WiFi and MQTT phases have concluded (whether successfully or via a normal failure), reporting status before entering the sleep-timing phase.
  esp_task_wdt_reset();

  // 6. CALCULATE SLEEP TIME COMPENSATION OUTPUT
  unsigned long waktuSelesai = millis();
  float lamaKerjaDetik = (waktuSelesai - waktuMulai) / 1000.0;

  Serial.print("Total Execution Time for This Cycle: ");
  Serial.print(lamaKerjaDetik, 2);
  Serial.println(" second.");

  float waktuTidurDetik = TOTAL_INTERVAL_DETIK - lamaKerjaDetik;
  if (waktuTidurDetik < 1.0) {
    waktuTidurDetik = 1.0;
  }

  Serial.print("Entering Deep Sleep. The device will hibernate for: ");
  Serial.print(waktuTidurDetik, 2);
  Serial.println(" second.\n");
  Serial.flush();

 // Final watchdog check before sleep — not strictly necessary (deep sleep shuts down
// the CPU anyway), but it’s inexpensive and safe to include.
  esp_task_wdt_reset();

  // 7.DEEP SLEEP EXECUTION
  esp_sleep_enable_timer_wakeup(waktuTidurDetik * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {
  
}
