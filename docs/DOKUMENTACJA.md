# Feberis Pro — dokumentacja techniczna i instrukcja obsługi

Dokument dotyczy firmware **0.3.1** i protokołu **FBR1 revision 2**.
Punktem wejścia do projektu jest [README](../README.md).
Pełne definicje pól i komunikatów są w
[FLIPPER_INTEGRATION.md](FLIPPER_INTEGRATION.md).

## 1. Przeznaczenie i zakres

Feberis Pro wykrywa pobliskie AP Wi-Fi i łączy pomiary radiowe z aktualnymi danymi
GNSS. Kluczem sieci jest BSSID. Tożsamość SSID nie powoduje połączenia różnych AP,
a ponowne użycie BSSID nie stanowi dowodu stałej fizycznej tożsamości urządzenia.

ESP32-C5 odpowiada za radio, GPS, deduplikację, agregaty, konfigurację i transport.
Flipper Zero steruje sesjami, prezentuje dane i ma zapisywać wyniki na microSD.
Kod FAP jest w osobnym projekcie; znajdujący się tutaj kontrakt opisuje wymagane
zachowanie aplikacji, a nie gotową implementację GUI i Storage API.

## 2. Sprzęt i połączenia

Moduł: **ESP32-C5-WROOM-1U-N8R8**, LCSC **C51950748**.
Pamięć modułu: 8 MiB flash i 8 MiB Quad PSRAM. GNSS używany w projekcie:
ATGM336H-6N-74, NMEA przez UART. Sygnalizacja: pojedyncza WS2812B.

| Sygnał | GPIO ESP32-C5 | Domyślne parametry |
| --- | --- | --- |
| ESP TX → Flipper RX | 11 | UART0, 115200, 8N1 |
| ESP RX ← Flipper TX | 12 | UART0, 115200, 8N1 |
| ESP RX ← GNSS TX | 14 | UART1, 115200, 8N1 |
| ESP TX → GNSS RX | 13 | UART1, 115200, 8N1 |
| WS2812B DIN | 27 | RMT, GRB |

Wspólna masa, UART 3,3 V. Numery GPIO odnoszą się do ESP32, nie do numerów pinów
złącza Flippera. Piny są konfigurowalne w `idf.py menuconfig` → **Feberis Pro**.
Muszą być różne. **GPIO15 jest zajęte przez PSRAM**; kompilacja odrzuca przypisanie
tego GPIO do UART lub LED. Inne zmiany wymagają sprawdzenia schematu płytki.

GPIO13/14 współdzielą natywne USB ESP32-C5. Przy wskazanym połączeniu GNSS
programowanie odbywa się przez UART0, w tym przez USB–UART Flippera z DTR/RTS
sterującymi download/reset. Aplikacja FAP musi wtedy zwolnić port.
Firmware wyłącza zwykłą konsolę i logi bootloadera; komunikaty ROM/panic mogą
nadal pojawić się na UART. Odbiornik ignoruje tekst bez poprawnej ramki i CRC.

Firmware nie zmienia baudrate ani zestawu zdań odbiornika GNSS. Moduł powinien
już nadawać RMC oraz GGA z prędkością ustawioną w konfiguracji.

## 3. Struktura repozytorium

| Ścieżka | Rola |
| --- | --- |
| `main/app_main.c` | Koordynacja komend, sesji, skanów i transmisji UART0. |
| `main/ap_memory.c` | Jednorazowa, ograniczona alokacja buforów w PSRAM. |
| `main/gps_service.c` | Odbiór UART1 i parser GNSS w osobnym zadaniu. |
| `main/status_led.c` | Sterowanie WS2812B przez RMT. |
| `main/core/wardrive.*` | AP, obserwacje, tryby, agregaty i kolejka. |
| `main/core/nmea.*` | Walidacja NMEA, synchronizacja zdań i snapshot fixa. |
| `main/core/wire.*`, `delivery.*` | Ramkowanie, CRC, ACK i retry. |
| `main/core/monotonic.h` | Bezpieczne porównania czasu monotonicznego. |
| `main/core/wigle.*` | Generowanie wierszy i szablonu nagłówka WiGLE. |
| `tests/` | Testy C/Python, mocki allocatorów i przypadki graniczne. |
| `tools/` | Encoder protokołu, diagnostyka i testy fizycznego urządzenia. |
| `docs/` | Architektura, kontrakt FAP, instrukcje i raporty. |
| `artifacts/` | Raporty testów JSON, bez danych wykrytych sieci i pozycji. |

`build/`, `build-host/`, lokalny `sdkconfig`, cache Pythona i ustawienia edytora
są pomijane przez Git. Wspólne ustawienia sprzętu są w `sdkconfig.defaults`.

## 4. Budowanie i wgrywanie

Wymagane: **ESP-IDF 6.0.0**, toolchain ESP32-C5 i **CMake ≥3.22**.
Nie ma dodatkowych bibliotek firmware spoza ESP-IDF. Zainstaluj narzędzia SDK,
a następnie aktywuj jego środowisko:

```sh
. /sciezka/do/esp-idf/export.sh
idf.py set-target esp32c5
idf.py menuconfig
idf.py build
```

`set-target` stosuj przy pierwszej konfiguracji lub zmianie układu; może odtworzyć
lokalny `sdkconfig`. Sprawdź w menu konfigurację modułu N8R8, flash 8 MiB,
Quad PSRAM 40 MHz, GPIO i kraj używania radia. Testowany obraz używa flash DIO
80 MHz; poprawne ustawienia dla bieżącej kompilacji są w `build/flasher_args.json`.

Budowanie tworzy:

```text
build/feberis_wardriver.bin
build/bootloader/bootloader.bin
build/partition_table/partition-table.bin
build/flasher_args.json
```

Sprawdzony przykład dla USB–UART Flippera w macOS:

```sh
idf.py -p /dev/cu.usbmodemflip_Tutel1 -b 460800 flash
```

Zastąp port aktualnym portem mostka. Nie uruchamiaj równocześnie FAP ani monitora
szeregowego. Używaj `idf.py flash` lub offsetów wygenerowanych przez daną kompilację;
sam obraz aplikacji nie zastępuje bootloadera i tablicy partycji. Zapis nie wymaga
kasowania całej pamięci flash i NVS.

Nie ma tutaj manifestu ani toolchaina aplikacji FAP. Instrukcję budowania tej
aplikacji należy utrzymywać w jej osobnym repozytorium.

## 5. Konfiguracja

### Ustawienia kompilacji

`idf.py menuconfig` → **Feberis Pro**:

| Ustawienie | Domyślne | Znaczenie |
| --- | --- | --- |
| `FEB_MAX_APS` | 128 | Maksymalna liczba pobranych wyników jednego skanu; 16..512. |
| `FEB_AP_CAPACITY` | 10000 | Różne BSSID w jednej sesji; 64..12000. |
| `FEB_OBSERVATION_CAPACITY` | 2048 | Oczekujące obserwacje w PSRAM; 32..8192. |
| `FEB_COUNTRY` | PL | Regulacyjny wybór kanałów dla miejsca używania. |
| `FEB_FLIPPER_BAUD` | 115200 | Początkowy baud UART0. |
| `FEB_GPS_BAUD` | 115200 | Baud odbiornika GNSS, który musi być zgodnie ustawiony. |

Limit 128 wyników skanu i limit 10 000 AP sesji są niezależne. ESP-IDF zwraca
wyniki uporządkowane według RSSI; pominięcia ponad limit partii zwiększają
`truncated`. Pełna tabela sesji wywołuje `AP_TABLE_FULL` i zatrzymanie sesji.

PSRAM jest inicjalizowana i testowana przy starcie. Wybrano
`SPIRAM_USE_CAPS_ALLOC`; duże bufory korzystają jawnie z `MALLOC_CAP_SPIRAM`.
Zwykłe alokacje, bufory UART/GPS i stosy pozostają w pamięci wewnętrznej.
Przy braku PSRAM lub błędzie alokacji diagnostyka UART pozostaje dostępna,
a `START_SCAN` zwraca `AP_MEMORY_UNAVAILABLE`. Nie ma cichej redukcji limitu AP.
Awaria sprzętowego testu pamięci może zatrzymać uruchamianie w ESP-IDF.

### Ustawienia w NVS

`GET_CONFIG` zwraca, a `SET_CONFIG` przyjmuje pełne 13 pól:

```text
version mode band active dwell_ms min_distance_m rssi_delta_db max_gap_ms
min_gap_ms gps_max_age_ms gps_max_hdop_x10 full_gap_ms baud
```

Wartości początkowe:

```text
1 0 0 0 120 15 6 30000 2000 3000 50 2000 115200
```

`mode`: 0 SMART, 1 FULL, 2 UNIQUE. `band`: 0 BOTH, 1 2,4 GHz, 2 5 GHz.
`active=0` oznacza pasywne skanowanie; tryb aktywny wymaga jawnej zmiany.
HDOP jest podawane jako wartość ×10. Obsługiwane baudrates: 115200, 460800 i 921600.

Zmiany wykonuj pomiędzy partiami: `PAUSE_SCAN`, oczekiwanie na zakończenie partii,
`SET_CONFIG`, `RESUME_SCAN`. Zmiana trybu zachowuje baseline i statystyki.
Baud można zmienić poza sesją, z pustą kolejką. Odpowiedź OK przychodzi ze starą
prędkością; potem odbiornik przełącza UART i ponawia HELLO/PING w razie timeoutu.
Zakresy i dokładny przebieg opisuje [kontrakt protokołu](FLIPPER_INTEGRATION.md).

## 6. Sesja i tryby pracy

Po restarcie radio czeka na `START_SCAN`. GNSS działa także bez aktywnej sesji.
Aplikacja Flippera przygotowuje miejsce zapisu, uzgadnia `HELLO 2`, odczytuje
`GET_INFO`, `GET_MEMORY`, `GET_CONFIG`, a następnie uruchamia sesję.

| Operacja | Zachowanie |
| --- | --- |
| `START_SCAN interval_ms` | Nowa sesja, gdy poprzednia została opróżniona; 1000..60000 ms. |
| `PAUSE_SCAN` | Kończy przetwarzanie bieżącego skanu i blokuje następne. |
| `RESUME_SCAN` | Wznawia tę samą sesję. |
| `STOP_SCAN` | Zatrzymuje kolejne skany, zachowuje kolejkę i przetwarza pobraną partię. |
| `GET_AP slot` | Odczyt agregatu AP; zajęte sloty to 0..unique_aps-1. |
| `GET_STATS` | Statystyki sesji; wariant TOTAL obejmuje bieżące uruchomienie. |

Po STOP odbiornik nadal zapisuje i potwierdza rekordy. Koniec sesji na karcie
następuje po opróżnieniu kolejki i partii, z `scanning=0`, `queued=0` oraz
`STATUS.phase=IDLE`. Następnie pobiera końcowe AP i statystyki.

### SMART

Każda detekcja aktualizuje agregaty, także jeśli nie powstaje obserwacja.
Porównania wykonywane są do ostatniej **zaakceptowanej obserwacji** danego BSSID.
Zapis następuje dla:

- pierwszego wykrycia;
- przemieszczenia co najmniej 15 m;
- zmiany RSSI co najmniej 6 dB;
- upływu 30 s od ostatniego zapisu;
- zmiany kanału, pasma, zabezpieczeń, SSID lub metadanych PHY;
- uzyskania ważnego GPS po wcześniejszym zapisanym pomiarze bez lokalizacji.

Zwykłe aktualizacje mają minimalny odstęp 2 s. Pierwsze wykrycie, metadane i
uzupełnienie GPS omijają tę blokadę. Przy odwróceniu kierunku zmiany RSSI
histereza wymaga dodatkowych 2 dB. Progi można zmieniać przez konfigurację.

FULL zapisuje wyniki zgodnie z `full_gap_ms`; 0 oznacza wszystkie pobrane wyniki.
UNIQUE zapisuje BSSID raz na sesję, nadal aktualizując agregaty AP.

## 7. Dane GPS i Wi-Fi

Parser sprawdza checksumy NMEA, składnię liczb, datę, format współrzędnych,
status fixa i zgodność RMC/GGA. Zdania mogą różnić się najwyżej o sekundę;
geograficznie sprzeczne pozycje są odrzucane. Domyślnie oba zdania muszą być
nie starsze niż 3000 ms, a HDOP nie większe niż 5.

Brak poprawnego fixa nie zatrzymuje Wi-Fi. Obserwacja ma nieważną pozycję i NAN,
a nie współrzędne 0,0 lub starą lokalizację. Rzeczywisty fix w 0,0 jest poprawny.
HDOP nie jest zamieniany na metry accuracy. Brak opcjonalnej prędkości, kursu,
wysokości lub accuracy jest oznaczany jako brak danych.

Snapshot pozycji jest zamrożony przy zakończeniu skanu. Standardowe API wyników
nie udostępnia timestampu odbioru każdego beacona; podczas jazdy pozostaje błąd
wynikający ze skanowania kolejnych kanałów. Jeden interfejs radiowy obsługuje oba
pasma kolejno. Najsilniejszy punkt pomiarowy nie jest lokalizacją routera.

Wewnętrzny model obsługuje SSID z jawną długością i dowolnymi bajtami.
API skanowania IDF udostępnia SSID zakończony NUL, więc osadzony NUL może uciąć
nazwę w adapterze. BSSID pozostaje kluczem identyfikacyjnym.

## 8. UART, trwałość i błędy

Format ramki ASCII:

```text
FBR1<TAB>request_id<TAB>type<TAB>fields*CRC16<LF>
```

CRC-16/CCITT-FALSE, bufor 1536 B z NUL, maksymalnie 48 pól. SSID i CSV są kodowane
hex. `request_id=0` oznacza zdarzenie. CRC i ograniczony alfabet umożliwiają
odrzucenie uszkodzonej ramki oraz resynchronizację na LF.

ESP wysyła jedną niepotwierdzoną obserwację naraz. Odbiornik najpierw trwale
zapisuje `(boot, session, sequence)`, potem wysyła ACK. Timeout wynosi 2000 ms,
limit to trzy retransmisje. Retransmisja jest identyczna bajtowo i nie może
powodować ponownego dopisania rekordu po stronie odbiorcy.

Po timeout kolejka pozostaje w RAM. `RESUME_LINK` uzgadnia checkpoint i może
potwierdzić tylko ostatni zatwierdzony rekord lub faktycznie wysłaną głowę kolejki.
Pełne bufory wstrzymują kolejne skany. `NACK STORAGE` zatrzymuje transport i skany
przy błędzie odbiorcy. Po naprawie wymagane są uzgodnienie checkpointu i wznowienie.

PSRAM i statystyki TOTAL nie są trwałe. Restart zmienia boot, czyści sesję i
niepotwierdzone bufory; NVS przechowuje konfigurację. Nie wdrożono dziennika
obserwacji we flash ESP32. Nie należy kasować NVS automatycznie po błędzie.

Stare `HELLO` bez argumentu 2 wybiera format legacy; tam STOP anuluje wyniki.
Nowa aplikacja powinna używać revision 2. Szczegóły każdej wiadomości i błędu:
[FLIPPER_INTEGRATION.md](FLIPPER_INTEGRATION.md).

## 9. Pliki i WiGLE

Docelową strukturę na microSD tworzy aplikacja FAP:

```text
/ext/apps_data/wardriving/
    config/settings.conf
    sessions/session_YYYYMMDD_HHMMSS/
        metadata.json
        events.journal
        access_points.csv
        observations.csv
        wigle.csv
        stats.json
    logs/errors.log
```

Dziennik zdarzeń jest źródłem prawdy dla checkpointu, odzyskania częściowego
zapisu i późniejszego generowania CSV. Należy sprawdzać pełny zapis i sync przed
ACK, usuwać niepełny ogon przy odzyskiwaniu i obsługiwać brak/pełną kartę.
To wymagania osobnego projektu Flippera.

WiGLE ma 11 standardowych kolumn i brand **Sapsan** w nagłówku metadanych.
Eksportowane są tylko pomiary z prawidłowym UTC i lokalizacją. Nie dodaje się
niestandardowych kolumn. Bez accuracy w metrach pole jest puste. SSID jest
cytowane i bezpiecznie escapowane; oryginalne bajty zachowuje `ssid_hex`.
Przykłady: [examples](examples).

## 10. Diagnostyka i testy

Dioda: niebieska — bezczynność; bursztynowa — praca bez fixa;
zielona — praca z fixem; czerwona — błąd. Zielony kolor nie potwierdza zapisu SD.
`GET_MEMORY` pokazuje osobno SRAM i PSRAM oraz limity i zajętość buforów.

Wynik docelowej kompilacji: AP 488 B, observation 168 B, engine 376 B.
Domyślne bufory PSRAM rezerwują **5 304 064 B**; indeks zawiera 20 000 wpisów,
więc jego zapełnienie nie przekracza 50%. Weryfikacja sprzętowa potwierdziła 8 MiB
PSRAM i około 3 MiB wolnej PSRAM po alokacji.

Testy hosta:

```sh
cmake -S tests -B build-host -DFEB_SANITIZERS=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Odczyt stanu urządzenia, z pyserial w środowisku SDK:

```sh
python tools/device_info.py /dev/cu.usbmodemflip_Tutel1
```

Narzędzie nie uruchamia skanów ani nie pulsuje resetem; sam mostek może zresetować
ESP przy otwieraniu portu. Opcja `--boot-log` jawnie resetuje ESP i wypisuje log,
który może zawierać pozycję GPS.

Próby akceptacyjne zmieniają chwilowo konfigurację, wykonują skany pasywne,
restart z kolejką i przywracają ustawienia. Odbiornik PC używa tymczasowego journal
z fsync przed ACK. Nie jest to test karty SD Flippera.

```sh
python tools/stress_test.py /dev/cu.usbmodemflip_Tutel1 --output /tmp/feberis-acceptance.json
python tools/stress_test.py /dev/cu.usbmodemflip_Tutel1 --soak-cycles 12 --output /tmp/feberis-soak.json
```

W 0.3.1 przeszło 7/7 zestawów hosta, 15/15 scenariuszy sprzętowych i 12 cykli
próby pamięci. Host sprawdza również 10 000 AP, kolejkę 2048, limity ramek,
nieprawidłową konfigurację, skraje czasu, NMEA i CSV. Pełna macierz:
[VALIDATION.md](VALIDATION.md). Rzeczywiste wyniki:
[HARDWARE_TEST_2026-11-23_ACCEPTANCE.md](HARDWARE_TEST_2026-11-23_ACCEPTANCE.md).
Archiwalne `tools/hardware_test.py` dotyczy kontraktu 0.1.0.

## 11. Granice potwierdzonego działania

Nie wykonano jeszcze rzeczywistych testów GUI/FAP i zapisu microSD, importu na
serwerze WiGLE, jazdy z GNSS ani wielogodzinnej pracy. Pełną tabelę AP i kolejkę
sprawdzono modelowo na hoście; radio widziało kilkanaście sieci. Próba 12 cykli
wykazała stałą ilość wolnej pamięci po opróżnianiu partii, ale nie zastępuje
wielogodzinnego obciążenia. Zawieszenia sterownika Wi-Fi i awarii NVS nie
wstrzykiwano fizycznie.

## 12. Rozwijanie projektu

Stosuj C, cztery spacje, `snake_case`, typy `_t` i `.clang-format`.
Moduły `core/` pozostają niezależne od ESP-IDF. Pamięć i bufory muszą mieć jawne
limity; nowe reguły zapisu porównują do zaakceptowanej obserwacji. Zmiany protokołu
muszą zostać opisane dla obu końców w dokumencie integracji FAP.

Nie dodawaj do repozytorium prywatnych kluczy, tokenów, lokalnego `sdkconfig`,
ustawień edytora ani rzeczywistych dzienników z SSID/BSSID i pozycjami.
Raporty testów zawierają wyłącznie podsumowania; pliki `docs/examples/` są syntetyczne.

## Źródła i dokumenty uzupełniające

- [Dokumentacja modułu N8R8](https://documentation.espressif.com/esp32-c5-wroom-1_wroom-1u_datasheet_en.pdf)
- [ESP-IDF 6.0: Wi-Fi C5](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c5/api-reference/network/esp_wifi.html)
- [ESP-IDF 6.0: PSRAM C5](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c5/api-guides/external-ram.html)
- [Architektura i wcześniejszy audyt](ARCHITECTURE.md)
- [Kontrakt UART i zadanie dla projektu Flippera](FLIPPER_INTEGRATION.md)
