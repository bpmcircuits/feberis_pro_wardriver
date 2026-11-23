# Feberis Pro 0.3.1 — test urządzenia i przypadków skrajnych

Data: 2026-11-23. ESP32-C5 rev. 1.0, moduł N8R8, 8 MiB flash / 8 MiB PSRAM.
USB–UART Flippera: /dev/cu.usbmodemflip_Tutel1. Wszystkie skany w tych próbach były
pasywne. Odbiornik testowy to PC, nie aplikacja FAP i nie karta SD Flippera.

## Wynik końcowy

- **15/15 scenariuszy sprzętowych: PASS**, czas 113.47 s.
- **7/7 zestawów hosta: PASS**, ASan/UBSan, w tym 10 000 AP i kolejka 2048.
- Clang --analyze: pięć modułów core, bez diagnostyki.
- Kompilacja ESP-IDF 6.0.0 dla C5 i zapis trzech obrazów: poprawne, hashy zapisu
  potwierdzone przez esptool. Obraz aplikacji 804480 B.
- **153 unikalne rekordy** odebrane do dziennika hosta,
  **146 wierszy WiGLE** sprawdzonych niezależnym csv.reader.
- **0 ramek z błędnym CRC** odebranych z ESP. Celowo błędny CRC wysłany do ESP
  został odrzucony i nie uruchomił skanowania.
- Brak nieoczekiwanego restartu przed celową próbą restartu z kolejką.
- Po testach przywrócono konfigurację, skanowanie zatrzymane, kolejka pusta.

SHA-256 końcowego obrazu aplikacji:
`538de2564b82eda4ccf34e98360d9f92dbd86e9f90798067fd890955756388aa`.
Ten sam hash znajduje się w raportach acceptance i soak oraz aktualnym build.
Zmiana brand WiGLE na **Sapsan** dotyczy nagłówka pliku, szablonu hosta i kontraktu
FAP. Nagłówek zapisuje Flipper; ESP przesyła wiersze. Po tej zmianie wykonano
ponownie testy CSV i kompilację: plik ESP .bin pozostał identyczny z testowanym.

## Scenariusze fizycznego urządzenia

| Przypadek | Wynik |
| --- | --- |
| Błędny CRC, ramka ponad limit i nieprawidłowe argumenty | PASS |
| Pasywne 2,4 GHz, pauza i wznowienie | PASS |
| Pasywne 5 GHz, pauza i wznowienie | PASS |
| Pasywne oba pasma, pauza i wznowienie | PASS |
| UNIQUE i zgodność liczników/agregatów | PASS |
| SMART i zgodność liczników/agregatów | PASS |
| FULL z limitem próbkowania | PASS |
| Zmiana trybu wewnątrz sesji bez resetu baseline | PASS |
| GPS odrzucony przez politykę, następnie poprawny fix i uzupełnienie | PASS |
| 3 identyczne retry, ACK_TIMEOUT, STOP zachowujący kolejkę i RESUME_LINK | PASS |
| NACK STORAGE bez utraty niezatwierdzonej kolejki | PASS |
| UART 460800: 10 PING i rekordy skanu | PASS |
| UART 921600: 10 PING i rekordy skanu | PASS |
| UART 115200: 10 PING i rekordy skanu | PASS |
| Restart z kolejką, trwałość NVS i odrzucenie starego boot/ACK | PASS |

Liczby wykryć zależą od chwilowej widoczności sieci. Przykładowo w końcowym
przebiegu UNIQUE: 23 detekcje, 11 obserwacji; SMART: 23 detekcje, 11 obserwacji.
FULL bez limitu spełniał observations_saved=total_detections. Po pauzie sumy
GET_AP detection_count/observation_count zgadzały się ze STATS. Zmiana FULL →
UNIQUE → SMART w tej samej sesji zachowała baseline, numer sesji i liczniki.

Odbiornik zapisywał pełną ramkę w tymczasowym journal i wykonywał fsync przed ACK.
Ponowienie tego samego (boot,session,sequence) nie dopisywało drugiego rekordu.
Porównano hash całych retransmisji — również pozycja, czas, RSSI i CSV pozostały
niezmienione. Testy nie przechowują SSID, BSSID ani współrzędnych w raportach;
tymczasowy dziennik odbiorcy jest usuwany po zakończeniu testu.

NACK STORAGE symulował odmowę zapisu po stronie odbiorcy. Odbiornik nie potwierdzał
niezapisanego rekordu. ESP zatrzymał transport/skany i zachował kolejkę;
po RESUME_LINK oraz RESUME_SCAN dane znów były zapisywane i potwierdzane.
Brak odbiorcy UART symulowano niewysyłaniem ACK przy zachowanym zasilaniu/przewodzie.
Nie odłączano fizycznie kabla USB.

Próbę GPS wykonano przez chwilową politykę HDOP 0,1 i wieku 100 ms, następnie
przywrócenie dopuszczalnego HDOP i świeżości. Wi-Fi działało bez poprawnej
geolokalizacji, współrzędne były NAN, CSV nie powstawało; po przywróceniu polityki
zapisano GPS_RECOVERED. Nie wymuszano fizycznego odłączenia modułu GNSS ani utraty
sygnału satelitów. Nie jest to test dokładności podczas jazdy.

Po celowym resecie ESP z niepustą kolejką RAM/PSRAM były puste, session=0 i
running=0. NVS zachowało konfigurację. RESUME_LINK i ACK poprzedniego boot zostały
odrzucone. To potwierdza **ulotność PSRAM**, nie trwałość niepotwierdzonych danych.
Część danych mogła już być trwale zapisana przez odbiorcę przed utratą ACK;
liczba rekordów w kolejce przed resetem nie oznacza liczby utraconych w dzienniku.

## Pamięć w 12 kolejnych cyklach

Dodatkowo wykonano 12 cykli FULL w 2,4 GHz, 40.01 s,
82 detekcje i 82 wiersze CSV. W każdym pomiarze po
pauzie i opróżnieniu partii było dokładnie **195 208 B wolnej SRAM** i
**3 070 748 B wolnej PSRAM**, bufory aplikacji stale **5 304 064 B**.
Nie było scan_errors, queue_overflows ani błędów odebranych ramek.
Nie zaobserwowano narastającego zużycia pamięci w tej krótkiej próbie; nie zastępuje
ona testu wielogodzinnego ani próby 10 000 fizycznych sieci.

## Znalezione i naprawione przypadki w kodzie

1. RESUME_LINK mógł usunąć głowę kolejki, której nie nadano. Nowe wd_resume_ack
   wymaga aktualnej sesji i ostatniego faktycznie wysłanego sequence; committed
   jest idempotentny. Test odrzuca checkpoint przyszłego/niewysłanego rekordu.
2. Wznowienie liczyło rows na podstawie GPS valid, mimo że CSV mogło nie istnieć.
   Zliczanie korzysta z pending_csv dla potwierdzanej transmisji.
3. PAUSE/STOP omijały watchdog trwającego skanu. Kontrola działa przed blokadą
   kolejnych skanów i raportuje timeout jeden raz, bez nakładania skanów.
   Nie wymuszano zawieszenia sterownika na sprzęcie: ta zmiana jest potwierdzona
   przeglądem przepływu sterowania; testy hosta obejmują arytmetykę deadline.
4. Odejmowanie signed int64 w wieku GPS/ACK mogło przepełnić się dla skrajnych
   timestampów. Zastosowano bezpieczną różnicę unsigned i kontrolę cofnięcia czasu.
5. Parser strtod przyjmował hex, exponent i whitespace; RMC/GGA nie porównywały
   położeń. Wymagana jest składnia dziesiętna ddmm/dddmm i spójne lokalizacje.
   Brak opcjonalnego speed/course jest oznaczany NAN, nie fikcyjnym pomiarem 0.
6. Równy strongest RSSI nie uzupełniał brakującego best_location po powrocie GPS.
   Teraz taki pomiar może uzupełnić pozycję; słabszy nie jest przypisywany do
   wcześniejszego silniejszego RSSI.

Testy regresyjne obejmują dodatkowo int64 MIN/MAX, UINT32_MAX, północ i rok,
leap day, biegun/date line, 1535 bajtów i 48 pól oraz przekroczenia, 21 złych
wartości konfiguracji, binarne/ukryte SSID, wyczerpanie sequence, 10 000 obrotów
kolejki i nieprawidłowe inicjalizacje pamięci.

Pierwszy przebieg na 0.3.0 zakończył się timeoutem pierwszego GET_INFO po zmianie
baud. W narzędziu dodano retry wyłącznie idempotentnych zapytań odczytowych,
obsługujące przejściowy brak gotowości mostka. Ustawienia przywrócono także po tym
błędzie. Końcowy przebieg 0.3.1 potwierdził trzy prędkości i 30 roundtrip PING
oraz rekordy skanów. Raport pierwotnego błędu zachowano jako acceptance-baseline.json.

## Ograniczenia i odtwarzanie

Do sprawdzenia pozostają: GUI/FAP i rzeczywisty zapis microSD, pełna/wyjęta karta,
import na serwerze WiGLE, jazda z GPS, wielogodzinne obciążenie i wymuszone awarie
sterowników/NVS. Tysiące AP i pełną kolejkę sprawdzono modelowo na hoście; radio
widziało kilkanaście sieci. Nie dodano aktywnych funkcji atakujących sieci.

```sh
# W aktywowanym środowisku ESP-IDF z pyserial:
python tools/stress_test.py /dev/cu.usbmodemflip_Tutel1 --output /tmp/feberis-acceptance.json
python tools/stress_test.py /dev/cu.usbmodemflip_Tutel1 --soak-cycles 12 --output /tmp/feberis-soak.json
```

Dane: [acceptance](../artifacts/hardware/2026-11-23-acceptance.json),
[soak](../artifacts/hardware/2026-11-23-soak.json),
[podsumowanie walidacji](../artifacts/validation/2026-11-23-acceptance.json).
