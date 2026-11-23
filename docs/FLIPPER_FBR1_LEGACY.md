> Archiwum kontraktu firmware 0.1.0. Dla 0.2.0 użyj FLIPPER_INTEGRATION.md; poniższe semantyki GPS i eksportu nie opisują nowej wersji.

# Feberis Pro — wytyczne dla aplikacji Flipper Zero

Kontrakt **FBR1**, firmware **0.1.0**. Ten plik można przekazać do osobnego projektu
aplikacji. Firmware ESP32 nie ma dostępu do SD Flippera. Aplikacja ma obsłużyć UART,
GUI, zapis plików, ACK i błędy zapisu. Nie jest jeszcze częścią tego repozytorium.

## 1. Transport

UART0 ESP32-C5: TX GPIO11 → RX Flippera, RX GPIO12 ← TX Flippera, wspólna masa,
3,3 V, **115200 baud, 8N1**, bez RTS/CTS. Użyj UART odpowiadającego fizycznemu
połączeniu na Flipperze; nie zakładaj numeracji jego złącza na podstawie GPIO ESP32.
Przejmij interfejs szeregowy na czas działania aplikacji i zwolnij go przy wyjściu.

GPS: TX modułu → GPIO14, RX modułu ← GPIO13, UART1 **115200**. WS2812B: GPIO27.
GPIO13/14 współdzielą USB ESP32-C5. UART0 musi zostać zwolniony do flashowania ESP32.

## 2. Ramka i walidacja

Każda ramka jest linią ASCII:

```text
FBR1<TAB>request_id<TAB>command_or_type<TAB>field...*HHHH<LF>
```

`<TAB>` oznacza jeden bajt `0x09`, `<LF>` — `0x0A`; nie wysyłaj CR.
CRC to **CRC-16/CCITT-FALSE**: poly `0x1021`, init `0xFFFF`, refin/refout false,
xorout `0x0000`. Licz po bajtach od `F` do bajtu poprzedzającego `*`, włącznie
z tabulatorami. Zapisz cztery cyfry hex. Wektor `123456789` → `29B1`.
Implementacja referencyjna: `tools/protocol.py`; C: `main/core/wire.c`.

Bufor odbiorczy: **1536 bajtów** z miejscem na NUL, maksymalnie 24 pola.
Firmware nadaje ramki do 1535 bajtów łącznie z LF. Odrzuć przepełnioną linię aż do
LF; błędną sumę, prefiks lub składnię również odrzuć. Zachowaj puste pola przy
dzieleniu po TAB (puste SSID jest poprawne). Żadne pole nie zawiera surowych TAB,
CR, LF ani `*`. Dane binarne i gotowe CSV są kodowane hex, po dwie cyfry na bajt.

`request_id`: uint32, zakres 1..4294967295 dla poleceń; `0` oznacza zdarzenie.
Odpowiedź ma ten sam identyfikator. ESP32 nie buforuje odpowiedzi według ID.
Komendy wysyłaj kolejno; po 2 s bez odpowiedzi można ponowić komendę. Uszkodzona
ramka nie wywołuje odpowiedzi. ACK jest wyjątkiem: poprawny ACK nie ma odpowiedzi.
Ignoruj tekst ROM/boot/panic bez poprawnego FBR1 i CRC; UART dostarcza fragmenty,
nie kompletne ramki. Nie uzależniaj działania od odebrania startowego `READY`.

Przykłady poniżej pokazują **pola bez prefiksu, ID i CRC**; pełne ramki tworzy
`python3 tools/protocol.py 1 HELLO` itd.

## 3. Polecenia

| Polecenie i argumenty | Odpowiedź / zachowanie |
| --- | --- |
| `HELLO` | `OK HELLO version boot baud country max_aps ack_ms retries` |
| `PING` | `OK PONG` |
| `STATUS` | Stan opisany niżej, z ID żądania |
| `START interval_ms` | `OK START`; zakres 1000..60000 ms, domyślnie zalecane 2000 |
| `STOP` | `OK STOP`, potem `STOPPED`; anuluje także niepotwierdzone rekordy |
| `BAND AUTO` / `BAND 2G` / `BAND 5G` | `OK BAND`; tylko poza sesją i po zakończeniu skanu |
| `ACK boot sequence` | Potwierdza bieżący AP; brak odpowiedzi |

`START` podczas pracy aktualizuje przerwę i nie resetuje liczników. Przerwa jest
liczona **po przesłaniu i potwierdzeniu ostatniego AP**, a nie od początku skanu.
`BAND AUTO` skanuje oba pasma kolejno, nie jednocześnie. Skan jest pasywny,
120 ms/kanał, z kanałami dopuszczonymi przez konfigurację kraju.

Odpowiedź błędu: `ERR code`, np. `BAD_ARGUMENT`, `BUSY`, `WIFI_UNAVAILABLE`,
`BOOT_MISMATCH`, `UNKNOWN_COMMAND`. Błąd ustawienia pasma: `ERR BAND`.
Boot to losowy uint32 identyfikujący uruchomienie ESP32. Zmiana boot oznacza restart;
zamknij poprzednią sesję, wyczyść stan deduplikacji, wykonaj HELLO/STATUS i poproś
użytkownika o ponowny start. Nie potwierdzaj AP pochodzących z poprzedniego boot.

## 4. Zdarzenia i dane AP

Wszystkie poniższe zdarzenia mają `request_id=0`.

```text
READY version boot
SCAN_BEGIN boot scan_id
SCAN_DONE boot scan_id found sent truncated
STOPPED boot reason discarded_total
ERROR code [esp_error_number]
```

`SCAN_DONE` oznacza potwierdzenie wszystkich `sent` rekordów danego skanu.
`found` to liczba wykrytych AP, `truncated` — liczba pominiętych przez limit bufora.
ESP-IDF sortuje wyniki według RSSI; limit domyślnie 128. `STOPPED` może nadejść
przed wewnętrznym zdarzeniem zakończenia anulowanego skanu — czekaj na
`STATUS` z fazą `IDLE` przed zmianą pasma lub nowym START; wcześniej możliwe BUSY.

Zdarzenie `AP` ma **20 pól łącznie z prefiksem i ID**, w tej kolejności:

| Indeks od 0 | Pole | Znaczenie |
| --- | --- | --- |
| 0..2 | `FBR1`, `0`, `AP` | Typ ramki |
| 3 | boot | Identyfikator uruchomienia |
| 4 | sequence | Rosnący uint32, wspólny dla skanów, od 1 |
| 5 | scan_id | Rosnący identyfikator skanu |
| 6 | bssid | MAC `AA:BB:CC:DD:EE:FF` |
| 7 | ssid_hex | Bajty SSID z API ESP-IDF, maks. 32 bajty; może być puste |
| 8 | auth | Np. `[WPA2-PSK][ESS]`; nierozpoznane tryby: `[UNKNOWN][ESS]` |
| 9..10 | channel, rssi | Kanał, RSSI w dBm |
| 11 | fix_valid | 0/1; jedyny warunek użycia pozycji |
| 12 | utc | `YYYY-MM-DD HH:MM:SS` UTC lub `-` bez fixa |
| 13..15 | latitude, longitude, altitude_m | Stopnie dziesiętne i wysokość n.p.m. |
| 16..18 | hdop, satellites, age_ms | HDOP, satelity, wiek RMC przy zakończeniu skanu |
| 19 | csv_hex | Kompletny wiersz CSV z LF zakodowany hex, albo `-` |

Bez fixa pola numeryczne mogą zawierać poprzednie dane: ignoruj je, gdy
`fix_valid=0`. Firmware wymaga poprawnych checksum NMEA, RMC ze statusem A oraz
GGA z fixem, obu młodszych lub równych 2500 ms i o czasie różniącym się najwyżej
o sekundę. Data RMC jest interpretowana jako rok 2000..2099. Pozycja jest
próbkowana na końcu skanu i zamrożona dla całej partii oraz retransmisji.
To przybliżenie lokalizacji AP; przy ruchu nie jest pozycją z chwili odebrania beacona.

SSID w strukturze ESP-IDF jest łańcuchem zakończonym NUL. Ukryte SSID są puste;
SSID zawierający osadzony NUL będzie ucięty. Ekran dekoduje `ssid_hex` i zastępuje
niedrukowalne/niewspierane znaki, bez używania SSID jako formatu printf lub ścieżki.

## 5. STATUS i ekran

`STATUS` pojawia się co około sekundę (ID 0) oraz w odpowiedzi na polecenie:

```text
FBR1 id STATUS boot running phase scan_id cycles observations acknowledged rows
no_gps truncated discarded fix_valid satellites hdop age_ms latitude longitude
altitude_m utc gps_ready wifi_ready
```

To jedna linia z TAB, **24 pola**. `phase`: `IDLE`, `SCANNING`, `DRAINING`.
`running=1` oznacza żądanie kolejnych skanów; podczas przerwy phase może być IDLE.
Liczniki są kumulowane od restartu: `cycles` — ukończone skany, `observations` —
AP wysłane po raz pierwszy, `acknowledged` — potwierdzone AP, `rows` — potwierdzone
AP zawierające CSV, `no_gps` — wysłane AP bez fixa, `truncated` — pominięte AP,
`discarded` — niewysłane/niepotwierdzone AP anulowane po pobraniu wyników.
Anulowanie skanu przed pobraniem wyników nie zwiększa discarded.
`rows` nie dowodzi fizycznego zapisu SD: firmware zna wyłącznie ACK.
`gps_ready`/`wifi_ready` informują o powodzeniu inicjalizacji sterowników.

Ekran główny: stan sesji i łącza, fix/oczekiwanie GPS, satelity, HDOP, pasmo,
liczba obserwacji, faktycznie zapisane wiersze, pominięte rekordy, ścieżka CSV.
Lista AP: SSID lub „ukryta sieć”, BSSID, RSSI, kanał, zabezpieczenia, znacznik GPS.
Przyciski: Start/Stop, wybór pasma poza sesją, szczegóły AP.

Odbiór UART powinien tylko kopiować dane do ograniczonego bufora. Parser i zapis
SD wykonuj w zadaniu roboczym; GUI odświeżaj na podstawie chronionego modelu,
nie z callbacku UART. Nie przechowuj nieograniczonej listy AP w RAM Flippera.
Brak STATUS przez 5 s oznacza problem łącza; sprawdź PING, pokaż błąd i nie kontynuuj
zapisu z założeniem, że sesja nadal działa.

## 6. CSV na karcie SD i ACK

1. Przed START otwórz nowy plik, np. `/ext/apps_data/feberis/scan_YYYYMMDD_HHMMSS.csv`.
   Nie nadpisuj istniejącego pliku; przy braku UTC użyj unikalnego lokalnego numeru.
2. Zapisz dokładnie dwa nagłówki poniżej, zakończone LF, bez BOM. Sprawdź wynik zapisu.
3. Po poprawnym AP sprawdź CRC, boot, liczbę pól, liczby i długości hex. Dla nowego
   `(boot, sequence)` z `csv_hex != -` zdekoduj hex i dopisz otrzymane bajty bez zmian.
   Wiersz powinien mieć jeden końcowy LF, żadnego innego CR/LF, maks. 511 bajtów.
4. Dopiero po udanym pełnym zapisie i synchronizacji pliku zapamiętaj sequence i wyślij
   `ACK boot sequence`. Bez CSV zaktualizuj ekran i również potwierdź AP.
5. Retransmisję ostatniego potwierdzonego sequence **potwierdź ponownie bez dopisywania**.
   Odrzucaj starsze sequence; ESP nadaje wyłącznie jeden niepotwierdzony AP naraz.
6. Przy pełnej SD/błędzie zapisu nie wysyłaj ACK. Wyślij STOP i pokaż błąd. W razie
   częściowego zapisu utnij plik do ostatniej pełnej linii albo oznacz go jako uszkodzony.
   Nie ponawiaj ślepo dopisywania częściowo zapisanego wiersza.

```csv
WigleWifi-1.4,appRelease=0.1.0,model=Feberis Pro,release=0.1.0,device=ESP32-C5,display=Flipper Zero,board=Feberis Pro,brand=Feberis
MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type
```

CSV używa kropki dziesiętnej, UTC i typu `WIFI`. `FirstSeen` to czas obserwacji
bieżącego skanu, nie historycznie pierwsze wykrycie BSSID. SSID jest cytowane,
cudzysłowy podwajane; bajty spoza drukowalnego ASCII oraz backslash są zapisywane
jako `\xNN`. Ten wariant WiGLE 1.4 jest bezpieczny dla binarnych SSID, ale zmienia
widoczną nazwę sieci Unicode. Oryginalne bajty dostępne są w `ssid_hex` w granicach
ograniczeń API ESP-IDF. `AccuracyMeters` to oszacowanie `HDOP × 5`, nie gwarancja.

Potwierdzenie ma timeout **2000 ms**; są **3 retransmisje**, identyczne bajtowo.
Po około 8 s bez ACK firmware zgłasza `ERROR ACK_TIMEOUT` i `STOPPED ACK_TIMEOUT`.
Nie rozpoczyna kolejnych skanów ani nie gromadzi nieskończonej kolejki. Zwiększenie
przepustowości wymaga zmiany kontraktu, nie wysyłania ACK przed zapisem.

Na Stop nadal przetwarzaj RX do `STOPPED`: AP wysłany wcześniej może być już w drodze.
Zapisz go raz, jeśli plik nadal jest otwarty. Następnie zsynchronizuj i zamknij plik.
STOP anuluje resztę partii; pełny ostatni skan gwarantuje dopiero SCAN_DONE.
Nie ma odtwarzania danych po zaniku zasilania ani gwarancji exactly-once po awarii
Flippera między zapisem a zapamiętaniem sequence; po restarcie aplikacji użyj STOP,
HELLO i nowego pliku.

## 7. Błędy i próby odbiorcze

Asynchroniczne `ERROR`: `GPS_INIT`, `LED_INIT`, `LED_WRITE`, `WIFI_INIT`, `UART_RX`, `NO_MEMORY`,
`SCAN_START`, `SCAN_FAILED`, `SCAN_READ`, `SCAN_TIMEOUT`, `ACK_TIMEOUT`, `FRAME_SIZE`,
`SEQUENCE_EXHAUSTED`. Część błędów zawiera dodatkowy numeryczny kod ESP-IDF.
Bez GPS nadal działa podgląd AP; brak fixa nie jest błędem UART. Przy błędzie
inicjalizacji Wi-Fi nie można rozpocząć skanu. Dla błędu NVS nie kasuj automatycznie
pamięci urządzenia — pokaż komunikat serwisowy. SEQUENCE_EXHAUSTED wymaga restartu ESP32.

Przed wydaniem aplikacji sprawdź na sprzęcie:

- HELLO/PING, Start/Stop, zmiany pasma i BUSY podczas anulowania skanu.
- AP 2,4 GHz oraz 5 GHz, ukryte SSID, przecinki/cudzysłowy i 32-bajtowe SSID.
- Brak anteny/fixa GPS, utratę fixa i brak wierszy CSV ze starą lokalizacją.
- Rzeczywiste nadawanie RMC/GGA przy 115200 i właściwy kierunek GPIO13/14.
- Celowe zgubienie ACK: brak duplikatu CSV i identyczna retransmisja.
- Odłączenie Flippera podczas transmisji, restart ESP32 i zmianę boot.
- Pełną/wyjętą SD, częściowy zapis, STOP przed zamknięciem pliku.
- Ponad 128 AP: jawny licznik truncated; zużycie RAM przy długiej sesji.
- Otwarcie CSV niezależnym parserem i próbny import do WiGLE.

Firmware przeszedł testy hosta i częściowe próby na fizycznym Feberis Pro przez
USB–UART Flippera: [raport z 2026-11-22](HARDWARE_TEST_2026-11-22.md).
Fix GPS, aplikacja Flippera, zapis SD i import WiGLE nadal wymagają prób.
