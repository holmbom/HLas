# HLas — TODO

Avklarat arbete ligger i [doc/CHANGELOG.md](doc/CHANGELOG.md).
Utredningar som ännu inte blivit kod ligger i [doc/plans/](doc/plans/).

## 1.1.6

### Octree över utbredningen — det som inte provades före 1.1.5

1.1.5 är släppt med `root_from_extent()`, se [doc/CHANGELOG.md](doc/CHANGELOG.md).

- [ ] **Filproven (`las/`) körda med ändringen.** Taggen byggdes på en maskin utan
      testdata, så Autzen hoppades över. Filen är konform och ska läsas
      med oförändrat antal läsningar (byte_source-provet: 3).
- [ ] **Omkodningsprovet för punktformat 6–7 mot en publik fil.** Det gjordes mot en
      format 7-COPC-fil som inte är publik, och ligger därför inte i sviten. Kandidaten
      är `autzen-classified.copc.laz` — oprövad: att varje nod blir byte-identisk är
      inte uppmätt för den filen.
- [ ] **De tre fältproven körda mot autzens COPC-fil.** `test_write_full`,
      `test_point_view_files` och `test_sidecar` läste tidigare en annan format 7-fil
      och pekar nu på `autzen-classified.copc.laz`, men har inte körts mot den.

### Resten av sidecar-utredningen

1.1.4 är släppt; cachen är `.hlz` v4, ett rutnät med glest cellindex. Se
[doc/CHANGELOG.md](doc/CHANGELOG.md), [doc/plans/sidecar.md](doc/plans/sidecar.md)
och [doc/plans/sidecar-las.md](doc/plans/sidecar-las.md).

Det här står kvar. De två sista är **korrekthetsfrågor**, resten är prestanda och
funktionalitet:

- [ ] **`CUSTOM` och `FULL`** — fler fält i cachen. Bara LITE finns.
- [ ] **En fil i ren SKANNORDNING är oprövad.** Alla mätningar är gjorda på filer
      som redan låg spatialt ordnade (COPC från två olika verktyg).
- [ ] **Cellstorleken är en avvägning som v4 förlorar ibland.** En cell kan inte
      vara liten i tät data och stor i gles, vilket ett Hilbert-block kunde. Mot
      RGB-filen (skala 0,001, liten utbredning) är v4 6 % större än v3 — 6,09 mot
      5,75 B/pt — medan den på de stora filerna är 1–3 % mindre. Ett adaptivt
      rutnät, eller en delning av täta celler, är oprövat.
- [ ] **Källans fingeravtryck är ett stickprov, inte en checksumma.** 32 fönster
      om 4 kB fångar en omkodning eller en annan fil av samma längd, men en
      ändring som helt ligger mellan två fönster passerar och den gamla cachen
      godkänns. Ett fullt hashvärde vore ett löfte men kostar en genomläsning av
      källan vid varje `open()` — alltså hela poängen med 0 ms-öppningen. Ett
      mellanting vore att hasha hela filen bara när `BuildSidecar` körs och
      stickprova vid `open()`, men det ger inget skydd som stickprovet saknar.
- [ ] **Cachens EGEN integritet kontrolleras inte.** Källan har ett
      fingeravtryck, så en källa som ändrats brett upptäcks — med luckan ovan —
      men en bitflipp i själva
      `.hlz`-filen gör det inte. I cellindexet ger den en LJUGANDE bbox, och
      då försvinner punkter tyst ur en sållad fråga. En checksumma över bara
      cellindexet skulle räcka (~0,3 ms för 140 M punkter) medan en över hela
      filen kostar en genomläsning och därmed hela poängen med 0 ms-öppningen.
      Ohanterat tills vidare: cachen är regenererbar, och den skrivs atomiskt
      med `.tmp` + rename, så delskrivningar är redan uteslutna.

⛔ **Två punkter ströks när 1.1.5 skrevs, och skälet står här så de inte kommer
tillbaka.** Båda var optimeringar av v3:s form som v4 gjorde ointressanta:

- **Tvånivåindex** — motivet var att skanningen av blockhuvudena var O(N/B).
  Rutnätets radindex hoppar redan över en hel rad utan att röra en enda cell, och
  frågorna ligger på 0,1–8 ms. Det optimerar något som inte är flaskhalsen.
- **Härleda cellens utbredning ur `min + 2^bits - 1`** i stället för att lagra
  `dx0`/`dy0` — sparar 8 byte per cell, alltså ~0,08 B/pt vid målbeläggningen 100
  punkter, mot grövre sållning. Avvägningen är känd nog att avfärdas utan mätning.

### Större arbeten

#### Chunk-cache för nätverksläsning — **avfärdad**
Motivet fanns inte. Rätt arbetsflöde är att hämta *en* gång och arbeta lokalt:

    CopcReader::read_polygon()  ->  LasWriter  ->  Reader::open()  ->  generate_dem()

Mätt på en korridor: hämtningen 4403 ms, sedan skriv 5 ms, öppna 0 ms, DEM 11685 ms —
steg 2–4 rör aldrig nätet. Hämtar man en DEM och sedan punkter har man själv valt två
hämtningar, och det ska biblioteket inte gissa bort åt någon.

Skulle det bli aktuellt igen är det två skilda saker med skilda motiv: en minnescache för
överlappande frågor i samma session, eller en disk-cache i sidecar-stil för samma ruta en
annan dag. Bygg ingetdera på spekulation.

#### Fas 16: Single header
Målet är att LAS/LAZ-läsaren blir *en* fil man droppar in och inkluderar — inget CMake-bygge,
inga 20+ källfiler. "Ladda ner en header, `#include`, klart" är hela poängen.

- [ ] En header per modul, stb-stil — `HLas.hpp` (core), `HLasCopc.hpp`, `HLasHttp.hpp`,
      `HLasDem.hpp`, `HLasManager.hpp`. Den som bara vill läsa punkter drar ner en fil.
      Följer modulgränserna från Fas 14.
- [ ] **Beslut:** äkta header-only (`inline`, stb-stil `#define HLAS_IMPLEMENTATION`) eller
      amalgamerings-script som genererar headerna från källfilerna vid release
- [ ] Test som kompilerar och kör mot enbart single headern, utan resten av repot

#### COPC-utskrift
- [ ] Bygg octreet ovanpå den befintliga LAZ-utskriften: välja spacing, fördela punkter på
      noder, skriva hierarki-EVLR och `copc info`-VLR. Chunkningen och chunk-tabellen finns
      redan sedan 1.1.1, men COPC kräver att chunkarna motsvarar octree-noder och inte bara
      löpande 50000-block. Motiverat först om urvalet ska läggas på en server och läsas
      partiellt igen; ska det bara in i ett annat verktyg räcker LAS eller LAZ.

## 1.1.4

*(klar — `.hlz` v4: rutnät med glest cellindex i stället för Hilbert-block,
polygonsållning i stället för bbox, cache även för `.las`, histogram i headern och
LOD med stride per klass. Se [doc/CHANGELOG.md](doc/CHANGELOG.md).)*

- [x] **A. Polygonsållning.** `polygon_row_span()` rastrerar rutnätet rad för rad.
- [x] **B. Formatet v4 — rutnät med GLEST cellindex.**
- [x] **C. `.las`-vägen.** ⛔ Den DUBBLA källan ströks: idén att låta cache och
      källa leva samtidigt för en `.las` gav `get_xyz(i)` ur cellordningen medan
      `points().x(i)` läste filens, med vyn märkt som cacheformat över LAS-poster.
      Cachen äger ordningen för båda källformerna.
- [x] **D. Histogram i headern**, `uint32[256]`.
- [x] **E. LOD** — `stride[256]` per klass. ⛔ **Budget ströks.** Gallringen sker
      per RUTNÄTSCELL på punktens plats i cellen, inte med en löpande räknare, så
      en stride på 10 ger samma punkter utzoomad som inzoomad. En budget är till
      sin natur beroende av frågans utsträckning och hade brutit just det.
- [x] **F. Skiftläget** — ändelsematchningen är skiftlägesokänslig.

Ändelsen är `.hlz` och **läggs till**: `foo.las.hlz`, `foo.laz.hlz`.

⚠️ **Byggtiden beror kraftigt på byggkonfigurationen, och det stod inte utskrivet
förut.** TODO sa "16,0 s för autzen och 223,5 s för en laserruta" utan att nämna
vilken konfiguration det gällde. Mätt i **Release** är v4:s `BuildSidecar` 3,5 s
för autzen och 44,9 s för laserrutan (v3: 4,8 s och 62,3 s). De gamla siffrorna kom
ur en byggkonfiguration som inte antecknades — troligen Debug — och en tidsuppgift
utan sin konfiguration är inte en mätning.

Kvar att mäta: en fil i ren skannordning, kall kontra varm sidcache, och
proxyfelet på en fil med fler än två klasser.

## Buggar

*(inga kända)*

## Plattformar

- [ ] **macOS är oprövat.** Källan bär inget Linux-specifikt — `mmap`, `madvise`
      och `MADV_SEQUENTIAL` finns även där, och inget i trädet nämner `__linux__`,
      `/proc` eller `MAP_POPULATE` — men ingen har byggt biblioteket på en Mac.
      Det är ett antagande, inte en mätning, och 1.1.4:s Windows-fynd är precis
      vad ett sådant antagande brukar dölja.
- [ ] **`HLAS_ENABLE_NET=ON` är oprövat med MSVC.** Med MinGW (MSYS2, libcurl
      8.22) bygger det och sviten går, men för MSVC finns inget libcurl på
      maskinen, så `src/net/` har aldrig kompilerats med MSVC — och releasen har
      därför inget `-net`-paket för MSVC.
- [ ] **Ingen CI.** Fyra av 1.1.4:s fem Windows-fel hade fångats av ett bygge per
      plattform. De hittades i stället för att någon råkade öppna repot på en
      annan maskin, efter att taggen redan var pushad.

## 1.1.0

*(klar)*


## 1.1.1

*(klar — egen LAZ-kodare, referensen borta ur bygget)*


## 1.1.2

*(klar — läsvägen, LITE/FULL för punktformat 0–3, 6 och 7, och ett
konfigurerbart nätverkslager. Se [doc/CHANGELOG.md](doc/CHANGELOG.md).)*

## 1.1.3

*(klar — `.hlaz` v3: Hilbert-sorterad med frame-of-reference per block, ~5 B/pt
i stället för 13. ⚠️ Blockens bbox var tänkt som ett spatialt index men sållade
mot FRÅGANS BBOX, och för en diagonal korridor var cachen därför långsammare än
ingen cache alls. Rättat i 1.1.4. Se [doc/CHANGELOG.md](doc/CHANGELOG.md).)*


## Medvetet uppskjutet

**Punktformat 4, 5, 8, 9 och 10 kan varken läsas eller skrivas.**

| codec | krävs av | avkodare | kodare |
|---|---|---|---|
| Wavepacket13 | format 4, 5 | **saknas** | **saknas** |
| RGBNIR14 | format 8, 10 | ✅ | **saknas** |
| Wavepacket14 | format 9, 10 | **saknas** | **saknas** |
| Byte14 (extra bytes) | alla | ✅ | **saknas** |

Skälet är detsamma för alla: **ingen testfil att mäta mot**, och referensen stöder
inte format 4, 5, 9 eller 10 heller — det finns alltså inget facit. Att bygga på
spekulation är precis vad chunk-cachen avfärdades för.

Läsaren avvisar dem numera med `LazNotSupported` i stället för att tyst hoppa
över ett okänt item, vilket den gjorde före 1.1.2: items delar EN aritmetisk
ström, så ett överhoppat item flyttar allt som ligger efter, och filen lästes
utan fel men till skräp. FULL avvisar dem av samma skäl i stället för att tappa
fält. Blir det aktuellt behövs en fil att mäta mot först.
