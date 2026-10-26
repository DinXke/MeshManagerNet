#include "BrownOut.h"

#include <string.h>
#include <stdlib.h>

#if defined(ESP32)
  #include <esp_attr.h>
#endif

// ------------------------------------------------------------- de LiPo-curve

/* Ontlaadcurve van een 1S LiPo-pouchcel bij lichte belasting, als meetpunten
 * waartussen stuksgewijs lineair geïnterpoleerd wordt.
 *
 * WAAR DIT VANDAAN KOMT. Dit is de gangbare 1S-LiPo-ontlaadtabel zoals die in
 * de RC- en modelbouwwereld circuleert en zoals Battery University hem voor
 * een kobalt-LiPo tekent, afgerond op 10 mV en aangevuld met onze eigen
 * waarnemingen aan deze node. De vorm is belangrijker dan de laatste procent:
 *
 *   - Tussen 4200 en 3700 mV zit ongeveer 80 % van de capaciteit. Daar daalt
 *     de spanning traag; een paar tiende volt is daar veel energie.
 *   - Onder 3700 mV komt de knie. Daar daalt de spanning snel en zit nog maar
 *     een fractie in. Dat is precies het gebied waar de oude lineaire formule
 *     "3000..4200 mV" er het verst naast zat: die zei bij 3600 mV nog 50 %,
 *     terwijl de cel daar op ~8 % staat.
 *   - 0 % staat op 3300 mV, niet op 3000. Onder 3,3 V is een LiPo leeg; wat
 *     daaronder zit, haal je er alleen uit ten koste van de cel. Die 300 mV
 *     is met opzet geen "nog te gebruiken lading" maar de reserve waarmee de
 *     node nog netjes kan gaan slapen in plaats van midden in een schrijf-
 *     actie dood te vallen.
 *
 * Dit is een RUSTCURVE. Onder belasting zakt de klemspanning, bij deze node
 * vooral tijdens een WiFi- of LoRa-zendpiek. Daarom werkt de rest van deze
 * module op een mediaan over acht metingen en niet op de laatste meting --
 * zie brnSample().
 *
 * De tabel staat hier en alleen hier. Wie hem wil bijstellen na echte
 * metingen aan dit paneel: alleen deze array aanpassen, aflopend op mV.
 */
static const struct { uint16_t mv; uint8_t pct; } BRN_CURVE[] = {
  { 4200, 100 },
  { 4150,  95 },
  { 4110,  90 },
  { 4080,  85 },
  { 4020,  80 },
  { 3980,  75 },
  { 3950,  70 },
  { 3910,  65 },
  { 3870,  60 },
  { 3850,  55 },
  { 3820,  50 },
  { 3800,  45 },
  { 3790,  40 },
  { 3770,  35 },
  { 3750,  30 },
  { 3730,  25 },
  { 3700,  20 },
  { 3660,  15 },
  { 3620,  10 },
  { 3540,   5 },
  { 3450,   2 },
  { 3300,   0 },
};
#define BRN_CURVE_N  ((int)(sizeof(BRN_CURVE) / sizeof(BRN_CURVE[0])))

/* Onder deze waarde gaan we ervan uit dat er helemaal geen deler aan hangt en
 * we dus niets meten, in plaats van dat de cel diep leeg is. Een bord dat niet
 * kan meten mag niet door deze module worden uitgeschakeld. */
#define BRN_NO_READING_MV  2000

int brn_percent(uint16_t mv) {
  if (mv < BRN_NO_READING_MV) return -1;          // geen bruikbare meting
  if (mv >= BRN_CURVE[0].mv) return 100;
  if (mv <= BRN_CURVE[BRN_CURVE_N - 1].mv) return 0;

  for (int i = 1; i < BRN_CURVE_N; i++) {
    if (mv >= BRN_CURVE[i].mv) {
      // mv ligt tussen punt i (lager) en punt i-1 (hoger)
      uint16_t lo_mv = BRN_CURVE[i].mv,     hi_mv = BRN_CURVE[i - 1].mv;
      uint8_t  lo_p  = BRN_CURVE[i].pct,    hi_p  = BRN_CURVE[i - 1].pct;
      uint32_t span  = (uint32_t)(hi_mv - lo_mv);
      if (span == 0) return lo_p;
      return (int)(lo_p + ((uint32_t)(mv - lo_mv) * (hi_p - lo_p) + span / 2) / span);
    }
  }
  return 0;
}

// ------------------------------------------------------------- instellingen

#define BRN_CFG_FILE      "/mmbrn.json"

/* Standaardwaarden. Zie de verantwoording bij elk veld in de CLI-hulptekst.
 *   write 15 %  stoppen met schrijven; met de nieuwe curve is dat ~3660 mV,
 *               ruim 350 mV boven echt leeg -- genoeg marge om een lopende
 *               schrijfactie af te maken en er geen nieuwe te beginnen.
 *   stop  10 %  (~3620 mV) gaan slapen. Bewust dicht bij de schrijfdrempel:
 *               tussen 15 % en 10 % zit op deze cel maar een half uur, en het
 *               enige wat in die tijd nog hoeft te gebeuren is niets.
 *   hyst   5 %  pas boven 20 % resp. 15 % weer vrijgeven. Een cel die op een
 *               drempel hangt is precies het geval waarin flapperen het
 *               duurst is.
 *   hold   5 min   zoveel opeenvolgende metingen onder stop voordat we slapen.
 *               Niet korter, en dat is nagerekend: bij drie minuten haalde
 *               een trage lader (3 mV per meting) de stijging niet op tijd en
 *               viel de node in slaap terwijl hij aan het opladen was. Vijf
 *               minuten geeft BRN_LOW_RISE_MV de ruimte om dat te zien, en is
 *               meteen het venster waarin de node na een wake bereikbaar is.
 *   wake  30 min   deep sleep tussen twee hermetingen.
 */
/* Per bord te overschrijven met een -D in platformio.ini. Een node die aan
 * netvoeding hangt wil de schrijfbescherming wel maar het slapen niet: daar
 * staat BRN_DEF_STOP_PCT op 0, en dan gaat hij alleen nog bij een werkelijk
 * lege cel slapen. */
#ifndef BRN_DEF_WRITE_PCT
  #define BRN_DEF_WRITE_PCT   15
#endif
#ifndef BRN_DEF_STOP_PCT
  #define BRN_DEF_STOP_PCT    10
#endif
#ifndef BRN_DEF_HYST_PCT
  #define BRN_DEF_HYST_PCT     5
#endif
#ifndef BRN_DEF_HOLD_MIN
  #define BRN_DEF_HOLD_MIN     5
#endif
#ifndef BRN_DEF_WAKE_MIN
  #define BRN_DEF_WAKE_MIN    30
#endif

static bool     _on        = true;
static uint8_t  _write_pct = BRN_DEF_WRITE_PCT;
static uint8_t  _stop_pct  = BRN_DEF_STOP_PCT;
static uint8_t  _hyst_pct  = BRN_DEF_HYST_PCT;
static uint16_t _hold_min  = BRN_DEF_HOLD_MIN;
static uint16_t _wake_min  = BRN_DEF_WAKE_MIN;

// ------------------------------------------------------------------- meting

/* Elke 10 s één meting, acht metingen in de ring. De mediaan daarvan is de
 * werkwaarde. Een gemiddelde zou ook kunnen, maar de mediaan gooit een enkele
 * diepe inzakking tijdens een zendpiek er helemaal uit in plaats van hem voor
 * een achtste mee te wegen -- en zo'n inzakking is nu juist de meting die je
 * niet wilt laten meetellen in een beslissing om af te sluiten. */
#define BRN_SAMPLE_MS   10000UL
#define BRN_RING        8

/* Trage referentie om laden aan te zien: elke BRN_TREND_MS leggen we de
 * huidige mediaan vast. Stijgt de mediaan sindsdien meer dan BRN_RISE_MV, dan
 * komt er stroom bij. 20 mV over vijf minuten is ruim boven de ruis van deze
 * ADC (10 bit over ~5,4 V is ~5 mV per stap) en ver onder wat een paneel in de
 * ochtendzon doet. */
#define BRN_TREND_MS   300000UL
#define BRN_RISE_MV         20

/* Tweede, scherpere laaddetectie, en die zit er omdat de eerste op zichzelf
 * NIET volstaat -- dat is nagerekend, niet aangenomen.
 *
 * De trend hierboven ijkt zichzelf elke vijf minuten opnieuw. Begint de
 * aftelling vlak na zo'n ijkpunt, dan heeft een traag stijgende spanning de
 * 20 mV nog niet gehaald tegen de tijd dat de aftelling afloopt: in een
 * doorrekening met een lader van 3 mV per meting viel de node in slaap terwijl
 * hij aan het laden was. Precies wat niet mag.
 *
 * Daarom houdt de aftelling zelf de LAAGSTE mediaan bij die hij gezien heeft.
 * Komt de spanning daar BRN_LOW_RISE_MV bovenuit, dan is er stroom bijgekomen
 * en begint de aftelling overnieuw. 15 mV is ongeveer drie stappen van deze
 * 10-bits ADC: ruim boven de ruis die de mediaan nog doorlaat, en met een
 * aanhoudtijd van vijf minuten ruim binnen bereik van zelfs een magere
 * winterzon. */
#define BRN_LOW_RISE_MV     15

static BrnReadMv  _read_mv = NULL;
static BrnExtPwr  _ext_pwr = NULL;
static BrnSleepFn _sleep   = NULL;
static fs::FS*    _fs      = NULL;

static uint16_t _ring[BRN_RING];
static uint8_t  _ring_n = 0;
static uint8_t  _ring_i = 0;
static unsigned long _next_sample = 0;

static uint16_t _mv      = 0;     // mediaan
static uint8_t  _pct     = 0;
static bool     _known   = false;
static bool     _charging = false;

static uint16_t _trend_mv = 0;
static unsigned long _trend_at = 0;

static BrnState _state = BRN_OK;
static uint16_t _low_count = 0;         // opeenvolgende metingen onder stop
static uint16_t _low_min_mv = 0;        // laagste mediaan tijdens deze aftelling
static bool     _inhibit = false;
static bool     _sleeping_announced = false;

static uint32_t _blocked = 0;
static const char* _last_blocked = "";

/* Overleeft deep sleep, niet een echte spanningsval. Daarmee weet de node na
 * het wakker worden of hij hier al eerder stond -- zonder daarvoor naar flash
 * te hoeven schrijven, wat juist het ding is dat we aan het vermijden zijn. */
/* LET OP: RTC-geheugen overleeft op ESP32 ook een gewone ESP.restart(), niet
 * alleen deep sleep. Alleen de magic bekijken zou dus na elke herstart zeggen
 * "ik kom uit brownout-slaap". Daarom een aparte vlag die we vlak voor het
 * slapen zetten en bij het wakker worden meteen wissen: die staat alleen aan
 * als wij hem daar zelf hebben neergelegd. */
#if defined(ESP32)
RTC_DATA_ATTR static uint32_t _rtc_magic = 0;
RTC_DATA_ATTR static uint8_t  _rtc_wakes = 0;
RTC_DATA_ATTR static uint8_t  _rtc_slept = 0;
#define BRN_RTC_MAGIC  0x42524E31UL    // "BRN1"
#endif
static bool    _woke_brownout = false;
static uint8_t _wake_count = 0;

static bool passedMs(unsigned long t) { return (long)(millis() - t) >= 0; }

static uint16_t ringMedian() {
  if (_ring_n == 0) return 0;
  uint16_t tmp[BRN_RING];
  memcpy(tmp, _ring, sizeof(uint16_t) * _ring_n);
  for (int i = 1; i < (int)_ring_n; i++) {   // insertion sort, hoogstens acht
    uint16_t k = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > k) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = k;
  }
  return tmp[_ring_n / 2];
}

static void ringPush(uint16_t mv) {
  _ring[_ring_i] = mv;
  _ring_i = (uint8_t)((_ring_i + 1) % BRN_RING);
  if (_ring_n < BRN_RING) _ring_n++;
}

/* Eén meting en de afgeleide waarden bijwerken. Kost op dit bord ~10 ms
 * (getBattMilliVolts() schakelt de deler in en wacht), dus niet vaker dan
 * BRN_SAMPLE_MS -- dat is ook waarom er geen "meet nu even" in de hete paden
 * zit. */
static void brnSample() {
  if (!_read_mv) return;
  ringPush(_read_mv());

  _mv = ringMedian();
  int p = brn_percent(_mv);
  _known = (p >= 0);
  _pct = (p < 0) ? 0 : (uint8_t)p;

  if (_trend_at == 0 || passedMs(_trend_at)) {
    _trend_at = millis() + BRN_TREND_MS;
    _trend_mv = _mv;
    _charging = false;             // nieuw ijkpunt: pas weer stijgen vanaf hier
  } else if (_trend_mv != 0 && _mv > _trend_mv + BRN_RISE_MV) {
    _charging = true;
  }
  if (_ext_pwr && _ext_pwr()) _charging = true;
}

// ------------------------------------------------------------- persistentie

/* Deze ene schrijfactie gaat BEWUST langs de poort heen.
 *
 * Het is tegenintuïtief, dus expliciet: juist als de cel laag staat wil je de
 * drempels nog kunnen bijstellen -- 'brownout stop 5' is een noodrem die je
 * niet kwijt mag zijn omdat de noodtoestand al is ingetreden. Het bestand is
 * één regel van ~70 bytes en wordt alleen door een mens aangeroepen, dus het
 * risicovenster is minimaal en het alternatief (een node die zijn eigen
 * noodinstelling niet meer kan opslaan) is erger. Onder BRN_HARD_FLOOR_MV
 * schrijven we alsnog niet: daar is niets meer te redden. */
#define BRN_HARD_FLOOR_MV  3400

static void brnSave() {
  if (!_fs) return;
  if (_known && _mv < BRN_HARD_FLOOR_MV) return;
  fs::File f = _fs->open(BRN_CFG_FILE, "w", true);
  if (!f) return;
  f.printf("{\"on\":%u,\"write\":%u,\"stop\":%u,\"hyst\":%u,\"hold\":%u,\"wake\":%u}",
           _on ? 1u : 0u, (unsigned)_write_pct, (unsigned)_stop_pct,
           (unsigned)_hyst_pct, (unsigned)_hold_min, (unsigned)_wake_min);
  f.close();
}

static long jsonNum(const String& s, const char* key, long def) {
  int i = s.indexOf(key);
  if (i < 0) return def;
  i = s.indexOf(':', i);
  if (i < 0) return def;
  return s.substring(i + 1).toInt();
}

static void brnLoad() {
  if (!_fs) return;
  fs::File f = _fs->open(BRN_CFG_FILE, "r");
  if (!f) return;                       // geen bestand: standaardwaarden
  String s = f.readString();
  f.close();

  _on        = jsonNum(s, "\"on\"",    1) != 0;
  long w     = jsonNum(s, "\"write\"", BRN_DEF_WRITE_PCT);
  long st    = jsonNum(s, "\"stop\"",  BRN_DEF_STOP_PCT);
  long hy    = jsonNum(s, "\"hyst\"",  BRN_DEF_HYST_PCT);
  long ho    = jsonNum(s, "\"hold\"",  BRN_DEF_HOLD_MIN);
  long wk    = jsonNum(s, "\"wake\"",  BRN_DEF_WAKE_MIN);

  if (w  >= 0 && w  <= 90) _write_pct = (uint8_t)w;
  if (st >= 0 && st <= 90) _stop_pct  = (uint8_t)st;
  if (hy >= 0 && hy <= 50) _hyst_pct  = (uint8_t)hy;
  if (ho >= 0 && ho <= 240)   _hold_min = (uint16_t)ho;
  if (wk >= 1 && wk <= 1440)  _wake_min = (uint16_t)wk;
  if (_stop_pct > _write_pct) _stop_pct = _write_pct;   // stop ligt nooit hoger
}

// -------------------------------------------------------------------- begin

void brn_begin(fs::FS& fs, BrnReadMv read_mv, BrnExtPwr ext_pwr, BrnSleepFn sleep_fn) {
  _fs = &fs;
  _read_mv = read_mv;
  _ext_pwr = ext_pwr;
  _sleep = sleep_fn;

#if defined(ESP32)
  if (_rtc_magic == BRN_RTC_MAGIC && _rtc_slept) {
    _woke_brownout = true;
    _wake_count = _rtc_wakes;
    _rtc_slept = 0;              // meteen wissen: geldt alleen voor deze boot
  } else {
    _rtc_magic = BRN_RTC_MAGIC;
    _rtc_wakes = 0;
    _rtc_slept = 0;
  }
#endif

  brnLoad();

  /* De ring in één keer vullen. Dit is de enige plek waar we acht metingen
   * achter elkaar doen, en met opzet: hier is de radio nog uit en WiFi nog
   * niet begonnen, dus dit is de rustigste -- en dus eerlijkste -- meting die
   * deze node vandaag doet. Daarna is de beslissing "mag ik schrijven" al
   * genomen voordat het eerste bestand opengaat. */
  if (_read_mv) {
    for (int i = 0; i < BRN_RING; i++) ringPush(_read_mv());
    _mv = ringMedian();
    int p = brn_percent(_mv);
    _known = (p >= 0);
    _pct = (p < 0) ? 0 : (uint8_t)p;
    _trend_mv = _mv;
    _trend_at = millis() + BRN_TREND_MS;
  }
  _next_sample = millis() + BRN_SAMPLE_MS;

  if (_on && _known && _pct <= _write_pct) {
    _state = (_pct <= _stop_pct) ? BRN_CRITICAL : BRN_BLOCK;
    Serial.printf("BrownOut: accu %u%% (%u mV) bij start -- schrijven naar flash geblokkeerd\n",
                  (unsigned)_pct, (unsigned)_mv);
  }
  if (_woke_brownout) {
    Serial.printf("BrownOut: wakker uit brownout-slaap (ronde %u), accu %u%% (%u mV)\n",
                  (unsigned)_wake_count, (unsigned)_pct, (unsigned)_mv);
  }
}

// ---------------------------------------------------------------------- loop

static void brnGoToSleep() {
  if (_sleeping_announced) return;
  _sleeping_announced = true;

  Serial.printf("BrownOut: accu %u%% (%u mV) -- deep sleep, hermeten over %u min\n",
                (unsigned)_pct, (unsigned)_mv, (unsigned)_wake_min);
  Serial.flush();

#if defined(ESP32)
  if (_rtc_wakes < 255) _rtc_wakes++;
  _rtc_slept = 1;
#endif

  if (_sleep) {
    _sleep((uint32_t)_wake_min * 60UL);   // keert niet terug
  }
  /* Geen sleep_fn meegegeven: niets doen. De module is dan een melder, niet
   * een uitschakelaar. Bewust geen while(1) hier -- dat zou de node juist
   * onbereikbaar maken, en dat is precies wat deze module moet voorkomen. */
}

void brn_loop() {
  if (!_read_mv) return;
  if (!passedMs(_next_sample)) return;
  _next_sample = millis() + BRN_SAMPLE_MS;

  brnSample();
  if (!_on || !_known) { _state = BRN_OK; _low_count = 0; return; }

  /* Toestand met hysterese. Omhoog (herstel) is altijd _hyst_pct strenger dan
   * omlaag, zodat een cel die op de drempel hangt niet heen en weer gaat. */
  switch (_state) {
    case BRN_OK:
      if (_pct <= _write_pct) _state = BRN_BLOCK;
#if defined(ESP32)
      /* Weer in orde: de reeks brownout-rondes is afgelopen. De teller op nul
       * zodat de volgende reeks weer bij 1 begint en in de status te lezen is
       * hoe lang deze slechte periode duurt, niet hoeveel er ooit waren. */
      else if (_rtc_wakes != 0) { _rtc_wakes = 0; _wake_count = 0; }
#endif
      break;
    case BRN_BLOCK:
      if (_pct >= _write_pct + _hyst_pct) _state = BRN_OK;
      else if (_pct <= _stop_pct)         _state = BRN_CRITICAL;
      break;
    case BRN_CRITICAL:
      if (_pct >= _stop_pct + _hyst_pct) _state = BRN_BLOCK;
      break;
  }

  if (_state != BRN_CRITICAL) { _low_count = 0; return; }

  /* Aftelling naar slapen. Niet op één dip: er moeten _hold_min minuten aan
   * opeenvolgende metingen onder de stopdrempel liggen. Elke meting die er
   * boven komt zet de teller terug op nul.
   *
   * Dat die aftelling tijd kost, is geen bijwerking maar het ontwerp: het is
   * meteen het venster waarin de node na een brownout-wake gewoon bereikbaar
   * is. Hij komt elke _wake_min minuten terug en staat dan _hold_min minuten
   * volledig in de lucht voordat hij opnieuw besluit te gaan slapen. */
  if (_charging) { _low_count = 0; return; }   // er komt stroom bij: nooit slapen
  if (_inhibit)  { _low_count = 0; return; }   // iemand is met de node bezig

  if (_low_count == 0) {
    _low_min_mv = _mv;                         // ijkpunt voor deze aftelling
  } else if (_mv < _low_min_mv) {
    _low_min_mv = _mv;                         // nog verder gezakt
  } else if (_mv >= _low_min_mv + BRN_LOW_RISE_MV) {
    /* De spanning is tijdens de aftelling duidelijk gestegen. Dat doet een
     * cel onder constante belasting niet; er komt stroom bij. Opnieuw
     * beginnen, en niet gaan slapen. */
    _charging = true;
    _low_count = 0;
    return;
  }

  if (_low_count < 0xFFFF) _low_count++;
  if (_low_count >= brn_low_needed()) brnGoToSleep();
}

// ------------------------------------------------------------------ de poort

bool brn_allow_write(const char* wie) {
  if (!_on || !_known) return true;     // uit, of we meten niets: niet tegenhouden
  if (_state == BRN_OK) return true;
  if (_charging) return true;           /* er komt stroom bij; dan is de kans op
                                         * een brownout tijdens deze write klein
                                         * en is niet kunnen opslaan erger */
  _blocked++;
  _last_blocked = (wie && *wie) ? wie : "?";
  /* Eén regel per weigering op de console. Dat is bewust niet gesmoord: het
   * zijn er hooguit een handvol per uur, en de hele reden dat deze module
   * bestaat is dat de vorige keer niemand iets zag gebeuren. */
  Serial.printf("BrownOut: schrijven '%s' geweigerd, accu %u%% (%u mV)\n",
                _last_blocked, (unsigned)_pct, (unsigned)_mv);
  return false;
}

// ------------------------------------------------------------------ uitlezen

uint8_t  brn_pct()             { return _pct; }
uint16_t brn_mv()              { return _mv; }
bool     brn_known()           { return _known; }
bool     brn_enabled()         { return _on; }
BrnState brn_state()           { return _state; }
bool     brn_charging()        { return _charging; }
uint32_t brn_blocked_count()   { return _blocked; }
const char* brn_last_blocked() { return _last_blocked; }
uint8_t  brn_write_pct()       { return _write_pct; }
uint8_t  brn_stop_pct()        { return _stop_pct; }
uint8_t  brn_hyst_pct()        { return _hyst_pct; }
uint16_t brn_hold_min()        { return _hold_min; }
uint16_t brn_wake_min()        { return _wake_min; }
uint16_t brn_low_count()       { return _low_count; }
uint8_t  brn_wake_count()      { return _wake_count; }
bool     brn_woke_from_brownout() { return _woke_brownout; }

bool brn_write_blocked() {
  if (!_on || !_known) return false;
  if (_charging) return false;
  return _state != BRN_OK;
}

uint16_t brn_low_needed() {
  unsigned long n = ((unsigned long)_hold_min * 60000UL) / BRN_SAMPLE_MS;
  if (n < 1) n = 1;
  return (uint16_t)n;
}

void brn_set_inhibit(bool aan) { _inhibit = aan; }

// -------------------------------------------------------------------- uitvoer

void brn_status_nl(char* uit, size_t max) {
  if (!uit || max == 0) return;
  if (!_on) { snprintf(uit, max, "brownout uit"); return; }
  if (!_known) {
    snprintf(uit, max, "brownout aan, geen accumeting (schrijven toegestaan)");
    return;
  }

  char staat[64];
  if (_state == BRN_CRITICAL && !_charging && !_inhibit) {
    snprintf(staat, sizeof(staat), "KRITIEK, slaapt over %u metingen",
             (unsigned)(brn_low_needed() > _low_count ? brn_low_needed() - _low_count : 0));
  } else if (_state == BRN_CRITICAL) {
    snprintf(staat, sizeof(staat), "kritiek maar %s", _charging ? "laadt" : "bezet");
  } else if (_state == BRN_BLOCK) {
    snprintf(staat, sizeof(staat), "laag");
  } else {
    snprintf(staat, sizeof(staat), "ok");
  }

  snprintf(uit, max,
           "%u%% (%u mV), %s, schrijven %s%s, drempels %u/%u%%+%u, "
           "%u min aanhouden, %u min slapen, %u geweigerd%s%s",
           (unsigned)_pct, (unsigned)_mv, staat,
           brn_write_blocked() ? "GEBLOKKEERD" : "toegestaan",
           _charging ? " (laadt)" : "",
           (unsigned)_write_pct, (unsigned)_stop_pct, (unsigned)_hyst_pct,
           (unsigned)_hold_min, (unsigned)_wake_min, (unsigned)_blocked,
           (_blocked && *_last_blocked) ? ", laatst: " : "",
           (_blocked && *_last_blocked) ? _last_blocked : "");
}

int brn_fill_json(char* uit, size_t max) {
  if (!uit || max == 0) return 0;
  int n = snprintf(uit, max,
    "\"brn\":{\"on\":%u,\"known\":%u,\"pct\":%u,\"mv\":%u,\"st\":%u,"
    "\"blocked\":%u,\"chg\":%u,\"refused\":%lu,\"who\":\"%s\","
    "\"wpct\":%u,\"spct\":%u,\"hyst\":%u,\"hold\":%u,\"wake\":%u,"
    "\"low\":%u,\"need\":%u,\"wokes\":%u}",
    _on ? 1u : 0u, _known ? 1u : 0u, (unsigned)_pct, (unsigned)_mv, (unsigned)_state,
    brn_write_blocked() ? 1u : 0u, _charging ? 1u : 0u,
    (unsigned long)_blocked, _last_blocked,
    (unsigned)_write_pct, (unsigned)_stop_pct, (unsigned)_hyst_pct,
    (unsigned)_hold_min, (unsigned)_wake_min,
    (unsigned)_low_count, (unsigned)brn_low_needed(), (unsigned)_wake_count);
  if (n < 0 || (size_t)n >= max) return 0;
  return n;
}

// ------------------------------------------------------------------------ CLI

static const char* brnArg(const char* a, const char* key) {
  size_t n = strlen(key);
  if (strncmp(a, key, n) != 0) return NULL;
  const char* p = a + n;
  if (*p != 0 && *p != ' ') return NULL;
  while (*p == ' ') p++;
  return p;
}

bool brn_handle_command(const char* cmd, char* reply, size_t reply_max) {
  if (cmd == NULL || memcmp(cmd, "brownout", 8) != 0) return false;
  if (cmd[8] != 0 && cmd[8] != ' ') return false;      // 'brownouts' is niet van ons

  const char* a = cmd + 8;
  while (*a == ' ') a++;
  const char* v;

  if (*a == 0) {                                       // kaal 'brownout' -> status
    brn_status_nl(reply, reply_max);
    return true;
  }

  if (strcmp(a, "on") == 0 || strcmp(a, "aan") == 0) {
    _on = true; brnSave();
    snprintf(reply, reply_max, "OK - brownout aan");
    return true;
  }
  if (strcmp(a, "off") == 0 || strcmp(a, "uit") == 0) {
    /* Uitzetten mag, maar niet stilletjes: dit is de bescherming die er
     * vorige keer niet was. */
    _on = false; _state = BRN_OK; _low_count = 0; brnSave();
    snprintf(reply, reply_max, "OK - brownout UIT; schrijven wordt niet meer geblokkeerd");
    return true;
  }
  if ((v = brnArg(a, "write")) != NULL) {
    if (*v == 0) { snprintf(reply, reply_max, "brownout write %u%%", (unsigned)_write_pct); return true; }
    long p = atol(v);
    if (p < 0 || p > 90) { snprintf(reply, reply_max, "Err - write 0..90 (%%)"); return true; }
    _write_pct = (uint8_t)p;
    if (_stop_pct > _write_pct) _stop_pct = _write_pct;
    brnSave();
    snprintf(reply, reply_max, "OK - niet meer schrijven onder %u%% (stop %u%%)",
             (unsigned)_write_pct, (unsigned)_stop_pct);
    return true;
  }
  if ((v = brnArg(a, "stop")) != NULL) {
    if (*v == 0) { snprintf(reply, reply_max, "brownout stop %u%%", (unsigned)_stop_pct); return true; }
    long p = atol(v);
    if (p < 0 || p > 90) { snprintf(reply, reply_max, "Err - stop 0..90 (%%)"); return true; }
    if (p > _write_pct) { snprintf(reply, reply_max, "Err - stop (%ld%%) mag niet boven write (%u%%)", p, (unsigned)_write_pct); return true; }
    _stop_pct = (uint8_t)p; brnSave();
    snprintf(reply, reply_max, "OK - slapen onder %u%%", (unsigned)_stop_pct);
    return true;
  }
  if ((v = brnArg(a, "hyst")) != NULL) {
    if (*v == 0) { snprintf(reply, reply_max, "brownout hyst %u%%", (unsigned)_hyst_pct); return true; }
    long p = atol(v);
    if (p < 0 || p > 50) { snprintf(reply, reply_max, "Err - hyst 0..50 (%%)"); return true; }
    _hyst_pct = (uint8_t)p; brnSave();
    snprintf(reply, reply_max, "OK - vrijgeven boven %u%% resp. %u%%",
             (unsigned)(_write_pct + _hyst_pct), (unsigned)(_stop_pct + _hyst_pct));
    return true;
  }
  if ((v = brnArg(a, "hold")) != NULL) {
    if (*v == 0) { snprintf(reply, reply_max, "brownout hold %u min (%u metingen)",
                            (unsigned)_hold_min, (unsigned)brn_low_needed()); return true; }
    long m = atol(v);
    if (m < 0 || m > 240) { snprintf(reply, reply_max, "Err - hold 0..240 (min)"); return true; }
    _hold_min = (uint16_t)m; brnSave();
    snprintf(reply, reply_max, "OK - %u min onder de drempel voordat hij slaapt (%u metingen)",
             (unsigned)_hold_min, (unsigned)brn_low_needed());
    return true;
  }
  if ((v = brnArg(a, "wake")) != NULL) {
    if (*v == 0) { snprintf(reply, reply_max, "brownout wake %u min", (unsigned)_wake_min); return true; }
    long m = atol(v);
    if (m < 1 || m > 1440) { snprintf(reply, reply_max, "Err - wake 1..1440 (min)"); return true; }
    _wake_min = (uint16_t)m; brnSave();
    snprintf(reply, reply_max, "OK - elke %u min wakker worden en hermeten", (unsigned)_wake_min);
    return true;
  }
  if (strcmp(a, "nu") == 0 || strcmp(a, "now") == 0) {
    /* Forceert een meting buiten het ritme om, zonder iets te beslissen.
     * Handig om na het verplaatsen van het paneel meteen te zien waar je
     * staat in plaats van tien seconden te wachten. */
    brnSample();
    brn_status_nl(reply, reply_max);
    return true;
  }

  snprintf(reply, reply_max,
           "Err - brownout [on|uit|write <pct>|stop <pct>|hyst <pct>|hold <min>|wake <min>|nu]");
  return true;
}
