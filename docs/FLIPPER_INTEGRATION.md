# Feberis 0.3.1 — kontrakt dla osobnego projektu aplikacji Flipper Zero

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
| `START_SCAN interval_ms` / `START interval_ms` | `OK START`; interwał 1000..60000; nowa sesja tylko gdy poprzednia opróżniona |
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
32 GPS_RECOVERED, 64 FULL_SAMPLE. `NO_GPS` reprezentuje fix_valid=0, współrzędne
obserwacji są wtedy `nan`. Nie zamieniaj ich na 0,0. UTC może być dostępne także
bez poprawnej geolokalizacji, jeśli RMC zawiera świeży czas. Gdy brak UTC, zachowaj
czas monotoniczny i brak czasu UTC w dzienniku; nie wstawiaj zegara PC jako GPS UTC.
Maska PHY: bit0 11b, bit1 11g, bit2 11n, bit3 11a, bit4 11ac, bit5 11ax.
Auth używa opisów `[ESS]`, `[WEP][ESS]`, WPA/WPA2/WPA3/OWE; nierozpoznane
wartości `[UNKNOWN][ESS]`, bez zgadywania zabezpieczeń.

Pozycja i czas są zamrożone przy **zakończeniu skanu**, a nie przy zapisie SD czy
retransmisji. Standardowe API skanowania nie podaje timestampu każdego beacona.
Jeden skan obejmuje kolejno kanały obu pasm; podczas jazdy występuje błąd pozycji
związany z czasem całego skanu. To ograniczenie tej implementacji. Nie przedstawiaj
tej pozycji jako dokładnego punktu odbioru radiowego ani lokalizacji routera.
SSID z API ESP-IDF jest zakończony NUL i może być ucięty przy osadzonym NUL;
wewnętrzny model obsługuje długość i dowolne 32 bajty, ale nie odzyska brakujących
bajtów z API skanowania. Pokazuj nietekstowe SSID bezpiecznie, nie jako format printf.

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
WigleWifi-1.4,appRelease=0.3.1,model=Feberis Pro,release=0.3.1,device=ESP32-C5,display=Flipper Zero,board=Feberis Pro,brand=Sapsan
MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type
```

Pole brand w metadanych WiGLE musi mieć dokładnie wartość **Sapsan**.
Bez BOM, LF, 11 kolumn, żadnych własnych kolumn. FirstSeen jest czasem obserwacji.
SSID cytowane, cudzysłów podwajany, nietekstowe bajty/backslash jako `\xNN`.
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
