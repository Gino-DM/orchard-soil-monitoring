
#include <Arduino.h>
#include <RadioLib.h>
#include <WiFi.h>
#include <PubSubClient.h>


// =====================================================
// LoRa CONFIGURATION
// Heltec WiFi LoRa 32 V3 / SX1262
// =====================================================

#define LORA_CS      8
#define LORA_DIO1    14
#define LORA_RST     12
#define LORA_BUSY    13

SX1262 radio = new Module(
  LORA_CS,
  LORA_DIO1,
  LORA_RST,
  LORA_BUSY
);


// =====================================================
// WIFI CONFIGURATION
// =====================================================

// Replace these placeholders with local deployment values before flashing.
// Do not commit real credentials or network addresses.
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";


// =====================================================
// MQTT CONFIGURATION
// =====================================================

const char* MQTT_SERVER = "YOUR_MQTT_BROKER_IP";
const int MQTT_PORT = 1883;


// =====================================================
// WIFI + MQTT CLIENT
// =====================================================

WiFiClient espClient;

PubSubClient mqttClient(
  espClient
);


// =====================================================
// SOIL DATA PACKET
//
// MUST MATCH THE SENDER EXACTLY
//
// 18 bytes  = nodeID
// 4 bytes   = sequenceNumber
// 4 bytes   = humidity
// 4 bytes   = temperature
// 4 bytes   = conductivity
// 4 bytes   = pH
// 2 bytes   = nitrogen
// 2 bytes   = phosphorus
// 2 bytes   = potassium
//
// TOTAL = 44 BYTES
// =====================================================

struct __attribute__((packed)) SoilData
{
  char nodeID[18];

  uint32_t sequenceNumber;

  float humidity;
  float temperature;
  float conductivity;
  float pH;

  uint16_t nitrogen;
  uint16_t phosphorus;
  uint16_t potassium;
};


// =====================================================
// ACK PACKET
//
// 18 bytes  = nodeID
// 4 bytes   = sequenceNumber
// 1 byte    = status
//
// TOTAL = 23 BYTES
//
// status:
// 0 = successful ACK
// non-zero = error
// =====================================================

struct __attribute__((packed)) AckPacket
{
  char nodeID[18];

  uint32_t sequenceNumber;

  uint8_t status;
};


SoilData soilData;


// =====================================================
// LoRa RECEIVE FLAG
// =====================================================

volatile bool receivedFlag = false;


// =====================================================
// DUPLICATE DETECTION
// =====================================================
//
// This version keeps track of the most recently
// successfully processed sequence number.
//
// For multiple nodes, a more advanced per-node
// sequence table can be added later.
// =====================================================

uint32_t lastSequenceNumber = 0;

bool haveReceivedPacket = false;


// =====================================================
// LoRa INTERRUPT
// =====================================================

void setFlag(void)
{
  receivedFlag = true;
}


// =====================================================
// CONNECT TO WIFI
// =====================================================

void connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }


  Serial.println();
  Serial.println("Connecting to Wi-Fi...");


  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );


  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);

    Serial.print(".");
  }


  Serial.println();
  Serial.println("Wi-Fi connected!");


  Serial.print("Gateway IP address: ");
  Serial.println(WiFi.localIP());


  Serial.print("MQTT server: ");
  Serial.println(MQTT_SERVER);
}


// =====================================================
// CONNECT TO MQTT
// =====================================================

void connectMQTT()
{
  while (!mqttClient.connected())
  {
    Serial.println();
    Serial.println("Connecting to MQTT broker...");


    String clientID = "HeltecGateway-";

    clientID += String(
      (uint32_t)ESP.getEfuseMac(),
      HEX
    );


    if (mqttClient.connect(clientID.c_str()))
    {
      Serial.println("MQTT connected!");

      Serial.println();
      Serial.println("MQTT mode: multiple soil nodes");

      Serial.println("Topic format:");
      Serial.println("orchard/soil/nodeX");
    }
    else
    {
      Serial.print("MQTT connection failed.");

      Serial.print(" State: ");

      Serial.println(
        mqttClient.state()
      );


      Serial.println(
        "Retrying in 5 seconds..."
      );


      delay(5000);
    }
  }
}


// =====================================================
// SEND ACK
// =====================================================

void sendAck(const SoilData& data)
{
  AckPacket ack;


  // Clear ACK structure
  memset(
    &ack,
    0,
    sizeof(ack)
  );


  // Copy node ID
  strncpy(
    ack.nodeID,
    data.nodeID,
    sizeof(ack.nodeID) - 1
  );


  ack.nodeID[
    sizeof(ack.nodeID) - 1
  ] = '\0';


  // Copy sequence number
  ack.sequenceNumber =
    data.sequenceNumber;


  // ===================================================
  // ACK STATUS
  //
  // 0 = SUCCESS
  //
  // This MUST be 0 because the sender checks:
  //
  // if (ack.status != 0)
  // {
  //     ACK error
  // }
  // ===================================================

  ack.status = 0;


  Serial.println();
  Serial.println("Sending ACK...");
  Serial.println("Waiting 200 ms before ACK...");



  Serial.print("ACK Node: ");
  Serial.println(ack.nodeID);


  Serial.print("ACK Sequence: ");
  Serial.println(
    ack.sequenceNumber
  );


  Serial.print("ACK Status: ");
  Serial.println(
    ack.status
  );


  Serial.print("ACK Packet Size: ");
  Serial.print(
    sizeof(ack)
  );

  Serial.println(" bytes");


  int state =
    radio.transmit(
      (uint8_t*)&ack,
      sizeof(ack)
    );


  if (state == RADIOLIB_ERR_NONE)
  {
    Serial.println(
      "ACK transmitted successfully."
    );
  }
  else
  {
    Serial.print(
      "ACK transmission failed."
    );

    Serial.print(
      " Error code: "
    );

    Serial.println(state);
  }
}


// =====================================================
// PRINT SOIL DATA
// =====================================================

void printSoilData()
{
  Serial.println();
  Serial.println(
    "Soil measurements:"
  );

  Serial.println(
    "----------------------------------------"
  );


  Serial.print(
    "Humidity:        "
  );

  Serial.print(
    soilData.humidity,
    1
  );

  Serial.println(
    " %RH"
  );


  Serial.print(
    "Temperature:     "
  );

  Serial.print(
    soilData.temperature,
    1
  );

  Serial.println(
    " C"
  );


  Serial.print(
    "Conductivity:    "
  );

  Serial.print(
    soilData.conductivity,
    0
  );

  Serial.println(
    " uS/cm"
  );


  Serial.print(
    "pH:              "
  );

  Serial.print(
    soilData.pH,
    1
  );

  Serial.println();


  Serial.print(
    "Nitrogen:        "
  );

  Serial.println(
    soilData.nitrogen
  );


  Serial.print(
    "Phosphorus:      "
  );

  Serial.println(
    soilData.phosphorus
  );


  Serial.print(
    "Potassium:       "
  );

  Serial.println(
    soilData.potassium
  );


  Serial.println(
    "----------------------------------------"
  );
}


// =====================================================
// SETUP
// =====================================================

void setup()
{
  Serial.begin(
    115200
  );


  delay(1000);


  Serial.println();
  Serial.println(
    "========================================"
  );

  Serial.println(
    "       LoRa + MQTT GATEWAY"
  );

  Serial.println(
    "       ACK + RETRANSMISSION"
  );

  Serial.println(
    "       Heltec WiFi LoRa 32 V3"
  );

  Serial.println(
    "========================================"
  );


  // ===================================================
  // VERIFY PACKET SIZES
  // ===================================================

  Serial.println();
  Serial.println(
    "Packet structure verification:"
  );


  Serial.print(
    "SoilData size: "
  );

  Serial.print(
    sizeof(SoilData)
  );

  Serial.println(
    " bytes"
  );


  Serial.print(
    "AckPacket size: "
  );

  Serial.print(
    sizeof(AckPacket)
  );

  Serial.println(
    " bytes"
  );


  if (sizeof(SoilData) != 44)
  {
    Serial.println();
    Serial.println(
      "WARNING: SoilData is NOT 44 bytes!"
    );
  }


  if (sizeof(AckPacket) != 23)
  {
    Serial.println();
    Serial.println(
      "WARNING: AckPacket is NOT 23 bytes!"
    );
  }


  // ===================================================
  // CONNECT TO WIFI
  // ===================================================

  connectWiFi();


  // ===================================================
  // CONFIGURE MQTT
  // ===================================================

  mqttClient.setServer(
    MQTT_SERVER,
    MQTT_PORT
  );


  // ===================================================
  // CONNECT TO MQTT BROKER
  // ===================================================

  connectMQTT();


  // ===================================================
  // INITIALIZE LoRa
  // ===================================================

  Serial.println();
  Serial.println(
    "Initializing LoRa..."
  );


  int state =
    radio.begin(
      868.0
    );


  if (state == RADIOLIB_ERR_NONE)
  {
    Serial.println(
      "LoRa initialized successfully!"
    );
  }
  else
  {
    Serial.print(
      "LoRa initialization failed."
    );

    Serial.print(
      " Error code: "
    );

    Serial.println(
      state
    );


    while (true)
    {
      delay(1000);
    }
  }


  // ===================================================
  // LoRa SETTINGS
  //
  // THESE MUST MATCH THE SENDER
  // ===================================================

  radio.setFrequency(
    868.0
  );


  radio.setBandwidth(
    125.0
  );


  radio.setSpreadingFactor(
    9
  );


  radio.setCodingRate(
    7
  );


  radio.setSyncWord(
    0x12
  );


  radio.setOutputPower(
    17
  );


  Serial.println();
  Serial.println(
    "LoRa configuration:"
  );


  Serial.println(
    "Frequency: 868 MHz"
  );


  Serial.println(
    "Bandwidth: 125 kHz"
  );


  Serial.println(
    "Spreading Factor: 9"
  );


  Serial.println(
    "Coding Rate: 4/7"
  );


  Serial.println(
    "Sync Word: 0x12"
  );


  Serial.println(
    "Power: 17 dBm"
  );


  // ===================================================
  // CONFIGURE LoRa INTERRUPT
  // ===================================================

  radio.setDio1Action(
    setFlag
  );


  // ===================================================
  // START LoRa RECEIVE MODE
  // ===================================================

  state =
    radio.startReceive();


  if (state == RADIOLIB_ERR_NONE)
  {
    Serial.println();
    Serial.println(
      "LoRa gateway ready."
    );


    Serial.println(
      "Listening for soil nodes..."
    );


    Serial.println();
  }
  else
  {
    Serial.print(
      "Failed to start LoRa receiver."
    );


    Serial.print(
      " Error code: "
    );


    Serial.println(
      state
    );


    while (true)
    {
      delay(1000);
    }
  }
}


// =====================================================
// LOOP
// =====================================================

void loop()
{
  // ===================================================
  // MAINTAIN WIFI CONNECTION
  // ===================================================

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println();
    Serial.println(
      "Wi-Fi connection lost."
    );


    connectWiFi();
  }


  // ===================================================
  // MAINTAIN MQTT CONNECTION
  // ===================================================

  if (!mqttClient.connected())
  {
    connectMQTT();
  }


  mqttClient.loop();


  // ===================================================
  // WAIT FOR LoRa PACKET
  // ===================================================

  if (!receivedFlag)
  {
    return;
  }


  receivedFlag = false;


  // ===================================================
  // READ LoRa DATA
  // ===================================================

  int state =
    radio.readData(
      (uint8_t*)&soilData,
      sizeof(soilData)
    );


  // ===================================================
  // CHECK LoRa DATA
  // ===================================================

  if (state == RADIOLIB_ERR_NONE)
  {
    // =================================================
    // GET LoRa COMMUNICATION INFORMATION
    // =================================================

    float rssi =
      radio.getRSSI();


    float snr =
      radio.getSNR();


    // =================================================
    // FORCE NODE ID TERMINATION
    // =================================================

    soilData.nodeID[
      sizeof(soilData.nodeID) - 1
    ] = '\0';


    // =================================================
    // CREATE NODE NAME
    // =================================================

    String nodeName =
      String(
        soilData.nodeID
      );


    // =================================================
    // CHECK FOR DUPLICATE
    // =================================================

    bool duplicatePacket = false;


    if (
      haveReceivedPacket &&
      soilData.sequenceNumber ==
      lastSequenceNumber
    )
    {
      duplicatePacket = true;


      Serial.println();
      Serial.println(
        "*** DUPLICATE PACKET DETECTED ***"
      );
    }


    // =================================================
    // DISPLAY LoRa INFORMATION
    // =================================================

    Serial.println();
    Serial.println(
      "========================================"
    );


    Serial.println(
      "          SOIL DATA RECEIVED"
    );


    Serial.println(
      "========================================"
    );


    Serial.print(
      "Soil Node:       "
    );


    Serial.println(
      nodeName
    );


    Serial.print(
      "Sequence:        "
    );


    Serial.println(
      soilData.sequenceNumber
    );


    Serial.print(
      "Packet size:     "
    );


    Serial.print(
      sizeof(soilData)
    );


    Serial.println(
      " bytes"
    );


    Serial.print(
      "RSSI:            "
    );


    Serial.print(
      rssi
    );


    Serial.println(
      " dBm"
    );


    Serial.print(
      "SNR:             "
    );


    Serial.print(
      snr
    );


    Serial.println(
      " dB"
    );
    delay(200);


    // =================================================
    // SEND ACK IMMEDIATELY
    // =================================================
    //
    // IMPORTANT:
    // ACK is sent for BOTH new packets and duplicates.
    //
    // This allows a sender that did not hear the first
    // ACK to retransmit and receive another ACK.
    // =================================================

    sendAck(
      soilData
    );


    // =================================================
    // PROCESS ONLY NEW PACKETS
    // =================================================

    if (!duplicatePacket)
    {
      // ===============================================
      // DISPLAY SOIL DATA
      // ===============================================

      printSoilData();


      // ===============================================
      // CREATE MQTT TOPIC
      // ===============================================

      String mqttTopic =
        "orchard/soil/" +
        nodeName;


      // ===============================================
      // CREATE JSON MQTT MESSAGE
      // ===============================================

      char mqttMessage[350];


      snprintf(
        mqttMessage,
        sizeof(mqttMessage),

        "{"
          "\"node\":\"%s\","
          "\"sequence\":%lu,"
          "\"humidity\":%.1f,"
          "\"temperature\":%.1f,"
          "\"conductivity\":%.0f,"
          "\"ph\":%.1f,"
          "\"nitrogen\":%u,"
          "\"phosphorus\":%u,"
          "\"potassium\":%u,"
          "\"rssi\":%.1f,"
          "\"snr\":%.1f"
        "}",

        nodeName.c_str(),

        (unsigned long)
        soilData.sequenceNumber,

        soilData.humidity,

        soilData.temperature,

        soilData.conductivity,

        soilData.pH,

        soilData.nitrogen,

        soilData.phosphorus,

        soilData.potassium,

        rssi,

        snr
      );


      // ===============================================
      // DISPLAY MQTT INFORMATION
      // ===============================================

      Serial.println();
      Serial.println(
        "MQTT topic:"
      );


      Serial.println(
        "----------------------------------------"
      );


      Serial.println(
        mqttTopic
      );


      Serial.println();
      Serial.println(
        "MQTT message:"
      );


      Serial.println(
        "----------------------------------------"
      );


      Serial.println(
        mqttMessage
      );


      Serial.println(
        "----------------------------------------"
      );


      // ===============================================
      // PUBLISH TO MQTT
      // ===============================================

      if (
        mqttClient.publish(
          mqttTopic.c_str(),
          mqttMessage
        )
      )
      {
        Serial.println(
          "MQTT message published successfully!"
        );
      }
      else
      {
        Serial.println(
          "MQTT publish FAILED!"
        );
      }


      // ===============================================
      // REMEMBER LAST PACKET
      // ===============================================

      lastSequenceNumber =
        soilData.sequenceNumber;


      haveReceivedPacket = true;
    }
    else
    {
      Serial.println();
      Serial.println(
        "Duplicate packet will NOT be published to MQTT."
      );


      Serial.println(
        "ACK has been sent again."
      );
    }


    Serial.println(
      "========================================"
    );
  }
  else
  {
    // =================================================
    // LoRa RECEIVE ERROR
    // =================================================

    Serial.print(
      "Failed to read LoRa packet."
    );


    Serial.print(
      " Error code: "
    );


    Serial.println(
      state
    );
  }


  // ===================================================
  // RETURN TO LoRa RECEIVE MODE
  // ===================================================

  radio.startReceive();
}
