# Feberis 0.4.0 — kontrakt dla osobnego projektu aplikacji Flipper Zero

Ten plik jest zadaniem integracyjnym dla projektu FAP. W tym repozytorium jest
wyłącznie firmware ESP32-C5 oraz narzędzia i testy hosta. Funkcje SD i GUI opisane
poniżej należy zaimplementować i przetestować w osobnym projekcie Flippera.
Nie oznaczaj ich jako już wykonane. Zachowaj tam istniejący styl i nawigację.

## Transport i migracja

Zachowano ramkowanie FBR1: ASCII `FBR1<TAB>id<TAB>type<TAB>fields*HHHH<LF>`.
CRC-16/CCITT-FALSE (poly 1021, init FFFF, bez odbicia/xor), liczone od F do bajtu
przed `*`. Wektor `123456789` → `29B1`. Maksymalny bufor 1536 bajtów z NUL,
do 48 pól, puste pola są znaczące. Hex dla bajtów binarnych; żadnych surowych
TAB/CR/LF/asterisk wewnątrz pól. Nie zmieniono sprawdzonego ramkowania na COBS,
ponieważ alfabet jest już jednoznacznie ograniczony, a CRC i resynchronizacja
po LF są testowane. Przy przepełnieniu odrzucaj całą linię do LF.

UART 8N1, bez flow control, domyślnie sprawdzone wcześniej 115200; opcje 460800,
921600 wymagają testu sprzętowego. Dotychczasowe konfigurowalne GPIO:
ESP TX11→Flipper RX, ESP RX12←Flipper TX; GPS TX→ESP RX14, ESP TX13→GPS RX.
WS2812 GPIO27. Wspólna masa i poziomy 3,3 V. GPIO13/14 współdzielą natywne USB.
Nie zgaduj numerów pinów złącza Flippera; sprawdź istniejący projekt sprzętowy.

Nowy FAP wysyła `HELLO 2`, potem `GET_INFO`, `GET_CONFIG`, `GET_STATUS`.
Odpowiedź HELLO pozostaje kompatybilna: `OK HELLO version boot baud country
max_scan_records ack_ms retries`. INFO potwierdza revision=2.
Bez argumentu `2` firmware wysyła stare 20-polowe `AP`; `STOP` anuluje kolejkę,
a `SCAN_DONE` czeka na ACK jak wcześniej. Nie mieszaj trybów podczas sesji.
FBR1 revision 2 i istniejące formaty STATUS/STATS/OBSERVATION pozostają bez zmian.
Nowa komenda GET_MEMORY jest rozszerzeniem w firmware 0.3.0.
Archiwalny kontrakt 0.1.0 jest w [FLIPPER_FBR1_LEGACY.md](FLIPPER_FBR1_LEGACY.md).
**W nowej aplikacji obowiązuje ten dokument, także dla CSV i GPS.**

Komendy mają ID uint32 >0; zdarzenia ID=0. Każda odpowiedź jest ramkowana z CRC.
`OK code` / `ERR code` są odpowiedziami, `ACK` jest jednokierunkowym poleceniem.
Uszkodzona ramka nie otrzymuje NACK (nie można ufać jej ID). Polecenia wykonuj
kolejno; po timeout sprawdź stan przed ponowieniem START/STOP/SET_CONFIG.
Firmware nie ma pamięci odpowiedzi według request_id.

## Polecenia revision 2

Poniższe pola pomijają `FBR1 id`. Liczby dziesiętne, kropka jako separator.

| Polecenie | Odpowiedź / semantyka |
| --- | --- |
| `HELLO 2` | `OK HELLO ...`; ponawia dostęp do zachowanej kolejki |
| `GET_INFO` | `INFO version boot baud country ap_capacity queue_capacity revision`; pojemności rzeczywiście zaalokowane, 0 przy błędzie PSRAM |
| `GET_MEMORY` | `MEMORY` i 15 pól diagnostyki pamięci (opis niżej) |
| `PING` | `OK PONG` |
| `STATUS` | Stara, 24-polowa ramka STATUS, opis niżej |
| `GET_STATUS` | `STATE boot session running paused scanning link_wait queued last_sequence committed elapsed_ms` |
| `GET_GPS` | `GPS valid utc lat lon altitude_m hdop satellites age_ms speed_kmh course_deg horizontal_accuracy_m` |
| `GET_CONFIG` | `CONFIG` i 13 pól konfiguracji |
| `SET_CONFIG` + 13 pól | `OK SET_CONFIG` lub błąd; pełna walidacja i zapis NVS |
| `START_SCAN [gap_ms]` / `START [gap_ms]` | `OK START`; dodatkowa przerwa 0..60000 ms, domyślnie 0; nowa sesja tylko gdy poprzednia opróżniona |
| `STOP_SCAN` / `STOP` | `OK STOP`; zatrzymuje kolejne skany, zachowuje zaakceptowane rekordy i przetwarza już pobraną partię |
| `PAUSE_SCAN` | `OK PAUSE_SCAN`; kończy bieżący skan i przetwarza jego wyniki, blokuje kolejne |
| `RESUME_SCAN` | `OK RESUME_SCAN`; nie tworzy nowej sesji i nie wznawia błędu SD/łącza |
| `ACK boot session sequence` | Usuwa wyłącznie aktualnie wysłaną głowę kolejki. Stary wariant `ACK boot sequence` też jest rozpoznawany |
| `RESUME_LINK boot session committed` | `OK RESUME_LINK`; committed musi być ostatnim ACK ESP lub faktycznie wysłaną głową kolejki, której ACK zagubiono |
| `NACK STORAGE` | `OK NACK`; zwiększa storage_errors, wstrzymuje skany i retransmisję, zachowuje kolejkę |
| `GET_STATS` / `GET_STATS TOTAL` | `STATS scope session ...`; bieżąca sesja lub całe bieżące uruchomienie urządzenia |
| `GET_AP slot` | `AP_DISCOVERED ...` gdy AP ma 1 detekcję, inaczej `AP_UPDATED ...`; `OK EMPTY` dla pustego slotu |
| `RESET_STATS` | Tylko po STOP i opróżnieniu wyników; zeruje tabelę i statystyki sesji, pozostawia TOTAL i numerację transportu |
| `BAND AUTO/2G/5G` | Stary skrót, tylko poza skanowaniem; zapisuje wybór w NVS |

`START` podczas aktywnej sesji zmienia jedynie przerwę. Nowy START po STOP zeruje
agregaty sesji; wcześniej pobierz GET_AP i GET_STATS. Nie wznawia automatycznie
skanowania po restarcie. Sekwencje nie zawijają się: wyczerpanie wymaga restartu.
Przy RESET_STATS najpierw zabezpiecz podsumowanie na SD.

### Konfiguracja

Kolejność 13 pól CONFIG / SET_CONFIG (16 pól razem z prefiksem, ID, typem):

```text
version mode band active dwell_ms min_distance_m rssi_delta_db max_gap_ms
min_gap_ms gps_max_age_ms gps_max_hdop_x10 full_gap_ms baud
```

Domyślnie: `1 0 0 0 120 15 6 30000 2000 3000 50 2000 115200`.
mode: 0 SMART, 1 FULL, 2 UNIQUE. band: 0 BOTH, 1 2.4, 2 5.
active=0 jest pasywne; 1 tylko jako świadoma opcja w GUI.
Dwell 20..1500 ms; odległość 1..10000 m, delta RSSI 1..100 dB;
min_gap 0..60000 ms, max_gap 1000..3600000 i >=min_gap;
wiek GPS 100..10000 ms, HDOP x10 1..500, FULL gap 0..60000 ms.
HDOP nie jest dokładnością w metrach. Brak speed/course w RMC oznacza `nan`, a nie zmierzoną prędkość/kierunek 0.
Parser RMC/GGA nie dostarcza pomiaru
horizontal_accuracy_m: obecnie pole `nan`, nie przeliczaj HDOP na metry.

Zmiany dozwolone pomiędzy partiami (`scanning=0`, bez nieprzetworzonej partii).
W GUI: PAUSE_SCAN → poczekaj na zakończenie partii → SET_CONFIG → RESUME_SCAN.
Zmiana trybu zachowuje tabelę, liczniki i ostatnie zapisane baseline każdego AP.
UNIQUE nie zapisuje ponownie wcześniej zapisanego BSSID; FULL respektuje full_gap,
SMART porównuje do ostatniej zaakceptowanej obserwacji, także z innego trybu.

Zmiana baud tylko poza sesją i z pustą kolejką. ESP zapisuje konfigurację,
wysyła OK ze starą prędkością, czeka maks. 200 ms na TX, przełącza UART.
FAP po odebraniu całego OK przełącza prędkość i wykonuje HELLO 2.
Zmiana prędkości mostka może zgubić pierwszą ramkę; ponów HELLO/PING po timeout,
bez ponownego wykonywania SET_CONFIG. Test sprzętowy 0.3.1 potwierdził 115200,
460800 i 921600 baud z PING oraz rekordami skanu.
W razie zagubienia OK spróbuj skonfigurowaną prędkość i HELLO. Po restarcie
obowiązuje baud z NVS; w procedurze odzyskiwania sprawdź trzy obsługiwane prędkości.

## Obserwacje i agregaty

`OBSERVATION` ma 28 pól. Indeksy od zera:

| Indeks | Pole |
| --- | --- |
| 0..2 | FBR1, 0, OBSERVATION |
| 3..5 | boot, sequence, session |
| 6..8 | bssid (AA:BB:CC:DD:EE:FF), ssid_hex (0..32 bajtów), auth |
| 9..12 | channel, rssi, fix_valid (0/1), utc (`YYYY-MM-DD HH:MM:SS` lub `-`) |
| 13..19 | latitude, longitude, altitude_m, hdop, satellites, age_ms, csv_hex lub `-` |
| 20..23 | band (2/5), frequency_mhz, reason_mask, detected_monotonic_ms (uint64) |
| 24..27 | speed_kmh, course_deg, horizontal_accuracy_m, phy_mask |

Powody to maska: 1 FIRST, 2 DISTANCE, 4 RSSI, 8 INTERVAL, 16 METADATA,
32 GPS_RECOVERED, 64 FULL_SAMPLE; 128 RX_UNVERIFIED jest flagą pochodzenia danych, nie wyzwalaczem zapisu. `NO_GPS` reprezentuje fix_valid=0, współrzędne
obserwacji są wtedy `nan`. Nie zamieniaj ich na 0,0. UTC może być dostępne także
bez poprawnej geolokalizacji, jeśli RMC zawiera świeży czas. Gdy brak UTC, zachowaj
czas monotoniczny i brak czasu UTC w dzienniku; nie wstawiaj zegara PC jako GPS UTC.
Maska PHY: bit0 11b, bit1 11g, bit2 11n, bit3 11a, bit4 11ac, bit5 11ax.
Auth używa opisów `[ESS]`, `[WEP][ESS]`, WPA/WPA2/WPA3/OWE oraz
`[WAPI-PSK][ESS]`, `[DPP][ESS]`, `[WPA-EAP][ESS]`, `[WPA3-EAP][ESS]`,
`[WPA2-EAP][WPA3-EAP][ESS]` i `[WPA3-EAP-SUITE-B-192][ESS]`.
Nierozpoznane wartości (również zarezerwowane DUMMY w SDK) pozostają
`[UNKNOWN][ESS]`, bez zgadywania zabezpieczeń. Aplikacja zachowuje opis auth
przesłany przez firmware, także gdy nie zna jego etykiety.

W 0.4.0 pozycja i RSSI pochodzą z najsilniejszej odebranej ramki beacon/probe
response, z timestampem callbacku odbiorczego i historycznym fixem GNSS dostępnym
najpóźniej w tej chwili. Dane pozostają niezmienne podczas kolejkowania i retry.
Wynik SDK bez dopasowanej ramki jest zachowany, ale ma fix_valid=0, nan oraz
RX_UNVERIFIED; detected_monotonic_ms jest wtedy fallbackiem końca skanu.
Nie przypisuj mu GPS z dashboardu. Szczegóły i metryki opisano w sekcji 0.4.0 niżej.
Nazwy SSID są pobierane z walidowanych IE z jawną długością, również z osadzonym
NUL. Fallback SDK może być ucięty przy NUL. Pokazuj bajty bezpiecznie, nie jako printf.

`GET_AP slot` zwraca 29 pól:

```text
FBR1 id AP_DISCOVERED|AP_UPDATED boot session slot bssid ssid_hex band channel auth
last_rssi strongest_rssi mean_rssi detection_count observation_count first_seen_ms
last_seen_ms metadata_change_count last_fix_valid last_lat last_lon best_fix_valid
best_lat best_lon first_utc last_utc frequency_mhz phy_mask
```

AP_DISCOVERED/AP_UPDATED są odpowiedziami na żądanie, nie osobnym niezawodnym
strumieniem zdarzeń. FIRST w obserwacji umożliwia natychmiastowe dodanie AP do GUI.
W 0.3.0 sloty są gęste: 0..unique_aps-1, stabilne przez sesję, bez eksmisji.
Nie przechowuj wszystkich AP w RAM FAP. Po STOP pobierz unique_aps z GET_STATS,
strumieniowo pobierz tylko te sloty do journal, potem generuj CSV. Nie trzeba
odpytywać wszystkich pustych slotów aż do ap_capacity.
Lista wszystkich zajętych slotów po STOP daje końcowe agregaty obejmujące również detekcje
odfiltrowane. Nie próbuj wyliczyć detection_count/mean_rssi tylko z observations.
Najsilniejszy punkt pomiarowy nie jest estymatą miejsca instalacji AP.
Zmiany parametrów sygnalizują możliwą zmianę urządzenia/spoofing/reuse BSSID;
firmware nie gwarantuje stałej fizycznej tożsamości BSSID.

## Zdarzenia, statystyki i przepływ

Zdarzenia ID=0:

```text
READY version boot
SESSION_STARTED boot session
SESSION_STOPPED boot session queued_at_stop
SCAN_BEGIN boot scan_id
SCAN_DONE boot scan_id found processed truncated
STOPPED boot reason discarded_total
ERROR code [esp_error_number]
```

W revision 2 SCAN_DONE oznacza przetworzenie partii, **nie trwały zapis SD**.
SESSION_STOPPED też nie jest końcem opróżniania kolejki. Po STOP odbieraj nadal,
pytaj GET_STATUS aż running=0, scanning=0, queued=0 i stary STATUS.phase=IDLE.
Dopiero wtedy pobierz agregaty, zsynchronizuj metadata/stats i zamknij sesję.
PAUSE kończy bieżącą partię. Wstrzymane wyniki nie dostają nowszej pozycji GPS.

STATS ma 26 pól:

```text
FBR1 id STATS scope session total_detections unique_aps observations_saved
observations_filtered gps_valid_observations gps_invalid_observations wifi_2g_aps
wifi_5g_aps open_aps secured_aps scan_cycles scan_errors uart_retransmissions
uart_dropped_frames storage_errors queue_overflows table_overflows queued committed
free_heap minimum_free_heap
```

scope=0 sesja, 1 od uruchomienia; TOTAL nie jest trwały po restarcie. Wszystkie
liczniki zdarzeń są uint64. Definicje:

- total_detections: wyniki pobrane z SDK i przekazane do modelu; bez uciętych wyników.
- unique_aps: różne przyjęte BSSID w sesji, bez eksmisji; TOTAL to suma unikalnych
  z sesji, nie liczba globalnie różnych BSSID z wszystkich sesji.
- observations_saved: zaakceptowane do kolejki RAM; trwały zapis oznacza dopiero ACK.
- observations_filtered: pomiary bez spełnionego warunku zapisu.
- gps_valid/invalid_observations: rozbicie zaakceptowanych obserwacji według fixa.
- wifi_2g/5g_aps, open/secured_aps: klasyfikacja przy pierwszym wykryciu BSSID;
  zmiany parametrów nie zmieniają historycznej klasyfikacji licznika.
- scan_cycles: poprawnie pobrane partie; scan_errors: błędy startu, wyniku, odczytu, timeout.
- uart_retransmissions: wysłane ponowienia; uart_dropped_frames: błędne wejściowe
  ramki/identyfikatory i zdarzenia błędu UART (to nie dokładny licznik utraconych bajtów).
- storage_errors: zgłoszenia NACK STORAGE od FAP (nie pomiar SD przez ESP32).
- queue_overflows: odrzucone kandydaty przy pełnej kolejce; adapter normalnie
  zapobiega temu przez wstrzymanie pobierania wyników. Nie oznacza czasu przeciążenia.
- table_overflows: nieprzyjęte nowe BSSID przy pełnej tabeli, po czym adapter kończy sesję.

STATUS zachowuje 24 pola starszego formatu:
`FBR1 id STATUS boot running phase scan_id cycles observations acknowledged rows
no_gps truncated discarded fix_valid satellites hdop age_ms latitude longitude
altitude_m utc gps_ready wifi_ready`. Jego liczniki są uint32 od restartu, służą
zgodności; nowa aplikacja używa uint64 STATS. rows to ACK rekordów z CSV, nie odczyt SD.
STATUS i STATS są emitowane co sekundę (STATS po HELLO 2). STATE/GPS pobieraj w razie
potrzeby, bez zasypywania łącza. Brak statusu przez 5 s → stan łącza nieznany, PING.

## Niezawodny zapis SD i wznowienie

Odbiornik ma jeden niepotwierdzony rekord transportowy jednocześnie. ACK po 2000 ms,
do 3 identycznych retransmisji. Po timeout ESP zgłasza ACK_TIMEOUT, zachowuje RAM,
wstrzymuje transport, a skanowanie ostatecznie zatrzymuje się na pełnych buforach.
HELLO 2 / RESUME_LINK wznawia transmisję. Nie ma ciągłego zalewania UART przy braku FAP.
Sekwencja rośnie przez całe uruchomienie ESP, także między sesjami.
Identyfikator trwałego rekordu to **(boot, session, sequence)**.

1. Otwórz sesję na SD przed START. Nazwa `session_YYYYMMDD_HHMMSS`, przy braku UTC
   lub kolizji dodaj lokalny numer; nie nadpisuj istniejącej sesji.
2. Odbierz OBSERVATION i zweryfikuj CRC, kompletność, zakresy, MAC, hex, sesję i seq.
   Wartość nan dopuszczalna tylko dla opcjonalnych pól/braku pozycji.
3. Dopisz pojedynczy rekord do ograniczonego **events.journal** zawierającego pełną
   ramkę z CRC i identyfikatorem. Sprawdź liczbę zapisanych bajtów i wynik sync.
4. Dopiero po udanej synchronizacji wyślij `ACK boot session sequence`.
   Journal jest źródłem prawdy checkpointu — osobny plik checkpoint nie zapewnia
   atomowości z zapisem danych. Retransmisję już utrwalonego rekordu potwierdź ponownie,
   bez ponownego dopisywania. Nie deduplikuj obserwacji po BSSID!
5. Po awarii FAP przeskanuj journal do ostatniej kompletnej ramki z poprawnym CRC,
   usuń/odizoluj niepełny ogon, odtwórz committed z kompletnego dziennika. Dopiero
   wtedy HELLO 2, GET_STATUS, RESUME_LINK boot session committed.
   GET_STATUS.committed jest bazą numeracji nowej sesji, nie zawsze zerem.
6. Przy zmianie boot zamknij starą sesję jako interrupted; brakujący RAM nie wróci.
   Nie ACK rekordów poprzedniego boot. Nie wznawiaj automatycznie START.
7. Brak SD, pełna SD, krótki zapis albo błąd sync → bez ACK, NACK STORAGE,
   komunikat GUI. Po naprawie odzyskaj journal, RESUME_LINK, następnie RESUME_SCAN.
   Nie dopisuj w kółko częściowo zapisanego rekordu.
8. Po STOP i opróżnieniu wyników pobierz wszystkie sloty GET_AP oraz GET_STATS,
   zapisz je do journal i zsynchronizuj. Wygeneruj CSV z journal do plików tymczasowych,
   zsynchronizuj, dopiero potem zamień nazwy; zachowaj journal do odzyskania.
   Nie przepisuj access_points.csv po każdej detekcji.

Docelowe ścieżki SDK Flippera (`/ext` = microSD):

```text
/ext/apps_data/wardriving/config/settings.conf
/ext/apps_data/wardriving/sessions/session_YYYYMMDD_HHMMSS/
    metadata.json
    events.journal
    access_points.csv
    observations.csv
    wigle.csv
    stats.json
/ext/apps_data/wardriving/logs/errors.log
```

metadata: schema_version, firmware, protocol_revision, boot, session, konfiguracja,
czas start/stop, powód zakończenia, interrupted, counters, truncate/discard/overflow.
access_points.csv: jeden wiersz BSSID, końcowe GET_AP, oryginalny ssid_hex.
observations.csv: wszystkie unikalne zaakceptowane observation, pozycja pusta gdy
invalid, powód, sequence, UTC opcjonalne i monotonic_ms zawsze. Przykłady w
`docs/examples/`. Jeśli awaria wystąpi przed pobraniem końcowych GET_AP, zaznacz
agregaty jako niekompletne; sam journal obserwacji nie odtworzy odfiltrowanych detekcji.

ESP32-C5-WROOM-1U-N8R8 (LCSC C51950748) ma 8 MiB PSRAM i 8 MiB flash.
Firmware 0.3.0 włącza Quad PSRAM 40 MHz, test pamięci przy starcie i jawne
alokacje MALLOC_CAP_SPIRAM. Domyślnie: **10 000 AP**, indeks 20 000 wpisów
(load factor <=0.5) i **2048 obserwacji**. Obie pojemności są build-time:
menuconfig FEB_AP_CAPACITY (64..12000) i FEB_OBSERVATION_CAPACITY (32..8192).
Nie są ustawieniami NVS ani częścią SET_CONFIG; GET_INFO podaje rzeczywiste limity.
Przy braku/niemożności zaalokowania PSRAM: ERROR AP_MEMORY_UNAVAILABLE,
GET_INFO pokazuje pojemności 0, START zwraca ERR AP_MEMORY_UNAVAILABLE;
UART, GPS i diagnostyka pozostają dostępne. Nie ma cichego powrotu do 256 AP.
Sprzętowy test pamięci ESP-IDF może zatrzymać boot, jeśli wykryje wadliwą pamięć.

GET_MEMORY zwraca 18 pól, z prefiksem i ID:

```text
FBR1 id MEMORY storage_ready psram_initialized psram_total_bytes psram_free_bytes
psram_minimum_free_bytes psram_largest_free_block internal_free_bytes
internal_minimum_free_bytes internal_largest_free_block storage_reserved_bytes
ap_record_bytes ap_capacity ap_count queue_capacity queued
```

storage_reserved to suma trzech buforów (AP, indeks i obserwacje), bez narzutu
alokatora. Odczyt jest migawką, nie rezerwacją wolnej pamięci. Nie zastępuj tych
wartości sumarycznym free_heap ze STATS — rozróżniaj SRAM i PSRAM w diagnostyce.
Przy starcie 0.3.0 wymagaj storage_ready=1 i sensownych limitów, a nie wartości
zaszytych na stałe w FAP. Wsparcie starego 0.2.0: GET_MEMORY może zwrócić
UNKNOWN_COMMAND, wtedy użyj GET_INFO i pokaż brak rozszerzonej diagnostyki.

Przy AP_TABLE_FULL firmware nadal kończy sesję z jawnym błędem, pozostawia
agregaty i kolejkę. Aplikacja musi zaoferować archiwizację i nową sesję.
Pełna kolejka wstrzymuje kolejne skany zamiast nadpisywać rekordy. Długie
rozłączenie ogranicza nowe pomiary; zanik zasilania ESP usuwa niepotwierdzoną
PSRAM i agregaty. NVS przechowuje konfigurację, nie dziennik obserwacji.
Flash 8 MiB nie oznacza trwałości zawartości PSRAM — nie dodano dziennika flash.

## WiGLE CSV

Eksport tylko OBSERVATION z valid=1, poprawnym UTC i skończonymi współrzędnymi
WGS84 w zakresach. Nie uzupełniaj braków późniejszym fixem ani najlepszą pozycją AP.
Nie eksportuj UNKNOWN auth jako rozpoznanej metody szyfrowania. Przy użyciu csv_hex
sprawdź dekodowany wiersz niezależnym parserem i zgodność z polami obserwacji.

```csv
WigleWifi-1.4,appRelease=0.4.0,model=Feberis Pro,release=0.4.0,device=ESP32-C5,display=Flipper Zero,board=Feberis Pro,brand=Sapsan
MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type
```

Pole brand w metadanych WiGLE musi mieć dokładnie wartość **Sapsan**.
Bez BOM, LF, 11 kolumn, żadnych własnych kolumn. FirstSeen jest czasem obserwacji.
SSID cytowane, cudzysłów podwajany. Poprawne sekwencje UTF-8 zachowują oryginalne
bajty; błędne/niepełne sekwencje, sterujące bajty ASCII, DEL i backslash są
zapisywane jako `\xNN`. SSID puste lub złożone wyłącznie z NUL daje pustą nazwę
w WiGLE. Osadzony NUL w pozostałych nazwach pozostaje `\x00`.
`ssid_hex` w OBSERVATION, dzienniku i GET_AP zawsze zachowuje oryginalne bajty.
Dotyczy to również eksportu odtwarzanego przez aplikację z dziennika; przy
bezpośrednim zapisie `csv_hex` aplikacja zapisuje zdekodowane bajty bez zmian.
Brak rzeczywistej accuracy → puste AccuracyMeters, nigdy HDOP×5. Nieznana wysokość
→ puste AltitudeMeters. Rzeczywisty fix w punkcie 0,0 jest dozwolony; zabronione
jest wstawianie 0,0 jako zamiennika braku fixa. Obsługę pustej accuracy przez
aktualny importer WiGLE trzeba potwierdzić próbą importu na rzeczywistym pliku.

## Ekrany FAP i zadania implementacyjne

GUI nie może parsować UART ani zapisywać SD w callbacku. UART RX kopiuje do
ograniczonego bufora, worker parsuje i zapisuje journal, GUI czyta chroniony model.
Storage service powinien sprawdzać wyniki wszystkich operacji, ograniczać bufory,
logować błędy i umożliwiać odzyskanie sesji bez blokowania ekranu.

- Dashboard: ESP/łącze, GPS i wiek, satelity, HDOP, accuracy jeśli dostępna,
  prędkość, tryb/pasmo, unique, detections, saved, filtered, czas i SD.
  BOTH oznacza harmonogram obu pasm, nie równoległe radia. Nie pokazuj wybranego
  trybu pasma jako chwilowego kanału — standardowy skan nie raportuje go w toku.
- Networks: SSID/BSSID, RSSI, kanał, pasmo, auth; szczegóły przez GET_AP, agregaty,
  zmiany parametrów, first/last/best observation. Ograniczona lista w RAM.
- GPS: lat/lon/alt, satellites, HDOP, speed/course, valid i age; ignoruj pozycję
  przy valid=0. Nie udawaj accuracy w metrach z samego HDOP.
- Settings: SMART/FULL/UNIQUE, pasma, jawne Active, dwell, progi SMART, GPS age/HDOP,
  full_gap, baud; opcje zapisu/eksportu lokalne w settings.conf. Źródłem konfiguracji
  radia i SMART jest GET_CONFIG ESP, nie lokalne domyślne wartości GUI.
- Sessions: nowa/stop, lista katalogów, podsumowanie, eksport journal→CSV,
  incomplete/interrupted i jawne liczniki braków; worker dla I/O.

Na Dashboard/diagnostyce dodaj stan PSRAM, zajęte AP/limit i queued/queue_capacity.
W osobnym projekcie: użyj istniejącego manifestu i wersji SDK FAP; buduj jego
zweryfikowaną komendą. Nie ma manifestu ani toolchaina FAP w tym repozytorium,
więc kompilacja FAP i testy GUI/SD nie zostały tutaj wykonane.

## Próby odbiorcze na sprzęcie

Poza testami hosta z tego repozytorium należy sprawdzić: oba pasma i kanały kraju,
ukryte/binarne SSID, fix w ruchu i utratę GPS, UART przy 3 prędkościach,
spójność AP/observation po zmianie trybu, odłączenie FAP z pełną kolejką,
restart obu urządzeń, utratę ACK po sync, wyjętą/pełną SD i częściowy zapis,
archiwizację po AP_TABLE_FULL, długą pracę i minimum heap, import WiGLE.
Nie przenoś wyników testu firmware 0.1.0 na nową wersję.

## Poprawki skrajnych przypadków w 0.3.1

- RESUME_LINK nie zatwierdza rekordów, których jeszcze nie wysłano. Powtórne
  potwierdzenie ostatniego committed jest idempotentne, także po timeout.
- rows po wznowieniu rośnie tylko dla rzeczywiście przygotowanego CSV, zgodnie
  z pending_csv; sam valid GPS nie wystarcza (np. nierozpoznane zabezpieczenia).
- Watchdog skanu działa również po PAUSE/STOP i zgłasza błąd raz. Brak zdarzenia
  zakończenia od sterownika nadal blokuje nowy skan, aby nie uruchomić nakładającego
  się skanowania. Wymaga potwierdzenia zakończenia lub restartu ESP32.
- Parser NMEA wymaga składni dziesiętnej, ddmm/dddmm i spójnych pozycji RMC/GGA.
  Odrzuca heksadecymalne liczby, notację wykładniczą i whitespace w liczbach.
  Heurystyka spójności pozycji dopuszcza 100 m + ruch wynikający z prędkości
  i rozbieżności sekund; to kontrola sprzecznych zdań, nie estymacja accuracy.
- Równy najsilniejszy RSSI może uzupełnić brakujący wcześniej best_location,
  gdy pojawi się poprawna pozycja. Słabszy RSSI nie jest podpisywany jako
  lokalizacja wcześniejszego silniejszego pomiaru.
- Arytmetyka timeoutów i wieku GPS nie przepełnia signed int64 na skrajnych
  timestampach. Zegar cofnięty oznacza nieważny wiek/nieosiągnięty timeout.

Wyniki rzeczywistych prób: HARDWARE_TEST_2026-11-23_ACCEPTANCE.md.
Pełna karta SD i brak odbiorcy są testowane przez NACK/symulowanego odbiorcę
na komputerze. Nie jest to test zapisu SD ani GUI osobnego projektu FAP.


## Continuous driving profile in 0.4.0

This section is the required handoff to the separate FAP project. No FAP source
is implemented in this repository. Existing FBR1 revision 2 field layouts remain
compatible; the new command below is optional.

- Send `START_SCAN 0` or `START_SCAN` without an argument for driving. Both mean
  no deliberate post-batch delay. Legacy `START` accepts the same optional argument.
  Valid values are 0..60000 ms, including subsecond values. Do not keep an old
  hardcoded 1000/2000 ms minimum in the app: it still requests that delay from ESP.
- The argument is an **extra gap after processing the completed scan**, not a scan
  period. Display it as “Cycle gap”. Default to Continuous (0 ms); 50/100/250 ms
  are optional. Keep long gaps in diagnostic settings, outside driving presets.
- Keep passive dwell 120 ms initially. One radio visits channels sequentially;
  complete 2.4/5 GHz sweeps still take seconds. Do not label 0 ms as an infinite
  scan rate. Show the measured period if useful. SMART `min_gap_ms` and FULL
  `full_gap_ms` limit repeated observations per BSSID, not radio scans.
- The firmware now handles up to 512 SDK results per cycle (build range 16..1024),
  with the array in PSRAM, in slices of at most 64 records or 2 ms. Scan acquisition
  proceeds independently of normal per-record ACK latency while queue space is
  available. Preserve durable ACK semantics. On queue pressure scanning waits;
  never ACK before storage just to improve the displayed rate.
- `OBSERVATION.reason_mask` adds bit 128 **RX_UNVERIFIED**. This provenance bit
  never forces a record. It means no current-scan BSSID/channel reception metadata
  was available; `fix_valid=0`, coordinates are NAN, and `csv_hex=-`. Retain the
  record in the internal journal and exclude it from WiGLE export. Unknown reason
  bits must not invalidate otherwise supported records.
- Verified records pair the strongest captured beacon/probe response with the GNSS
  snapshot published at or before its reception. Software callback timestamps,
  receiver latency, GNSS sample rate, and position uncertainty still apply. A later
  fix cannot retroactively locate an earlier record. SSID is a byte string, not C
  text; the capture path preserves embedded NUL and hidden SSIDs.
- `ERROR RADIO_CAPTURE_INIT` indicates degraded reception provenance. Wi-Fi SDK
  scanning can continue with unlocalized results; show this diagnostic explicitly.
  Capture-queue/cache overflow is metadata loss, separately counted from accepted
  observation loss. Do not combine these counters.
- Keep 115200 as the compatible connection rate. Offer explicit coordinated
  460800/921600 negotiation; tests cover these rates, but the actual FAP and SD
  throughput still need validation. Do not switch either endpoint unilaterally.

`GET_SCAN_METRICS` returns exactly **26 tab-separated fields**, counting the
`FBR1 id SCAN_METRICS` prefix:

```text
FBR1 id SCAN_METRICS boot session requested_gap_ms dwell_ms capture_ready
radio_ms ingest_ms gap_ms max_gap_ms period_ms ingest_cpu_us
captured dropped malformed cache_overflows metadata_hits metadata_misses
queue_high_water backpressure_ms scan_buffer_bytes capture_cache_bytes
main_stack_free scan_capacity
```

- Timing fields describe the last completed radio/ingestion batch and latest
  consecutive scan start; they can refer to adjacent cycles while scanning.
  `gap_ms` is completion-to-next-start time, including processing, the requested
  gap, and any queue pressure. `period_ms` is start-to-start. Pause/resume resets
  the timing anchor so a deliberate pause does not inflate the next measured gap.
  First-cycle/unfinished fields may be zero. `ingest_cpu_us` accumulates actual
  slice wall time; `ingest_ms` also includes waiting and scheduling. A STOP or
  failed scan preserves the previous batch's `radio_ms` and `ingest_cpu_us`;
  these are replaced only after successful retrieval of the next scan results.
- `captured`, `dropped`, and `malformed` count received valid metadata frames,
  queue drops, and rejected beacon/probe-response bodies. The cache has
  `4 * scan_capacity` slots, with at most 50% occupied per scan. `cache_overflows`
  counts rejected insert attempts. These are not observation-drop counters.
- `metadata_hits/misses` count SDK results processed with/without matching metadata.
  `queue_high_water` is accepted observations waiting for durable ACK.
  `backpressure_ms` accumulates completed periods when that queue is full.
- Counts/high-water marks reset on a new session. `scan_buffer_bytes` and
  `capture_cache_bytes` are additional fixed PSRAM allocations;
  `GET_MEMORY.storage_reserved` retains its old three-buffer meaning.
  `main_stack_free` is the ESP-IDF stack high-water result in bytes.
- The firmware uses 240 MHz CPU, a 1000 Hz RTOS tick, Wi-Fi power saving disabled,
  and a 12 KiB main-task stack. This is a powered driving profile, not a low-power
  stationary preset. No wire field or NVS configuration layout was removed.
