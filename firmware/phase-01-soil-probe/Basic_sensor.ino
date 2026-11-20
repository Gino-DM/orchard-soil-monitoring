#include <Arduino.h>

#define RS485_RX     19
#define RS485_TX     20
#define RS485_RE_DE  21

HardwareSerial RS485Serial(1);

// Read 7 registers:
// 0000 Humidity
// 0001 Temperature
// 0002 Conductivity
// 0003 pH
// 0004 Nitrogen
// 0005 Phosphorus
// 0006 Potassium
//
// CRC = 0x0804
// Modbus sends CRC low byte first: 04 08
uint8_t byteRequest[8] = {
  0x01,
  0x03,
  0x00,
  0x00,
  0x00,
  0x07,
  0x04,
  0x08
};

uint8_t byteResponse[32];

void setup()
{
  Serial.begin(9600);

  pinMode(RS485_RE_DE, OUTPUT);

  // Start in receive mode
  digitalWrite(RS485_RE_DE, LOW);

  // Sensor communication:
  // 4800 baud, 8 data bits, no parity, 1 stop bit
  RS485Serial.begin(
    4800,
    SERIAL_8N1,
    RS485_RX,
    RS485_TX
  );

  Serial.println();
  Serial.println("7-in-1 Soil Sensor RS485 Test");
  Serial.println("--------------------------------");
}

void loop()
{
  // Clear any old data
  while (RS485Serial.available())
  {
    RS485Serial.read();
  }

  // -----------------------------
  // TRANSMIT MODE
  // -----------------------------
  digitalWrite(RS485_RE_DE, HIGH);

  delayMicroseconds(100);

  Serial.println("Sending Modbus request...");

  RS485Serial.write(
    byteRequest,
    sizeof(byteRequest)
  );

  // Wait until the complete request
  // has been transmitted
  RS485Serial.flush();

  // -----------------------------
  // RECEIVE MODE
  // -----------------------------
  digitalWrite(RS485_RE_DE, LOW);

  delayMicroseconds(100);

  Serial.println("Waiting for sensor...");

  // -----------------------------
  // READ RESPONSE
  // -----------------------------
  unsigned long startTime = millis();

  int responseLength = 0;

  while (millis() - startTime < 1000)
  {
    if (RS485Serial.available())
    {
      if (responseLength < sizeof(byteResponse))
      {
        byteResponse[responseLength] =
          RS485Serial.read();

        responseLength++;
      }
    }
  }

  // -----------------------------
  // DISPLAY RAW RESPONSE
  // -----------------------------
  if (responseLength > 0)
  {
    Serial.print("Received ");
    Serial.print(responseLength);
    Serial.println(" bytes:");

    for (int i = 0; i < responseLength; i++)
    {
      if (byteResponse[i] < 0x10)
      {
        Serial.print("0");
      }

      Serial.print(byteResponse[i], HEX);
      Serial.print(" ");
    }

    Serial.println();

    // --------------------------------
    // EXPECTED RESPONSE
    // --------------------------------
    //
    // Byte 0 = Device address
    // Byte 1 = Function code
    // Byte 2 = Number of data bytes
    //
    // Bytes 3-4  = Humidity
    // Bytes 5-6  = Temperature
    // Bytes 7-8  = Conductivity
    // Bytes 9-10 = pH
    // Bytes 11-12 = Nitrogen
    // Bytes 13-14 = Phosphorus
    // Bytes 15-16 = Potassium
    //
    // Bytes 17-18 = CRC

// -----------------------------
// FIND VALID MODBUS FRAME
// -----------------------------

int frameStart = -1;

for (int i = 0; i <= responseLength - 19; i++)
{
  if (byteResponse[i] == 0x01 &&
      byteResponse[i + 1] == 0x03 &&
      byteResponse[i + 2] == 0x0E)
  {
    frameStart = i;
    break;
  }
}

if (frameStart >= 0)
{
  Serial.println();
  Serial.println("Valid Modbus frame found!");

  // -----------------------------
  // CONVERT RAW VALUES
  // -----------------------------

  uint16_t humidity =
    ((uint16_t)byteResponse[frameStart + 3] << 8) |
     byteResponse[frameStart + 4];

  int16_t temperature =
    ((int16_t)byteResponse[frameStart + 5] << 8) |
     byteResponse[frameStart + 6];

  uint16_t conductivity =
    ((uint16_t)byteResponse[frameStart + 7] << 8) |
     byteResponse[frameStart + 8];

  uint16_t ph =
    ((uint16_t)byteResponse[frameStart + 9] << 8) |
     byteResponse[frameStart + 10];

  uint16_t nitrogen =
    ((uint16_t)byteResponse[frameStart + 11] << 8) |
     byteResponse[frameStart + 12];

  uint16_t phosphorus =
    ((uint16_t)byteResponse[frameStart + 13] << 8) |
     byteResponse[frameStart + 14];

  uint16_t potassium =
    ((uint16_t)byteResponse[frameStart + 15] << 8) |
     byteResponse[frameStart + 16];

  // -----------------------------
  // DISPLAY SENSOR VALUES
  // -----------------------------

  Serial.println();
  Serial.println("Sensor readings:");
  Serial.println("-----------------------------");

  Serial.print("Humidity:       ");
  Serial.print(humidity / 10.0);
  Serial.println(" %RH");

  Serial.print("Temperature:    ");
  Serial.print(temperature / 10.0);
  Serial.println(" C");

  Serial.print("Conductivity:   ");
  Serial.print(conductivity);
  Serial.println(" uS/cm");

  Serial.print("pH:             ");
  Serial.print(ph / 10.0);
  Serial.println();

  Serial.print("Nitrogen:       ");
  Serial.println(nitrogen);

  Serial.print("Phosphorus:     ");
  Serial.println(phosphorus);

  Serial.print("Potassium:      ");
  Serial.println(potassium);

  Serial.println("-----------------------------");
}
else
{
  Serial.println();
  Serial.println("No valid Modbus frame found.");
 
}
  }
  delay(2000);
}
