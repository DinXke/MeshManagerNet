#pragma once

#include <Arduino.h>
#include <FS.h>

/* Brownout-bescherming: niet meer naar flash schrijven voordat de cel op is,
 * en netjes gaan slapen in plaats van midden in een schrijfactie doodvallen.
 *
 * WAAROM DIT BESTAAT. De dakrepeater is leeggelopen en kwam terug met
 * "Heltec Repeater" als naam en een adminwachtwoord dat niet meer werkte:
 * /prefs.json was gesneuveld. Dat is geen toeval maar het verwachte gedrag.
 * CommonCLI::savePrefs() doet remove() gevolgd door open()+write(); valt de
 * voeding daartussen weg, dan is het bestand weg of half. Hetzelfde geldt voor
 * elk ander bestand dat wij met "w" openen: SPIFFS wist de pagina voordat hij
 * de nieuwe inhoud heeft. Eén brownout op dat moment kost je de instelling.
 *
 * De maatregel is dus niet "beter schrijven" maar "op tijd stoppen met
 * schrijven". Onder brn_write_pct() weigert deze module elke niet-essentiele
 * schrijfactie, en die weigering is zichtbaar (teller + laatste weigeraar in
 * de status) in plaats van stil.
 *
 * DE WEG TERUG. Een node op een dak mag nooit een knopdruk nodig hebben.
 * board.powerOff() is daarom fout: dat is deep sleep zonder wektijd, en dan
 * staat hij daar tot iemand een ladder haalt, ook als de zon de volgende
 * ochtend gewoon terugkomt. Wij gebruiken deep sleep MET timer-wake: de node
 * wordt periodiek wakker, meet opnieuw, en blijft op als er geladen is. Op
 * ESP32 is wakker worden uit deep sleep een volledige herstart, dus hij komt
 * terug via de normale boot en de normale metingen -- er is geen half-wakkere
 * toestand om te onderhouden.
 *
 * Wat de node WEL zelf kan: terugkomen zodra het paneel weer levert, zonder
 * dat iemand erbij hoeft. Wat hij NIET kan: iets doen tijdens de slaap. In die
 * periode is hij over LoRa noch WiFi bereikbaar. Dat is de prijs; het
 * alternatief was een kapot bestandssysteem.
 */

// --------------------------------------------------------------- de curve

/* Spanning -> laadtoestand van een 1S LiPo-pouchcel, stuksgewijs lineair
 * tussen de meetpunten in brn_percent(). Geeft -1 bij een onbruikbare meting.
 *
 * Dit vervangt de lineaire 3000..4200 mV die hier stond. Die was niet een
 * beetje onnauwkeurig maar systematisch te optimistisch in precies het gebied
 * waar het ertoe doet: bij 3600 mV zei hij 50 %, terwijl de cel daar nog
 * ongeveer 8 % heeft. Een node die "50 %" meldt en twee uur later dood is,
 * is hoe wij hier beland zijn.
 */
int brn_percent(uint16_t mv);

// ------------------------------------------------------------------- types

/* Wat de node op dit moment van de cel vindt. Loopt maar één kant op zonder
 * hysterese; zie brn_loop(). */
enum BrnState {
  BRN_OK = 0,        // niets aan de hand
  BRN_BLOCK,         // onder de schrijfdrempel: niet-essentiele writes geweigerd
  BRN_CRITICAL       // onder de stopdrempel, aftelling naar slapen loopt
};

/* De twee haken die de module van de omgeving nodig heeft. Zo blijft dit
 * bestand bordonafhankelijk en kan dezelfde module op de bureau-node draaien.
 *   read_mv   verplicht; ruwe celspanning in mV (board.getBattMilliVolts())
 *   ext_pwr   optioneel; true = netvoeding/lader aanwezig. Mag NULL zijn:
 *             op de Heltec V4 bestaat die meting niet (isExternalPowered()
 *             geeft daar altijd false), dus daar leiden we het af uit een
 *             stijgende spanning -- zie brn_loop().
 *   sleep_fn  optioneel; wordt aangeroepen met het aantal seconden deep sleep
 *             en hoort NIET terug te keren. NULL = alleen melden, nooit
 *             slapen (handig om het gedrag te bekijken zonder risico).
 */
typedef uint16_t (*BrnReadMv)();
typedef bool     (*BrnExtPwr)();
typedef void     (*BrnSleepFn)(uint32_t secs);

/* Leest de instellingen en vult de meetring meteen met een reeks metingen.
 * Bewust zo vroeg mogelijk in setup() aanroepen, VOORDAT de radio en WiFi aan
 * gaan: dat is de rustigste meting die deze node ooit doet, zonder zendpiek
 * eroverheen. Daarmee is de beslissing "mag ik wel schrijven vandaag" al
 * genomen voordat het eerste bestand opengaat. */
void brn_begin(fs::FS& fs, BrnReadMv read_mv, BrnExtPwr ext_pwr, BrnSleepFn sleep_fn);

/* Eén hap per ronde, blokkeert nooit. Meet hoogstens eens per
 * BRN_SAMPLE_MS en doet verder alleen vergelijkingen. */
void brn_loop();

// ------------------------------------------------------------------ de poort

/* DE POORT. Elke niet-essentiele schrijfactie begint hiermee:
 *
 *     if (!brn_allow_write("prefs")) return;
 *
 * `wie` is een korte vaste string (geen gekopieerde buffer!) die in de status
 * verschijnt als laatste geweigerde schrijver, zodat een geweigerde write
 * terug te vinden is en niet stil faalt.
 *
 * Geeft altijd true zolang de module uitstaat, de meting onbruikbaar is of er
 * geladen wordt: liever een schrijfactie te veel dan een node die zijn
 * instellingen niet meer kwijt kan omdat de ADC stuk is. */
bool brn_allow_write(const char* wie);

// ------------------------------------------------------------------- uitlezen

uint8_t  brn_pct();            // gemiddelde laadtoestand, 0 als onbekend
uint16_t brn_mv();             // gemiddelde celspanning in mV
bool     brn_known();          // false = geen bruikbare meting
bool     brn_enabled();
bool     brn_write_blocked();  // true = brn_allow_write() weigert nu
BrnState brn_state();
bool     brn_charging();       // spanning stijgt, of ext_pwr zegt ja
uint32_t brn_blocked_count();  // aantal geweigerde schrijfacties sinds boot
const char* brn_last_blocked();// naam van de laatste geweigerde, "" als geen

/* Drempels, zodat de status ze kan tonen zonder ze te dupliceren. */
uint8_t  brn_write_pct();
uint8_t  brn_stop_pct();
uint8_t  brn_hyst_pct();
uint16_t brn_hold_min();
uint16_t brn_wake_min();

/* Hoeveel opeenvolgende metingen al onder de stopdrempel zitten, en hoeveel
 * er nodig zijn. Zo is in de status te zien dat de aftelling loopt. */
uint16_t brn_low_count();
uint16_t brn_low_needed();

/* Aantal keren dat deze node al uit brownout-slaap is gewekt. Staat in
 * RTC-geheugen: overleeft deep sleep, maar niet een echte spanningsval. Een
 * waarde > 0 na een herstart betekent dus "ik ben hier al eerder geweest en
 * de voeding is sindsdien niet weggevallen". */
uint8_t  brn_wake_count();
bool     brn_woke_from_brownout();

/* Iemand is met de node bezig ('wifi on 30', een OTA, een restore): dan nooit
 * slapen, wat de cel ook zegt. Buitengesloten worden kost meer dan de lading.
 * De aanroeper zet dit elke ronde opnieuw; het is geen toggle. */
void brn_set_inhibit(bool aan);

// -------------------------------------------------------------------- uitvoer

/* Eén Nederlandse regel voor de CLI-status. */
void brn_status_nl(char* uit, size_t max);

/* De velden voor de statistiek-JSON, zonder omsluitende accolades en zonder
 * komma ervoor of erna. Geeft het aantal geschreven tekens. */
int brn_fill_json(char* uit, size_t max);

/* De CLI: 'brownout', 'brownout on|uit', 'brownout write <pct>',
 * 'brownout stop <pct>', 'brownout hyst <pct>', 'brownout hold <min>',
 * 'brownout wake <min>', 'brownout nu'. Geeft false als het commando niet
 * voor ons is, zodat de aanroeper kan doorvallen. */
bool brn_handle_command(const char* cmd, char* reply, size_t reply_max);
