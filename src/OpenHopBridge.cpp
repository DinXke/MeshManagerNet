#include "OpenHopBridge.h"
#include "MyMesh.h"
#include "MeshManagerNet.h"   /* MESHMANAGER_NAME/VERSION voor GET_VERSION */
#include <WiFi.h>
#include <string.h>

/* Zie OpenHopBridge.h voor het waarom en de draadvorm. */

#define OHB_CFG_PATH      "/openhop.cfg"
#define OHB_PORT_DEFAULT  5055
#define OHB_TOKEN_MAX     33
#define OHB_MAX_PAYLOAD   264
#define OHB_RX_RING       6
/* Failover: hoe lang de gast weg moet zijn voordat wij het repeteren
 * overnemen, en hoe lang hij terug moet zijn voordat we het teruggeven. Een
 * wifi-hik van een paar tellen hoort geen omschakeling te worden. */
#define OHB_FO_HOLD_DEF   60
#define OHB_FO_HOLD_MIN   10
#define OHB_FO_HOLD_MAX   3600
/* Een verbonden host die niets zegt is normaal: hun driver praat alleen als
 * er iets te zenden valt. Pas na dit veelvoud van de wachttijd zonder enig
 * frame noemen we hem vastgelopen. */
#define OHB_FO_STALL_MULT 10

/* Het protocol; namen letterlijk uit protocol_constants.py. */
#define OH_SYNC                 0xAA
#define OH_CMD_TX_REQUEST       0x01
#define OH_CMD_SET_CONFIG       0x10
#define OH_CMD_GET_CONFIG       0x11
#define OH_CMD_STATUS_REQ       0x20
#define OH_CMD_NOISE_REQ        0x22
#define OH_CMD_CAD_REQUEST      0x30
#define OH_CMD_RX_START         0x31
#define OH_CMD_SET_CAD_PARAMS   0x34
#define OH_CMD_SET_WIFI         0x41
#define OH_CMD_AUTH             0x50
#define OH_CMD_WIFI_RESET       0x60
#define OH_CMD_GET_WIFI         0x61
#define OH_CMD_GET_VERSION      0x70
#define OH_CMD_PING             0xFF
#define OH_CMD_TX_DONE          0x02
#define OH_CMD_TX_FAIL          0x03
#define OH_CMD_RX_PACKET        0x04
#define OH_CMD_CONFIG_RESP      0x12
#define OH_CMD_STATUS_RESP      0x21
#define OH_CMD_NOISE_RESP       0x23
#define OH_CMD_CAD_RESP         0x32
#define OH_CMD_RX_STARTED       0x33
#define OH_CMD_CAD_PARAMS_RESP  0x35
#define OH_CMD_AUTH_OK          0x51
#define OH_CMD_VERSION_RESP     0x71
#define OH_CMD_ERROR            0xFE
#define OH_CMD_PONG             0xFF
#define OH_ERR_CRC              0x01
#define OH_ERR_INVALID_CMD      0x02
#define OH_ERR_PAYLOAD_TOO_BIG  0x05
#define OH_ERR_INVALID_CONFIG   0x06
#define OH_ERR_UNAUTHORIZED     0x09

#define OHB_LOG(...) do { Serial.printf("[oh] " __VA_ARGS__); Serial.println(); } while (0)

// --------------------------------------------------------------------- staat
static fs::FS*    _fs = nullptr;
static MyMesh*    _mesh = nullptr;
static bool       _on = false;
static uint16_t   _port = OHB_PORT_DEFAULT;
static char       _token[OHB_TOKEN_MAX] = {0};
static bool       _fo_on = false;
static uint16_t   _fo_hold = OHB_FO_HOLD_DEF;

static WiFiServer _server(OHB_PORT_DEFAULT);
static WiFiClient _cl;
static bool       _listening = false;
static bool       _authed = false;
static char       _client_ip[16] = {0};

static uint8_t    _in[OHB_MAX_PAYLOAD + 8];
static size_t     _in_len = 0;

static uint8_t    _rx[OHB_RX_RING][OHB_MAX_PAYLOAD];
static uint16_t   _rx_len[OHB_RX_RING];
static uint8_t    _rx_head = 0, _rx_count = 0;

static uint32_t   _n_rx = 0, _n_rx_drop = 0, _n_tx = 0, _n_tx_ref = 0, _n_fo = 0;
static unsigned long _guest_seen = 0, _guest_back = 0, _guest_gone = 0;
static bool       _fo_taken = false;
static char       _note[80] = "uit";

// ------------------------------------------------------------------- framing
static uint16_t crc16(const uint8_t* d, size_t n, uint16_t crc = 0xFFFF) {
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static bool sendFrame(uint8_t cmd, const uint8_t* payload, size_t len) {
  if (!_cl.connected()) return false;
  uint8_t hdr[4] = { OH_SYNC, cmd, (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF) };
  uint16_t crc = crc16(&hdr[1], 3);
  if (payload && len) crc = crc16(payload, len, crc);
  if (_cl.write(hdr, 4) != 4) return false;
  if (payload && len && _cl.write(payload, len) != len) return false;
  uint8_t tail[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };
  return _cl.write(tail, 2) == 2;
}

static void sendErr(uint8_t code) { sendFrame(OH_CMD_ERROR, &code, 1); }

// ------------------------------------------------------------- persistentie
static void saveCfg() {
  if (_fs == nullptr) return;
  fs::File f = _fs->open(OHB_CFG_PATH, "w", true);
  if (!f) return;
  f.printf("%d\n%u\n%s\n%d\n%u\n", _on ? 1 : 0, (unsigned)_port, _token,
           _fo_on ? 1 : 0, (unsigned)_fo_hold);
  f.close();
}

static void loadCfg() {
  if (_fs == nullptr || !_fs->exists(OHB_CFG_PATH)) return;
  fs::File f = _fs->open(OHB_CFG_PATH, "r");
  if (!f) return;
  char line[OHB_TOKEN_MAX + 8];
  int n = 0;
  /* Een ontbrekende regel laat de standaardwaarde staan: zo blijft een bestand
   * van een oudere versie leesbaar. */
  while (f.available() && n < 5) {
    size_t len = 0;
    while (f.available() && len < sizeof(line) - 1) {
      int ch = f.read();
      if (ch < 0 || ch == 10) break;
      if (ch == 13) continue;
      line[len++] = (char)ch;
    }
    line[len] = 0;
    long v = strtol(line, nullptr, 10);
    if (n == 0) _on = (line[0] == '1');
    else if (n == 1) { if (v >= 1 && v <= 65535) _port = (uint16_t)v; }
    else if (n == 2) { strncpy(_token, line, sizeof(_token) - 1); _token[sizeof(_token)-1] = 0; }
    else if (n == 3) _fo_on = (line[0] == '1');
    else if (v >= OHB_FO_HOLD_MIN && v <= OHB_FO_HOLD_MAX) _fo_hold = (uint16_t)v;
    n++;
  }
  f.close();
}

// ------------------------------------------------------------------ server
static void dropClient(const char* waarom) {
  if (_cl) { _cl.stop(); OHB_LOG("host losgekoppeld: %s", waarom ? waarom : ""); }
  _authed = false;
  _client_ip[0] = 0;
  _in_len = 0;
  _rx_head = _rx_count = 0;
}

static void startServer() {
  if (_listening) return;
  _server = WiFiServer(_port);
  _server.begin();
  _server.setNoDelay(true);
  _listening = true;
  snprintf(_note, sizeof(_note), "luistert op poort %u", (unsigned)_port);
  OHB_LOG("brug aan, poort %u", (unsigned)_port);
}

static void stopServer() {
  dropClient("brug uitgezet");
  if (_listening) { _server.end(); _listening = false; }
  strncpy(_note, "uit", sizeof(_note) - 1);
  OHB_LOG("brug uit");
}

// ------------------------------------------------------------------ inkomend
void ohb_on_raw_rx(float snr, float rssi, const uint8_t raw[], int len) {
  if (!_on || !_authed || len <= 0 || len > (int)(OHB_MAX_PAYLOAD - 6)) return;
  if (!_cl.connected()) return;
  if (_rx_count >= OHB_RX_RING) {   // vol: de oudste eruit, en dat wordt geteld
    _rx_head = (uint8_t)((_rx_head + 1) % OHB_RX_RING);
    _rx_count--;
    _n_rx_drop++;
  }
  uint8_t slot = (uint8_t)((_rx_head + _rx_count) % OHB_RX_RING);
  uint8_t* p = _rx[slot];
  int16_t r = (int16_t)rssi, s10 = (int16_t)(snr * 10.0f);
  /* Hun vorm: rssi | snr x10 | signaal-rssi | de ruwe bytes. Een aparte
   * signaal-RSSI meet deze radio niet; dan is de gewone de eerlijkste. */
  memcpy(&p[0], &r, 2); memcpy(&p[2], &s10, 2); memcpy(&p[4], &r, 2);
  memcpy(&p[6], raw, len);
  _rx_len[slot] = (uint16_t)(len + 6);
  _rx_count++;
}

static void flushRx() {
  while (_rx_count > 0) {
    if (!sendFrame(OH_CMD_RX_PACKET, _rx[_rx_head], _rx_len[_rx_head])) return;
    _rx_head = (uint8_t)((_rx_head + 1) % OHB_RX_RING);
    _rx_count--;
    _n_rx++;
  }
}

// ----------------------------------------------------------------- commando's
static void cmdGetConfig() {
  uint32_t freq, bw; uint8_t sf, cr; int8_t pwr;
  _mesh->ohRadioParams(freq, bw, sf, cr, pwr);
  uint8_t p[14];
  memcpy(&p[0], &freq, 4);
  memcpy(&p[4], &bw, 4);
  p[8] = sf; p[9] = cr; p[10] = (uint8_t)pwr;
  uint16_t sync = 0x12;
  memcpy(&p[11], &sync, 2);
  p[13] = 16;
  sendFrame(OH_CMD_CONFIG_RESP, p, sizeof(p));
}

static void cmdSetConfig(const uint8_t* payload, size_t len) {
  if (len < 14) { sendErr(OH_ERR_INVALID_CONFIG); return; }
  uint32_t freq, bw; memcpy(&freq, &payload[0], 4); memcpy(&bw, &payload[4], 4);
  uint8_t sf = payload[8], cr = payload[9];
  uint32_t of, ob; uint8_t osf, ocr; int8_t opwr;
  _mesh->ohRadioParams(of, ob, osf, ocr, opwr);
  uint32_t verschil = (freq > of) ? (freq - of) : (of - freq);
  /* Een gast verzet onze radio niet: dat zou deze node uit zijn eigen mesh
   * tillen omdat er ergens een daemon opstartte. Marge op de frequentie omdat
   * de host in hele hertz rekent en wij een kommagetal bewaren. */
  if (verschil > 2000 || bw != ob || sf != osf || cr != ocr) {
    snprintf(_note, sizeof(_note), "SET_CONFIG geweigerd (%lu/%lu/%u/%u)",
             (unsigned long)freq, (unsigned long)bw, (unsigned)sf, (unsigned)cr);
    OHB_LOG("%s -- onze stand: %lu/%lu/%u/%u", _note,
            (unsigned long)of, (unsigned long)ob, (unsigned)osf, (unsigned)ocr);
    sendErr(OH_ERR_INVALID_CONFIG);
  }
  cmdGetConfig();   // en altijd zeggen wat het WEL is
}

static void cmdStatus() {
  uint8_t p[24];
  uint32_t uptime = millis() / 1000;
  uint32_t rx = _mesh->getNumRecvFlood() + _mesh->getNumRecvDirect();
  uint32_t tx = _mesh->getNumSentFlood() + _mesh->getNumSentDirect();
  uint32_t crc_err = 0;
  int16_t rssi = (int16_t)_mesh->ohLastRssi();
  int16_t snr10 = (int16_t)(_mesh->ohLastSnr() * 10.0f);
  int16_t nf10 = (int16_t)(_mesh->ohNoiseFloor() * 10);
  memcpy(&p[0], &uptime, 4); memcpy(&p[4], &rx, 4);
  memcpy(&p[8], &tx, 4); memcpy(&p[12], &crc_err, 4);
  memcpy(&p[16], &rssi, 2); memcpy(&p[18], &snr10, 2); memcpy(&p[20], &nf10, 2);
  p[22] = 0;
  p[23] = _mesh->ohRadioBusy() ? 1 : 0;
  sendFrame(OH_CMD_STATUS_RESP, p, sizeof(p));
}

static void cmdTx(const uint8_t* payload, size_t len) {
  if (len == 0 || len > 255) { _n_tx_ref++; sendErr(OH_ERR_PAYLOAD_TOO_BIG); return; }
  uint32_t airtime = 0;
  if (!_mesh->ohInjectRaw(payload, (int)len, &airtime)) {
    /* TX_FAIL en niet TX_DONE: "verzonden" zeggen over iets dat nooit de lucht
     * in gaat is de ene fout die deze firmware probeert te vermijden. */
    _n_tx_ref++;
    snprintf(_note, sizeof(_note), "TX geweigerd (%u byte)", (unsigned)len);
    sendFrame(OH_CMD_TX_FAIL, nullptr, 0);
    return;
  }
  _n_tx++;
  uint32_t us = airtime * 1000UL;
  sendFrame(OH_CMD_TX_DONE, (uint8_t*)&us, 4);
}

static void handleFrame(uint8_t cmd, const uint8_t* payload, size_t len) {
  /* Zolang er een token staat mag er niets gebeuren voordat dat gezien is --
   * ook geen PING, want dat is een gratis manier om te weten of hier iets
   * luistert. */
  if (_token[0] != 0 && !_authed && cmd != OH_CMD_AUTH) { sendErr(OH_ERR_UNAUTHORIZED); return; }

  switch (cmd) {
    case OH_CMD_AUTH: {
      char aangeboden[OHB_TOKEN_MAX];
      size_t n = len < sizeof(aangeboden) - 1 ? len : sizeof(aangeboden) - 1;
      memcpy(aangeboden, payload, n);
      aangeboden[n] = 0;
      if (_token[0] == 0 || strcmp(aangeboden, _token) == 0) {
        _authed = true;
        sendFrame(OH_CMD_AUTH_OK, nullptr, 0);
        OHB_LOG("host aangemeld (%s)", _client_ip);
      } else {
        sendErr(OH_ERR_UNAUTHORIZED);
        dropClient("verkeerd token");
      }
      break;
    }
    case OH_CMD_PING:       sendFrame(OH_CMD_PONG, nullptr, 0); break;
    case OH_CMD_GET_VERSION: {
      const char* v = MESHMANAGER_NAME " v" MESHMANAGER_VERSION " (openHop-brug)";
      sendFrame(OH_CMD_VERSION_RESP, (const uint8_t*)v, strlen(v));
      break;
    }
    case OH_CMD_GET_CONFIG: cmdGetConfig(); break;
    case OH_CMD_SET_CONFIG: cmdSetConfig(payload, len); break;
    case OH_CMD_STATUS_REQ: cmdStatus(); break;
    case OH_CMD_NOISE_REQ: {
      int16_t nf10 = (int16_t)(_mesh->ohNoiseFloor() * 10);
      sendFrame(OH_CMD_NOISE_RESP, (uint8_t*)&nf10, 2);
      break;
    }
    case OH_CMD_TX_REQUEST: cmdTx(payload, len); break;
    case OH_CMD_RX_START:   sendFrame(OH_CMD_RX_STARTED, nullptr, 0); break;
    case OH_CMD_CAD_REQUEST: {
      /* Geen echte CAD -- die onderbreekt de ontvangst van een node die voor
       * het hele mesh luistert. Wat we wel eerlijk weten: is onze radio nu
       * bezet? Onze eigen zendlus doet zijn eigen CAD vlak voor het zenden. */
      uint8_t bezet = _mesh->ohRadioBusy() ? 1 : 0;
      sendFrame(OH_CMD_CAD_RESP, &bezet, 1);
      break;
    }
    case OH_CMD_SET_CAD_PARAMS: {
      uint8_t ok[4] = {0, 0, 0, 0};
      sendFrame(OH_CMD_CAD_PARAMS_RESP, ok, sizeof(ok));
      break;
    }
    case OH_CMD_SET_WIFI:
    case OH_CMD_WIFI_RESET:
    case OH_CMD_GET_WIFI:
      /* NIET. Deze node heeft zijn eigen weg voor netwerkinstellingen, achter
       * een login en over de mesh-CLI. Een TCP-poort op het LAN is niet de plek
       * om de wifi van een dakrepeater om te gooien. */
      OHB_LOG("wifi-commando 0x%02X geweigerd", cmd);
      sendErr(OH_ERR_INVALID_CMD);
      break;
    default: sendErr(OH_ERR_INVALID_CMD); break;
  }
}

static void readSocket() {
  int budget = 512;             // begrensde hap: de meshlus wacht hier niet op
  while (_cl.available() > 0 && budget-- > 0) {
    if (_in_len >= sizeof(_in)) _in_len = 0;
    int b = _cl.read();
    if (b < 0) break;
    if (_in_len == 0 && (uint8_t)b != OH_SYNC) continue;
    _in[_in_len++] = (uint8_t)b;
    if (_in_len < 4) continue;
    uint16_t plen = (uint16_t)_in[2] | ((uint16_t)_in[3] << 8);
    if (plen > OHB_MAX_PAYLOAD) { _in_len = 0; sendErr(OH_ERR_PAYLOAD_TOO_BIG); continue; }
    if (_in_len < (size_t)(4 + plen + 2)) continue;
    uint16_t gekregen = (uint16_t)_in[4 + plen] | ((uint16_t)_in[5 + plen] << 8);
    if (gekregen == crc16(&_in[1], 3 + plen)) {
      _guest_seen = millis();   // levensteken voor de failover
      handleFrame(_in[1], &_in[4], plen);
    } else {
      OHB_LOG("frame met foute CRC (cmd 0x%02X, %u byte)", _in[1], (unsigned)plen);
      sendErr(OH_ERR_CRC);
    }
    _in_len = 0;
  }
}

// ----------------------------------------------------------------- failover
static void failoverTick() {
  if (!_fo_on || _mesh == nullptr) return;
  const unsigned long nu = millis();
  const unsigned long hold = (unsigned long)_fo_hold * 1000UL;

  /* De VERBINDING is het levensteken; stilte telt pas als vastgelopen na een
   * veelvoud van de wachttijd. Een host die alleen luistert is gewoon stil. */
  const bool verbonden = _cl.connected();
  const bool vastgelopen = verbonden && _guest_seen != 0 &&
      (nu - _guest_seen) > hold * OHB_FO_STALL_MULT;
  const bool levend = verbonden && !vastgelopen;

  if (levend) {
    _guest_gone = 0;
    if (_guest_back == 0) _guest_back = nu;
  } else {
    _guest_back = 0;
    if (_guest_gone == 0) _guest_gone = nu;
  }

  if (!levend && !_fo_taken && (nu - _guest_gone) >= hold) {
    if (_mesh->ohForwarding()) return;         // stond al aan; niets over te nemen
    _mesh->ohSetForwarding(true);
    _fo_taken = true;
    _n_fo++;
    snprintf(_note, sizeof(_note), "FAILOVER: gast weg, deze node repeteert zelf");
    OHB_LOG("%s (na %u s stilte)", _note, (unsigned)_fo_hold);
    return;
  }
  if (levend && _fo_taken && (nu - _guest_back) >= hold) {
    _mesh->ohSetForwarding(false);
    _fo_taken = false;
    snprintf(_note, sizeof(_note), "gast terug; repeteren weer aan openHop");
    OHB_LOG("%s", _note);
  }
}

// --------------------------------------------------------------------- lus
void ohb_begin(fs::FS& fs, MyMesh* mesh) {
  _fs = &fs;
  _mesh = mesh;
  loadCfg();
  if (_on) startServer();
}

void ohb_loop() {
  if (!_on || _mesh == nullptr) return;
  failoverTick();
  if (WiFi.status() != WL_CONNECTED) return;   // zonder netwerk valt er niets te doen
  if (!_listening) startServer();

  WiFiClient nieuw = _server.available();
  if (nieuw) {
    /* Een tweede verbinding vervangt de eerste: een daemon die herstart laat
     * zijn oude socket soms hangen, en dan komt de nieuwe er nooit in. */
    if (_cl.connected()) dropClient("nieuwe host meldt zich");
    _cl = nieuw;
    _cl.setNoDelay(true);
    _in_len = 0;
    _authed = (_token[0] == 0);
    _guest_seen = millis();
    snprintf(_client_ip, sizeof(_client_ip), "%s", _cl.remoteIP().toString().c_str());
    snprintf(_note, sizeof(_note), "host %s verbonden", _client_ip);
    OHB_LOG("%s", _note);
  }
  if (!_cl.connected()) {
    if (_client_ip[0]) dropClient("verbinding weg");
    return;
  }
  readSocket();
  flushRx();
}

// --------------------------------------------------------------------- CLI
void ohb_status_line(char* out, size_t cap) {
  snprintf(out, cap,
           "openhop %s poort %u token %s host %s rx %lu tx %lu (geweigerd %lu) "
           "failover %s%s repeat %s",
           _on ? "aan" : "uit", (unsigned)_port, _token[0] ? "gezet" : "leeg",
           _cl.connected() ? _client_ip : "geen",
           (unsigned long)_n_rx, (unsigned long)_n_tx, (unsigned long)_n_tx_ref,
           _fo_on ? "aan" : "uit", _fo_taken ? " (OVERGENOMEN)" : "",
           (_mesh && _mesh->ohForwarding()) ? "on" : "off");
}

bool ohb_handle_command(const char* command, char* reply) {
  if (memcmp(command, "openhop", 7) != 0) return false;
  const char* p = command + 7;
  while (*p == ' ') p++;

  if (*p == 0 || strcmp(p, "status") == 0) {
    ohb_status_line(reply, 155);
    return true;
  }
  if (strcmp(p, "on") == 0 || strcmp(p, "off") == 0) {
    bool aan = (p[1] == 'n');
    if (aan != _on) {
      _on = aan;
      saveCfg();
      if (_on) startServer(); else stopServer();
    }
    snprintf(reply, 155, "OK - openhop %s", _on ? "aan" : "uit");
    return true;
  }
  if (memcmp(p, "port ", 5) == 0) {
    long v = strtol(p + 5, nullptr, 10);
    if (v < 1 || v > 65535) { strcpy(reply, "Err - poort 1..65535"); return true; }
    _port = (uint16_t)v;
    saveCfg();
    if (_on) { stopServer(); startServer(); }
    snprintf(reply, 155, "OK - poort %u", (unsigned)_port);
    return true;
  }
  if (memcmp(p, "token", 5) == 0) {
    const char* t = p + 5;
    while (*t == ' ') t++;
    strncpy(_token, t, sizeof(_token) - 1);
    _token[sizeof(_token) - 1] = 0;
    saveCfg();
    /* Een lopende sessie is aangegaan onder de oude regels. */
    dropClient("token gewijzigd");
    snprintf(reply, 155, "OK - token %s", _token[0] ? "gezet" : "gewist");
    return true;
  }
  if (memcmp(p, "failover", 8) == 0) {
    const char* a = p + 8;
    while (*a == ' ') a++;
    if (memcmp(a, "on", 2) == 0 || memcmp(a, "off", 3) == 0) {
      bool aan = (a[1] == 'n');
      if (!aan && _fo_taken && _mesh) {   // netjes teruggeven bij uitzetten
        _mesh->ohSetForwarding(false);
        _fo_taken = false;
      }
      _fo_on = aan;
      const char* getal = strchr(a, ' ');
      if (getal) {
        long h = strtol(getal, nullptr, 10);
        if (h >= OHB_FO_HOLD_MIN && h <= OHB_FO_HOLD_MAX) _fo_hold = (uint16_t)h;
      }
      saveCfg();
      snprintf(reply, 155, "OK - failover %s, wachttijd %u s",
               _fo_on ? "aan" : "uit", (unsigned)_fo_hold);
      return true;
    }
    snprintf(reply, 155, "failover %s, wachttijd %u s%s",
             _fo_on ? "aan" : "uit", (unsigned)_fo_hold,
             _fo_taken ? " (nu OVERGENOMEN)" : "");
    return true;
  }
  strcpy(reply, "Err - openhop on|off|port <n>|token [t]|failover on|off [s]|status");
  return true;
}
