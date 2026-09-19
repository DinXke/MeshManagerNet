#pragma once

#include <Arduino.h>
#include <FS.h>

/* ============================================================================
 * OpenHopBridge -- deze dakrepeater als RADIO voor een openHop-host, zonder
 * iets in te leveren (MeshManagerNet 2.11.0).
 *
 * WAT OPENHOP IS. openHop (openhop-dev) is een Python-herimplementatie van
 * MeshCore: zelfde protocol, zelfde mesh. De repeater-daemon draait op Linux en
 * praat met zijn radio over een klein binair protocol, over USB of TCP. Hun
 * eigen firmware (`openhop_modem`) maakt van dit bord een DOMME modem -- en dan
 * is alles wat deze node verder is (repeteren, het pakketfilter, de
 * beheerpagina, MQTT, de veilige modus) weg.
 *
 * DIT IS DE ANDERE KANT VAN DIE RUIL. Wij spreken hun modemprotocol, maar
 * blijven zelf de repeater. De openHop-host wordt een PASSAGIER op onze radio:
 * hij hoort alles wat wij horen en mag zenden via onze wachtrij. Dat kan alleen
 * omdat het dezelfde pakketten op dezelfde frequentie zijn.
 *
 * DE DRIE GRENZEN (gelijk aan de MeshUptime-node, met opzet: twee keer dezelfde
 * regels op twee plekken is een keer te veel om uit te leggen):
 *
 *  1. EEN GAST VERZET ONZE RADIO NIET. `SET_CONFIG` met een andere frequentie,
 *     bandbreedte, spreiding of codering wordt geweigerd; die zou deze node uit
 *     zijn eigen mesh tillen. We antwoorden met onze werkelijke stand.
 *  2. EEN GAST DRINGT NIET VOOR. Zijn pakketten gaan in DEZELFDE wachtrij met
 *     de laagste prioriteit en binnen hetzelfde zendtijdbudget. Past er niets
 *     meer bij, dan TX_FAIL -- nooit een TX_DONE voor iets dat niet vertrok.
 *  3. EEN GAST HERCONFIGUREERT ONS NIET. De wifi-commando's uit hun protocol
 *     krijgen ERR_INVALID_CMD; deze node heeft daar zijn eigen weg voor.
 *
 * DE FAILOVER. Laat je openHop het repeteren doen, dan hangt de dekking van dit
 * dak aan een container, een LAN en een wifi-verbinding. Staat de failover aan,
 * dan zet deze node zijn eigen doorsturen weer aan zodra de gast wegvalt, en
 * geeft hij het uit handen zodra die terug is. Dat hoort HIER te draaien en
 * niet op de server: als openHop wegvalt is er vaak meer weg.
 *
 * BEDIENING OVER DE MESH. Alles gaat via `openhop ...` in de gewone CLI, en die
 * bereik je ook over LoRa. Een dakrepeater die je alleen via zijn webpagina kunt
 * instellen, is een dakrepeater die je kwijt bent zodra de wifi wegvalt.
 *
 * DE DRAADVORM (openhop_core/hardware/protocol_constants.py, v0.7):
 *
 *   SYNC 0xAA | CMD (1) | LEN (2, LE) | PAYLOAD | CRC-16/CCITT-FALSE (2, LE)
 *
 * De CRC loopt over CMD+LEN+PAYLOAD, niet over de SYNC.
 * ==========================================================================*/

class MyMesh;

/* Aanroepen na het bestandssysteem en de mesh, naast mmnet_begin(). */
void ohb_begin(fs::FS& fs, MyMesh* mesh);

/* Uit de hoofdlus, naast mmnet_loop(). Doet nooit iets dat lang blokkeert. */
void ohb_loop();

/* Elk ruw ontvangen pakket, uit MyMesh::logRxRaw. Kopieert alleen. */
void ohb_on_raw_rx(float snr, float rssi, const uint8_t raw[], int len);

/* De CLI: `openhop ...`. true = afgehandeld (antwoord staat in reply). */
bool ohb_handle_command(const char* command, char* reply);

/* Een regel voor de beheerpagina / het statusbericht. */
void ohb_status_line(char* out, size_t cap);
