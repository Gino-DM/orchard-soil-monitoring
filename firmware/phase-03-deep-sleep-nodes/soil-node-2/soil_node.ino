#include <Arduino.h>
#include <RadioLib.h>
#include <Preferences.h>

// ============================================================
// LoRa
// ============================================================

#define LORA_CS    8
#define LORA_DIO1  14
#define LORA_RST   12
#define LORA_BUSY  13

SX1262 radio = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY);

// ============================================================
// RS485 Soil Sensor
// ============================================================

#define RS485_RX     19
#define RS485_TX     20
#define RS485_RE_DE  21

HardwareSerial RS485Serial(1);

// ============================================================
// Sensor Relay
// ============================================================

#define SENSOR_RELAY 47

// ============================================================
// Node ID
// ============================================================

const char NODE_ID[] = "NODE-48CA435625D8";

// ============================================================
// Timing / Reliability
// ============================================================

const unsigned long MEASUREMENT_INTERVAL = 15UL * 60UL * 1000UL;  // 15 minutes (ms)
const unsigned long ACK_TIMEOUT = 5000;             // 5 seconds

const int MAX_RETRIES = 2;
const unsigned long RETRY_DELAY = 500;

const unsigned long TX_TO_RX_DELAY = 20;
// ============================================================
// NVS
// ============================================================

Preferences preferences;

// ============================================================
// Soil packet
// MUST match receiver exactly
// ============================================================

struct __attribute__((packed)) SoilPacket {

  char nodeId[18];

  uint32_t sequence;

  float humidity;
  float temperature;
  float conductivity;
  float pH;

  uint16_t nitrogen;
  uint16_t phosphorus;
  uint16_t potassium;
};

// ============================================================
// ACK packet
// MUST match receiver exactly
// ============================================================

struct __attribute__((packed)) AckPacket {

  char nodeId[18];

  uint32_t sequence;

  uint8_t status;
};

// ============================================================
// Sequence number
// ============================================================

uint32_t sequenceNumber = 0;

// ============================================================
// DIO1 receive flag
// ============================================================

volatile bool receivedFlag = false;

void setReceivedFlag(void)
{
  receivedFlag = true;
}

// ============================================================
// CRC16 Modbus
// ============================================================

uint16_t modbusCRC(uint8_t *buffer, uint8_t length)
{
  uint16_t crc = 0xFFFF;

  for (uint8_t pos = 0; pos < length; pos++)
  {
    crc ^= buffer[pos];

    for (uint8_t i = 8; i != 0; i--)
    {
      if ((crc & 0x0001) != 0)
      {
        crc >>= 1;
        crc ^= 0xA001;
      }
      else
      {
        crc >>= 1;
      }
    }
  }

  return crc;
}

// ============================================================
// Read soil sensor
// ============================================================

bool readSoilSensor(
  float &humidity,
  float &temperature,
  float &conductivity,
  float &pH,
  uint16_t &nitrogen,
  uint16_t &phosphorus,
  uint16_t &potassium
)
{
  uint8_t request[] = {
    0x01,
    0x03,
    0x00,
    0x00,
    0x00,
    0x07,
    0x04,
    0x08
  };

  while (RS485Serial.available())
  {
    RS485Serial.read();
  }

  digitalWrite(RS485_RE_DE, HIGH);

  delay(2);

  RS485Serial.write(request, sizeof(request));
  RS485Serial.flush();

  digitalWrite(RS485_RE_DE, LOW);

  unsigned long start = millis();

  uint8_t response[32];
  uint8_t index = 0;

  bool headerFound = false;

  while (millis() - start < 1000)
  {
    if (RS485Serial.available())
    {
      uint8_t b = RS485Serial.read();

      // ------------------------------------------------------
      // Synchronise on:
      // 01 03 0E
      // ------------------------------------------------------

      if (!headerFound)
      {
        if (index == 0 && b == 0x01)
        {
          response[0] = b;
          index = 1;
        }
        else if (index == 1 && b == 0x03)
        {
          response[1] = b;
          index = 2;
        }
        else if (index == 2 && b == 0x0E)
        {
          response[2] = b;
          index = 3;

          headerFound = true;
        }
        else
        {
          index = 0;
        }
      }
      else
      {
        response[index++] = b;

        // Expected response:
        //
        // 01 03 0E
        // + 14 data bytes
        // + 2 CRC bytes
        //
        // Total = 19 bytes

        if (index >= 19)
        {
          break;
        }
      }
    }
  }

  if (!headerFound || index < 19)
  {
    Serial.println("Sensor response timeout.");
    return false;
  }

  // ----------------------------------------------------------
  // Verify sensor ID
  // ----------------------------------------------------------

  if (response[0] != 0x01)
  {
    Serial.println("Invalid sensor ID");
    return false;
  }

  // ----------------------------------------------------------
  // Verify function code
  // ----------------------------------------------------------

  if (response[1] != 0x03)
  {
    Serial.println("Invalid Modbus function");
    return false;
  }

  // ----------------------------------------------------------
  // Verify byte count
  // ----------------------------------------------------------

  if (response[2] != 0x0E)
  {
    Serial.println("Invalid byte count");
    return false;
  }

  // ----------------------------------------------------------
  // CRC
  // ----------------------------------------------------------

  uint16_t calculatedCRC = modbusCRC(response, 17);

  uint16_t receivedCRC =
    response[17] |
    (response[18] << 8);

  if (calculatedCRC != receivedCRC)
  {
    Serial.println("Sensor CRC error.");
    return false;
  }

  // ----------------------------------------------------------
  // Decode registers
  // ----------------------------------------------------------

  uint16_t rawHumidity =
    (response[3] << 8) |
    response[4];

  uint16_t rawTemperature =
    (response[5] << 8) |
    response[6];

  uint16_t rawConductivity =
    (response[7] << 8) |
    response[8];

  uint16_t rawPH =
    (response[9] << 8) |
    response[10];

  nitrogen =
    (response[11] << 8) |
    response[12];

  phosphorus =
    (response[13] << 8) |
    response[14];

  potassium =
    (response[15] << 8) |
    response[16];

  // ----------------------------------------------------------
  // Convert values
  // ----------------------------------------------------------

  humidity = rawHumidity / 10.0;

  temperature = rawTemperature / 10.0;

  conductivity = rawConductivity;

  pH = rawPH / 10.0;

  return true;
}

// ============================================================
// Wait for ACK
// ============================================================

bool waitForAck(uint32_t expectedSequence)
{
  Serial.println();
  Serial.println("Waiting for ACK...");

  receivedFlag = false;

  // ----------------------------------------------------------
  // Put radio into receive mode
  // ----------------------------------------------------------

  int state = radio.startReceive();

  if (state != RADIOLIB_ERR_NONE)
  {
    Serial.print("Failed to start ACK receive mode. Code: ");
    Serial.println(state);

    return false;
  }

  unsigned long startTime = millis();

  while (!receivedFlag &&
         millis() - startTime < ACK_TIMEOUT)
  {
    delay(1);
  }

  // ----------------------------------------------------------
  // Timeout
  // ----------------------------------------------------------

  if (!receivedFlag)
  {
    Serial.println("ACK timeout.");

    radio.standby();

    return false;
  }

  // ----------------------------------------------------------
  // ACK received
  // ----------------------------------------------------------

  receivedFlag = false;

  Serial.println("LoRa packet received while waiting for ACK.");

  size_t packetLength = radio.getPacketLength();

  Serial.print("Received packet size: ");
  Serial.println(packetLength);

  // ----------------------------------------------------------
  // Safety check
  // ----------------------------------------------------------

  if (packetLength != sizeof(AckPacket))
  {
    Serial.print("Unexpected ACK packet size. Expected: ");
    Serial.print(sizeof(AckPacket));
    Serial.print(" Received: ");
    Serial.println(packetLength);

    radio.standby();

    return false;
  }

  // ----------------------------------------------------------
  // Read ACK
  // ----------------------------------------------------------

  AckPacket ack;

  state = radio.readData(
    (uint8_t *)&ack,
    sizeof(AckPacket)
  );

  radio.standby();

  if (state != RADIOLIB_ERR_NONE)
  {
    Serial.print("Failed to read ACK. Code: ");
    Serial.println(state);

    return false;
  }

  // ----------------------------------------------------------
  // Display ACK
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("========== ACK RECEIVED ==========");

  Serial.print("ACK Node: ");
  Serial.println(ack.nodeId);

  Serial.print("ACK Sequence: ");
  Serial.println(ack.sequence);

  Serial.print("ACK Status: ");
  Serial.println(ack.status);

  Serial.println("==================================");

  // ----------------------------------------------------------
  // Validate node ID
  // ----------------------------------------------------------

  if (strcmp(ack.nodeId, NODE_ID) != 0)
  {
    Serial.println("ACK belongs to another node.");

    return false;
  }

  // ----------------------------------------------------------
  // Validate sequence
  // ----------------------------------------------------------

  if (ack.sequence != expectedSequence)
  {
    Serial.println("ACK sequence does not match.");

    return false;
  }

  // ----------------------------------------------------------
  // Validate status
  //
  // status = 0 means SUCCESS
  // ----------------------------------------------------------

  if (ack.status != 0)
  {
    Serial.println("ACK reports an error.");

    return false;
  }

  unsigned long responseTime = millis() - startTime;

  Serial.print("ACK response time: ");
  Serial.print(responseTime);
  Serial.println(" ms");

  Serial.println("ACK VALID.");

  return true;
}

// ============================================================
// Send packet with ACK + retransmission
// ============================================================

bool sendSoilPacket(SoilPacket &packet)
{
  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++)
  {
    Serial.println();
    Serial.println("--------------------------------");
    Serial.print("Transmission attempt: ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.println(MAX_RETRIES);

    Serial.print("Node: ");
    Serial.println(packet.nodeId);

    Serial.print("Sequence: ");
    Serial.println(packet.sequence);

    Serial.print("Packet size: ");
    Serial.print(sizeof(SoilPacket));
    Serial.println(" bytes");

    // --------------------------------------------------------
    // Transmit
    // --------------------------------------------------------

    int state = radio.transmit(
      (uint8_t *)&packet,
      sizeof(SoilPacket)
    );

    if (state != RADIOLIB_ERR_NONE)
    {
      Serial.print("LoRa transmission failed. Code: ");
      Serial.println(state);

      delay(RETRY_DELAY);

      continue;
    }

    Serial.println("Packet transmitted successfully.");

    // --------------------------------------------------------
    // IMPORTANT:
    //
    // Now switch immediately into RX mode.
    // --------------------------------------------------------
 delay(TX_TO_RX_DELAY);
    if (waitForAck(packet.sequence))
    {
      Serial.println();
      Serial.println("TRANSMISSION SUCCESSFUL");
      delay(20);
      Serial.println("Packet acknowledged by gateway.");
      Serial.println("================================");

      return true;
    }

    // --------------------------------------------------------
    // Retry
    // --------------------------------------------------------

    if (attempt < MAX_RETRIES)
    {
      Serial.println();
      Serial.println("No valid ACK received.");
      Serial.print("Retrying in ");
      Serial.print(RETRY_DELAY);
      Serial.println(" ms...");

      delay(RETRY_DELAY);
    }
  }

  Serial.println();
  Serial.println("TRANSMISSION FAILED");
  Serial.println("Maximum retries reached.");
  Serial.println("================================");

  return false;
}

// ============================================================
// Deep sleep
// ============================================================

void goToSleep()
{
  Serial.println();
  Serial.println("Going to deep sleep for 15 minutes...");
  Serial.flush();

  // Put LoRa radio to sleep
  radio.sleep();

  // Make sure the sensor relay stays OFF during sleep
  digitalWrite(SENSOR_RELAY, LOW);
  gpio_hold_en((gpio_num_t)SENSOR_RELAY);
  gpio_deep_sleep_hold_en();

  // Wake up after the interval (value is in microseconds)
  esp_sleep_enable_timer_wakeup(MEASUREMENT_INTERVAL * 1000ULL);

  esp_deep_sleep_start();
}

// ============================================================
// Setup
// ============================================================

void setup()
{
  Serial.begin(115200);

  delay(2000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("SOIL MONITORING NODE");
  Serial.println("ACK + RETRANSMISSION TEST");
  Serial.println("========================================");

  // ----------------------------------------------------------
  // Relay
  // ----------------------------------------------------------

  gpio_hold_dis((gpio_num_t)SENSOR_RELAY);

  pinMode(SENSOR_RELAY, OUTPUT);

  digitalWrite(SENSOR_RELAY, LOW);

  // ----------------------------------------------------------
  // RS485
  // ----------------------------------------------------------

  pinMode(RS485_RE_DE, OUTPUT);

  digitalWrite(RS485_RE_DE, LOW);

  RS485Serial.begin(
    4800,
    SERIAL_8N1,
    RS485_RX,
    RS485_TX
  );

  // ----------------------------------------------------------
  // NVS
  // ----------------------------------------------------------

  preferences.begin("soil-node", false);

  sequenceNumber =
    preferences.getUInt("sequence", 0);

  Serial.print("Starting sequence number: ");
  Serial.println(sequenceNumber + 1);

  // ----------------------------------------------------------
  // LoRa
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("Initializing LoRa...");

  int state = radio.begin(
    868.0,
    125.0,
    7,
    5,
    0x12,
    14
  );

  if (state != RADIOLIB_ERR_NONE)
  {
    Serial.print("LoRa initialization failed. Code: ");
    Serial.println(state);

    while (true)
    {
      delay(1000);
    }
  }

  // ----------------------------------------------------------
  // Configure DIO1 interrupt
  // ----------------------------------------------------------

  radio.setDio1Action(setReceivedFlag);

  Serial.println("LoRa initialized successfully.");

  Serial.print("Soil packet size: ");
  Serial.print(sizeof(SoilPacket));
  Serial.println(" bytes");

  Serial.print("ACK packet size: ");
  Serial.print(sizeof(AckPacket));
  Serial.println(" bytes");

  Serial.println();
  Serial.print("Node ID: ");
  Serial.println(NODE_ID);

  Serial.println();
  Serial.println("System ready.");
}

// ============================================================
// Main loop
// ============================================================

void loop()
{
  // ----------------------------------------------------------
  // Increment sequence
  // ----------------------------------------------------------

  sequenceNumber++;

  // Save immediately to NVS.
  //
  // This ensures that after a power failure,
  // the node will not reuse the same sequence number.

  preferences.putUInt(
    "sequence",
    sequenceNumber
  );

  // ----------------------------------------------------------
  // Power sensor ON
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("========================================");
  Serial.println("Starting soil measurement");
  Serial.println("========================================");

  digitalWrite(SENSOR_RELAY, HIGH);

  Serial.println("Sensor power ON.");

  // Give sensor time to start

  delay(3000);

  // ----------------------------------------------------------
  // Sensor variables
  // ----------------------------------------------------------

  float humidity = 0;
  float temperature = 0;
  float conductivity = 0;
  float pH = 0;

  uint16_t nitrogen = 0;
  uint16_t phosphorus = 0;
  uint16_t potassium = 0;

  // ----------------------------------------------------------
  // Read sensor
  // ----------------------------------------------------------

  bool sensorOK = readSoilSensor(
    humidity,
    temperature,
    conductivity,
    pH,
    nitrogen,
    phosphorus,
    potassium
  );

  // ----------------------------------------------------------
  // Sensor OFF
  // ----------------------------------------------------------

  digitalWrite(SENSOR_RELAY, LOW);

  Serial.println("Sensor power OFF.");

  // ----------------------------------------------------------
  // Sensor failure
  // ----------------------------------------------------------

  if (!sensorOK)
  {
    Serial.println();
    Serial.println("Sensor reading failed.");

    goToSleep();
  }

  // ----------------------------------------------------------
  // Display measurement
  // ----------------------------------------------------------

  Serial.println();
  Serial.println("========== SOIL DATA ==========");

  Serial.print("Humidity:      ");
  Serial.print(humidity);
  Serial.println(" %");

  Serial.print("Temperature:   ");
  Serial.print(temperature);
  Serial.println(" C");

  Serial.print("Conductivity:   ");
  Serial.print(conductivity);
  Serial.println(" uS/cm");

  Serial.print("pH:            ");
  Serial.println(pH);

  Serial.print("Nitrogen:      ");
  Serial.println(nitrogen);

  Serial.print("Phosphorus:    ");
  Serial.println(phosphorus);

  Serial.print("Potassium:     ");
  Serial.println(potassium);

  Serial.println("===============================");

  // ----------------------------------------------------------
  // Create packet
  // ----------------------------------------------------------

  SoilPacket packet;

  memset(&packet, 0, sizeof(packet));

  strncpy(
    packet.nodeId,
    NODE_ID,
    sizeof(packet.nodeId) - 1
  );

  packet.sequence = sequenceNumber;

  packet.humidity = humidity;
  packet.temperature = temperature;
  packet.conductivity = conductivity;
  packet.pH = pH;

  packet.nitrogen = nitrogen;
  packet.phosphorus = phosphorus;
  packet.potassium = potassium;

  // ----------------------------------------------------------
  // Send
  // ----------------------------------------------------------

  bool success = sendSoilPacket(packet);

  // ----------------------------------------------------------
  // Result
  // ----------------------------------------------------------

  if (success)
  {
    Serial.println();
    Serial.println("Data was successfully acknowledged.");
  }
  else
  {
    Serial.println();
    Serial.println("Data was not acknowledged.");
  }

  Serial.println();
  Serial.println("Next measurement in 15 minutes...");

  goToSleep();
}
