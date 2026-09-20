# Changelog - MeshManagerNet

De volledige toelichting per versie staat in de kop van
`src/MeshManagerNet.cpp`; dit is de korte lijst.

- **2.12.0** - Verbonden is niet hetzelfde als repeterend: failover op zendverzoeken
- **2.11.6** - Een failover hoort iemand wakker te maken: melding naar je companion
- **2.11.5** - De brug gooide elk RX-frame weg; availableForWrite() bestaat hier niet
- **2.11.4** - De drempel hoort bij de pakketstroom, niet bij de antwoorden (PONG)
- **2.11.3** - Niet blijven duwen tegen een socket die niets aanneemt
- **2.11.2** - Een gast mag zendtijd kosten, geen hoofdlus (deugde niet; zie 2.11.5)
- **2.11.1** - De failover zag een VERBONDEN maar stille host aan voor een dode. Hun
- **2.11.0** - De openHop-brug: deze node als radio voor een openHop-daemon, zonder
- **2.10.0** - Twee dingen die dezelfde denkfout rechtzetten: een filter op de
- **2.9.0** - De telemetrie van een SENSORNODE komt nu heel binnen, in plaats van
- **2.8.2** - Er is geen bug: voor bijna al het verkeer valt er niets te beslissen.
- **2.8.1** - Het oordeel van het filter kwam voor tweederde van het floodverkeer te
- **2.8.0** - Het derde vervoermiddel: 'set <param> <waarde>' op het cmd-topic.
- **2.7.0** - Een geweerd pakket is in het archief te herkennen, met de reden erbij.
- **2.6.0** - 'radio' kan niet meer van afstand gezet worden, en het filter houdt bij
- **2.5.0** - De beheerpagina van de node zelf kan nu alles wat de app en het filter
- **2.4.0** - De schrijfweg over LoRa. Deze node kan één CLI-instelling zetten op een
- **2.3.0** - Een pakketfilter op de repeater, en beheer ervan vanaf de site.
- **2.2.0** - De sweep vraagt ook 'ver', zodat de site van elke gemonitorde node de
- **2.1.0** - De site kan een instelling schrijven in plaats van alleen lezen: POST
- **2.0.1** - De eenmalige verhuizing van het topicvoorvoegsel ging nooit af.
- **2.0.0** - Alles heet MeshManager: de module, de bestanden, de defines, het
- **1.12.0** - An upgrade path that tells the truth: POST /api/fw with the image as
- **1.11.0** - The sweep collects the region tree again, under the key the site has
- **1.10.0** - The site can set this node's clock, and this node then checks the
- **1.9.1** - The name of a monitored repeater is escaped before it goes into a
- **1.9.0** - A monitoring node can now read the CLI settings of a repeater it
- **1.8.0** - The node listens on '<prefix>/<node>/cmd' and accepts exactly two
- **1.7.2** - The sweep asks for flood.max.unscoped as well. The parameter list on
- **1.7.1** - The automatic monitor round never started. passed() reads 0 as 'not
- **1.7.0** - The CLI settings sweep runs once a day instead of every six hours,
- **1.6.0** - Battery-to-interval is now a table the user edits (add/remove rules,
- **1.5.0** - Adverts are cached on the file system (key, name, type, last heard,
- **1.4.0** - A monitored repeater is now read with three requests instead of one:
- **1.3.1** - A poll that stalled after a successful login looked exactly like one
- **1.3.0** - Fixed monitored repeaters never arriving anywhere: their statistics
- **1.2.0** - Monitor other repeaters: pick them from the heard list or paste a
- **1.1.0** - Task watchdog: a hung loop() now becomes a reboot, so the existing
- **1.0.0** - MQTT publishing (own stats + every raw packet), battery- and
