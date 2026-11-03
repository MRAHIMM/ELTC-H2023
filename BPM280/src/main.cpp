#include <Arduino.h>

void setup()
{
    Serial.begin(115200);

    delay(2000);

    Serial.println();
    Serial.println("================================");
    Serial.println("ESP32 SERIAL TEST");
    Serial.println("The program is running!");
    Serial.println("================================");
}

void loop()
{
    Serial.println("ESP32 is alive...");
    delay(2000);
}