# Próba Feberis Pro — 2026-11-22

Połączenie: USB–UART Flippera Zero, automatyczny download/reset przez DTR/RTS.
Układ odczytany przez esptool: ESP32-C5 rev. 1.0, kwarc 48 MHz, flash 8 MB.
ESP-IDF 6.0, firmware 0.1.0, kraj PL, UART 115200 8N1, flash DIO 80 MHz.
Lokalny `sdkconfig` ustawiono na rzeczywiste 8 MB; tablica partycji pozostaje single-app.

Wgrano bootloader, tablicę partycji i aplikację; po poprawce Start wgrano ponownie
aplikację. esptool potwierdził hash każdego zapisanego obrazu. Flashowanie działało
przy 115200 oraz 460800 baud. SHA-256 końcowego pliku `build/feberis_wardriver.bin`:

```text
20b83a0379cc0105151d7e8c6205a80ef424edf4aa48c198ff882ce34e5a1839
```

## Wyniki

- Kompilacja firmware oraz 2/2 testów hosta z ASan/UBSan: poprawne.
- Reset przez RTS, READY, HELLO, PING, STATUS, Start i Stop: poprawne.
- Błędny CRC nie uruchamia skanu; zbyt mały interwał zwraca BAD_ARGUMENT.
- Zmiana pasma podczas sesji zwraca BUSY.
- 2,4 GHz: 7 rekordów, kanały 1, 5, 6, 11.
- 5 GHz: 4 rekordy, kanał 44.
- AUTO: 11 rekordów, kanały 6, 10, 44.
- Wszystkie odebrane ramki FBR1 miały prawidłowy CRC; liczby rekordów zgadzały się
  ze SCAN_DONE. Potwierdzono 22 rekordy z trzech pełnych skanów.
- Pierwszy AP w każdym trybie celowo pozostawiono bez ACK: retransmisja była
  identyczna bajtowo, po ACK transmisja była kontynuowana.
- Przy całkowitym braku ACK: cztery identyczne transmisje AP, potem ACK_TIMEOUT,
  STOPPED i powrót do IDLE. Anulowano 9 rekordów z ostatniej partii.
- Przez cały końcowy test identyfikator boot był stały; brak nieoczekiwanego restartu.
- Po teście wysłano STOP i przywrócono pasmo AUTO; port zamknięto.

Znaleziono i poprawiono błąd: nowa sesja po Stop dziedziczyła termin kolejnego skanu
z poprzedniej sesji. Start nowej sesji ustawia teraz termin na bieżący czas.
Test używa interwału 60000 ms i wymaga SCAN_BEGIN w ciągu 2 s po każdym Start,
więc sprawdza tę regresję na urządzeniu.

## Ograniczenia

Sterowniki Wi-Fi i GPS zgłaszały gotowość. GPS przez około 31 s końcowego testu
nie zgłosił fixa ani satelitów. Wszystkie AP miały fix_valid=0 i csv_hex=-,
zgodnie z kontraktem; licznik rows pozostał zerowy. Gotowość sterownika nie
potwierdza odbioru NMEA, prawidłowego baudrate ani sprawności odbiornika GPS.
Do sprawdzenia pozostają fix na zewnątrz, utrata fixa i CSV z rzeczywistą pozycją.

Nie sprawdzono wizualnie kolorów LED, ukrytych/specjalnych SSID, limitu 128 AP,
długiej sesji, fizycznego rozłączania, aplikacji Flippera, karty SD ani importu WiGLE.

Powtarzalny test: `tools/hardware_test.py` (Python 3.10+, pyserial).
Wynik maszynowy: `artifacts/hardware/2026-11-22.json`; bez SSID i BSSID.
