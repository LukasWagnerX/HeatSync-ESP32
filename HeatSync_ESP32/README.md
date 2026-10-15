# HeatSync-Arduino-Sketch

Öffne `HeatSync_ESP32.ino` in Arduino IDE und lasse alle Header-Dateien in
diesem Ordner. Eine externe Verkabelung zur Heizung ist nicht erforderlich.

- Boardpaket: **esp32 by Espressif Systems 3.3.5**
- Board: **ESP32 Dev Module**, **4 MB Flash**
- Partition: **Huge APP (3MB No OTA/1MB SPIFFS)**
- Bibliotheken: **ArduinoJson 7.4.2**, **TinyXML von Adafruit 1.0.3**
- Serieller Monitor: **115200 Baud**

Nach dem Upload das Board in HeatSync als **HeatSync ESP32** hinzufügen.
WLAN und Kontozuordnung werden in der App eingerichtet, nicht im Quellcode.

Die [vollständige Anleitung](../README.md) enthält Einrichtung, LED-Signale,
Werksreset, Besitzerwechsel und Lösungen für häufige Fehler.
