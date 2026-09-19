# MeshManagerNet

**Een MeshCore-repeater met een IP-leven ernaast** — wifi, een beheerpagina,
firmware-upgrades met terugrol, een console, MQTT-publicatie, een pakketfilter
en (sinds 2.11.0) een brug waarmee een [openHop](https://github.com/openhop-dev)-daemon
deze node als radio kan gebruiken zónder dat hij ophoudt repeater te zijn.

Deze repo is een **overlay**: hij bevat alleen de eigen modules plus één patch,
en laat de MeshCore-broncode waar die hoort. Zo blijft zichtbaar wat van
[MeshCore](https://github.com/meshcore-dev/MeshCore) is en wat hier geschreven is,
en kan de overlay meeschuiven naar een nieuwere MeshCore-release.

Draait sinds augustus 2026 op een zonne-repeater op een dak (Heltec V4.3).

## Wat het toevoegt

| | |
|---|---|
| **WiFi met vangnet** | valt het netwerk weg, dan zet hij zijn eigen SSID op met dezelfde beheerpagina en blijft hij je netwerk proberen |
| **Beheerpagina + console** | instellingen, logboek, pakketfilter; de mesh-CLI blijft werken als de pagina stuk is |
| **Upgrade die de waarheid vertelt** | `POST /api/fw` controleert het beeld vóór hij de bootpartitie omzet; het vorige beeld blijft in de andere partitie staan en `POST /api/fw/rollback` haalt het terug |
| **Veilige modus** | drie mislukte starts achter elkaar → mesh + AP + pagina, verder niets |
| **Watchdog** | een vastgelopen `loop()` wordt een herstart, zodat de bootteller hem kan zien; hij stapt opzij tijdens een flash-schrijfactie |
| **MQTT** | statistieken (inclusief burenlijst) en elk ontvangen pakket, plus één inkomend `cmd`-topic met vier woorden — nadrukkelijk géén CLI op afstand |
| **Pakketfilter** | per pakkettype hops/rate/kanaal/hash, met tellers die als metingen meegaan |
| **openHop-brug** | zie hieronder |
| **Accu-bewust** | het publicatietempo volgt de celspanning; in spaarstand gaat de wifi grotendeels uit |

De leidende aanname staat in de kop van `src/MeshManagerNet.h`: *deze repeater
hangt op een dak en loopt op een zonnepaneel. Hij mag nooit onbereikbaar worden,
en nooit meer verbruiken dan het paneel binnenbrengt.* Elke keuze hierboven is
daaruit te herleiden.

## De openHop-brug (2.11.0)

openHop is een Python-herimplementatie van MeshCore. Hun eigen modemfirmware
maakt van een bord een **domme** radio — dan is alles hierboven weg. Deze brug
doet het omgekeerd: de node blijft de repeater en de openHop-host wordt een
passagier op zijn radio.

Drie grenzen zitten in de code, niet in de documentatie:

1. **Een gast verzet onze radio niet.** Een `SET_CONFIG` met een andere
   frequentie, bandbreedte, spreiding of codering wordt geweigerd — die zou de
   node uit zijn eigen mesh tillen.
2. **Een gast dringt niet voor.** Zijn pakketten gaan in dezelfde wachtrij met
   de laagste prioriteit en binnen hetzelfde zendtijdbudget. Past er niets meer
   bij, dan `TX_FAIL` — nooit een `TX_DONE` voor iets dat niet vertrok.
3. **Een gast herconfigureert ons niet.** De wifi-commando's uit hun protocol
   worden geweigerd.

Plus een **failover**: valt de host weg, dan zet de node zijn eigen doorsturen
weer aan, en geeft hij het uit handen zodra de host terug is. Alleen in RAM, dus
na een herstart geldt weer de ingestelde stand.

Bediening gaat via de gewone CLI, en dus ook over LoRa:

```
openhop                      stand opvragen
openhop on | off             brug aan/uit
openhop port <n>             poort (standaard 5055)
openhop token [tekst]        token zetten of wissen
openhop failover on|off [s]  failover en wachttijd
```

## Bouwen

Zie [bouwen.md](bouwen.md). Kort: check MeshCore uit, kopieer `src/` naar
`examples/simple_repeater/`, pas de patch toe, neem de voorbeeld-`platformio.local.ini`
over en bouw de env voor jouw bord.

## Verwante projecten

- **[MeshStats](https://github.com/DinXke/MeshStats)** — MeshManager: de site die deze repeaters uitleest, bewaakt en beheert
- **[MeshUptime](https://github.com/DinXke/MeshUptime)** — de bewakingsnode (room-server, bots, IRC, poller) en de T1000-E-companionfirmware
- **[MeshCore](https://github.com/meshcore-dev/MeshCore)** — de basis waar dit een overlay op is

## Licentie

MIT, net als MeshCore zelf. Zie [LICENSE](LICENSE).
