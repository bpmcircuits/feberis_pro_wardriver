# Validation 0.4.0 — continuous acquisition

Current results, commands, timing measurements, and hardware limitations are in
[HARDWARE_TEST_2026-11-23_CONTINUOUS.md](HARDWARE_TEST_2026-11-23_CONTINUOUS.md)
(English). The 0.3.1 and earlier reports below are historical.

# Walidacja 0.3.1 — test urządzenia i skrajnych przypadków, 2026-11-23

Przeprowadzono 7/7 zestawów testów hosta z ASan/UBSan (PASS), docelową kompilację
ESP-IDF 6.0.0 i analizę Clang --analyze pięciu modułów core (bez diagnostyki).
Wgrano poprawiony firmware. Wyniki testów sprzętowych oraz zakres potwierdzenia:
[HARDWARE_TEST_2026-11-23_ACCEPTANCE.md](HARDWARE_TEST_2026-11-23_ACCEPTANCE.md).

Nowy zestaw `edges` obejmuje:

- 21 nieprawidłowych wartości konfiguracji oraz dopuszczalne minima/maksima;
- granicę 1535 bajtów ramki, przekroczenie bufora, 48 i 49 pól;
- int64 MIN/MAX i cofnięcie zegara, przejście przez UINT32_MAX;
- kolejkę wielokrotnie zawijaną podczas 10 000 zapisów/ACK;
- wyczerpanie numeracji UINT32_MAX bez zawinięcia do 0;
- odrzucenie potwierdzenia niewysłanej głowy kolejki i obcej sesji, idempotentny checkpoint;
- RMC/GGA o sprzecznych pozycjach, midnight/year rollover, leap day, biegun/date line;
- brak opcjonalnej wysokości, prędkości i kursu, błędne formaty liczb NMEA;
- SSID zawierający NUL/FF, ukryty SSID, złą długość i ignorowanie bajtów poza długością.

Dodatkowa regresja w `wardrive` sprawdza uzupełnienie best_location przy odzyskaniu
GPS i równym strongest RSSI. Dotychczasowe testy 10 000 różnych AP i kolejki 2048,
obsługi braku PSRAM, odmowy alokacji, CSV i retransmisji nadal przechodzą.
Nie wykonano wstrzyknięcia zawieszonego sterownika Wi-Fi na sprzęcie; przeniesienie
watchdoga przed sprawdzanie PAUSE/STOP zweryfikowano przeglądem kodu, a arytmetykę
jego deadline pokrywają testy hosta. Clang analizuje portable core, nie zamknięte
biblioteki sterownika Wi-Fi ani cały FAP.

Polecenia:

```sh
cmake -S tests -B build-host -DFEB_SANITIZERS=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
xcrun clang --analyze -std=c11 -I main/core -Xanalyzer -analyzer-output=text main/core/wardrive.c main/core/nmea.c main/core/delivery.c main/core/wire.c main/core/wigle.c
# po aktywacji SDK:
idf.py build
python tools/stress_test.py /dev/cu.usbmodemflip_Tutel1 --output /tmp/feberis-acceptance.json
python tools/stress_test.py /dev/cu.usbmodemflip_Tutel1 --soak-cycles 12 --output /tmp/feberis-soak.json
```

---

Poniżej zachowano historyczne raporty 0.3.0 i 0.2.0.

# Walidacja 0.3.0 — PSRAM N8R8, 2026-11-23

Wykonano kompilację ESP-IDF 6.0.0 dla ESP32-C5 i **6/6 zestawów testów hosta
z ASan/UBSan: PASS**. Cztery dotychczasowe zestawy pozostają, dodano capacity
oraz ap_memory. Następnie na wyraźne polecenie użytkownika wgrano tę wersję
przez USB–UART Flippera (/dev/cu.usbmodemflip_Tutel1), zrestartowano Feberisa i
potwierdzono działanie UART oraz 8 MiB PSRAM. Szczegóły i ograniczenia:
[raport sprzętowy PSRAM](HARDWARE_TEST_2026-11-23_PSRAM.md).

Polecenia hosta pozostają takie jak poniżej. Firmware: po aktywacji SDK
`idf.py build`; po końcowych zmianach inkrementalnie `ninja -C build`.

Nowe testy:

- capacity: rzeczywiste przyjęcie 10 000 różnych BSSID, ACK wszystkich obserwacji,
  ponowne wyszukanie wszystkich BSSID w odwrotnej kolejności, brak nowych observation
  w UNIQUE, stabilne gęste sloty GET_AP, jawne TABLE_FULL dla 10 001 BSSID.
- capacity: kolejka 2048 rekordów podczas braku ACK, brak nadpisania, brak przesunięcia
  baseline odrzuconej obserwacji, brak destrukcyjnego resetu sesji z niezatwierdzonymi
  rekordami, późniejszy zapis FIRST po zwolnieniu kolejki.
- capacity: walidacja buforów/capacity/indexu i wielkości przepełniających size_t,
  brak zmiany stanu przy złej inicjalizacji, bezpieczny brak modelu po błędzie pamięci.
- ap_memory: rzeczywisty adapter main/ap_memory.c z mockami wyłącznie API pamięci
  ESP-IDF; brak PSRAM, odmowa każdej z trzech alokacji, zwolnienie częściowych alokacji,
  wymagane jawne MALLOC_CAP_SPIRAM|8BIT, poprawne wielkości, brak ponownych alokacji
  przy starcie sesji i podczas skanowania.

Kompilacja potwierdziła Quad PSRAM 40 MHz, SPIRAM_USE_CAPS_ALLOC, boot init,
MEMTEST, IGNORE_NOTFOUND, flash 8 MiB i limity 10000/2048 w sdkconfig.
Rozmiary z DWARF docelowego ELF: AP 488 B, observation 168 B, engine 376 B.
Bufory PSRAM: **5 304 064 B**; dawny statyczny engine w SRAM miał 130 656 B.
To obliczenia ze skompilowanego obrazu, nie odczyt fizycznej wolnej pamięci.
Nowe GET_MEMORY i rozszerzone opisy są w FLIPPER_INTEGRATION.md.
Końcowy obraz aplikacji: 0xc4350 = 803664 B, 23% wolnej partycji aplikacji 1 MiB.
SHA-256 końcowego obrazu: `ecf13cd55b7c7cc1434d86a519e5284f73ccbf20bb33763400f73863484cf904`.
Raport maszynowy: [artifacts/validation/2026-11-23-psram.json](../artifacts/validation/2026-11-23-psram.json).

Po wgraniu przez USB–UART Flippera sprawdzono GET_INFO oraz GET_MEMORY:
ready=1, initialized=1, PSRAM=8 388 608 B, ap_capacity=10000,
queue_capacity=2048. PING i gotowość sterowników GPS/Wi-Fi potwierdzone.
Do wykonania pozostaje obciążenie przy skanach obu pasm, minimum SRAM,
czas przetwarzania dużych partii, NVS i powrót po odłączeniu UART.
Brak PSRAM/alokacji ma blokować START bez cichego obniżenia limitu. Błąd testu
samej pamięci może zatrzymać boot w SDK. PSRAM nie zapewnia trwałości po resecie.

Dokumentacja producenta i źródła API:
[moduł N8R8](https://documentation.espressif.com/esp32-c5-wroom-1_wroom-1u_datasheet_en.pdf),
[ESP-IDF C5 external RAM](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c5/api-guides/external-ram.html).

---

Poniżej zachowano historyczny raport 0.2.0. Jego limit 256 AP oraz 32 observation
nie opisuje obecnego firmware 0.3.0.

# Walidacja 0.2.0 — 2026-11-23

Wykonano testy hosta z ASan/UBSan oraz docelową kompilację ESP-IDF 6.0.0 dla
ESP32-C5. Nie wgrywano nowego firmware i nie wykonywano nowych prób sprzętowych.
Raport HARDWARE_TEST_2026-11-22.md dotyczy wyłącznie starszego firmware 0.1.0.

## Wykonane polecenia

```sh
cmake -S tests -B build-host -DFEB_SANITIZERS=ON
cmake --build build-host
ctest --test-dir build-host --output-on-failure
# po aktywacji środowiska ESP-IDF 6.0.0:
idf.py build
# końcowa kompilacja zmienionych źródeł w już skonfigurowanym build:
ninja -C build
python3 tools/protocol.py 1 HELLO 2
python3 tools/protocol.py 2 GET_CONFIG
python3 -m py_compile tools/protocol.py tools/hardware_test.py
```

CTest: **4/4 zestawy PASS**: core, csv_and_protocol, wardrive, pipeline.
Kompilator hosta Apple Clang, C11, -Wall/-Wextra/-Werror/-pedantic i sanitizery.
Kompilator firmware riscv32-esp-elf GCC 15.2.0, SDK v6.0, -Wall/-Wextra/-Werror.
Wynik: build/feberis_wardriver.bin, bootloader, partition table i flasher_args.json.
Końcowy obraz aplikacji 0xc2e10 = 798224 bajty, 24% wolnej partycji 1 MiB.
Rozmiar pliku nie jest dowodem wystarczającej wolnej RAM podczas pracy Wi-Fi.

Lokalna aktywacja SDK wymagała wskazania istniejącego virtualenv przez
IDF_PYTHON_ENV_PATH. Brakowało lokalnego pliku constraints v6.0, więc użyto
IDF_PYTHON_CHECK_CONSTRAINTS=no; pakiety nie były instalowane/aktualizowane.
Pierwszą rekonfigurację blokował sandbox (psutil/sysctl); wykonano ją po uzyskaniu
zgody poza sandboxem. Końcowa inkrementalna kompilacja ninja przeszła w sandboxie.
Nie są to dodatkowe wymagania firmware — poprawnie zainstalowany IDF ma własne
constraints i środowisko. Te informacje pozwalają odróżnić build od pełnej
weryfikacji instalacji zależności.

## Macierz wymaganych przypadków

| Przypadek | Test / wynik i granica |
| --- | --- |
| 1. 100 detekcji bez ruchu | wardrive: 1 observation, 100 detections, 99 filtered |
| 2. Przemieszczenie 20 m | wardrive: DISTANCE po min_gap |
| 3. RSSI delta 10 dB | wardrive: RSSI |
| 4. RSSI poniżej progu | wardrive: filtr, kumulacja względem saved; histereza kierunku |
| 5. Zmiana kanału | wardrive: METADATA także bez min_gap |
| 6. Identyczny SSID, różne BSSID | wardrive: 2 unique |
| 7. Utrata GPS | wardrive/core: invalid, NAN, bez distance; skanowanie nie wymaga fixa |
| 8. Powrót GPS | wardrive: GPS_RECOVERED także bez min_gap |
| 9. Błędne NMEA | core: CRC, data, współrzędne, quality, niespójne epoki i noise |
| 10. Brak UART | delivery/pipeline: 3 retry, timeout, kolejka pozostaje |
| 11. Uszkodzona ramka | core/pipeline/Python: odrzucenie CRC, resynchronizacja |
| 12. Retransmisja observation | pipeline: checkpoint odbiorcy, jeden zapis, ponowny ACK |
| 13. Pełna kolejka | wardrive: jawny błąd, brak nadpisania i brak przesunięcia baseline |
| 14. Restart ESP podczas sesji | model: stan nowej sesji i obce ACK; fizyczny restart nadal do sprawdzenia |
| 15. Odłączenie Flippera | pipeline: brak odbiorcy, bounded retry, wznowienie checkpointu; przewód do sprawdzenia |
| 16. Brak miejsca SD | pipeline: symulowany nieudany commit nie pozwala ACK; rzeczywista SD poza repo |
| 17. Tysiące unikalnych AP | wardrive: 10 000 prób, przyjęte 256, reszta jawnie TABLE_FULL, stała pamięć; brak obsługi tysięcy w jednej sesji |
| 18. Zawinięcie czasu 32-bit | wardrive: uint64 przed/po UINT32_MAX, sekwencja nie zawija się |
| 19. Nietekstowy/ukryty SSID | model ma bajty A/NUL/FF; CSV ma pusty SSID, 32 bajty FF, quoting i escaping; SDK ucina NUL |
| 20. Brak pozycji przy eksporcie | core/Python: brak wiersza przy invalid/NAN/out-of-range; GPS nie jest brany przy zapisie |

Dodatkowo: FULL sampling, UNIQUE, zmiana trybu, stale GPS, HDOP ponad limit,
interwał 30 s, oba rodzaje statystyk i zachowanie TOTAL między sesjami,
nieprawidłowa konfiguracja, wyczerpanie sequence, 100 000 losowych bajtów parserów.
CSV jest czytany niezależnym Python csv.DictReader, dokładnie 11 kolumn;
AccuracyMeters jest puste bez estymacji odbiornika, oddzielnie test 3.2 m.
Nierozpoznane zabezpieczenia nie generują wiersza WiGLE.

Pipeline używa prawdziwych modułów C wardrive/wire/delivery oraz **symulowanego**
odbiorcy i wyniku trwałego zapisu. Nie testuje API Flipper Storage, UART HAL,
parsera komend app_main ani fizycznej atomowości karty. Testy hosta nie potwierdzają
pracy radia, zużycia heap przez zamknięte biblioteki Wi-Fi i dokładności GNSS.

## Obowiązkowa dalsza walidacja

Regresje eksportu po analizie przejazdów z 2026-11-23: test `core` sprawdza
zachowanie UTF-8 (2/3/4 bajty i granice zakresów), odrzucenie sekwencji overlong,
surogatów, wartości ponad U+10FFFF oraz urwanych/błędnych sekwencji. Sprawdza też
puste i 32-bajtowe zerowe SSID, osadzony NUL, quoting, kontrolne ASCII, backslash,
niezmienność oryginalnych bajtów i dokładne granice bufora. `csv_and_protocol`
dekoduje wynik jako ścisłe UTF-8 i potwierdza nazwę z polskimi znakami, CJK,
emoji, przecinkiem i cudzysłowami niezależnym parserem CSV.
Są to testy hostowe. Na urządzeniu pozostaje sprawdzenie nowych trybów auth,
zachowania `ingest_cpu_us` po STOP/błędzie skanu oraz eksportu i importu WiGLE
przez rzeczywistą aplikację Flippera.

Na ESP: RF 2,4/5 GHz, kanały kraju/DFS, passive/active, ukryte SSID, praca obu UART,
fix i prędkość w ruchu, utrata GPS, retry po błędach Wi-Fi, 3 baudrates,
NVS po restarcie i odcięciu zasilania, minimum heap i watermark stosów, pełna tabela.
Na FAP: kompilacja z właściwym SDK, GUI responsywne podczas I/O, dokładne formaty
revision 2, brak duplikatów po restarcie w oknie sync→ACK, partial journal,
pełna/wyjęta SD, końcowe agregaty GET_AP, sesje i eksport.
Na WiGLE: import pliku z pustą accuracy oraz escaped SSID i poprawna interpretacja
zabezpieczeń. Nie deklarowano wyniku importu bez jego faktycznego wykonania.
