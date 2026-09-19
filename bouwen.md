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
