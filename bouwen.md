# Bouwen en flashen

## Waarom een overlay en geen fork

De MeshCore-broncode blijft van MeshCore. Deze repo bevat alleen wat hier
geschreven is plus één patch met de drie haken in upstream-bestanden. Dat houdt
twee dingen makkelijk: zien wat van wie is, en meeschuiven naar een nieuwere
MeshCore-release zonder een fork bij te houden die steeds verder afdrijft.

## Eenmalig opzetten

```bash
git clone https://github.com/meshcore-dev/MeshCore.git
cd MeshCore
git checkout v1.17.0            # de release waarop dit draait
```

Kopieer de eigen modules erin:

```bash
cp ../MeshManagerNet/src/*.h ../MeshManagerNet/src/*.cpp examples/simple_repeater/
```

Breng de haken aan in de drie upstream-bestanden (`MyMesh.h`, `MyMesh.cpp`,
`main.cpp`):

```bash
git apply ../MeshManagerNet/patches/repeater-hooks.patch
```

Neem de bouwconfiguratie over en vul je eigen gegevens in — **dit bestand hoort
niet in git**:

```bash
cp ../MeshManagerNet/platformio.local.ini.voorbeeld platformio.local.ini
$EDITOR platformio.local.ini      # WIFI_SSID, WIFI_PWD, ADMIN_PASSWORD, ADVERT_*
```

## Bouwen

```bash
python -m platformio run -e heltec_v4_repeater_meshmanager
```

Pas de env aan voor jouw bord; de voorbeeldconfiguratie bevat er meer dan één.

## Flashen

**De eerste keer over USB:**

```bash
python -m platformio run -e heltec_v4_repeater_meshmanager -t upload --upload-port COM4
```

**Daarna over de lucht**, en dat is de weg die je op een dak wil. De node
controleert het beeld vóór hij de bootpartitie omzet:

```bash
SHA=$(sha256sum .pio/build/heltec_v4_repeater_meshmanager/firmware.bin | cut -d' ' -f1)
curl -u admin:JOUWWACHTWOORD \
     -X POST --data-binary @.pio/build/heltec_v4_repeater_meshmanager/firmware.bin \
     -H "Content-Type: application/octet-stream" \
     "http://NODE-IP/api/fw?sha256=$SHA"
```

Het antwoord zegt `geschreven en geverifieerd` met de gemeten en de verwachte
hash naast elkaar, en of hij herstart. `GET /api/fw` toont wat er draait en wat
er in de andere partitie klaarstaat; `POST /api/fw/rollback` zet die terug.

Gaat er tóch iets mis met de pagina zelf: `/update` (ElegantOTA, achter dezelfde
login) is het vangnet, en `wifi fw rollback` over de mesh-CLI is het vangnet
daaronder — die werkt ook als de wifi weg is.

## Eerste start

De node komt op met je wifi-gegevens uit de bouwconfiguratie. Lukt dat niet, dan
zet hij zijn eigen SSID op met dezelfde beheerpagina erop. Vanaf daar:

- **beheerpagina** — `http://NODE-IP/`, login uit `ADMIN_PASSWORD`
- **console** — TCP 23, zelfde login; hier werken alle MeshCore-commando's plus
  `wifi ...` en `openhop ...`
- **mesh-CLI** — dezelfde commando's over LoRa, voor als het netwerk weg is

## Verificatie na een upgrade

```
ver                 -> MeshManager (by DinX) v2.11.1 - MeshCore v1.17.0
openhop             -> stand van de brug
wifi                -> netwerkstand
```

## De melding bij failover

Zodra deze node het repeteren overneemt omdat openHop wegviel, stuurt hij een
kort tekstbericht — en nog eens één als hij het teruggeeft. De bestemming moet
een contact zijn dat al in de ACL van deze repeater staat (je companion logt daar
op in, dus de sleutel en het gedeelde geheim zijn er al):

```
openhop melding 2cb0c5eb          -> zet de bestemming (begin van de pubkey, max 8 bytes)
openhop melding test              -> stuurt er nu één, zodat je het ziet werken
openhop melding                   -> toont wat er staat
openhop melding uit               -> af
```

Standaard staat het uit. Kent deze node die sleutel niet, dan zegt `test` dat
meteen in plaats van stil te falen — log met je companion één keer in op de
repeater en probeer opnieuw.

## De droogtetoets

De failover kijkt sinds 2.12.0 niet alleen of openHop verbonden is, maar ook of
hij nog zendverzoeken stuurt. Komt er gedurende de droogtetijd geen enkel
verzoek terwijl deze node hem wel pakketten bleef aanreiken, dan repeteert hij
niet en neemt deze node het over.

```
openhop droogte           -> toont de stand
openhop droogte 600       -> tien minuten (standaard)
openhop droogte uit       -> af
```

**Wanneer je hem uit moet zetten.** De toets neemt aan dat elke kop af en toe
een zendverzoek krijgt. Dat klopt bij `fabric.tx_mode: bridge` en voor de
`default_radio`, maar bij `default` of `sticky` kan een tweede kop terecht
nooit iets te zenden krijgen — daar zou de node dan onnodig gaan dubbelrepeteren.
