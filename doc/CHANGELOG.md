# HLas — CHANGELOG

Avklarat arbete. Öppna punkter ligger i [TODO.md](../TODO.md).

## Nästa

### Färdigbyggda bibliotek på varje release

- `cmake --install` ger biblioteken, `HLas.h` och ett CMake-paket, så att
  `find_package(HLas)` ger `HLas::HLas`, `HLas::core` och de andra modulerna.
  `HLAS_INSTALL` styr reglerna och har samma förval som `HLAS_BUILD_TESTS`.
- `HLas::HLas` finns också som alias i källbygget, så att FetchContent och
  `find_package` länkas likadant.
- `.github/workflows/release.yml` bygger paketet för Windows MSVC, Windows MinGW
  och Linux GCC vid varje tagg och lägger det på releasen. MinGW och Linux finns
  också som `-net`-paket med nätstödet; MSVC-paketet saknar det. Release-byggen.
- `HLAS_ENABLE_NET=ON` är byggt och provat på Windows med MinGW (MSYS2, libcurl
  8.22) — första gången `src/net/` kompilerats på Windows.

## 1.1.5

### Licensgaten borttagen — biblioteket är MIT

- `SetLicense()`, `HasValidLicense()`, `LicenseData` och `Status::NoLicense` är
  borta ur det publika API:t, och `src/manager/license.*` och
  `tests/test_license.cpp` likaså. **API-brytande:** ett program som anropade
  `SetLicense()` kompilerar inte längre och ska bara ta bort anropet. De övriga
  `Status`-värdena är oförändrade — `NoLicense` låg sist.
- Punktdatafunktionerna börjar med `Impl::begin_call()`, som rensar
  `last_error()` på samma sätt som gaten gjorde. En flyttad-från `Reader` ger
  `FileOpenFailed` från histogramfunktionerna, samma som `open()`.
- `BuildSidecar()` kontrollerar ingen licens.
- `LICENSE` (MIT), `THIRD_PARTY_NOTICES.md` för GPS-tidens kodning, en kort
  `README.md` och den fullständiga beskrivningen i `README.sv.md`.
- De gyllene vektorerna heter `tests/golden_ref.h` och `tests/golden_ref10.h`.
- Testerna använder bara publika testfiler. `test_reencode` (omkodningen av en
  format 7-fil som inte är publik) är borttaget, och `test_write_full`,
  `test_point_view_files` och `test_sidecar` läser `autzen-classified.copc.laz`.
- `HLAS_BUILD_TESTS` — tester, mätprogram och verktyg byggs bara när HLas är
  toppnivåprojektet.
- MSVC:s C4805 i `point14.cpp` (en `bool` i en bitoperation) är rättad.

### Octree byggt över filens utbredning i stället för kuben

Det finns sammanfogade COPC-filer med rektangulär utbredning som har en kub i
COPC-infon men octreet byggt över headerns min/max per axel. En sådan fil är
10 × 5 km med kuben 10 × 10 km, och med kuben hamnade de
djupa noderna fel: 59,8 M av 117,7 M punkter låg i noder utanför filen, och en
korridor över filen fick **2 % av punkterna — tyst**.

⛔ **UPPMÄTT, INTE ANTAGET.** Seed-posten (chunkens första punkt, okomprimerad) i
30 djupa noder låg 30/30 i nodens ruta räknad från utbredningen och 2/30 räknad
från kuben. En kvadratisk kontrollfil ur samma serie gav 30/30 med båda.

- `CopcInfo` har en rotlåda per axel och `root_from_extent`. `copc_node_bounds`
  delar varje axel för sig ur lådan när flaggan är satt, annars kuben som förut —
  en `CopcInfo` som byggts på annat sätt beter sig oförändrat.
- `read_copc_index` känner igen en rot som inte stämmer: punkterna i noder vars kub
  skär headerns utbredning är färre än alla. Då läses seed-posten i upp till
  `kCopcRootProbes` (8) av de djupaste noderna, och lådan byts bara om varenda en
  ligger i sin ruta räknad från utbredningen. ⛔ Aldrig en gissning: förklarar ingen
  tolkning seed-punkterna behålls kuben, och en läsning som misslyckas är ett fel,
  inte en tyst kub. En konform fil kostar ingen läsning till.
- `CopcReader::root_from_extent()` — anroparen kan logga det.
- Provet `test_copc_root_extent`: en COPC-fil byggd i minnet, fyra fall —
  rektangulär (lådan byts, urvalet träffar noden som kuben missar), kvadratisk
  (inget byts, 3 läsningar), seed-punkter som ingen tolkning förklarar (kuben
  behålls), läsfel under kontrollen (`ReadPastEnd`).
- ⚠️ Z ur utbredningen är inte uppmätt — urvalen i HLas är 2D.
- **Provat i ett program:** korridoren 50 × 1000 m över den rektangulära filen
  gav 212 417 punkter (4,2/m²) mot 2 586 i 1.1.4.

### Sviten bygger med MSYS2

`tests/test_data.h:ifstream_utf8` skickade en `std::wstring` till `std::ifstream`,
en överlagring som bara MSVC har. `w.c_str()` — `const wchar_t*` finns också i
libstdc++ på MinGW. Sviten grön med MSYS2 UCRT64 g++; filproven hoppades över
(ingen `las/` på maskinen, se TODO).

## 1.1.4

### Cachen är ett rutnät, sållningen går mot polygonen, och `.las` får en cache

`.hlz` version 4. Ändelsen är ny och **läggs till** i stället för att ersätta:
`foo.las.hlz`, `foo.laz.hlz`. `.las` och `.laz` skiljer sig på en bokstav och
kollapsade annars till samma cachefil, som de sedan byggde om åt varandra i all
evighet. Matchningen är skiftlägesokänslig.

⛔ **Fyndet som startade hela versionen: 1.1.3:s cache var LÅNGSAMMARE än ingen
cache alls för en diagonal korridor** — 568 ms mot en platt fils 524. Sållningen
jämförde blockens bbox mot **frågans bbox**, och en diagonal korridors bbox täcker
hela filen, så ingenting sållades bort. Ren algoritmbugg, ingen formatfråga — och
korridorer är precis den fråga cachen finns för.

**Sållningen går nu mot POLYGONEN.** Rutnätet rastreras rad för rad:
`polygon_row_span()` i [src/core/geometry.h](../src/core/geometry.h) ger stråkets
x-utbredning i just den raden, radindexet ger radens första cell, och ingen cell
utanför rörs. `Impl::each_point_in_poly()` används av `get_xyz_polygon`,
`get_xyzc_polygon`, `get_classification_histogram_polygon` och DEM:en.

**Cellindexet är GLEST, och det är inte valfritt.** Bara upptagna celler lagras,
plus ett radindex `row_start[ny+1]`, med cellerna sorterade på `(rad, cx)`. Med en
tät celltabell blir en klippt korridorleverans **79 % större**, eftersom 96 % av
cellerna är tomma och ändå betalas. Cellstorleken följer **målbeläggning**
(~100 punkter per cell), inte en fast meterstorlek: kostnaden per punkt är
`log2(ex·ey) − log2(C) + bz + 8 + 216·C/n` och derivatan är noll vid `C ≈ n/150`.
En fast 10 m-cell kostade 0,96 B/pt extra på en fil med skala 0,001.

**Storleken, mätt på tre filer:**

| | punkter | v3 | **v4** | källan som `.laz` |
|---|---|---|---|---|
| autzen | 10,7 M | 54,4 MB — 5,35 B/pt | **53,0 MB — 5,22** | 53,7 MB — 5,29 |
| laserruta | 140,3 M | 674,9 MB — 5,04 | **666,0 MB — 4,98** | 689,4 MB — 5,15 |
| RGB-filen | 1,8 M | 9,8 MB — 5,75 | **10,3 MB — 6,09** | 13,9 MB — 8,21 |

⚠️ **Rättelse mot commit `5cc67a0`.** Den sa "v4 är 6 % STÖRRE" som om det gällde
generellt. Det gällde bara RGB-filen — skala 0,001 och liten utbredning. På de två
stora filerna är v4 1–3 % **mindre**. Skälet är att en cell inte kan vara liten i
tät data och stor i gles, vilket ett Hilbert-block kunde. Att den siffran gick att
skriva utan förbehåll berodde på att labbet bara mätte autzen; A/B-provet nedan
finns för att det inte ska gå igen.

**Farten** — medel över 12 slumpvis placerade *ormar*, alltså buffrade polylines
med 8–18 hörn, vilket är den form en korridorfråga faktiskt har. En rak
fyrhörning hade dolt både polygonsållningen och kostnaden för hörnrika polygoner.
Träffantalet är identiskt rad för rad i båda versionerna:

| fråga | autzen v3 | autzen **v4** | laser v3 | laser **v4** |
|---|---|---|---|---|
| orm 12 m × 300 m | 0,413 ms | **0,101** | 1,951 ms | **0,243** |
| orm 50 m × 1 km | 3,840 | **0,727** | 16,940 | **2,517** |
| orm 50 m × 3 km | 32,133 | **2,138** | 162,522 | **8,528** |
| orm 200 m × 1 km | 5,307 | **2,194** | 23,574 | **8,750** |
| histogram | 43,9 | **0,000** | 581,0 | **0,000** |

Vinsten är störst för den **långa smala** ormen — 15x och 19x — och det är väntat:
där var bbox-sållningen som mest värdelös. Bygget är också 20–28 % snabbare
(autzen 4,8 → 3,5 s, laserrutan 62,3 → 44,9 s, Release).

Provet ligger i [tests/bench_ab.cpp](../tests/bench_ab.cpp) och använder bara API
som finns i **båda** versionerna, så det som mäts är implementationen och inte
anropet.

### Histogrammet ligger i headern

`uint32[256]`, så `get_classification_histogram()` är ett minnesuppslag. Den
skanningen var dessutom **långsammare med cache än utan** — 141 mot 50 ms på
autzen — eftersom punkten är bitpackad med koordinaterna interfolierade med
klassen. Läsordningen är cache → källa, och svaret är detsamma oavsett vilken väg
som gav det. API:t är oförändrat.

### LOD — `stride[256]` per klass

Båda polygonfrågorna tar en `stride[256]`: `stride[c] == N` behåller var N:te punkt
av klass `c`, och 0 och 1 betyder full täthet.

⛔ **Gallringen sker per RUTNÄTSCELL, på punktens plats i cellen** — aldrig med en
löpande räknare över träffarna. Därför beror urvalet inte på frågan: en stride på
10 ger **samma punkter utzoomad som inzoomad**, och en vy som panorerar eller
zoomar flimrar inte. Provat: 987 punkter ur en inzoomad fråga, varenda en
återfunnen i den utzoomade.

⛔ **Budget ströks ur planen.** En budget är till sin natur beroende av frågans
utsträckning — samma yta ger olika urval beroende på hur mycket annat som råkade
frågas samtidigt — och hade brutit precis den egenskap stride finns för.

Utan cache faller stride tillbaka på det **globala punktindexet**: samma andel,
men urvalet följer filens ordning och är inte spatialt jämnt. Att tyst ignorera
stride där hade varit värre än ett fel — anroparen ber om en tiondel och får allt.

`accepted[256]`, `classification` och `stride[256]` är förenade i
`Impl::ClassFilter` och appliceras **före** avkodningen av koordinaterna:
`SidecarReader::class_in_cell()` läser bara klassen med en enda bitläsning, så en
bortgallrad punkt kostar aldrig tre koordinatuppackningar.

### `.las` får en cache

`BuildSidecar()` kräver inte längre `.laz`. Provet som saknades finns nu — ingen
byggde en cache för en `.las`, och det var därför buggen nedan överlevde.

⚠️ **JAG UPPREPADE 1.1.3:S MISSTAG TVÅ GÅNGER — kod som byggdes men aldrig
kopplades in.** I 1.1.3 skrevs det spatiala indexet till filen medan `Reader`
skannade alla block ändå. Här hände det igen:

1. **Histogrammet** skrevs till headern medan `get_classification_histogram()`
   skannade som förut. Hittat först när frågan ställdes om alla delarna var klara.
2. **Den dubbla källan för `.las`** byggdes halvvägs: `get_xyz(i)` läste ur
   cellordningen medan `points().x(i)` läste ur filens, och vyn märktes som
   `kSidecarFormat` fast bufferten bar riktiga LAS-poster. **Idén är BORTTAGEN,
   inte lappad** — cachen äger ordningen för båda källformerna. Två ordningar
   bakom ett index ger tyst fel data, och det är inte värt en snabb `points()`.

### Granskning

**Sju blockerande fynd i första rundan**, alla åtgärdade: den dubbla källan;
nollskala som gav olika svar ur cache och källa; NaN- och Inf-skala som gav UB i
konverteringen till int64; rutnätsaritmetik utan spillkontroll för en fientlig
`cell_w`; `row_start[0]` och `cell_x`-ordningen som binärsökningen förutsätter;
`bad_alloc` genom det publika `open()`; och histogramsumman som inte validerades
mot punktantalet.

**Två till i granskningen av LOD:**

- **BLOCKERANDE:** det globala indexet trunkerades till `uint32_t` på den ocachade
  vägen. En LAS 1.4-fil får ha fler än 2³² punkter, och vid index 4294967296
  startade gallringsfasen om. `ClassFilter::keep()` tar nu `uint64_t`.
- `tests/bench_ab.cpp` var refererad från CMake men otrackad — en ren checkout
  hade inte byggt.

Provluckorna är fyllda: `class_in_cell()` jämförs mot `point_in_cell()` för
**varje** punkt i formattestet — en avvikelse där hade gett tyst fel klass i varje
filtrerad fråga — plus stride 0 och 1, klassvis stride, `accepted` och stride
samtidigt, fem `start`/`count`-omgångar med samma resultat, och den ocachade vägen.

Sex byggkonfigurationer gröna: Debug, Release, `HLAS_ENABLE_LAZ=OFF`,
`HLAS_ENABLE_NET=ON`, `BUILD_SHARED_LIBS=ON` och ASan+UBSan.

### API

- `Reader::get_xyz_polygon()` och `get_xyzc_polygon()` tar en `const uint8_t*
  stride` sist. Defaultargument, så befintlig källkod är oförändrad — men den
  manglade symbolen ändras, så ett program byggt mot 1.1.3:s delade bibliotek
  måste byggas om.
- `BuildSidecar()` accepterar `.las`.
- Cacheformatet är inte bakåtkompatibelt: en `.hlaz` från 1.1.3 används inte, och
  ändelsen är dessutom en annan. Bygg om.

### ⛔ Windows-bygget var trasigt i det som taggades först

1.1.4 byggdes, mättes och taggades på Linux. På Windows gick den inte att bygga
alls, och felen visade sig vara sex olika sätt att låta en plattform falla ur —
plus ett shared-bygge som inte länkade och en testsvit som inte tålde en svensk
sökväg.

**1. `windows.h` drog in makrona `min` och `max`.**
[src/core/path_utf8.h](../src/core/path_utf8.h) är det enda stället i biblioteket
som inkluderar `windows.h`, och den gjorde det utan `NOMINMAX`. Varje
`std::min(a, b)` i en fil som nådde hit blev `std::(a, b)`, och felet pekade på
std::min-raden i `sidecar_lite.cpp` — alltså långt ifrån orsaken. `file_io.cpp`
definierade visserligen `NOMINMAX`, men **efter** sin `#include "path_utf8.h"`:
spärren satt på fel sida om den include som behövde den, och räddade bara den fil
som råkade inte använda `std::min`. Den sitter nu i headern, där windows.h dras in.

**2. `utf8_to_wide()` tog bara `const char*`**, medan fyra anropsställen bär
sökvägen som `std::string`. En överlagring finns nu. Friktionen där lockar annars
någon att i stället nå efter `fopen()` med byten som de ligger — alltså tillbaka
till ANSI-tolkningen headern finns för att undvika.

**3. Testerna skrev till `/tmp`, som inte finns på Windows.** `TMPDIR` är en
POSIX-konvention; Windows har `TEMP` och `TMP`. Symptomet var vilseledande:
`LasWriter::create()` gav `FileOpenFailed`, och 34 rader i `test_las_writer`
rapporterade det som ett fel i SKRIVAREN fast ingen katalog fanns att skriva i.
(Den trettiofemte låg i `test_classification_flags`, av samma skäl.)
`testdata::tmp_dir()` i [tests/test_data.h](../tests/test_data.h) provar TMPDIR
först, så en Linux-körning beter sig exakt som förr. Fyra testfiler hade var sin
kopia av samma rad; nu finns den på ett ställe.

**4. `test_integration()` sprängde stacken.** Nio block med
`double bx[10000], by[10000], bz[10000]` ligger i EN funktion — drygt 2 MB. Linux
ger en tråd 8 MB stack, Windows 1 MB. Ramen allokeras vid funktionens **ingång**,
så den sprack även när varje block hoppades över för att testdatan saknades:
processen dödades med `0xC0000409` mitt i en helt orelaterad rad, och den
buffrade utskriften gjorde att den sista synliga raden pekade på fel test.
Buffertarna ligger nu på heapen. Biblioteket självt har inga stora stackposter —
det här var testkod.

**5. `std::remove()` tolkar byten i ANSI-kodsidan på Windows**, precis som
`fopen()` gör. Sidecar-skrivningens fyra `.tmp`-raderingar går nu genom `remove_utf8()`, och
namnbytet genom `rename_utf8()` — den senare samlar dessutom `MoveFileExW` och
`std::rename` på ett ställe, i stället för två grenar i skrivningen. Den här kraschar ingenting och syntes därför inte i något test —
den lämnar en `.tmp`-fil bredvid användarens data, för alltid, och bara för
sökvägar med tecken utanför kodsidan. Alltså ett fel som aldrig visar sig i en
körning i `C:\Temp`.

**Och en sjätte.** `CreateFileW` öppnade utan `FILE_SHARE_DELETE`. Windows vägrar då radera eller byta namn på en
fil så länge vi har den mappad, medan POSIX tillåter det och låter mappningen
leva vidare på det gamla innehållet. Följden var inte bara skräp i TEMP:
**`BuildSidecar()` misslyckades med `FileOpenFailed` om någon hade den gamla
`.hlz`-filen öppen**, eftersom `MoveFileExW` inte fick ersätta den — och samma
anrop lyckas på Linux. Flaggan är tillagd, och de två plattformarna beter sig nu
lika.

**Testerna själva öppnade filer med ANSI-API:er.** `std::fopen`, `std::remove`
och `std::ifstream` går alla via A-varianterna på Windows. Testerna skriver till
TEMP, och på en svensk maskin heter den ofta något i stil med
`C:\Users\Åsa\AppData\Local\Temp` — alltså hade hela sviten felat på en maskin
vars användarnamn inte är ren ASCII, vilket för målgruppen är det normala fallet.
`testdata::fopen_utf8`, `remove_utf8` och `ifstream_utf8` i
[tests/test_data.h](../tests/test_data.h) är vägen nu, och `tmp_dir()` läser
TEMP med `_wgetenv` i stället för `getenv`: `getenv` ger byten i ANSI-kodsidan,
alltså inte UTF-8, och sökvägen hade avvisats av precis den konvertering
biblioteket gör vid varje filöppning.

⛔ **Provat, inte antaget:** hela sviten körd med
`TMPDIR=%TEMP%\HLas Sträcka Öst\Åsa` — mellanslag, `ä`, `Ö` och `Å` — går grön,
och lämnar **noll** filer kvar i katalogen. Det är den sökväg
[src/core/path_utf8.h](../src/core/path_utf8.h) skrevs för, och den hade aldrig
prövats end-to-end förut.

**Och `BUILD_SHARED_LIBS=ON` byggde inget importbibliotek.** Windows exporterar
ingenting ur en DLL utan `__declspec(dllexport)`, och utan exporter skapas ingen
`.lib`: bygget föll på `LNK1181: cannot open input file HLas_core.lib`, ett fel
som pekar på en saknad fil i stället för på en saknad annotering.
`CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` låter CMake lista symbolerna ur objektfilerna
och generera en `.def`. Alternativet — ett dllexport/dllimport-makro genom hela
det publika API:t — hade lagt en plattformsdetalj i en header som medvetet inte
känner till någon, och varje ny publik funktion hade behövt komma ihåg det.

Med den på plats länkade bygget klart och vägrade sedan **starta**, med
`0xC0000135` och utan att säga vilken DLL som saknades: Windows har ingen RPATH,
och en `.exe` letar sina DLL:er i sin egen katalog medan modulerna byggs en per
katalog under `src/`. `hlas_stage_runtime_dlls()` kopierar dem bredvid varje
körbar efter bygget, och gör ingenting alls på Linux och macOS.

**Mätt på Windows** — MSVC 19.50 (VS 2026), `las/autzen.laz`:
`test_reencode_legacy` ger **214 av 214 hela chunkar byte-identiska**, alltså
samma kodarutdata som Linux-bygget ner till byten. Cachen bygger till 53,0 MB och
5,22 B/pt — exakt siffrorna ur Linux-mätserien. De fyra ormfrågorna ger 0,109 /
0,728 / 2,011 / 1,980 ms mot Linux-tabellens 0,101 / 0,727 / 2,138 / 2,194, alltså
samma storleksordning fråga för fråga. Fem byggkonfigurationer gröna:
Release, Debug, `HLAS_ENABLE_LAZ=OFF`, `BUILD_SHARED_LIBS=ON` och MSVC:s ASan.

⚠️ **`HLAS_ENABLE_NET=ON` är INTE bland dem.** libcurl finns inte på maskinen som
verifierade resten, så `find_package(CURL REQUIRED)` faller redan i
konfigureringen. Det är ett saknat beroende, inte ett fel i koden — men `src/net/`
har därmed inte kompilerats med MSVC, och det påståendet ska ingen göra åt den.

⚠️ **macOS är fortfarande inte provat.** Källan bär inget Linux-specifikt —
`madvise` och `MADV_SEQUENTIAL` finns även där — men ingen har byggt den på en
Mac, och det är ett antagande och inte en mätning. Står i TODO.

⚠️ **Taggen `v1.1.4` flyttades** till den här commiten i stället för att en 1.1.5
släpptes. Den som hämtade `v1.1.4` innan dess har en tagg med samma namn på en
annan commit och måste hämta om den.

## 1.1.3

### Cachen är omgjord — ~5 B/pt i stället för 13, och den skapas inte längre i smyg

`.hlaz` version 3. Den gamla cachen låg på **13 byte per punkt**, rått — 132 MB
för autzen och 1740 MB för en nationell laserruta, alltså större än båda
källfilerna. Den var dessutom **förstörande** utan
att säga det, och **skapades tyst** som en bieffekt av att läsa — bredvid
användarens data, och misslyckades utan ett ord på en skrivskyddad katalog.

Den nya är Hilbert-sorterad med frame-of-reference per block:

| | autzen (10,6 M) | laserruta (140 M) |
|---|---|---|
| gammal cache | 132 MB | 1740 MB |
| **v3** (block 512) | **54,4 MB** | **674,9 MB** |
| **B/pt** | **5,35** | **5,04** |
| källan som COPC | 77,4 MB | 689,4 MB |
| källan som `.laz` | 53,7 MB | — |

⚠️ **Rättelse mot en tidigare formulering här.** Den påstod att cachen är mindre
än källfilen, och jämförde autzen mot dess COPC-variant på 77,4 MB. Den vanliga
`autzen.laz` är 53,7 MB — alltså MINDRE än cachen, som vid block 512 är 1,3 %
större. Påståendet håller mot COPC, som bär punktformat 6 eller 7 med långt mer
per punkt än de fyra fält cachen håller, men inte mot en mager LAZ.

Vinsten som är sann överallt är **13 B/pt → ~5**. Mätserierna sa det rätt hela
tiden — de skriver "54,2 MB mot `autzen.laz`:s 53,7 MB, alltså i praktiken jämnt".
Det var sammanfattningen som valde den smickrande jämförelsen.

⛔ **Indexet byggs inte vid sidan om — det ÄR komprimeringen.** Frame-of-reference
måste lagra blockets min för att kunna packa bitarna, och blockets min och max är
dess bbox. Vi letade efter mindre disk och fick ett spatialt index på köpet.

Formen kommer ur sju mätserier, inte ur en gissning; de ligger i
[doc/plans/sidecar.md](plans/sidecar.md). Nyckeltalet bakom allt: uppackning
kostar ~400 ns per punkt, en skanning i rått minne under 1 ns. Därför är generisk
komprimering med zstd eller lz4 fel byte — den handlar tillbaka just den
egenskapen — och därför var ett rent index utan data inte värt det.

### Skapandet är skilt från läsningen — två halvor, inte en

```cpp
HLas::BuildSidecar(path);                     // SKAPAR — uttryckligen, en gång
r.open(path, HLas::Cache::Fast);              // ANVÄNDER den om den finns
```

⛔ **`Cache`-enumet skapar ingenting.** Alla tre lägena styr bara om en befintlig
cache får användas. `BuildSidecar()` är den enda vägen till en cache, och den
bygger för vilken `.laz` som helst — COPC inräknad.

Det är lätt att läsa `Cache::Fast` som "cacha den här filen". Det gör den inte.
Öppnar man med `Fast` utan att ha byggt något finns ingen cache att ta, och
källan packas upp som vanligt — tyst, eftersom det är ett giltigt utfall och inte
ett fel.

`Smart` är bara ett *läs*-undantag: den hoppar över cachen för en COPC-fil, som
redan har ett octree. Bygger man ändå en för en COPC-fil ignorerar `Smart` den —
då ska man välja `Fast`. Vill man inte cacha sina COPC-filer låter man bli att
anropa `BuildSidecar()` för dem.

⚠️ **`open(path)` rör inte längre någon cache.** Förvalet var `Cache::Fast` i allt
utom namnet; nu är det `Off`. Ett förstörande förval är fel väg när hela poängen
med läsningen är att bära hela punkten.

### Cachen lovar ingen indexmotsvarighet, och det är designat

Punkterna ligger i spatial ordning, inte källans. Index `i` i cachen är därför en
annan punkt än index `i` i filen. Att köpa tillbaka löftet hade kostat 30 MB för
källindexet, alltså mer än sorteringen sparade.

**Gränsen går vid råa poster, inte vid index.** `points()`, `point_data()` och
`read_points()` svarar tomt och sätter `last_error()` — högt, inte tyst — eftersom
cachen inte *innehåller* några poster att peka på. Allt annat fungerar, `get_xyz(i)`
och `get_classification(i)` inräknade: ett index är då en position i den läsaren,
och de två är eniga om vilken punkt de avser. `point_record_length` rapporterar
källans postlängd; en nolla där hade sett ut som en trasig fil.

DEM:en matas i omgångar in i samma rutnät via `dem_accumulate`, samma väg COPC
redan använde, så binningen finns på ett ställe.

### Ett index som byggs men inte används är inget index

Första utkastet skrev blockens bbox till filen och lät sedan `Reader` skanna alla
block ändå. Frågorna var alltså **O(N), inte O(N/B)**, och prestandapåståendena i
CLAUDE.md var falska för koden som låg — filformatet bar ett index som ingen
frågade. Fyndet kom ur granskningen, inte ur sviten, eftersom varje test gav rätt
*svar*: sållningen syns bara i tiden.

Sållningen sitter nu i `Impl::each_point_in_rect()` och används av
`get_xyz_polygon`, `get_xyzc_polygon`, `get_classification_histogram_polygon` och
DEM:en.

⛔ **Och den fixen bar en egen bugg: `start` och `count` slutade gälla.** Den
sållade vägen sökte hela cachen och använde `count` bara som utdatakapacitet.
Det gav två fel på en gång. En anropare som läser i omgångar med växande `start`
fick samma första träffar om och om igen och såg aldrig slutet av filen. Och
taket på antalet skrivna punkter försvann: frågan skrev så många punkter som ryms
i POLYGONEN i stället för så många som ryms i anroparens buffert — en
överskrivning av högen, styrd av filens innehåll.

Nu begränsas både block och punkter till `[start, start+count)`, och taket följer
av det. Två prov: samma fråga i sju omgångar ger samma mängd som ett anrop, och
ett vaktvärde efter taket står kvar orört.

README:s eget exempel bar samma fälla, och gjorde det redan före 1.1.3: det
deklarerade `xs[10000]` och skickade sedan hela filens punktantal som `count`.
Rättat till buffertar som rymmer det som begärs, plus ett sidvis mönster.

### Vad som avgör om cachen hör till filen

Punktantal, källans storlek, punktformat, skala och offset — plus ett
**fingeravtryck** över källans byte. Bara punktantalet räckte inte: byttes
`x.laz` mot en annan fil med samma antal användes den gamla cachen ändå, och
QGIS har samma bugg i samma form.

De fem första räcker inte heller hela vägen. En fil som klassas om och kodas om
till **samma längd** har samma punktantal, samma storlek, samma format och samma
kvantisering — och hade fått sin gamla cache godkänd.

⚠️ **Fingeravtrycket är ett stickprov, inte en checksumma**, och det ska inte
läsas som ett löfte. Det hashar 32 fönster om 4 kB spridda över filen, alltså
128 KiB för en fil som är stor nog att rymma dem utan överlapp. Det fångar det som ändrar filen brett — en omkodning,
en annan fil av samma längd — men en ändring som helt ligger mellan två fönster
passerar. Ett fullt hashvärde vore ett löfte och kostar en genomläsning av källan
vid varje `open()`, alltså hela poängen med 0 ms-öppningen.

Valet stod också mot filens mtime, som är billigare men fel av ett annat skäl: en
backup-återställning eller en `cp` utan `-p` hade slagit ut varje cache på
maskinen.

Cachens **egen** integritet kontrolleras inte alls. En bitflipp i blocktabellen
ger en ljugande bbox, och då försvinner punkter tyst ur en sållad fråga. Båda
ligger i TODO.md — delskrivningar är redan uteslutna av `.tmp` + rename, och
filen är regenererbar.

### Iterationen har ett ställe

`Impl::each_point()` tar grenen mellan platt buffert och cache **en gång per
anrop**, inte en gång per punkt, och lambdan inlinas i respektive loop. Det är
skälet `PointSource` är icke-virtuell, och det skälet gäller fortfarande.

### Vad som INTE finns än

Bara LITE — X, Y, Z och klass. `CUSTOM` och `FULL` är oprövade.

## 1.1.2

En läsväg som bär hela punkten, och LITE/FULL på skrivningen. Läsningen var
förstörande: `read_region`, `read_polygon` och `PointSink` lämnade bara
x/y/z/klass som doubles, så RGB, intensitet, GPS-tid, scanvinkel och returfälten
kastades — trots att uppackningen redan producerat hela posten.

**API-brytande.** Fas 16 bryter API:t ändå, så anroparna stod inför en migrering
oavsett, och två i samma vända är billigare än två i tur och ordning.

### Läsningen

`PointView` är den **fullständiga** läsformen: en icke-ägande vy över punktposter
med format-medvetna fältaccessorer. `point_data`, `read_points` och `get_xyz*`
finns kvar som bekvämlighet ovanpå samma data — skillnaden är att de inte längre
är det enda som går att få ut. Layouten för alla elva punktformat ligger i
`set_offsets()` i den publika headern.

- `Reader::points()` ger hela filen med allt källan bär.
- `CopcReader::read_region/read_polygon` levererar **råa poster**; `view()` tolkar
  dem. `PointSink` får en `const PointView&`.
- Konverteringen till doubles var *extra* arbete ovanpå uppackningen, inte en
  snabb väg. En enad läsning är billigare än den gamla, inte dyrare.
- Ett fält som inte ryms i den faktiska posten blir **osynligt**, inte farligt:
  en fil som deklarerar format 7 med 30 byte per post ger `has_rgb() == false` i
  stället för sex byte lästa ur nästa punkt.

### Skrivningen

`WriteMode::Lite` (standard) skriver X, Y, Z och klass som punktformat 6.
`WriteMode::Full` behåller det källan har, och utdataformatet följer källan.

FULL sätts bara via `create_like()` — utan en källa finns inget format och ingen
kvantisering att ärva, och läget hade tyst fallit tillbaka på format 6.

**Stödda källformat är 0–3, 6 och 7.** Format 4 och 5 kräver `Wavepacket13`,
format 8 `RGBNIR14`, format 9 `Wavepacket14` och format 10 båda; ingen av dem
finns som kodare och det finns ingen testfil att verifiera mot, så de avvisas —
liksom extra bytes. En översättning mellan formaten vore en *konvertering*, inte
ett bevarande.

De två formatfamiljerna är **olika filupplägg hela vägen**, och det är där
arbetet låg. Punktformat 6–10 skrivs som LAS 1.4 med compressor 3: nio lager per
chunk, var och en med sin egen aritmetiska ström, och ett punktantal i chunken.
Punktformat 0–3 skrivs som LAS 1.2 med compressor 2: seed-posten rått följd av
EN ström där alla items kodas efter varandra per punkt — ingen lagerindelning,
inget punktantal, antalet står i chunk-tabellen. Filversionen följer alltså
formatet och inte källans versionsfält: punktformat 6 finns inte i 1.2, och de
äldre har ingen anledning att skrivas i 1.4.

`global_encoding` och `file_source_id` ärvs från källan. Det förra för att bit 0
bär GPS-tidens typ, och en nolla där gör tiderna feltolkade av den som läser — det
är punktdata i allt utom namnet. I 1.2 är bara bit 0 definierad, så resten maskas
bort; i 1.4 läggs bit 4 (WKT) till, som formatet kräver.

Verifierat mot 250 123 punkter ur `autzen.las`, punktformat 3 med RGB och
GPS-tid, skrivna i format 0, 1, 2 och 3 — komprimerat och okomprimerat, med en
partiell sista chunk: **noll avvikande råposter i alla åtta fallen**. Testet läser
dessutom filens egna byte och kontrollerar headerstorlek, version,
`offset_to_point_data`, compressor-värdet, hela itemlistan i LAZ-VLR:n och de
ärvda headerfälten — inte bara det `Reader` tolkar fram.

Mätt på hela testfilen, 1 777 643 punkter i punktformat 7:

| | tid | storlek | per punkt |
|---|---|---|---|
| LITE | 202 ms | 7 094 742 byte | 3,99 B |
| FULL | 311 ms | 14 679 681 byte | 8,26 B |

LITE är alltså **48 % av FULL** och 1,54x snabbare. Skillnaden är inte att lager
hoppas över: `classification`, `flags`, `intensity` och `user_data` kodas för varje
punkt även i LITE. Tre av de fyra lagren blir sedan tomma eftersom fälten är
konstant noll — men `classification`-lagret gör det inte, för LITE bevarar
klassningen och den varierar. Det LITE slipper är RGB-itemet helt, samt arbetet i
de villkorliga lagren för scanvinkel, point source och GPS-tid, som aldrig ändras
när fälten är noll.

FULL blir 0,6 % större än originalfilen: 94 920 byte mer komprimerad punktdata,
minus 5 111 byte enklare filstruktur. Orsaken är **chunkgränserna**, inte antalet
chunkar — COPC-filen har en chunk per octree-nod, alltså 75 spatialt sammanhållna
block, medan vi skriver 36 fasta 50000-block. Med originalets gränser producerar
kodaren exakt originalets byte, vilket 75/75-provet nedan visar.

### Kodaren

`Point14Encoder` gick från tre lager till nio, och `RGB14Encoder` tillkom.
Kodaren använder `Point14Context` respektive `RGB14Context` — samma structar som
avkodarna — med fyra kontexter, en per scannerkanal. GPS-tiden **väljer som
referensens kodare** och är inte en spegling av avkodaren: avkodningen läser ett
multi-värde, kodningen måste välja det.

Lagerpolicyn är avläst ur referensens källkod och inte antagen. Den är inte
enhetlig: `classification`, `flags`, `intensity` och `user_data` kodas för *varje*
punkt men lagret skrivs bara om något ändrats, medan `scan_angle`, `point_source`
och `gps_time` kodas bara vid ändring.

### Verifieringen

`tests/golden_ref.h` fryser fjorton chunkar producerade av **referensimplementationen**, som
vi inte skrivit — tretton syntetiska kantfall som testfilerna saknar, plus ett
riktigt. Vår avkodare klarar alla fjorton, och vår kodare producerar alla fjorton
byte för byte.

`tests/golden_ref10.h` gör samma sak för punktformat 0–3, som har ett annat
filupplägg: fjorton chunkar utan lagerindelning. Alla fjorton avkodas rätt och
kodas byte för byte.

`tests/test_reencode.cpp` packar upp hela testfilen med våra avkodare, kodar om
den med våra kodare och jämför mot filens egna byte: **75 av 75 HELA noder
byte-identiska, 1 777 643 punkter** — seed-post, punktantal, lagerhuvuden och
lagerdata. Filen är skriven av ett tredjepartsverktyg, alltså varken av oss eller av referensen.

`tests/test_reencode_legacy.cpp` gör detsamma med hela `autzen.laz`, punktformat
3: **214 av 214 HELA chunkar byte-identiska, 10 653 336 punkter**. Där ligger
chunkgränserna dessutom redan rätt, eftersom filen använder samma chunkstorlek som
vi — 50 000 punkter.

Hela den filen går att göra räkenskap för. Vår utdata är 1195 byte mindre än
originalet, och varje byte av skillnaden är mätt: punktdatan är identisk
(56 348 978 byte), chunk-tabellen är byte-identisk (474 byte), 1163 byte är fyra
VLR:er vi inte bär — tre projektions-VLR:er och en verktygs-VLR — och 32 byte är data
originalet har efter chunk-tabellen.

**Och referensen läser tillbaka det vi skrev.** Ett nytt `readlaz`-läge i generatorn
utanför repot packar upp en fil HLas producerat:

10 653 336 poster, **noll avvikelser** mot vad som matades in. Att vår egen
avkodare klarar vår egen kodares utdata bevisar bara att de är eniga; det här är
skrivningen prövad av en implementation vi inte skrivit.

Det provet ensamt visar bitströmskompatibilitet, men inte att posterna *betyder*
rätt: ett ömsesidigt inverst fel i vår egen avkodare och kodare hade överlevt det.
Därför gjordes också det omvända provet, med generatorns `records`-läge:

Referensen packar upp samma 75 noder och dumpar posterna; HLas packar upp samma fil
och jämför. **0 avvikande poster av 1 777 643.** Två oberoende avkodare ger alltså
samma innehåll, inte bara samma byteström.

Kvar som osäkerhet: ett fel som HLas och referensen DELAR skulle överleva båda
proven. Det kräver en tredje avkodare att utesluta, och den finns inte i
räckhåll.

### Nätverkslagret går att ställa in

`CopcReader::set_network_settings()` tar en `NetworkSettings` med **rena värden** —
inte ett HTTP-handtag, eftersom den publika headern medvetet inte känner till
libcurl. Fälten speglar det anroparen redan vet om sitt nät: proxy i tre lägen,
CA-fil eller plattformens eget lager, verifiering av och på, HTTP-version,
komprimering, skalade timeouts, spårning och User-Agent.

**Ett förvalt objekt ändrar ingenting.** Fälten är valda så att de återger
beteendet från före de fanns.

Bakgrunden: hela uppsättningen var hårdkodad, och på en Windows-jobbdator där
libcurls inkompilerade CA-sökväg inte fanns gav den första förfrågan
`CURLE_SSL_CACERT_BADFILE` — kod 77, alltså "CA-lagret gick inte att läsa in alls",
inte kod 60 som betyder att ett certifikat underkändes. Ingen väg fanns utifrån
för att peka ut en CA-fil, använda systemets trust store eller stänga av
verifieringen.

Fyra fällor, alla prövade som beslut i `tests/test_net_settings.cpp` — testet
granskar planen, inte anropen mot libcurl:

- `ProxyMode::None` och `System` är **motsatser**. `None` sätter tom sträng, vilket
  stänger av proxyn och ignorerar `http_proxy`/`https_proxy`; `System` sätter
  ingenting alls, och det är frånvaron som får libcurl att läsa dem.
- **Namnkontrollen följer kedjekontrollen**, 1/2 eller 0/0. På ett nät med
  TLS-inspektion är det just namnkontrollen som fäller anslutningen, så en
  avstängning som lämnar den kvar gör inte det anroparen bad om.
- Tom `ca_file` betyder **sätt ingenting**, inte hitta på en sökväg.
- Komprimering av sätts **uttryckligen till `nullptr`**. Handtaget återanvänds över
  alla range-läsningar, och där är "hoppa över anropet" inte samma sak som "av".

Proxyns TLS **härleds** ur samma fält i stället för att ha egna, vilket antar en
gemensam CA-rot — sant på ett nät med TLS-inspektion, och relevant bara när
proxy-URL:en själv är `https`. Paret sätts uttryckligen även när verifieringen är
av: att hoppa över anropen hade lämnat libcurls förval 1/2 kvar, och proxy-benet
hade fortsatt verifiera trots motsatt begäran.

⚠️ Inställningarna följer med i `clone()`. Med `thread_count > 1` kopplar varje tråd
upp på nytt, och en klon med förvalen hade felat mitt under en hämtning i stället
för vid `open_url()`.

`timeout_scale` når **både** uppkopplingen och varje enskild läsning — den senare
är den som failar först på en långsam förbindelse. Noll och negativa tal tolkas som
100 och resultatet kläms nedåt till en sekund, eftersom noll betyder "ingen gräns"
i libcurl.

Besluten ligger i `make_curl_plan()`, skilda från anropen som sätter dem: fällorna
sitter i besluten, och som ett värde går de att pröva utan nät eller server.

### Nio läsarbuggar som skrivandet avslöjade

1. **RGB14 och flera scannerkanaler.** Formatet har ett egendomligt beteende: vid
   byte till en kanal som redan använts pekar `lastColor` fortfarande på
   *föregående* kanals färg, både för prediktion och tillbakaskrivning, medan
   modellerna hämtas ur den nya kanalen. HLas gjorde "rätt" och avkodade därför
   **flerkanaliga punktformat 7-filer med fel färger**, tyst.
2. **GPS-tidens fyra fack.** `init_context` fyllde alla fyra med seed-tiden;
   referensen fyller bara fack 0. En fil med seed-tid 1000,0 och nästa tid 0,0
   får en konform kodare att välja ett nollställt fack, och vi hade räknat från
   fel värde.
3. **Klassificeringen maskades inte i punktformat 0–5**, där klassbyten delas med
   synthetic, key-point och withheld. En markpunkt med synthetic satt lagrades som
   34 och var osynlig för `accepted[2]`. Rättat i hela kedjan, och `HLAZ_VERSION`
   bumpad till 2 så gamla sidecars byggs om.
4. **`generate_dem` kunde terminera processen.** Stor polygon med liten cell gav
   ett odefinierat cast och sedan `std::bad_alloc` ur en allokering på hundratals
   GB. Taket `kDemMaxCells` ger nu ett tomt resultat.
5. **`PointView` kunde vara giltig och ändå läsa utanför posten** — rättat med
   `clip_offsets_to_stride()`.
6. **Buffertaritmetiken i `CopcReader::collect`** kontrollerade inte att
   `max_points * record_length` ryms i `size_t`.
7. **GPSTime11 räknade fram fel referensfack.** Referensen håller TVÅ räknare:
   `last` flyttas av ett sekvensbyte, `next` pekar ut facket som tas i bruk vid
   nästa fulla värde. Vi räknade fram det nya facket ur `last`, vilket är rätt
   ända tills ett sekvensbyte skilt dem åt — och först då, vid nästa fulla värde,
   hamnar avkodaren i fel fack och ger fel GPS-tider för resten av chunken. Det
   rör punktformat 1, 3, 4 och 5; Point14-lagret hade det rätt sedan tidigare.
   Med buggen kvar föll tre av de fyra GPS-bärande gyllene vektorerna.
8. **GPSTime11 räknade prediktionen i int64 i stället för int32.** Prediktionen
   `multi * delta` skickas till `IntegerCompressor` och ska wrappa i 32 bitar.
   Kodaren bildar korrektorn mot det wrappade värdet, så det här var inte bara en
   avvikelse från referensen utan en oenighet mellan vår egen kodare och
   avkodare — som slår till när produkten spiller över. Point14 gjorde redan
   rätt. Hittad av granskningen, inte av vektorerna: referensen väljer inte
   multiplar som spiller över på de indata vi frusit.
9. **Ett okänt LAZ-item hoppades tyst över** i den chunkade läsvägen. Items delar
   EN aritmetisk ström och ligger efter varandra i posten, så ett item vi inte
   avkodar flyttar allt som kommer efter det. I praktiken gäller det
   `Wavepacket13`, alltså punktformat 4 och 5, som lästes **utan fel men till
   skräp**. Nu avvisas de med `LazNotSupported`, och objektstorlekarna
   kontrolleras på samma sätt som i den lagerindelade vägen.

### Två avsiktliga avvikelser från referensen

Båda är ställen där referensen har odefinierat beteende och vi inte vill ha det:
kvoten i GPS-tidens `multi` kläms före castet till int32, och en `return_number`-
symbol utanför modellen avvisas i stället för att indexera förbi. Den senare
grenen är dessutom oåtkomlig — differensen −15 kräver 15 → 0, som klassas som en
ökning.

## 1.1.1

Egen LAZ-kodare. Referensen är borta ur bygget, och repot har åter noll
obligatoriska externa beroenden — libcurl är fortsatt valfritt bakom
`HLAS_ENABLE_NET`.

**Kodaren.** `ArithmeticEncoder`, `IntegerCompressor::compress` och
`Point14Encoder` är speglar av de avkodare som redan fanns, och delar deras
modeller i stället för att kopiera dem — två kopior kan glida isär, en kan inte.
Skrivaren äger filen: LAS 1.4-header, LAZ-VLR, chunkning om 50000 punkter och
chunk-tabell.

**Sex av nio lager skrivs med längd 0.** En skriven punkt bär bara X, Y, Z och
klass, så flags, intensity, scan angle, user data, point source och GPS-tid är
konstant noll. Det är formatets egen mekanism, verifierad i referensens källa
(`xy_enc_ = true`, `z_enc_ = true`, övriga `false` + `makeValid()`), inte ett
antagande. `Point14Fields` är en reducerad struct just för att de fälten inte ska
kunna få ett värde, och `point14_fields_tripwire()` gör en utökning till ett
kompileringsfel.

**Verifierat mot referensen, inte bara mot oss själva.** Rundgång genom vår egen
avkodare hade varit blind för en gemensam missuppfattning — och en sådan fanns.
Mätt på 10653336 punkter ur autzen.las med samma skala och offset:

| | HLas | referensen |
|---|---|---|
| storlek | 29 773 303 byte | 29 773 303 byte |
| skrivning | 895–1091 ms | 1338–1692 ms |

Hela filerna skiljer sig på **35 av 29 773 303 byte**, alla i header och VLR. All
komprimerad punktdata och hela chunk-tabellen är byte-identisk. 29 av byten är ren
identifikation; de sex övriga är två ställen där vi följer LAS 1.4 och referensen
inte gör det — legacy point count ska vara noll för punktformat 6, och
returtabellen ska summera till punktantalet.

De 1,5x beror inte på bättre kodning utan på att referensen kodar tre lager per
punkt som den sedan kastar. Räkna med att skillnaden försvinner när 1.1.2 bär
alla fält.

**Fyra buggar i den befintliga läsaren, alla funna av att skriva:**
- `readCorrector` kortslöt vid `k >= bits_` i stället för `k == 32`. För
  16-bitars kompressorer — intensity, scan angle, point source — tappade
  strömmen fasen när korrektorn var exakt `corr_min`, och varje efterföljande
  punkt i chunken blev fel. Mätt mot referensen: 19 av 20 värden fel före, 0 efter.
- Chunk-längderna kumulerades i `uint32` och wrappade förbi 4 GiB komprimerad
  punktdata. De enskilda längderna är `uint32` — det är formatet — men summan är
  det inte.
- `num_chunks + 1` räknades i `uint32` och wrappade till noll vid `UINT32_MAX`,
  varefter varje indexering gick utanför vektorn.
- Odefinierat beteende i GPS-tid-lagret: `multi * last_gpstime_diff` var signed
  overflow på fem ställen, GPS-summorna likaså, och två signed vänsterskift.
  Semantiken *är* wrappning, men den fick inte skrivas som UB. Nya hjälpare
  `IMul32`, `ISum64` och `IDiff32`.

**Verifiering.** Gyllene bytevektorer i testsviten för IntegerCompressor och
Point14, alla avkodade av referensen innan de skrevs in — beviset överlever att
referensen tas ur bygget. Fuzz på kodare/avkodare-paret med krav på att
carry-vägen faktiskt körs (147290 propageringar, varav 669 byte genom 0xFF-svit).
Strukturell filvalidering som läser råa byte. Hela filer lästa av referensens egen
läsare från 1 till 120000 punkter. ASan+UBSan rena.

**Utskriftens header** följer nu LAS 1.4 på två punkter till: `global_encoding`
bit 4 (WKT) sätts för punktformat 6, och returtabellen fylls i.


## 1.1.0

Modulär uppdelning, riktigt COPC-stöd med partiell hämtning över HTTP, och
LAS-utskrift. Senast släppta version före det var 1.0.5.

**Modulär uppdelning:** `src/core` (LAS/LAZ-parsning, noll beroenden),
`src/copc` (octree-index), `src/dem` (grid-binning), `src/net` (HTTP, opt-in),
`src/write` (utskrift), `src/manager` (Reader, CopcReader, sidecar, licensgate — borttagen igen, se Nästa).
Beroenden pekar åt ett håll och gränsen upprätthålls av include-stigarna, så ett
felaktigt beroende blir ett kompileringsfel och inte en kodgranskningsfråga.
`HLas::core` går att länka ensam — 452 KB som läser punkter utan DEM, Reader
eller licenskod.

**`CopcReader` — partiell läsning ur COPC**, lokalt eller över HTTP.
Octree-indexet läses med tre riktade läsningar (9485 byte av 81 MB), bara berörda
noder packas upp, och en fråga som träffar 20 av 278 noder tar 739 ms mot
10 135 ms för `Reader::open()` på hela filen.

Nodurvalet är **polygonmedvetet**, vilket är avgörande för korridorer: en linje
buffrad ±12 m har en omslutande rektangel hundra gånger större än korridoren
själv. 278 → 52 noder, 4,9x mindre data, identiska punkter.

Därtill strömmande läsning nod för nod, `accepted[256]`-filter, LOD via
`max_level`, och DEM direkt ur COPC-filen — cell för cell identisk med
`Reader::generate_dem()`.

**Parallellism med två oberoende rattar:** `thread_count` är den egna
processorn, `set_max_connections()` någon annans server.

| | 1 tråd | 8 trådar |
|---|---|---|
| lokal fil | 1764 ms | 284 ms |
| VPS i Sverige | 3226 ms | 897 ms |

HTTP range requests via libcurl bakom `HLAS_ENABLE_NET`, med HTTP Basic för
nedladdningsservrar som kräver det. Verifierat mot en 1,26 GB-ruta med 261
miljoner punkter.

**Utskrift.** `LasWriter` skriver okomprimerad LAS 1.4 punktformat 6, strömmande
— format 6 och inte 1.2 format 0, där klassificeringen delar byte med flaggbitar
och klasser över 31 förvanskas. `LazWriter` skrev komprimerat *tillfälligt via
referensen* bakom `HLAS_ENABLE_LAZ_WRITE`; den ersattes av en egen kodare i 1.1.1.
Utbredningen i en skriven fil beskriver de **skrivna punkterna**, inte källan —
ett korridorurval ur en 10×10 km-ruta får korridorens bounds.

**Rättade buggar:** heap buffer overflow i den lagrade LAZ-stigen; avkodare som
läste bortom indata; odefinierat beteende i DEM:ens atomiska min-skrivning; en
flyttad-från `Reader` som dereferade null; UTF-8-sökvägar på Windows; HTTP-svar
med fel `Content-Range` som accepterades; saknad protokollbegränsning; och
COPC-undersidor som aldrig följdes — den sista gjorde att **44 % av en
nationell laserrutas punkter tyst uteblev**.

Testsviten kör i fyra byggkonfigurationer och hoppar över i stället för att
krascha när testdata saknas.

## Fas 1: Binära strukturer + felhantering ✓
## Fas 2: Memory-mapped fil I/O ✓
## Fas 3: Header-parsning ✓
## Fas 4: VLR/EVLR-parsning ✓
## Fas 5: Punktdata-läsare ✓
## Fas 6: Publik API (HLas::Reader) ✓

## Fas 7: Validering, filtrering, DEM ✓
- [x] `tests/test_integration.cpp` — tester mot riktig LAS-fil (6M punkter)
- [x] Full XYZ-scan, RGB, points-by-return, reopen, raw/batch
- [x] Batch-hastighetstest (120 Mpts/s, ~3 GB/s)
- [x] Klassificeringsfilter — `get_classification()`, `get_xyz_filtered()`
- [x] Polygonfilter — `get_xyz_polygon()` med ray casting + AABB pre-filter
- [x] DEM-generering — `generate_dem()` med grid-binning (min Z per cell)
- [x] Fill-metoder: None, Nearest (5 celler), IDW (1/d², 5 celler)
- [x] TIN-interpolering — Bowyer-Watson Delaunay med adjacency-baserad BFS
- [x] `src/dem/dem.h` + `.cpp` — fill_nearest, fill_idw, fill_tin
- [x] `src/core/point_reader.h/cpp` — classification_offset

## Fas 8: Native LAZ-dekompression ✓
- [x] CMake-option: HLAS_ENABLE_LAZ
- [x] Nativ LAZ-dekompression — ingen extern beroende
- [x] Chunked (LAS 1.2–1.3): Point10, GPSTime11, RGB12, Byte
- [x] LayerChunked (LAS 1.4): Point14, RGB14, RGBNIR14, Byte14
- [x] Aritmetisk kodare (LAZ-kompatibel)
- [x] COPC-filer läsbara — men bara som vanlig LAZ 1.4, octree-indexet används inte (se Fas 15)
- [x] Transparent i Reader::open() — samma API för LAS och LAZ

## Fas 9: LAS 1.4 + parallell DEM + licenssystem ✓
- [x] LAS 1.4 format 6–10 med extended return numbers
- [x] `generate_dem()` med thread_count — parallell punktscan + atomic min-write
- [x] AABB-shortcut i generate_dem() för rektangulära polygoner
- [x] `LicenseData` + `SetLicense()` + `HasValidLicense()` — modul "LAS"
- [x] Licensguard på alla punktdata-funktioner

## Fas 10: Klassificeringshistogram + accepted[]-filter ✓
- [x] `get_classification_histogram()` — single-pass scan, LAS + LAZ
- [x] `get_classification_histogram_polygon()` — histogram inom polygon
- [x] `get_xyzc_polygon()` — accepted[256]-parameter för tidig exit
- [x] `get_xyzc_polygon()` — XYZ + klassificering per punkt
- [x] `generate_dem()` — `accepted[256]` istället för enskild `classification`

## Fas 11: Parallell TIN-triangulering ✓
- [x] `fill_tin()` — tile-baserad Bowyer-Watson, horisontella strips med 100-cells overlap
- [x] Parallell triangulering per tråd
- [x] `src/CMakeLists.txt` — länka `Threads::Threads`
- [x] `tests/bench_dem.cpp` — DEM-prestandamätning

## Fas 12: .hlaz-sidecar ✓
- [x] `Reader::open(path, no_cache=false)` — sidecar `<fil>.hlaz` bredvid `.laz` för snabb open (mmap, ~0 ms)
- [x] Sidecar-format: 13 B/pt fixed-stride (int32 X/Y/Z + uint8 class), 256-byte header
- [x] Atomisk skrivning via `.tmp` + rename, seekable utan att läsa hela filen i minnet
- [x] `src/manager/hlaz_sidecar.h` + `.cpp`
- [x] Hot paths (DEM, polygon, histogram) oförändrade — läser via samma `pt_raw()`-stig
- [x] `HLas_test_sidecar` — identiska XYZ + class + histogram mellan `no_cache` och sidecar
- [x] `bench_dem` mäter både default (sidecar) och `no_cache` (LAZ-baseline)

## Fas 13: Felrapportering + licensgate ✓
- [x] `Reader::last_error()` — `const std::string&`
- [x] `Status::NoLicense` + `status_message()` "No valid license"
- [x] Licensgate via `Impl::licensed()` — alla 11 punktdata-funktioner sätter `last_error`
- [x] `get_classification_histogram(_polygon)` returnerar `Status::NoLicense`, nollställer `counts[256]` före gaten
- [x] `open()` sätter `last_error` vid fel, rensar vid `Status::Ok` (inte licensgatad)
- [x] `tests/test_license.cpp` — verifierar last_error, NoLicense, nollställda counts, rensning efter licensierat anrop
- [x] CLAUDE.md + README.md — felrapportering dokumenterad

## Fas 14: Modulär uppdelning ✓
**Görs först.** Allt som följer bygger på de här gränserna: `ByteSource` i Fas 15 är
kärnans sömm, och amalgameringen i Fas 16 följer modulgränserna. Byggs COPC och HTTP
ovanpå dagens monolitiska `reader.cpp` får kärnan trås igenom en gång och dras isär en
gång till — och en extern dep landar i odelad kod, vilket är precis det uppdelningen
ska förhindra.

Ett target per modul, med egen katalog och egen publik header. Poängen är att den som bara
vill läsa en `.las` tar `core` och ingenting annat, och att en eventuell extern dep (HTTP)
är instängd i en katalog som är opt-in.

```
core/     LAS/LAZ-parsning + dekompression + ByteSource-interface   → noll beroenden
copc/     octree-index, nod-uppslagning                             → beror på core
net/      ByteSource över HTTP range requests                       → enda stället en extern dep får landa
dem/      grid-binning, nearest/IDW/TIN                             → beror på core
manager/  sidecar-cache, LOD-policy, trådning                       → beror på allt ovan
```

- [x] **Regel:** beroenden pekar åt ett håll. `core` får aldrig veta att `net`, `dem`,
      `copc` eller `manager` existerar. Ingen cirkularitet.
- [x] CMake-targets `HLas::core`, `HLas::dem`, `HLas::manager` — konsumenten länkar bara
      det den vill ha; `HLas` är ett INTERFACE-paraply som drar in alla tre.
      `HLas::copc` och `HLas::net` tillkommer i Fas 15, `net` bakom en opt-in-flagga.
- [x] Tunna ut `reader.cpp` — 743 → 554 rader. `Impl` höll licensgate, sidecar, LAZ-dispatch
      och DEM i samma klass; nu återstår punktdata-API:t och licensgaten. Gjordes i tre steg:
      PointSource-vyn, DEM ut till `dem/`, sidecar-cachen ut till `manager/`.
- [x] `src/laz/` → `src/core/laz/` — redan självständig, flyttar nästan gratis
- [x] `src/dem.*` → `src/dem/` — beror bara på en punktström, flyttar nästan gratis
- [x] `ByteSource` definieras i `core` — mmap-fil i `core`, HTTP i `net`
- [x] `manager` ska vara valfri — `HLas::core` går att länka ensam (452 KB, ingen DEM,
      ingen licenskod), verifierat med ett program som läser punkter utan Reader
- [x] Per-modul-test: `HLas::core` länkad ensam läser punkter utan dem/manager/licens
- [x] Modulgränsen verifierad — `core` kan inte inkludera `dem.h` eller `cached_laz_source.h`,
      det blir ett kompileringsfel
- [x] `copc/` och `net/` skapade i Fas 15
- (flyttad till 1.1.x — samma arbete som "Bryt ut licensen" i Fas 16)

## Fas 15: Riktigt COPC-stöd — partiell hämtning ✓
Före den här fasen lästes COPC bara som vanlig LAZ 1.4: hela filen dekomprimerades eagerly
och octree-indexet ignorerades. Nu hämtas *delar*, lokalt och över HTTP.

Mätt på en nationell laserruta (10x10 km, 1,26 GB, 261 miljoner punkter): `open` tar 792 ms
eftersom bara indexet läses, och en 250x250 m-fråga hämtar 27 av 4827 noder.

- [x] `src/copc/` — ny modul, beror bara på core
- [x] Parsa `copc info`-VLR (user_id `copc`, record_id 1) — rot-nod, spacing, offset/storlek till hierarkin.
      Kräver att den är *första* VLR:n enligt spec; annars `NotCopc` och fallback till vanlig LAZ.
      Garantin är inte formalia — den låter HTTP-stigen hämta header + info i en enda range request.
- [x] Läsa hierarki-EVLR — octree-noder (level/x/y/z → chunk-offset, byte-storlek, punktantal).
      Verifierat mot Autzen: 278 noder, nivå 0–5, punktsumman är exakt filens `point_count`.
- [x] **Bounds ligger i `ByteSource`, inte hos varje anropare.** `range_ok()` i subtraktionsform,
      obligatorisk i varje implementation. Hela intervallet valideras dessutom *innan* hierarkin
      allokeras, plus ett tak på 64 MB per sida — en fjärrkälla kan ljuga om sin storlek.
- [x] **Hierarkisidor hämtas i omgångar, inte en i taget.** Att följa undersidor tog
      öppningstiden mot en nationell laserruta från 792 ms till 33 s: 154 sidor, en rundtur
      var. Sidorna visade sig ligga perfekt sammanhängande — 391456 byte, noll glapp — så
      de hämtas nu i en enda läsning. 4 förfrågningar i stället för 157, 2,4 s i stället
      för 33. Glapp upp till 256 KB överbryggas; större delar upp körningen.
      Tvärtemot nodhämtning, där sammanslagning vore fel: noder ligger utspridda, men
      hierarkisidor skriver skrivaren tillsammans.
- [x] **Undersidor i hierarkin följs.** En COPC-hierarki kan delas på flera sidor, där en
      post med `point_count == -1` pekar ut nästa. Tidigare lästes bara rotsidan och
      pekarposterna hoppades över överallt — en uppdelad hierarki hade alltså tyst gett ett
      ofullständigt index utan att något felade. Varken Autzen (278 noder) eller en
      nationell laserruta (4827) delar upp sin, så stigen fanns inte ens.
      Tak på 4096 sidor och 8M noder, cykeldetektering på besökta offsets, och
      nodtaket kontrolleras före allokering. Pekarposterna tas bort ur resultatet — de är
      ett format-internt mellansteg.
      Testat med en källa som serverar den riktiga Autzen-filen men med hierarkin omstöpt
      till tre sidor (78 + 100 + 100): identisk nodmängd, och en självrefererande sida avvisas.
- [x] Lazy chunk-dekompression — `LazReader::init_layout()` (bara punktlayout, ingen chunk-tabell,
      ingen dekompression) + `decompress_chunk()` för en ensam självständig chunk.
      `read_copc_node()` hämtar och packar upp en nod. Mätt: 9 noder / 347867 punkter på 325 ms
      mot 10 010 ms för hela filen, och bit-identiska med den fullständiga uppackningen.
- [x] Polygonmedvetet nodurval — `select_copc_nodes_polygon()` testar nodens kub mot
      polygonen i stället för mot dess omslutande rektangel. Avgörande för korridorer:
      en linje buffrad ±12 m över 5 km har en AABB 104 gånger större än korridoren, och
      urval mot den hämtade hela filen. Mätt på Autzen: 278 → 52 noder, 4,9× mindre data,
      identiska 89865 punkter. `CopcReader::estimate_polygon()` i det publika API:t.
- [x] Spatial query — `select_copc_nodes()` väljer noder vars kub överlappar en XY-rektangel.
      Verifierat att inget missas: 237101 punkter i en ruta, identiskt räknat ur hela filen
      och ur bara 20 av 278 noder. Hämtar 7,5 % av filens byte.
- [x] LOD-query — samma funktion med `max_level`. **Obs:** COPC-noder är inte redundanta,
      djupare noder innehåller ytterligare punkter snarare än kopior. Ett nivåtak ger alltså
      ett glesare moln, inte ett komplett moln av färre noder. Bevisat av att summan över
      alla noder är exakt filens punktantal.
- [x] `copc_node_bounds()` — nodens kub ur rotens center/halfsize och nyckeln, vidgad en ULP
      utåt så att flyttalsavrundning inte kan lämna glapp mellan intilliggande noder.
- [x] `ByteSource`-implementation över HTTP i `net/` — `HttpByteSource` med libcurl,
      bakom `HLAS_ENABLE_NET=OFF`. Enda modulen med externt beroende; paraplyet
      refererar den bara när flaggan är på, så övriga konsumenter ser aldrig libcurl.
- [x] SSRF dokumenterat — `open_url()` begränsar till http/https genom hela
      omdirigeringskedjan och följer högst fem hopp, men värdpolicyn är anroparens
      ansvar: biblioteket har inget underlag för att avgöra vad som är internt i
      någon annans nät.
- [x] HTTP range requests — verifierat mot tre servrar (lokal webbserver, VPS i Sverige, S3),
      alla ger samma 237101 punkter som den lokala mmap-stigen.
- [x] Strömmande läsning — `read_region_streaming()` / `read_polygon_streaming()` med
      `PointSink`-callback, en batch per nod. Löser att `estimate()` är en lös övre gräns:
      på en nationell laserruta 9,4 miljoner uppskattade punkter mot ~800 000 verkliga, alltså
      0,2 GB buffertar för ~26 MB data.
- [x] `accepted[256]`-filter i `CopcReader` — samma mening som `Reader::get_xyzc_polygon`.
      Filtret körs före koordinatskalningen, så det är också det billigaste snittet.
- [x] `CopcReader::generate_dem()` — binnar nod för nod in i samma rutnät. Krävde att
      `dem.cpp` delades i `dem_make_grid` / `dem_accumulate` / `dem_fill`; verifierat
      bit-identiskt och att COPC-DEM matchar `Reader::generate_dem` cell för cell.
- [x] UTF-8-sökvägar på Windows — `CreateFileW`, `_wfopen`, `MoveFileExW` via
      `src/core/path_utf8.h`. **Otestat på Windows** — jag har bara Linux här.
- [x] **Parallell hämtning + uppackning.** `thread_count` på läs- och DEM-funktionerna,
      och `set_max_connections()` skilt från det: trådarna är den egna processorn,
      uppkopplingarna någon annans server. En tråd lånar en uppkoppling ur en pool för
      hämtningen och lämnar tillbaka den innan uppackningen, så åtta trådar kan packa upp
      samtidigt utan att åtta uppkopplingar öppnas.
      Krävde `ByteSource::clone()` och att `read_copc_node` delades i `copc_node_range` +
      `decompress_copc_node`.
      Lokalt 1764 → 284 ms med 8 trådar (6,2x), över en server i Sverige 3226 → 897 ms.
      Uppkopplingstaket: 4 är bäst, 8 är sämre än 4. Med *en* uppkoppling halveras tiden
      ändå, eftersom uppackningen överlappar hämtningen.
      Verifierat identiskt resultat vid 1/2/4/8 trådar för punkter, strömning och DEM.
- [x] **Publikt API — `HLas::CopcReader`.** open/open_url, estimate, read_region,
      read_polygon. Verifierat att samma fråga ger identiska punkter som
      `Reader::get_xyzc_polygon` på hela filen (sorterad koordinatjämförelse, inte bara
      antal): 237101 punkter, 739 ms mot 10135 ms.
- [x] **Beslut taget:** libcurl i `net/`, bakom `HLAS_ENABLE_NET=OFF`. Uppdelningen
      begränsar skadan — depen landar bara i `net/`, aldrig i `core/`, och den som inte
      slår på flaggan får ett bibliotek utan tredjepartsberoenden alls.

## Utskrift

- [x] **LAS** — `HLas::LasWriter`, okomprimerad LAS 1.4 punktformat 6. Strömmande: headern
      skrivs om vid `close()` med verkligt antal och utbredning, så ett urval på tiotals
      miljoner punkter behöver aldrig ligga i minnet. `create_like()` ärver källans skala
      och offset så kvantiseringen blir bit-identisk.
      Format 6 och inte 1.2 format 0: där delar klassificeringen byte med flaggbitar och
      klasser över 31 förvanskas — verkliga filer använder dem (Autzen har 64, 65, 73, 76, 77).

## Rättade buggar

- [x] **Heap buffer overflow i `decompress_all_layered`** — `pts_in_chunk` lästes ur filen
      och loopen skrev så många poster utan att klampa mot bufferten. `points_remaining`
      räknades ner men kontrollerades aldrig, till skillnad från den icke-lagrade stigen.
      En trasig LAS 1.4/COPC-fil kunde skriva utanför `decompressed_`.
- [x] **Avkodarna läste bortom indata** — `read_chunk_sizes` och `init_chunk` tar ingen
      slutpekare utan litar på anroparen. Kritiskt när byten kommer från nätverket.
- [x] **LAZ-VLR:ns objektstorlekar validerades inte mot `record_length`** — Point14 skriver
      alltid 30 byte oavsett vad VLR:n påstår.
- [x] **Flyttad-från `Reader` dereferade null** i nästan varje metod. Fanns sedan början.
- [x] **Falskt godkänt RGB-test** — läste offset 30 ur en 13-bytes sidecar-post, alltså
      oinitierat stackminne.
- [x] **Odefinierat beteende i DEM:ens atomiska min-skrivning** — `reinterpret_cast` från
      `float*` till `std::atomic<uint32_t>*`. Det finns inget atomic-objekt på den adressen.
      Ersatt med `std::atomic_ref<float>`, som är precis det verktyget. Fanns sedan Fas 9.
- [x] **Strömmande callback kunde anropas efter att den sagt stopp** — flera trådar hinner
      köa på sink-låset medan en av dem får nej. Kontrollen görs nu efter låset.
- [x] **UTF-8-sökvägar på Windows** — `CreateFileA`, `fopen` och `MoveFileExA` tolkade byten
      i ANSI-kodsidan medan anroparen skickar UTF-8, så `C:\Projekt\Sträcka Öst\` gav
      `FileNotFound`. Nu `CreateFileW` / `_wfopen` / `MoveFileExW` via
      `src/core/path_utf8.h`. **Otestat på Windows.**
- [x] **HTTP: `Content-Range` verifierades inte vid läsning** — en server kunde svara 206
      med ett annat intervall och byten hade tolkats som punktdata.
- [x] **HTTP: ingen protokollbegränsning** — `file://`, `ftp://` och `scp://` gick igenom.
- [x] **Testsviten segfaultade** när testdata saknades i stället för att hoppa över.
