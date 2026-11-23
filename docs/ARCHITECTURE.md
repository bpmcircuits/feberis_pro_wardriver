# Audyt, projekt i zakres implementacji 0.3.1

## Audyt stanu wejściowego

Zastano firmware 0.1.0, nie pusty projekt (stare AGENTS.md było nieaktualne).
`main/app_main.c` obsługiwał UART0, komendy FBR1, asynchroniczne skany IDF i transfer
pojedynczych AP z CRC oraz ACK. `gps_service.c` miał niezależne zadanie UART1,
mutex i parser RMC/GGA. `status_led.c` sterował WS2812 przez RMT.
`core/` zawierał NMEA, ramkowanie CRC, stop-and-wait i encoder WiGLE;
`tests/` — testy C i niezależny parser Python. Nie było aplikacji FAP ani obsługi SD
po stronie ESP. Dokument integracyjny był jedynym kontraktem dla drugiego projektu.

Radio używało oficjalnego IDF 6.0.0 dla C5: pasywne skany 120 ms/kanał, oba pasma,
ukryte SSID, brak nakładających się skanów. Limit 128 najsilniejszych AP na partię.
GPS przypisywano przy końcu partii, nie odbiorze beacona. GPS UART nie blokował się
od Wi-Fi. CSV bez fixa pomijano, ale accuracy wyliczano jako HDOP×5.

Nie było deduplikacji BSSID, agregatów, sesji logicznych, konfiguracji runtime/NVS
ani trybów SMART/FULL/UNIQUE. Następny skan czekał na wszystkie ACK; timeout i STOP
usuwały wyniki. RAM nie był trwały, wznowienie po awarii SD/FAP nie było rozwiązane.
Zachowano sprawdzone moduły, GPIO, UART, CRC i interfejs legacy.

## Projekt i wdrożone zmiany

`core/wardrive.[ch]` to testowalny bez SDK model C11: tabela hash BSSID o stałej
pojemności, agregaty AP, osobne immutable observations, kolejka pierścieniowa,
konfiguracja, walidacja GPS i statystyki uint64 sesji/uruchomienia.
Model nie wykonuje malloc: wd_engine_init wiąże przekazane przez adapter bufory
ze stałymi pojemnościami. Każdy przyjęty AP zachowuje stan przez całą sesję.
W 0.3.0 main/ap_memory.c rezerwuje raz przy starcie 10 000 rekordów AP,
20 000 pozycji indeksu uint32 i 2048 obserwacji, wyłącznie MALLOC_CAP_SPIRAM|8BIT.
Rekordy są gęste, stabilnie numerowane 0..ap_count-1, a oddzielny indeks hash
ma load factor <=0.5 również przy pełnej tabeli. Wyszukiwanie istniejących BSSID
nie zamienia się w skan całej tablicy po zapełnieniu rekordów.
Brak PSRAM/alokacji blokuje START z AP_MEMORY_UNAVAILABLE, zachowując diagnostykę.
Nie ma fallbacku do SRAM ani zmiany limitu bez wiedzy odbiorcy.
Polityka pojemności: bez eksmisji; nowy BSSID ponad limit kończy sesję w adapterze,
co zachowuje dokładną liczbę przyjętych unikalnych AP i baseline UNIQUE/SMART.
Limit jest jawny: domyślnie 10 000, konfigurowalne 64..12000, bez nieograniczonego wzrostu.

`app_main.c` jest adapterem IDF i jedynym właścicielem modelu, skanów i TX UART0.
Kolejka 2048 rekordów w PSRAM oddziela akwizycję od ACK. Gdy pełna, pozostają bounded wyniki
bieżącej partii i nie rozpoczyna się następny skan. GPS nadal ma niezależne zadanie.
Nie ma zadań zapisu na SD po stronie ESP32. Retransmisja używa tej samej ramki;
revision 2 ma ACK boot/session/sequence i wznowienie committed po rozłączeniu.
STOP revision 2 nie usuwa kolejki; legacy zachowuje stare anulowanie.
NVS zapisuje cały zwalidowany blob konfiguracji; błędy nie kasują NVS.

GPS waliduje checksum, RMC/GGA, datę, zakresy, wiek obu zdań i zgodność sekund.
Dodano speed/course RMC i osobne pole accuracy (niedostępne z RMC/GGA → NAN).
Polityka wieku/HDOP jest stosowana w chwili snapshotu partii; opóźnienie ACK nie
przeterminowuje historycznej pozycji obserwacji. Brak geolokalizacji oznacza NAN,
a nie starą pozycję. WiGLE odrzuca brak poprawnej pozycji/czasu.

SMART zawsze aktualizuje last_seen, latest, last_rssi, mean, strongest, count,
lokalizacje oraz licznik zmian metadanych. Następnie porównuje do **ostatnio
zaakceptowanej obserwacji**, nigdy do poprzedniej surowej detekcji.
Pierwszy rekord, zmiana metadanych i uzupełnienie GPS omijają min_gap.
Zwykłe reguły distance/RSSI/max_gap respektują min_gap. Haversine liczy metry,
RSSI ma Schmitt deadband: zmiana kierunku wymaga dodatkowych 2 dB, bez opóźnienia
pierwszej obserwacji. FULL ma niezależny limit full_gap; UNIQUE zapisuje raz na sesję.
Zmiana trybu nie resetuje baseline ani liczników. Metadata change nie oznacza,
że BSSID identyfikuje niezmienny fizyczny router.

Firmware korzysta z istniejących struktur ESP-IDF 6.0.0: `wifi_ap_record_t`,
`phy_11b/g/n/a/ac/ax`, `wifi_scan_config_t.scan_time`, `esp_wifi_set_band_mode`.
Sprawdzono nagłówki zainstalowanego SDK oraz kompilację docelową. Jedno radio skanuje
kolejno; passive jest domyślne, active jawne. W razie błędu startu/odczytu/zakończenia
następuje próba po 2 s; timeout bez potwierdzenia zakończenia zatrzymuje sesję,
aby nie dopuścić do równoległych skanów. Nie ma funkcji atakujących sieci.

## Pamięć modułu N8R8

Potwierdzony model użytkownika: ESP32-C5-WROOM-1U-N8R8, LCSC C51950748.
8 MiB flash jest już ustawione w sdkconfig i sdkconfig.defaults. PSRAM 8 MiB:
Quad, 40 MHz, boot init i test pamięci. SPIRAM_USE_CAPS_ALLOC zapobiega
przenoszeniu zwykłego malloc/stosów/buforów sterowników do pamięci zewnętrznej.
Nie włączono XiP, BSS bibliotek w PSRAM ani przenoszenia buforów Wi-Fi.
GPS i UART ISR nie czytają buforów AP; ich właścicielem jest główne zadanie.
Operacje NVS nie odwołują się do PSRAM, gdy cache jest wyłączony.
GPIO15 zajmuje SPICS1 i nie może służyć jako GPIO aplikacji w module R8.

Rozmiary z DWARF docelowego ELF: wd_ap_t=488, wd_observation_t=168,
wd_engine_t=376. Domyślne bufory PSRAM: 4 880 000 + 80 000 + 344 064 =
**5 304 064 bajty (5,058 MiB)**, bez narzutu alokatora. Statyczny engine
w SRAM zmalał ze 130 656 do 376 bajtów. GET_MEMORY pokazuje realne limity,
zarezerwowane bufory i osobno free/minimum/largest SRAM i PSRAM.
Flash 8 MiB nie jest RAM: obecna partycja aplikacji 1 MiB pozostaje wystarczająca;
nie dodano nieużywanych partycji ani automatycznego zapisu obserwacji do flash.

## Granice i dalsza praca

- Pozycja z końca skanu pozostaje przybliżeniem. Nie wdrożono timestampów i pozycji
  poszczególnych beaconów ani skanowania po jednym kanale z historią GPS.
- SDK nie udostępnia długości SSID w wynikach skanowania; osadzony NUL ucina SSID
  w adapterze. Wewnętrzny model i testy zachowują dowolne bajty z jawną długością.
- Domyślnie 10 000 AP i kolejka 2048, obie pojemności kontrolowane w menuconfig.
  Nie ma trwałości po utracie zasilania ani bezlimitowej bazy.
  Test hosta wstawia i ponownie wyszukuje 10 000 BSSID; na urządzeniu potwierdzono
  8 MiB PSRAM i rezerwację buforów. Fizyczna wydajność pod obciążeniem pozostaje do sprawdzenia.
- Konfiguracja jest trwała, obserwacje i TOTAL są tylko RAM; awaria zasilania usuwa je.
  Nie ma trwałego bufora ESP ani historii sesji w jego flash.
- RAM Flippera, dziennik SD, odzyskiwanie, eksport i GUI są zadaniem osobnego projektu,
  opisanym dokładnie w FLIPPER_INTEGRATION.md, nie implementacją w tym repozytorium.
- W 0.3.1 sprawdzono pasywne RF obu pasm, tryby zapisu, UART, PSRAM i fix GPS.
  Jazda testowa, wielogodzinna praca, import WiGLE i SD pozostają do sprawdzenia.

Źródła: [ESP-IDF C5 Wi-Fi API](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32c5/api-reference/network/esp_wifi.html),
[nagłówki oficjalnego SDK](https://github.com/espressif/esp-idf/blob/v6.0/components/esp_wifi/include/esp_wifi_types_generic.h),
[źródła WiGLE](https://github.com/wiglenet/wigle-wifi-wardriving).

## Przegląd skrajnych przypadków 0.3.1

Dodatkowy przegląd i regresje poprawiły zatwierdzanie niewysłanych rekordów przy
wznowieniu, zliczanie potwierdzonych CSV, kontrolę timeoutu podczas PAUSE/STOP,
uzupełnianie best_location przy równym RSSI i walidację NMEA. Wprowadzono
core/monotonic.h dla odejmowania czasu bez signed overflow. Parser porównuje
pozycje RMC/GGA i nie łączy geograficznie sprzecznych zdań mimo poprawnych checksum.
Nieznana prędkość/kierunek nie są przedstawiane jako pomiar 0.
Regresje obejmują skraje int64, datę i północ, leap day, granice ramek/pól,
walidację całej konfiguracji, binarne/ukryte SSID oraz wielokrotne zawijanie kolejki.
Szczegóły sprzętowe i ograniczenia: HARDWARE_TEST_2026-11-23_ACCEPTANCE.md.
