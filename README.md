# Feberis Pro Wardriver

**Feberis Pro** to firmware urządzenia do pasywnego wykrywania sieci Wi-Fi
2,4 GHz i 5 GHz oraz rejestrowania ich parametrów wraz z pozycją GNSS.
Urządzeniem steruje Flipper Zero przez UART. ESP32 wykonuje skanowanie,
deduplikację BSSID i przygotowuje obserwacje do zapisania przez aplikację Flippera.

Aktualna wersja: **0.3.1**. Docelowy moduł: **ESP32-C5-WROOM-1U-N8R8**,
LCSC **C51950748**, z 8 MiB flash i 8 MiB PSRAM.

## Możliwości

- Pasywne skany Wi-Fi 2,4 GHz, 5 GHz lub obu pasm; uwzględnianie ukrytych SSID.
- Trzy tryby zapisu: **SMART**, **FULL** i **UNIQUE**.
- Identyfikacja AP po BSSID, agregaty RSSI i wykrywanie zmian parametrów sieci.
- GPS/GNSS w niezależnym zadaniu: NMEA RMC/GGA, UTC, pozycja, wysokość,
  prędkość, kierunek, satelity, HDOP oraz kontrola świeżości i spójności danych.
- Domyślnie **10 000 AP** i kolejka **2048 obserwacji** w PSRAM.
- Protokół **FBR1 revision 2**: CRC-16, sekwencje, ACK, retransmisje i wznowienie
  transmisji po utracie połączenia.
- Konfiguracja w NVS, statystyki sesji i diagnostyka SRAM/PSRAM przez UART.
- Przygotowywanie wierszy WiGLE CSV; brand w nagłówku eksportu: **Sapsan**.

| Tryb | Zastosowanie |
| --- | --- |
| SMART | Zapis pierwszego wykrycia, ruchu, zmiany RSSI/metadanych, upływu czasu i uzupełnienia GPS. |
| FULL | Wszystkie wyniki lub próbkowanie z konfigurowalnym odstępem. |
| UNIQUE | Jeden zapis BSSID na sesję przy dalszej aktualizacji agregatów. |

## Podział systemu

```text
Wi-Fi 2,4/5 GHz ──skan──> ESP32-C5 <──UART1/NMEA── GPS/GNSS
                              │
                         UART0 / FBR1
                              │
                         Flipper Zero
                              │
                       aplikacja FAP → microSD
```

To repozytorium zawiera **firmware ESP32-C5, testy i narzędzia PC**.
Aplikacja FAP, jej GUI i zapis na microSD są rozwijane w osobnym projekcie.
Wytyczne dla tego projektu znajdują się w
[FLIPPER_INTEGRATION.md](docs/FLIPPER_INTEGRATION.md).

## Szybki start

Wymagane: **ESP-IDF 6.0.0**, narzędzia dla ESP32-C5 i **CMake ≥3.22**.
Pobierz repozytorium i przejdź do jego katalogu:

```sh
git clone https://github.com/bpmcircuits/feberis_pro_wardriver.git
cd feberis_pro_wardriver
```

Po aktywowaniu środowiska ESP-IDF:

```sh
idf.py set-target esp32c5
idf.py menuconfig
idf.py build
```

`set-target` wykonaj przy pierwszej konfiguracji; może odtworzyć lokalny `sdkconfig`.
W `menuconfig` sprawdź GPIO, kraj używania radia i prędkość GNSS.
Przykładowy zapis przez USB–UART Flippera:

```sh
idf.py -p /dev/cu.usbmodemflip_Tutel1 -b 460800 flash
```

Podaj port właściwy dla swojego komputera. Podczas programowania aplikacja FAP
musi zwolnić UART. Szczegóły połączeń, PSRAM i konfiguracji:
[DOKUMENTACJA.md](docs/DOKUMENTACJA.md).

## Testy

Testy hosta wymagają kompilatora C, CMake i Pythona 3.10+; nie wymagają urządzenia:

```sh
cmake -S tests -B build-host -DFEB_SANITIZERS=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

W wersji 0.3.1 przeszło **7/7 zestawów testów hosta z ASan/UBSan**,
**15/15 scenariuszy sprzętowych** oraz próba stabilności pamięci przez 12 cykli.
Sprawdzono oba pasma, tryby zapisu, GPS, retransmisje, wznowienie, restart i trzy
prędkości UART. [Raport testów](docs/HARDWARE_TEST_2026-11-23_ACCEPTANCE.md)
oddziela wyniki rzeczywistego urządzenia od symulacji.

## Dokumentacja

- [Podręcznik techniczny i instrukcja obsługi](docs/DOKUMENTACJA.md)
- [Protokół UART i integracja aplikacji Flippera](docs/FLIPPER_INTEGRATION.md)
- [Architektura, model danych i algorytm SMART](docs/ARCHITECTURE.md)
- [Macierz testów i walidacja](docs/VALIDATION.md)
- [Przykładowe pliki wynikowe](docs/examples)

## Ograniczenia

Pozycja jest przypisywana przy zakończeniu skanu, więc podczas ruchu występuje
błąd związany z czasem skanowania kanałów. PSRAM jest ulotna: restart usuwa
niepotwierdzoną kolejkę i agregaty. Przekroczenie limitu AP jest jawnie zgłaszane.
Wyniki bez prawidłowej lokalizacji nie są eksportowane do WiGLE.

Rzeczywisty zapis microSD/FAP, jazda z GNSS, wielogodzinna praca i import na serwerze
WiGLE wymagają dalszych prób. Repozytorium nie zawiera implementacji aplikacji FAP.
