# Wgranie i weryfikacja PSRAM — 2026-11-23

Na polecenie użytkownika wgrano firmware 0.3.0 przez USB–UART Flippera Zero,
port `/dev/cu.usbmodemflip_Tutel1`. Esptool wykrył ESP32-C5 rev. 1.0 i kwarc 48 MHz.
Flashowanie przy 460800 baud: bootloader 0x2000, tablica partycji 0x8000,
aplikacja 0x10000, flash 8 MB/DIO/80 MHz. Hash każdego zapisanego obrazu potwierdzony.
Nie kasowano całego flash ani NVS. SHA-256 pliku aplikacji:
`ecf13cd55b7c7cc1434d86a519e5284f73ccbf20bb33763400f73863484cf904`.

Po restarcie READY potwierdziło 0.3.0. Zapytania odczytowe GET_INFO, GET_MEMORY,
STATUS i PING potwierdziły:

- PSRAM zainicjalizowana, bufor AP gotowy.
- PSRAM: **8 388 608 B**; wolne 3 070 748 B, największy blok 3 014 656 B.
- Bufory aplikacji w PSRAM: **5 304 064 B**.
- Dostępne **10 000 AP** i kolejka **2048 obserwacji**.
- Wewnętrzna SRAM: wolne 195 424 B, minimum 195 184 B, największy blok 172 032 B.
- UART 115200, protokół revision 2, PING poprawny, oba sterowniki gotowe.
- W logu startu zaobserwowano poprawny fix GPS: 11 satelitów, HDOP 1,50.
  Współrzędnych nie zapisano w raporcie.
- Po sprawdzeniu urządzenie pozostało bez aktywnej sesji, kolejka pusta.

Pierwsze zapytanie po otwarciu portu nie otrzymało odpowiedzi. Po odczycie logu
startu potwierdzono działający firmware. Narzędzie device_info.py ponawia teraz
idempotentne zapytania odczytowe co sekundę przez maksymalnie 12 s, aby obsłużyć
start urządzenia i przejściowy brak gotowości UART po otwarciu mostka.

Nie wykonywano w tej próbie skanów RF, zbierania 10 000 fizycznych AP, prób SD,
importu WiGLE ani jazdy testowej. Te wyniki potwierdzają rzeczywistą pojemność
PSRAM i start firmware, nie pełne testy obciążeniowe systemu.

Odczyt bez uruchamiania skanów:

```sh
python tools/device_info.py /dev/cu.usbmodemflip_Tutel1
```

Wymagany pyserial (dostępny w użytym środowisku ESP-IDF).
Raport maszynowy: [2026-11-23-psram.json](../artifacts/hardware/2026-11-23-psram.json).
