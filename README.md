# HeatSync ESP32

Die WLAN-Brücke zwischen deiner Heizung und der HeatSync-App auf dem iPhone.

Der ESP32 liest die lokale Heizungs-API schonend nacheinander aus und überträgt
Messwerte und Betriebszustände an den HeatSync-Dienst. Dadurch stehen die Daten
auch außerhalb des Heimnetzwerks für Dashboard, Statistiken, Benachrichtigungen,
Widgets und Live Activities zur Verfügung.

**Die Einrichtung erfolgt vollständig in der HeatSync-App über Bluetooth. Du
musst keine WLAN-Passwörter, Serveradressen oder API-Schlüssel im Code eintragen.**

## Inhalt

- [Voraussetzungen](#voraussetzungen)
- [Firmware installieren](#firmware-installieren)
- [In HeatSync einrichten](#in-heatsync-einrichten)
- [Werte, Aktualisierung und bekannte Statuswerte](#werte-aktualisierung-und-bekannte-statuswerte)
- [LED und Tasten](#led-und-tasten)
- [Zurücksetzen und Besitzerwechsel](#zurücksetzen-und-besitzerwechsel)
- [Firmware aktualisieren](#firmware-aktualisieren)
- [Probleme lösen](#probleme-lösen)
- [Sicherheit und Datenschutz](#sicherheit-und-datenschutz)
- [Projektaufbau](#projektaufbau)
- [Kontakt](#kontakt)

## Voraussetzungen

### Hardware

- Ein klassisches **ESP32-DevKit mit ESP32-WROOM-32, WLAN, Bluetooth Low Energy
  und mindestens 4 MB Flash**. Die unten genannten Einstellungen gelten für
  dieses Board.
- Ein passendes **USB-Datenkabel**, nicht nur ein Ladekabel.
- Eine zuverlässige USB-Stromversorgung für den dauerhaften Betrieb.
- Ein 2,4-GHz-WLAN mit Internetzugang, das auch die lokale Heizung erreicht.
- Ein iPhone mit der HeatSync-App.

ESP8266 und ESP32-S2 sind nicht geeignet: Ihnen fehlt das benötigte Bluetooth
Low Energy. ESP32-C3 und ESP32-S3 sind nicht Bestandteil dieser geprüften
Board-Konfiguration. Verwende für diese Anleitung ein klassisches ESP32-DevKit.

**Keine Verkabelung zur Heizung notwendig:** Der ESP32 kommuniziert über das
Netzwerk. Schließe keine Heizungsleitungen, Pumpen oder Netzspannung am Board an.

### Heizung und Konto

Aktuell unterstützt diese Firmware **ETA-Heizungsanlagen mit ETA Touch und
aktivierter lokaler REST-API**. Der verfügbare Wertebaum hängt von der Anlage ab.
Die Heizung und der ESP32 müssen sich gegenseitig im LAN erreichen können;
Gastnetz-Isolation oder getrennte Netze können das verhindern.

Für die ESP32-Anbindung brauchst du ein HeatSync-Konto und **HeatSync Pro**.
Die Hardware ist im Pro-Kauf nicht enthalten. Ohne ESP32 reicht die Gratisversion
für die rein lokale, lesende Anzeige aus; Pro ohne ESP32 bietet Werbefreiheit.
Die zusätzlichen Fernzugriffs- und Hintergrundfunktionen benötigen den
eingerichteten ESP32 sowie eine funktionierende Internetverbindung.

## Firmware installieren

### 1. Arduino IDE installieren

Installiere die [Arduino IDE 2](https://www.arduino.cc/en/software/).

Öffne die Einstellungen und ergänze unter **Zusätzliche Boardverwalter-URLs**:

```text
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

Öffne den Boardverwalter, suche **esp32 by Espressif Systems** und installiere
**Version 3.3.5**. Die [offizielle Espressif-Anleitung](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)
beschreibt die Installation des Boardpakets.

### 2. Quellcode herunterladen

Klicke oben auf dieser GitHub-Seite auf **Code → Download ZIP** und entpacke
das Archiv. Öffne anschließend:

```text
HeatSync_ESP32/HeatSync_ESP32.ino
```

**Wichtig:** Nicht nur die `.ino`-Datei einzeln herunterladen. Alle `.h`-Dateien
müssen im Ordner `HeatSync_ESP32` neben der `.ino`-Datei bleiben. Der Sketchordner
und die Hauptdatei müssen denselben Namen haben.

### 3. Bibliotheken installieren

Installiere über den Bibliotheksverwalter diese Versionen:

| Bibliothek | Version |
| --- | --- |
| ArduinoJson von Benoit Blanchon | **7.4.2** |
| TinyXML von Adafruit | **1.0.3** |

WLAN, Bluetooth, HTTPS, Preferences und SPIFFS sind bereits im ESP32-Boardpaket
enthalten. Zusätzliche Bluetooth-Bibliotheken sind nicht erforderlich.

### 4. Board und Speicher einstellen

Verbinde den ESP32 per USB mit dem Computer. Wähle in der Arduino IDE:

| Einstellung | Auswahl |
| --- | --- |
| Board | **ESP32 Dev Module** |
| Flash Size | **4MB (32Mb)** |
| Partition Scheme | **Huge APP (3MB No OTA/1MB SPIFFS)** |
| Port | Der USB-/COM-Port deines ESP32 |
| Upload Speed | **115200** |
| Erase All Flash Before Sketch Upload | **Disabled** |
| Übrige Einstellungen | Standardwerte des Boards |

Die normale 1,31-MB-Programmpartition ist zu klein. Verwende **Huge APP**.
Diese Speicheraufteilung ist für Updates per USB vorgesehen, nicht für OTA.

### 5. Übertragen und starten

Klicke auf **Überprüfen**, danach auf **Hochladen**. Wenn die IDE dauerhaft
bei `Connecting...` wartet, halte **BOOT** gedrückt, bis die Übertragung beginnt,
und lasse die Taste anschließend los.

Nach erfolgreichem Upload BOOT loslassen und **EN/RESET einmal kurz drücken**.
Im seriellen Monitor bei **115200 Baud** erscheint der Start der HeatSync-Firmware.
Die Status-LED blinkt während der offenen Bluetooth-Einrichtung langsam.

## In HeatSync einrichten

Der Assistent führt dich durch vier Schritte:

1. **ESP32 koppeln:** In HeatSync anmelden, Pro aktivieren und **ESP32 hinzufügen**
   öffnen. **HeatSync ESP32** auswählen. Bluetooth am iPhone muss aktiviert und
   für HeatSync erlaubt sein. Eine eventuelle iOS-Kopplungsbestätigung akzeptieren;
   es ist kein PIN und kein USB-Pairingkey einzutragen.
2. **WLAN verbinden:** Nach Netzwerken suchen, das eigene **2,4-GHz-WLAN**
   auswählen, das WLAN-Passwort eingeben und die Verbindung abwarten.
3. **Mit dem Konto verknüpfen:** Die Zuordnung zu deinem angemeldeten
   HeatSync-Konto bestätigen. Der Assistent überträgt die individuelle,
   signierte Gerätekonfiguration automatisch.
4. **Heizung auswählen:** Die Heizung im LAN suchen und den passenden Treffer
   bestätigen. Falls sie nicht gefunden wird, ihre lokale Adresse aus den
   Netzwerkeinstellungen der Heizung eingeben. Das Format ist beispielsweise
   `http://192.168.1.50:8080`; verwende die tatsächliche Adresse deiner Anlage.

Danach werden der Wertebaum und nach und nach die Messwerte eingelesen. Prüfe
die Zuordnung der Beispielblöcke: Fühler, Namen und vorhandene Komponenten können
bei deiner Anlage anders sein. Wähle die richtigen Werte anhand ihrer
Bezeichnung und ihres aktuellen Werts aus.

**Nach der Einrichtung wird Bluetooth abgeschaltet**, auch nach einem Neustart.
Das ist normal. Die weitere Datenübertragung erfolgt über WLAN. Lass den ESP32
dauerhaft an einer zuverlässigen Stromversorgung angeschlossen.

Ein noch nicht eingerichtetes Board ist nach dem Start **zehn Minuten** sichtbar.
Ist dieses Zeitfenster abgelaufen, starte es neu und öffne die Suche erneut.

## Werte, Aktualisierung und bekannte Statuswerte

- Der ESP32 liest Werte **nacheinander mit Pausen**, nicht die ganze Anlage
  gleichzeitig. Das schont die Heizungssteuerung.
- Verknüpfte Dashboard-, Widget-, Live-Activity- und Benachrichtigungswerte sowie
  aktuell ausgewählte Werte erhalten eine höhere Priorität. Die übrigen Werte
  werden in weiteren Durchläufen gelesen.
- Ein aktueller ESP32-Verbindungsstatus bedeutet nicht, dass jeder einzelne
  Sensor im selben Moment gemessen wurde. Für die Aktualität zählt der
  **Zeitstempel des jeweiligen Werts**.
- Das Dictionary bekannter Textzustände wächst mit tatsächlich beobachteten
  Zuständen, etwa `Bereit`, `Heizen`, `Ein` oder `Aus`. Direkt nach der
  Einrichtung können Einträge fehlen. Sie erscheinen, sobald diese Zustände
  beim Auslesen auftreten. Ein fehlender Eintrag ist kein Beweis dafür, dass
  die Heizung diesen Zustand nicht unterstützt.
- Die Speicherung der übertragenen Messwerte, bekannten Zustände und
  konfigurierten Statistiken erfolgt beim HeatSync-Dienst. Das Board benötigt
  keine SD-Karte.

Netzwerk, Heizungsantworten und Apples Hintergrundregeln beeinflussen die
Aktualisierung. Widgets und Live Activities sind **keine garantierte
sekündliche Echtzeitanzeige**. Benachrichtigungen ersetzen keine technische
Alarm- oder Sicherheitseinrichtung.

## LED und Tasten

Die Statusanzeige verwendet beim klassischen DevKit **GPIO 2**:

| Signal | Bedeutung |
| --- | --- |
| Langsames Blinken | Bluetooth-Einrichtung ist offen |
| Aus | Einrichtung abgeschlossen oder Suchfenster abgelaufen |
| Schnelles Blinken beim Halten von BOOT | Werksreset ist vorgemerkt; BOOT loslassen |
| Wiederholtes Doppelblinken | Reset wartet auf Internet und bestätigte Serverfreigabe |

Nicht jedes DevKit hat dieselbe LED. Bei abweichenden Boards lassen sich
`STATUS_LED_PIN` und `STATUS_LED_ACTIVE_HIGH` oben im Sketch anpassen.
Eine fest verdrahtete Power-LED kann die Firmware nicht ausschalten.

**EN/RESET** startet das Board neu. **BOOT** dient beim laufenden Board dem
Werksreset und während des Uploads dem Start des Bootloaders.

## Zurücksetzen und Besitzerwechsel

### Werksreset am Board

1. Das Board normal starten lassen. BOOT dabei nicht gedrückt halten.
2. **BOOT mindestens fünf Sekunden gedrückt halten.**
3. Sobald die Status-LED schnell blinkt, **BOOT loslassen**.
4. Das bisherige WLAN, den Internetzugang und die Stromversorgung verfügbar lassen.
5. Die Firmware lässt die Kontozuordnung serverseitig freigeben, löscht danach
   die lokalen Einstellungen und startet im Einrichtungsmodus neu.

Dabei werden WLAN-Zugang, Heizungsadresse, Gerätezugang, Bluetooth-Kopplungen und
lokale Zwischenspeicher entfernt. Der bisherige Besitzer behält sein privates
Heizungsprofil und dessen Statistiken; ein neuer Besitzer erhält keinen Zugriff
darauf. Die bisherige Geräteberechtigung wird widerrufen.

Bei fehlendem Internet **wartet der Reset auf die Freigabe** und übersteht auch
einen Neustart. Währenddessen werden keine Heizungswerte gelesen und keine
Heizungsbefehle ausgeführt. Das bisherige WLAN bleibt ausschließlich für die
Verbindung zur Freigabe erhalten. Doppelblinken zeigt diesen Wartezustand an.
Ein noch nicht mit einem Konto verbundenes Board kann sofort lokal zurückgesetzt
werden.

### Reset in der App und Austausch des Boards

Nur der Besitzer kann einen Reset über sein Konto anfordern. Ein offline
befindlicher ESP32 führt ihn nach der nächsten Internetverbindung aus.

Beim Verbinden eines Ersatz-ESP32 bietet die App an, ein vorhandenes Heizungsprofil
zu **übernehmen** oder **neu anzufangen**. Übernehmen erhält Einstellungen und
Statistiken und widerruft den Zugang des vorherigen Boards. Neu anfangen erzeugt
ein separates Profil.

**Achtung:** BOOT beim Einschalten oder beim Drücken von EN gedrückt zu halten,
startet den ROM-Bootloader, keinen Werksreset. In diesem Fall BOOT loslassen
und EN einmal kurz drücken. Ein normaler Firmware-Upload löscht die gespeicherte
Einrichtung üblicherweise nicht. Vollständiges Löschen des Flashs ersetzt keine
serverseitige Besitzerfreigabe.

## Firmware aktualisieren

1. Den aktuellen Quellcode mit **Code → Download ZIP** herunterladen und entpacken.
2. Die neue `HeatSync_ESP32.ino` mitsamt ihren Header-Dateien öffnen.
3. Die oben genannten Board-, Bibliotheks- und Partitionseinstellungen verwenden.
4. **Erase All Flash Before Sketch Upload deaktiviert lassen.**
5. Über USB hochladen und das Board neu starten.

Bei einem normalen Upload mit unveränderter Speicheraufteilung bleiben
WLAN- und Geräteeinstellungen erhalten. Ein Wechsel der Partitionstabelle oder
ein vollständiges Flash-Löschen kann diese Daten entfernen. Für einen geplanten
Besitzerwechsel immer zuerst den vorgesehenen Werksreset durchführen.

## Probleme lösen

| Problem | Lösung |
| --- | --- |
| `Sketch too big` | **Huge APP (3MB No OTA/1MB SPIFFS)** und **4MB Flash** auswählen. |
| `ArduinoJson.h` oder `TinyXML.h` fehlt | Die oben angegebenen Bibliotheken über den Bibliotheksverwalter installieren. |
| Ein lokaler Header fehlt | Ganzes ZIP entpacken; alle `.h`-Dateien neben der `.ino` belassen. |
| Kein USB-/COM-Port | USB-Datenkabel und anderen USB-Port versuchen. Falls nötig den zum USB-Chip passenden Treiber vom Boardhersteller installieren. |
| Upload bleibt bei `Connecting...` | BOOT bis zum Übertragungsbeginn halten, dann loslassen. Seriellen Monitor vorher schließen. |
| Board startet nach Upload nicht | BOOT loslassen, EN kurz drücken und Monitor mit 115200 Baud öffnen. |
| ESP32 wird nicht gefunden | Bluetooth-Berechtigung prüfen, Board in der Nähe halten, nach abgelaufenem Zehn-Minuten-Fenster neu starten. Bereits eingerichtete Boards senden kein Einrichtungssignal. |
| Nur ein altes ETA-Dashboard erscheint | Dieses ist ein anderes Gerät bzw. eine andere Firmware. Das richtige Board mit der vollständigen HeatSync-Firmware bespielen. |
| WLAN-Verbindung schlägt fehl | 2,4-GHz-Netz, Passwort, Signalstärke und erreichbaren Internetzugang prüfen; kein Anmeldeportal verwenden. |
| Heizung wird nicht gefunden | Lokale API aktivieren, Adresse und Port prüfen, Gastnetz-/Client-Isolation ausschließen und die Adresse manuell eintragen. Die Suche setzt ein übliches IPv4-/24-Heimnetz voraus. |
| Gerät online, einzelne Werte fehlen | Ersten Werteumlauf abwarten, Fühlerzuordnung und jeweiligen Zeitstempel prüfen. Nicht alle Werte werden gleichzeitig gelesen. |
| Status fehlt im Dictionary | Den tatsächlichen Zustand abwarten; bekannte Texte werden beim Auslesen gesammelt. |
| Reset bleibt im Doppelblinken | Bisheriges WLAN und Internet wieder verfügbar machen; Serverfreigabe abwarten. |
| HTTPS-/Zeitfehler | Internetzugang einschließlich DNS und Zeitsynchronisierung prüfen. Zertifikatsprüfung nicht deaktivieren. |

Für die Fehlersuche: Boardtyp, installierte Versionen, Fehlermeldung und
anonymisierten Auszug aus dem seriellen Monitor notieren. Niemals WLAN-Passwörter,
Gerätetoken oder persönliche Heizungsdaten öffentlich posten.

## Sicherheit und Datenschutz

Die Firmware enthält **keine feste HeatSync-Serveradresse und keine gemeinsamen
Dienstzugangsdaten**. Erst die App überträgt über verschlüsseltes Bluetooth eine
individuelle, zeitlich begrenzte und signierte Einrichtungskonfiguration. Sie
enthält die Dienstadresse und den widerrufbaren Gerätezugang.

Die enthaltenen Zertifikate und der öffentliche Prüfschlüssel sind absichtlich
öffentlich: Sie prüfen HTTPS-Verbindungen und signierte Konfigurationen und
ermöglichen keine Anmeldung als anderer Benutzer. Private Signierschlüssel,
Apple-Push-Schlüssel, Firebase- und Zahlungsdienst-Zugangsdaten gehören nicht
auf das Board und sind nicht Bestandteil dieses Repositorys.

Bluetooth verwendet verschlüsselte Secure Connections mit **Just Works**, ohne
PIN-basierten Schutz vor einem aktiven Angreifer während der Kopplung. Richte das
Board deshalb in einer vertrauenswürdigen Umgebung ein. Nach der Einrichtung
bleibt Bluetooth deaktiviert. Die Verbindung zum Dienst verwendet HTTPS mit
Zertifikatsprüfung; die lokale Heizungs-API wird im Heimnetz über HTTP gelesen.

Standard-Arduino-Preferences/NVS sind **nicht verschlüsselt**. Physischer Zugriff
auf den Flash kann gespeicherte WLAN- und Gerätezugangsdaten offenlegen. Der
Quellcode ist kein Nachweis für die Herkunft eines gekauften Boards. Die
Dienstadresse ist kein Geheimnis und kann von einem eingerichteten Gerät durch
eine technisch versierte Person ermittelt werden.

### Schalten der Heizung

Die normale Erfassung ist lesend. Zusätzlich unterstützt die Firmware explizite
Ein-/Aus-Befehle vom HeatSync-Dienst für den Besitzer oder berechtigte
Familienmitglieder mit Pro. Vor einem Schreibzugriff werden die
Schreibberechtigung und die binären Ein-/Aus-Werte der Heizungs-API geprüft.
Es gibt keine anonyme lokale Schaltschnittstelle und keine automatische Änderung
von Temperaturen oder Heizungsparametern. Die lokale Gratisnutzung der App ist
ausschließlich lesend.

HeatSync ersetzt keine Heizungssteuerung, Wartung, Herstellerfreigabe oder
Sicherheitseinrichtung. Bei Störungen gelten die Hinweise des Heizungsherstellers.
HeatSync ist ein unabhängiges Projekt und kein offizielles Produkt von ETA.

## Projektaufbau

```text
HeatSync_ESP32/
  HeatSync_ESP32.ino       Start, Bluetooth-Einrichtung, Wertebaum und Leseumlauf
  CloudTransport.h        HTTPS und Prüfung signierter Gerätekonfigurationen
  HeatSyncCA.h            Öffentliche HTTPS-Vertrauensanker
  ProvisioningPublicKey.h Öffentlicher Prüfschlüssel für die Einrichtung
  HeatSyncControls.h      Prüfung und Ausführung berechtigter Ein-/Aus-Befehle
  ReadingScheduler.h      Priorisierung der Heizungsabfragen
  LoopHealth.h            Überwachung des laufenden Leseumlaufs
  FactoryResetButton.h    Entprellung und Erkennung des langen BOOT-Tastendrucks
  StatusLED.h             LED-Signale
```

Die iPhone-App und der HeatSync-Server sind nicht Bestandteil dieses Repositorys.

### Mit Arduino CLI kompilieren

Alternativ zur IDE kann [Arduino CLI](https://arduino.github.io/arduino-cli/)
verwendet werden. Im entpackten Repository ausführen:

```sh
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.5 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install "ArduinoJson@7.4.2" "TinyXML@1.0.3"
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app,FlashSize=4M HeatSync_ESP32
```

Das Kompilieren überträgt nichts auf ein angeschlossenes Board.

## Kontakt

**Lukas Wagner** · [wagnerlukas@icloud.com](mailto:wagnerlukas@icloud.com)

[Datenschutz](https://wagnerlukas.at/heatsync/datenschutz.php) ·
[Impressum](https://wagnerlukas.at/heatsync/impressum.php) ·
[Nutzungsbedingungen](https://wagnerlukas.at/heatsync/nutzung.php)

Copyright © 2026 Lukas Wagner.
