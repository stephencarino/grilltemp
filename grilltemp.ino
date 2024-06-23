
/*

most of these codes came from
https://dronebotworkshop.com/wifimanager/
MAX31865 example

*/

#include <Adafruit_MAX31865.h>
#include <WiFiManager.h>
#include <PubSubClient.h> //MQTT
#include <ArduinoJson.h>
#include <FS.h>
#include <SPIFFS.h>
#include <fauxmoESP.h>

// json
#define JSON_CONFIG_FILE "/test_config.json"

static bool shouldSaveConfig = false;
static bool forceConfig = false;

// MQTT Broker
static char mqtt_broker[16];
static char mqtt_user[20];
static char mqtt_pass[20];
static int mqtt_port = 1883;

static WiFiClient espClient;
static PubSubClient mqttClient(espClient);
static fauxmoESP fauxmo;

#define numRtdSensors 2
// rtdSensors[1]
// Use software SPI: CS, DI, DO, CLK
#define PIN_CS1 5  // CS for sensor 1
#define PIN_CS2 17 // CS for sensor 2
// #define PIN_CS3 16  // CS for sensor 3
// #define PIN_CS4 4 // CS for sensor 4
#define PIN_DI 23  // SDI
#define PIN_DO 19  // SDO
#define PIN_CLK 18 // CLK
#define publishRate 60

Adafruit_MAX31865 rtdSensors[numRtdSensors] = {
    Adafruit_MAX31865(PIN_CS1, PIN_DI, PIN_DO, PIN_CLK),
    Adafruit_MAX31865(PIN_CS2, PIN_DI, PIN_DO, PIN_CLK)};

// The value of the Rref resistor. Use 430.0 for PT100 and 4300.0 for PT1000
#define RREF 430.0
// The 'nominal' 0-degrees-C resistance of the sensor
// 100.0 for PT100, 1000.0 for PT1000
#define RNOMINAL 100.0

// create instance of WiFiManager
static WiFiManager wm;

// AP name
const char *apName = "GrillTempMon";
const char *apPass = "password";

// bool wm_nonblocking = false; // change to true to use non blocking

// trigger the configuration portal when set to LOW
#define TRIGGER_PIN 0

static int timeout = 120; // seconds to run for

void saveConfigFile()
// Save Config in JSON format
{
    // for testing, format the file system
    // SPIFFS.format();

    Serial.println(F("Saving configuration..."));

    // Create a JSON document
    StaticJsonDocument<512> json;
    json["mqtt_broker"] = mqtt_broker;
    json["mqtt_user"] = mqtt_user;
    json["mqtt_pass"] = mqtt_pass;

    // Open config file
    File configFile = SPIFFS.open(JSON_CONFIG_FILE, "w");
    if (!configFile)
    {
        // Error, file did not open
        Serial.println("failed to open config file for writing");
    }

    // Serialize JSON data to write to file
    serializeJsonPretty(json, Serial);
    if (serializeJson(json, configFile) == 0)
    {
        // Error writing file
        Serial.println(F("Failed to write to file"));
    }
    // Close file
    configFile.close();
}

bool loadConfigFile()
// Load existing configuration file
{
    // Uncomment if we need to format filesystem
    // SPIFFS.format();

    // Read configuration from FS json
    Serial.println("Mounting File System...");

    // May need to make it begin(true) first time you are using SPIFFS
    if (SPIFFS.begin(false) || SPIFFS.begin(true))
    {
        Serial.println("mounted file system");
        if (SPIFFS.exists(JSON_CONFIG_FILE))
        {
            // The file exists, reading and loading
            Serial.println("reading config file");
            File configFile = SPIFFS.open(JSON_CONFIG_FILE, "r");
            if (configFile)
            {
                Serial.println("Opened configuration file");
                StaticJsonDocument<512> json;
                DeserializationError error = deserializeJson(json, configFile);
                serializeJsonPretty(json, Serial);
                if (!error)
                {
                    Serial.println("Parsing JSON");

                    strcpy(mqtt_broker, json["mqtt_broker"]);
                    strcpy(mqtt_user, json["mqtt_user"]);
                    strcpy(mqtt_pass, json["mqtt_pass"]);

                    return true;
                }
                else
                {
                    // Error loading JSON data
                    Serial.println("Failed to load json config");
                }
            }
        }
    }
    else
    {
        // Error mounting file system
        Serial.println("Failed to mount FS");
    }

    return false;
}

void saveConfigCallback()
// Callback notifying us of the need to save configuration
{
    Serial.println("Should save config");
    shouldSaveConfig = true;
}

void configModeCallback(WiFiManager *myWiFiManager)
// Called when config mode launched
{
    Serial.println("Entered Configuration Mode");

    Serial.print("Config SSID: ");
    Serial.println(myWiFiManager->getConfigPortalSSID());

    Serial.print("Config IP Address: ");
    Serial.println(WiFi.softAPIP());
}

void mqttConnect()
{
    if (!mqttClient.connected())
    {
        String client_id = "esp32-client-";
        client_id += String(WiFi.macAddress());
        Serial.printf("The client %s is attempting to connect to the MQTT broker\n", client_id.c_str());
        if (mqttClient.connect(client_id.c_str(), mqtt_user, mqtt_pass))
        {
            Serial.println("Connected to the MQTT broker");
        }
        else
        {
            Serial.print("Failed to connect with state ");
            Serial.println(mqttClient.state());
            delay(2000);
        }
    }
}

void setup()
{

    // Change to true when testing to force configuration every time we run

    // format the file system
    // SPIFFS.format();

    bool spiffsSetup = loadConfigFile();
    // bool spiffsSetup = false;
    if (!spiffsSetup)
    {
        Serial.println(F("Forcing config mode as there is no saved config"));
        forceConfig = true;
    }

    WiFi.mode(WIFI_STA);
    Serial.begin(115200);
    delay(10);

    // Set config save notify callback
    wm.setSaveConfigCallback(saveConfigCallback);

    // Set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wm.setAPCallback(configModeCallback);

    setupPortalConfig();

    Serial.println("Adafruit MAX31865 PT100 Sensor Test!");
    pinMode(TRIGGER_PIN, INPUT_PULLUP);

    // connecting to a mqtt broker
    mqttClient.setServer(mqtt_broker, mqtt_port);

    // By default, fauxmoESP creates it's own webserver on the defined port
    // The TCP port must be 80 for gen3 devices (default is 1901)
    // This has to be done before the call to enable()
    fauxmo.createServer(true); // not needed, this is the default value
    fauxmo.setPort(80);        // This is required for gen3 devices

    for (int i = 0; i < numRtdSensors; i++)
    {
        rtdSensors[i].begin();
    }
}

void setupWifi()
{
    if (forceConfig)
    // Run if configuration is needed
    {
        if (!wm.startConfigPortal(apName, apPass))
        {
            Serial.println("failed to connect and hit timeout");
            delay(3000);
            // reset and try again, or maybe put it to deep sleep
            ESP.restart();
            delay(5000);
        }
    }
    else
    {
        if (!wm.autoConnect(apName, apPass))
        {
            Serial.println("failed to connect and hit timeout");
            delay(3000);
            // if we still have not connected restart and try all over again
            ESP.restart();
            delay(5000);
        }
    }

    // If we get here, we are connected to the WiFi

    Serial.println("");
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
}

void setupPortalConfig()
{
    // Text box (String)
    WiFiManagerParameter custom_text_box_mqtt_broker("broker", "MQTT Broker IP", mqtt_broker, 16);
    WiFiManagerParameter custom_text_box_mqtt_user("user", "MQTT Username", mqtt_user, 20);
    WiFiManagerParameter custom_text_box_mqtt_pass("pass", "MQTT Password", mqtt_pass, 20);

    // Add all defined parameters
    wm.addParameter(&custom_text_box_mqtt_broker);
    wm.addParameter(&custom_text_box_mqtt_user);
    wm.addParameter(&custom_text_box_mqtt_pass);
    // wm.addParameter(&custom_text_box_num);

    // call standard setup
    setupWifi();

    // Lets deal with the user config values

    // Copy the string value
    strncpy(mqtt_broker, custom_text_box_mqtt_broker.getValue(), sizeof(mqtt_broker));
    // Serial.print("mqtt_broker: ");
    // Serial.println(mqtt_broker);

    strncpy(mqtt_user, custom_text_box_mqtt_user.getValue(), sizeof(mqtt_user));
    // Serial.print("mqtt_username: ");
    // Serial.println(mqtt_user);

    strncpy(mqtt_pass, custom_text_box_mqtt_pass.getValue(), sizeof(mqtt_pass));
    // Serial.print("mqtt_password: ");
    // Serial.println(mqtt_pass);

    // Convert the number value

    // Save the custom parameters to FS
    if (shouldSaveConfig)
    {
        saveConfigFile();
    }
}

void readTemp(int rtdSensorNum)
{
    char str_val[7];
    float degC;
    float degF;
    uint16_t rtd = rtdSensors[rtdSensorNum].readRTD();

    // rtdSensorNum+1 translates the array index to user-world index
    Serial.print("Sensor ");
    Serial.print(rtdSensorNum + 1);
    Serial.print(" ");
    Serial.print("RTD value: ");
    Serial.println(rtd);
    float ratio = rtd;
    ratio /= 32768;
    Serial.print("Sensor ");
    Serial.print(rtdSensorNum + 1);
    Serial.print(" ");
    Serial.print("Ratio = ");
    Serial.println(ratio, 8);
    Serial.print("Sensor ");
    Serial.print(rtdSensorNum + 1);
    Serial.print(" ");
    Serial.print("Resistance = ");
    Serial.println(RREF * ratio, 8);
    degC = rtdSensors[rtdSensorNum].temperature(RNOMINAL, RREF);
    degF = (degC * 9 / 5) + 32;

    snprintf(str_val, 7, "%4.2f", degC);
    String topic = "iot/grill_probe" + String(rtdSensorNum + 1) + "_temp_degC";
    mqttClient.publish(topic.c_str(), str_val);
    Serial.print("Sensor ");
    Serial.print(rtdSensorNum + 1);
    Serial.print(" ");
    Serial.print("Temperature (C) = ");
    Serial.println(str_val);

    snprintf(str_val, 7, "%4.2f", degF);
    topic = "iot/grill_probe" + String(rtdSensorNum + 1) + "_temp_degF";
    mqttClient.publish(topic.c_str(), str_val);
    Serial.print("Sensor ");
    Serial.print(rtdSensorNum + 1);
    Serial.print(" ");
    Serial.print("Temperature (F) = ");
    Serial.println(str_val);

    // Check and print any faults
    uint8_t fault = rtdSensors[rtdSensorNum].readFault();
    if (fault)
    {
        Serial.print("Sensor ");
        Serial.print(rtdSensorNum + 1);
        Serial.println(" Fault");
        Serial.print("Fault 0x");
        Serial.println(fault, HEX);
        if (fault & MAX31865_FAULT_HIGHTHRESH)
        {
            Serial.println("RTD High Threshold");
        }
        if (fault & MAX31865_FAULT_LOWTHRESH)
        {
            Serial.println("RTD Low Threshold");
        }
        if (fault & MAX31865_FAULT_REFINLOW)
        {
            Serial.println("REFIN- > 0.85 x Bias");
        }
        if (fault & MAX31865_FAULT_REFINHIGH)
        {
            Serial.println("REFIN- < 0.85 x Bias - FORCE- open");
        }
        if (fault & MAX31865_FAULT_RTDINLOW)
        {
            Serial.println("RTDIN- < 0.85 x Bias - FORCE- open");
        }
        if (fault & MAX31865_FAULT_OVUV)
        {
            Serial.println("Under/Over voltage");
        }
        rtdSensors[rtdSensorNum].clearFault();
    }
}

void loop()
{
    // if trigger is pressed, reset the wifi settings
    if (digitalRead(TRIGGER_PIN) == LOW)
    {
        wm.resetSettings();
    }
    mqttConnect();
    for (int i = 0; i < numRtdSensors; i++)
    {
        readTemp(i);
    }

    Serial.println();
    delay(publishRate * 1000);
}
