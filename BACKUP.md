# Back-up en herstel van een MeshManager-node

Hoort bij `MeshManagerNet.cpp` (`writeBackupFile()`, `applyRestore()`,
`fsCommand()`). Dit document bestaat vooral om één fout vast te leggen die ons
drie identiteiten heeft gekost.

## De valkuil: `File::name()` geeft niet het pad

**Op Arduino-ESP32 2.x geeft `File::name()` alleen de BASISNAAM terug, niet het
volledige pad.** `File::path()` geeft het volledige pad. In de kern:

```cpp
const char* VFSFileImpl::name() const { return pathToFileName(path()); }
```

De identiteit van een node staat niet in de wortel:

```cpp
IdentityStore store(SPIFFS, "/identity");          // main.cpp
sprintf(filename, "%s/%s.id", _dir, name);         // IdentityStore.cpp
```

dus op `/identity/_main.id`.

De back-upschrijver liep de wortel af met `openNextFile()` en noteerde
`f.name()`, met een schuine streep ervoor als die ontbrak. Resultaat:

```
FILE /_main.id 64          <-- fout: het bestand staat op /identity/_main.id
```

### Wat dat in de praktijk deed

Bij het terugzetten werd de sleutel naar `/_main.id` geschreven — een pad dat
nergens gelezen wordt. Het echte bestand in `/identity/` bleef onaangeroerd, de
node hield zijn nieuwe sleutel, en in de volgende dump stonden twee
`_main.id`-ingangen. Dat is drie keer herhaald, met drie verschillende nieuwe
identiteiten, voordat duidelijk was waar het zat (22 oktober, dakrepeater).

**De back-up zag er geldig uit.** Hij had de juiste bytes, de juiste lengte en
de juiste kopregel. Alleen stond er een pad in waar niemand kijkt. Dat is erger
dan een ontbrekende back-up: je merkt het pas op de dag dat je hem nodig hebt.

## Wat er sinds 2.15.0 gebeurt

1. **`backupWalk()` bouwt het pad zelf op.** Het gebruikt `f.path()` als de kern
   die invult, valt anders terug op `map + "/" + f.name()`, en duikt mappen in
   (nodig op LittleFS; SPIFFS is plat en levert `/identity/_main.id` als één
   naam uit de wortel).
2. **De back-up controleert zichzelf.** Ontbreekt `/identity/_main.id`, dan
   staat er een WAARSCHUWING op de console in plaats van niets.
3. **De restore weigert een dump zonder identiteit**, vóór hij iets schrijft
   (`restoreHasIdentity()`, een aparte leesronde). Halverwege ontdekken dat de
   sleutel ontbreekt is te laat; een restore is onomkeerbaar.
4. **Oude, kapotte dumps worden gered.** `restoreTargetPath()` legt `/_main.id`
   om naar `/identity/_main.id`. Zonder die omlegging is elke dump van voor
   2.15.0 onbruikbaar, en dat zou niemand merken.

   > **Exact dit ene pad, niet "elk `.id`-bestand uit de wortel".** Die bredere
   > regel stond er eerst en is fout. De bureau-node gebruikt hetzelfde
   > dumpformaat en bewaart `/room_id_0.id` en `/snode_id_0.id` wél in de
   > wortel (`IdentityStore(fs, "")` in `RoomMesh.cpp`). Een brede omlegging
   > zou die bij een restore naar `/identity/` verplaatsen en de room-sleutels
   > kwijtmaken: dezelfde fout nog eens, maar dan veroorzaakt door de
   > reparatie. Gevonden door de omlegging op papier langs de echte paden van
   > beide firmwares te leggen, niet op hardware.
5. **Per bestand eerst `remove()`, en mappen worden aangemaakt**
   (`makeParentDirs()`, een lege handeling op SPIFFS).
6. **`fs ls` en `fs rm <pad>`** in de CLI, zodat rommel zoals een los
   `/_main.id` op afstand weg kan zonder flashen. Wissen onder `/identity`
   vereist `force` erachter.

## Formaat

Ongewijzigd, zodat bestaande controlescripts blijven werken:

```
MESHMANAGER-BACKUP 1
FILE <pad> <bytes>
<hex, 64 bytes per regel>
...
END
```

De kopregel `MESHSTATS-BACKUP` van voor de hernoeming wordt bij het terugzetten
nog steeds aanvaard.

> **Na te kijken:** de nachtelijke back-upjob controleert of `/_main.id` in de
> dump zit. Nieuwe dumps schrijven `/identity/_main.id`. Als substring klopt dat
> nog; controleert de job op een los veld, dan moet hij aangepast worden.

## Regels om te onthouden

- Een back-up zonder `/identity/_main.id` is geen back-up. Controleer dat, bij
  het maken én bij het terugzetten.
- Vertrouw `File::name()` nooit voor een pad. Gebruik `File::path()`, of bouw
  het pad op uit de map die je aan het doorlopen bent.
- Oefen een herstel op een wegwerpnode, nooit op het origineel. Deze fout was
  alleen zichtbaar door na afloop te kijken welk bestand de node werkelijk las.
