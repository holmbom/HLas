# HLas - Development Guide

## Build
```bash
cmake -B build -G Ninja
cmake --build build
```

På Windows: samma rader i en `x64 Native Tools`-prompt, eller efter
`vcvars64.bat`. Se Plattformar längre ned för de fällor som bara finns där.

LAZ support is enabled by default. To disable:
```bash
cmake -B build -G Ninja -DHLAS_ENABLE_LAZ=OFF
```

`HLAS_BUILD_TESTS` styr tester, mätprogram och `tools/`. Den är på när HLas är
toppnivåprojektet och av när det dras in med FetchContent eller
`add_subdirectory`.

## Run Tests
```bash
./build/tests/HLas_tests
```

### Sanitizers
LAZ-koden räknar avsiktligt med wrappande heltal — det ÄR algoritmen — och det är
lätt att skriva som signed overflow, vilket är odefinierat beteende. Kör därför
UBSan innan något i `src/core/laz/` committas:

```bash
cmake -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-san && UBSAN_OPTIONS=print_stacktrace=1 ./build-san/tests/HLas_tests
```

Kräver `libasan` och `libubsan` installerade. Använd `ISum32`, `ISum64`, `IMul32`
och `ISum16`/`ISum8` i [integer_compressor.h](src/core/laz/integer_compressor.h)
i stället för råa `+` och `*` på de värden som ska wrappa.

## Project Structure
- `include/HLas.h` - Single public header (the only include consumers need)

Källkoden är uppdelad i moduler under `src/`, ett CMake-target per modul.
**Beroenden pekar åt ett håll** — gränsen upprätthålls av include-stigarna i
respektive `CMakeLists.txt`, så ett felaktigt beroende blir ett kompileringsfel:

| Modul | Target | Innehåll | Beror på |
|---|---|---|---|
| `src/core/` | `HLas::core` | LAS/LAZ-parsning + dekompression | inget |
| `src/dem/` | `HLas::dem` | grid-binning, nearest/IDW/TIN | core |
| `src/manager/` | `HLas::manager` | Reader, sidecar-cache | core + dem |

`HLas` är ett INTERFACE-paraply som länkar alla tre. Den som bara vill läsa
punkter kan länka `HLas::core` ensam — den drar inte in DEM eller cachen.

- `src/core/laz/` - LAZ-avkodare OCH -kodare (aritmetisk kodare/avkodare, Point10, GPSTime11, RGB12, Point14, RGB14, Byte14)
- `src/core/laz_reader.h` + `.cpp` - LAZ reader orchestration (chunk table, item dispatch)
- `src/core/laz_vlr.h` - LAZ VLR parser
- `src/core/point_source.h` - Icke-ägande vy över punktbufferten (base + index * stride). Sömmen som gör att dem/ och manager/ slipper veta vilken backend punkterna kom ur.
- `src/core/geometry.h` - `point_in_polygon` (ray casting), delad av polygonfilter och DEM
- `src/dem/dem.h` + `.cpp` - `generate_dem(const PointSource&, ...)` + fill (nearest, IDW, TIN)
- `src/manager/sidecar_lite.h` + `.cpp` - `.hlz` v4: rutnät över filens utbredning, glest cellindex, frame-of-reference per cell, bitpackad
- `src/manager/cached_laz_source.h` + `.cpp` - LAZ-punktkälla med cachen som policy
- `tools/make_las.cpp` - Genererar okomprimerad LAS ur en LAZ (testdata, inte del av biblioteket)
- `tests/` - Test project
- `tests/test_data.h` - Sökvägar + förväntade värden för testdata, och SKIP_IF_MISSING
- `tests/test_integration.cpp` - Integration tests against real LAS/LAZ files
- `tests/bench_dem.cpp` - DEM benchmark

## PointView — den form läsningen lämnar ut data i

`PointView` är en icke-ägande vy över punktposter: `base + index * stride`, plus
punktformatet och skala/offset. Den äger ingenting och gäller så länge bufferten
bakom den gör det.

**Vyn är den fullständiga läsvägen.** Uppackningen ger ändå hela posten, så att
plocka ut fyra fält ur den och skala dem till doubles är *extra* arbete ovanpå —
inte en snabb väg. Och det som plockas bort är borta för gott. Vyn lämnar ut
posterna som de ligger och anroparen nyttjar det den vill.

Accessorerna är format-medvetna, och det är hela poängen: ingen anropare ska
behöva veta att scanvinkeln flyttade från byte 16 till 18 mellan format 5 och 6
och samtidigt bytte från int8-grader till int16-steg om 0,006 grader. Vyn ger
grader oavsett. Fälten som formatet saknar hittas inte på — `has_rgb()` är false
för format 6, och `rgb()` returnerar då false utan att röra utparametrarna.

- `count()`, `point_format()`, `record_length()`, `extra_bytes()`
- `record(i)` — rå post, `nullptr` utanför vyn
- `raw_x/y/z(i)` — heltalen som de ligger; `x/y/z(i)` — skalade
- `xyz(start, n, x, y, z)` — batch, ett anrop i stället för tre per punkt
- `classification`, `intensity`, `return_number`, `number_of_returns`,
  `scanner_channel`, `scan_direction`, `edge_of_flight_line`, `synthetic`,
  `key_point`, `withheld`, `overlap`, `scan_angle`, `user_data`,
  `point_source_id`, `gps_time`, `rgb`, `nir`
- `has_*()` för de fält som inte finns i alla format

Layouten för alla elva punktformat ligger i `set_offsets()` i
[include/HLas.h](include/HLas.h). Det är den enda platsen den PUBLIKA vyn känner
posten på fältnivå; internt finns motsvarande offsetar i
[point14_item.h](src/core/laz/point14_item.h), som kodare och avkodare delar,
och i `src/write/point_encode.h` för LITE-postens fyra fält. Ett tolfte "format" finns: `PointView::kSidecarFormat`
(0xFF) för cachens punkter, som bara bär X, Y, Z och klass — men en cache-backad
läsare lämnar inte ut någon vy alls, se nedan.

**Cachen lämnar inte ut någon vy.** Den är bitpackad med olika bitbredd per cell
och har alltså ingen `base + i * stride`-form, och dess ordning är inte filens.
`points()` svarar därför tomt i cache-läge **och sätter `last_error()`** — högt,
inte tyst. Behövs råa poster: öppna med `Cache::Off`.

## Reader API

- `points()` — vy över hela filen, med alla fält källan bär. Den fullständiga
  läsvägen; `get_xyz*` nedan är bekvämlighet ovanpå samma data.

### Coordinate access
- `get_xyz(index, x, y, z)` — single point
- `get_xyz_batch(start, count, x, y, z)` — batch read (120 Mpts/s)

### Classification
- `get_classification(index)` — klassificering, **maskad**

I punktformat 0–5 delar klassbyten plats med tre flaggbitar (bit 5–7:
synthetic, key-point, withheld), så bara bit 0–4 är klassen. Hela biblioteket
maskar bort dem via `PointSource::class_mask` — histogram, `accepted[256]`,
`get_xyz_filtered`, DEM och sidecar-skrivningen. En markpunkt med synthetic satt
räknades annars som klass 34 och blev osynlig för `accepted[2]`.
Format 6–10 har en egen klassbyte och maskas inte, så klasser över 31 bevaras.

Cachen lagrar den **maskade** klassen — maskningen sker en gång, när punkterna
plockas ur källan i `BuildSidecar()`.
- `get_xyz_filtered(start, count, classification, x, y, z)` — XYZ filtered by single class

### Spatial polygon filtering
- `get_xyz_polygon(start, count, poly_x, poly_y, num_verts, out_x, out_y, out_z, classification, stride)` — XYZ within polygon
- `get_xyzc_polygon(start, count, poly_x, poly_y, num_verts, out_x, out_y, out_z, out_classification, classification, accepted, stride)` — XYZ + classification within polygon, optional `accepted[256]` filter for early exit

#### LOD — `stride[256]`, gallring per klass

`stride[c] == N` behåller var N:te punkt av klass `c`. 0 och 1 betyder full
täthet; `nullptr` betyder ingen gallring alls.

⛔ **Urvalet styrs av punktens plats i CACHENS CELL, aldrig av en löpande räknare
över träffarna.** Det är hela poängen: en stride på 10 ger **samma punkter
utzoomad som inzoomad**, så en vy som panorerar eller zoomar flimrar inte. En
global räknare hade gett ett nytt urval för varje fråga.

⛔ **Budget finns INTE, och det är ett beslut.** En budget beror till sin natur på
frågans utsträckning — samma yta ger olika urval beroende på vad som råkade frågas
samtidigt — och hade brutit precis den egenskap stride finns för.

Utan cache faller gallringen tillbaka på det **globala punktindexet**: samma
andel, men urvalet följer filens ordning och är inte spatialt jämnt. Att tyst
ignorera stride där hade varit värre än ett fel — anroparen ber om en tiondel och
får allt, och märker det när minnet tar slut.

`accepted[256]`, `classification` och `stride[256]` är förenade i
`Impl::ClassFilter` och appliceras **före** avkodningen av koordinaterna:
`SidecarReader::class_in_cell()` läser bara klassen med en enda bitläsning, så en
bortgallrad punkt kostar aldrig tre koordinatuppackningar. Genvägen måste läsa
samma bitar som `point_in_cell()`, och det provas för varje punkt i
`tests/test_sidecar_lite.cpp` — en avvikelse hade gett tyst fel klass i varje
filtrerad fråga.

### Classification histogram
- `get_classification_histogram(counts[256])` — per-class point count, full file (LAS + LAZ)

Finns en cache läses histogrammet **ur dess header** (`uint32[256]`), alltså ett
minnesuppslag i stället för en skanning: 43,9 → 0,000 ms på autzen och 581 → 0 ms
på laserrutan. Ordningen är cache → källa, och svaret är detsamma oavsett vilken väg
som gav det. ⚠️ Det räckte inte att LAGRA det — 1.1.4:s första utkast skrev
histogrammet till headern och lät `get_classification_histogram()` skanna ändå,
alltså exakt samma fel som 1.1.3 gjorde med indexet.
- `get_classification_histogram_polygon(counts[256], poly_x, poly_y, num_verts)` — per-class point count within polygon

### DEM generation
- `generate_dem(poly_x, poly_y, num_verts, cell_size, fill, accepted, thread_count)` — grid-binned DEM
  - `accepted[256]`: which classifications to include (nullptr = all), e.g. `accepted[2]=true` for ground, `accepted[9]=true` for water
  - `thread_count`: parallel point scan with atomic min-write
  - AABB-shortcut: skips `point_in_polygon()` for axis-aligned rectangular polygons
  - Fill methods: None, Nearest (5 cells), IDW (1/d², 5 cells), TIN (Bowyer-Watson Delaunay)
  - **Tak på rutnätet:** `kDemMaxCells` = 512 M celler (2 GB float). En stor polygon
    med liten cell ger ett tomt `DemResult` (`cols == 0`) i stället för att försöka
    allokera hundratals GB och terminera processen på `std::bad_alloc`.

### Raw access
- `point_data(index)` — raw pointer to point record
- `read_points(start, count, dest)` — bulk raw read (LAS och LAZ; **inte** i cache-läge)

### Open och cachen

```cpp
enum class Cache { Fast, Smart, Off };
Status open(const char* path, Cache cache = Cache::Off);
```

⚠️ **Förvalet ändrades i 1.1.3.** Tidigare tog `open(path)` en cache i bruk *och
skapade en om den saknades*. Nu rör den ingen. Skälet är att en cache-backad
läsare inte kan svara på allt — och ett förstörande förval är fel väg när hela
poängen med läsningen är att bära hela punkten.

⛔ **Enumet SKAPAR ingenting.** Alla tre lägena styr bara om en BEFINTLIG cache
får användas. Skapandet är `BuildSidecar()`, och de två halvorna är skilda:

```cpp
HLas::BuildSidecar(path);              // skapar — en gång, uttryckligen
r.open(path, HLas::Cache::Fast);       // använder den om den finns
```

Öppnar du med `Fast` utan att ha byggt något finns ingen cache att ta, och källan
packas upp som vanligt.

- `Fast` — använd cachen om den finns och hör till filen.
- `Smart` — som Fast, men hoppa över cachen för en COPC-fil. En `.copc.laz`
  slutar på `.laz` och fick förut en cache bredvid en fil som redan har ett
  octree. Bygger du ändå en för en COPC-fil ignorerar Smart den — välj `Fast`.
- `Off` — rör aldrig en cache.

`BuildSidecar()` bygger för vilken `.las` eller `.laz` som helst, COPC inräknad.
Vill man inte cacha sina COPC-filer låter man bli att anropa den för dem.

**Ändelsen LÄGGS TILL, den ersätter inte:** `foo.las.hlz`, `foo.laz.hlz`.
`.las` och `.laz` skiljer sig på en bokstav och kollapsade annars till samma
cachefil, som de sedan byggde om åt varandra i all evighet. Matchningen är
skiftlägesokänslig.

⛔ **En `.las` med cache lämnar inte heller ut råa poster.** Det vore frestande
att låta cachen och den mmap:ade källan leva samtidigt, eftersom en `.las` redan
ligger uppackad — men då läser `get_xyz(i)` ur cellordningen medan
`points().x(i)` läser ur filens, och de två indexen betyder olika punkter. Idén
byggdes halvvägs i 1.1.4 och ströks: cachen äger ordningen för båda källformerna.

**Cachen skapas av `BuildSidecar(path)`, inte av att läsa.** Före 1.1.3 skrevs den
som en bieffekt, bredvid användarens data, och misslyckades tyst på en
skrivskyddad katalog.

**Formen är mätt fram, inte vald.** Se [doc/plans/sidecar.md](doc/plans/sidecar.md)
och [doc/plans/sidecar-las.md](doc/plans/sidecar-las.md) för mätserierna.
Rutnät över filens utbredning, glest cellindex, frame-of-reference per cell:

| | autzen (10,7 M) | laserruta (140,3 M) | RGB-filen (1,8 M) |
|---|---|---|---|
| gamla v2-cachen | 132 MB | 1740 MB | — |
| v3 (Hilbert-block) | 54,4 MB — 5,35 B/pt | 674,9 MB — 5,04 | 9,8 MB — 5,75 |
| **v4** (rutnät) | **53,0 MB — 5,22 B/pt** | **666,0 MB — 4,98** | **10,3 MB — 6,09** |
| källan som `.laz` | 53,7 MB — 5,29 | 689,4 MB — 5,15 | 13,9 MB — 8,21 |
| källan som COPC | 77,4 MB | 689,4 MB | 13,9 MB |

⚠️ **v4 är inte mindre än v3 överallt.** På de två stora filerna är den 1–3 %
mindre, på den lilla 6 % större. Skälet är att en cell inte kan vara liten i tät
data och stor i gles, vilket ett Hilbert-block kunde — och det slår igenom på en
fil med fin skala och liten utbredning. Commit `5cc67a0` sa "6 % större" utan det
förbehållet; den siffran gällde bara RGB-filen.

⚠️ **"Mindre än källfilen" gäller mot COPC, inte mot en mager LAZ.** En COPC-fil
bär punktformat 6 eller 7 med långt mer per punkt än de fyra fält cachen håller.
Mot en vanlig LAS 1.2 format 3-`.laz` är det i praktiken jämnt. Vinsten som är
sann överallt är ~13 B/pt → ~5, inte att slå varje källa.

⛔ **Indexet byggs inte vid sidan om — det ÄR komprimeringen.** FOR måste lagra
cellens min för att kunna packa bitarna, och cellens min och max är dess bbox.

⛔ **Cellindexet är glest, och det är inte valfritt.** Bara upptagna celler
lagras, plus ett radindex `row_start[ny+1]`; cellerna ligger sorterade på
`(rad, cx)`. Med en tät celltabell blir en klippt korridorleverans 79 % större,
eftersom 96 % av cellerna är tomma och ändå betalas.

**Cellstorleken följer målbeläggning, inte en fast meterstorlek** — ~100 punkter
per cell. Kostnaden per punkt är `log2(ex·ey) − log2(C) + bz + 8 + 216·C/n`, och
derivatan är noll vid `C ≈ n/150`. En fast 10 m-cell kostade 0,96 B/pt extra på
en fil med skala 0,001.

⛔ **Cachen lovar ingen indexmotsvarighet.** Punkterna ligger i spatial ordning,
inte källans, så index `i` är en annan punkt än index `i` i filen. Att köpa
tillbaka löftet hade kostat 30 MB för källindexet — mer än sorteringen sparade.
Det är designat bort, inte dokumenterat bort.

**Gränsen går vid RÅA POSTER, inte vid index.** `points()`, `point_data()` och
`read_points()` svarar tomt och sätter `last_error()`, eftersom cachen inte
*innehåller* några poster — den är bitpackad, och det finns ingenting att peka
på. Allt annat fungerar, `get_xyz(i)` och `get_classification(i)` inräknade: ett
index är då en position i DEN läsaren, och de två är eniga med varandra om vilken
punkt de avser. `header().point_record_length` rapporterar **källans** postlängd,
inte noll — en nolla hade sett ut som en trasig fil i stället för som "cachen bär
inga poster".

⚠️ **Indexet måste också ANVÄNDAS, inte bara byggas.** 1.1.3 skrev blockens bbox
till filen men lät `Reader` skanna alla block ändå — frågorna var alltså O(N),
och prestandapåståendena här var falska för den kod som låg.

⛔ **Och sållningen måste gå mot POLYGONEN, inte mot dess bbox.** Det var fyndet
som startade 1.1.4: v3:s cache var *långsammare än ingen cache alls* för en
diagonal korridor, 568 ms mot en platt fils 524. En diagonal korridors bbox
täcker hela filen, så ingenting sållades bort. Rutnätet rastreras nu rad för rad
— `polygon_row_span()` i [src/core/geometry.h](src/core/geometry.h) ger stråkets
x-utbredning i just den raden, radindexet ger radens första cell, och ingen cell
utanför rörs. Sållningen sitter i `Impl::each_point_in_poly()` och används av
`get_xyz_polygon`, `get_xyzc_polygon`, `get_classification_histogram_polygon` och
DEM:en.

⛔ **`start` och `count` gäller ÄVEN den sållade vägen.** Ett första utkast lät
den söka hela cachen och använde `count` bara som utdatakapacitet. Det gav två
fel: en anropare som läser i omgångar med växande `start` fick samma första
träffar om och om igen, och taket på antalet skrivna punkter försvann — en
polygonfråga skrev så många punkter som ryms i POLYGONEN i stället för så många
som ryms i anroparens buffert. Nu begränsas både celler och punkter till
`[start, start+count)`, och taket följer av det.

Callbacken får ändå returnera `false` för att avbryta; `call_point()` tar emot
både `bool` och `void` via `if constexpr`, så histogram och DEM slipper bry sig. DEM:en matas i omgångar in i samma rutnät via `dem_accumulate` — samma väg
COPC redan använder, så binningen finns bara på ett ställe.

**Cachen kontrollerar att den hör till källan** med punktantal, källans storlek,
punktformat, skala och offset — plus ett **fingeravtryck** över 32 fönster ur
källans byte. De fem första är alla oförändrade om en fil klassas om och kodas om
till samma längd, och då är fingeravtrycket det enda som skiljer dem åt.

⚠️ **Fingeravtrycket är ett stickprov, inte en checksumma.** Det fångar det som
ändrar filen brett, men en ändring som helt ligger mellan två fönster passerar.
Ett fullt hashvärde vore ett löfte och kostar en genomläsning av källan vid varje
`open()` — alltså hela poängen med en cache som öppnas på 0 ms. Cachens *egen*
integritet kontrolleras inte alls. Båda ligger i TODO.md.

**Mätt fart** — medel över 12 slumpvis placerade *ormar*, alltså buffrade
polylines med 8–18 hörn, vilket är den form en korridorfråga faktiskt har. En
rak fyrhörning hade dolt både polygonsållningen och kostnaden för hörnrika
polygoner. Samma träffantal rad för rad i båda versionerna:

| fråga | autzen v3 | autzen **v4** | laser v3 | laser **v4** |
|---|---|---|---|---|
| orm 12 m × 300 m | 0,413 ms | **0,101** | 1,951 ms | **0,243** |
| orm 50 m × 1 km | 3,840 | **0,727** | 16,940 | **2,517** |
| orm 50 m × 3 km | 32,133 | **2,138** | 162,522 | **8,528** |
| orm 200 m × 1 km | 5,307 | **2,194** | 23,574 | **8,750** |
| histogram | 43,9 | **0,000** | 581,0 | **0,000** |

Vinsten är störst för den **långa smala** ormen — 15x och 19x — och det är
väntat: där var bbox-sållningen som mest värdelös. Bygget är också 20–28 %
snabbare (autzen 4,8 → 3,5 s, laserrutan 62,3 → 44,9 s).

Provet ligger i [tests/bench_ab.cpp](tests/bench_ab.cpp) och använder bara API som
finns i båda versionerna, så det som mäts är implementationen och inte anropet.

### CopcReader — partiell läsning ur COPC
`Reader::open()` packar upp hela filen (~10 s för 10,6 M punkter). `CopcReader` läser
octree-indexet, väljer bara de noder som berör frågan och packar upp dem.

- `open(path)` / `open_url(url)` — lokal fil eller HTTP. `open_url` kräver `-DHLAS_ENABLE_NET=ON`
  och en server som stödjer range requests; annars `Status::LazNotSupported`.
- `estimate(min_x, min_y, max_x, max_y, max_level=-1)` → `{node_count, point_count, byte_size}`.
  **Övre gräns** — buffertar av den storleken räcker alltid.
- `read_region(...)` / `read_polygon(...)` — levererar **råa poster**. Anroparen
  allokerar `max_points * header().point_record_length` byte och `max_points` är
  taket; `view(records, count)` ger en `PointView` över det som skrevs.
  `accepted[256]` filtrerar på klassificering, samma mening som i
  `Reader::get_xyzc_polygon`. Filtret är ett *urval* — posterna följer med
  oförändrade, så RGB, intensitet och GPS-tid finns kvar.
- `read_region_streaming(...)` / `read_polygon_streaming(...)` — **använd de här för långa
  stråk.** Punkterna levereras nod för nod till en `PointSink`-callback som får en
  `const PointView&`, så inget större än en nod materialiseras. Vyn gäller bara
  under anropet. `estimate()` räknar alla punkter i berörda noder, vilket för en
  korridor kan vara tiotals gånger fler än de som hamnar innanför — mätt på en
  nationell laserruta: 9,4 miljoner uppskattade mot ~800 000 verkliga.
  Returnera `false` ur callbacken för att avbryta.
- `generate_dem(...)` — DEM direkt ur COPC-filen, binnad nod för nod. Ger cell-för-cell
  samma rutnät som `Reader::generate_dem()` på hela filen.

**Parallellism — två oberoende rattar.** `thread_count` (sista parametern på läs- och
DEM-funktionerna) är din processor. `set_max_connections(n)` är någon annans server.
Åtta trådar med taket 4 packar upp på åtta kärnor men håller aldrig mer än fyra
uppkopplingar: en tråd lånar en uppkoppling för hämtningen och lämnar tillbaka den innan
uppackningen. Standard är 1 tråd och 4 uppkopplingar.

Ordningen blir **obestämd** med `thread_count > 1` — punkterna hör ihop radvis, men vilken
nod som hamnar först avgörs av trådarna. Callbacken i de strömmande varianterna anropas
aldrig samtidigt från två trådar, och aldrig igen efter att den returnerat `false`.

Uppmätt på en fråga som träffar 20 noder:

| | 1 tråd | 4 trådar | 8 trådar |
|---|---|---|---|
| lokal fil | 1764 ms | 466 ms | 284 ms |
| VPS i Sverige | 3226 ms | 1169 ms | 897 ms |

Uppkopplingstaket mot VPS:en med 8 trådar: 1 → 1490 ms, 2 → 959, **4 → 897**, 8 → 1229.
Fler än fyra är alltså sämre. Och redan med *en* uppkoppling halveras tiden mot seriellt,
eftersom uppackningen överlappar hämtningen.
- `node_count()`, `max_level()` — octreets form.
- ⛔ **`root_from_extent()` — octreet byggt över filens UTBREDNING, inte över kuben.**
  Specen kräver att roten är COPC-infons kub, men det finns sammanfogade filer med
  rektangulär utbredning som bygger octreet över headerns min/max per axel: en fil
  på 10 × 5 km med en 10 × 10 km-kub, och med kuben hamnar de djupa noderna fel — en korridor fick 2 % av punkterna,
  TYST. `read_copc_index` känner igen det (punkterna i noder vars kub skär
  utbredningen < alla punkter) och läser då seed-posten i upp till
  `kCopcRootProbes` (8) av de djupaste noderna; ligger varenda en i sin ruta räknad
  från utbredningen byts rotlådan (`CopcInfo::root_from_extent`). ⛔ Aldrig på en
  gissning — förklarar ingen av tolkningarna seed-punkterna behålls kuben. En konform
  fil kostar ingen läsning till (provet räknar: 3). Z ur utbredningen är INTE uppmätt
- `max_level` ger ett **glesare** moln, inte ett komplett moln av färre noder: COPC-noder är
  inte redundanta, djupare noder innehåller ytterligare punkter snarare än kopior.

Uppmätt för en fråga som träffar 20 av 278 noder (7,5 % av filen, 237101 punkter):

| Källa | open | fråga |
|---|---|---|
| lokal fil | 4 ms | 739 ms |
| lokal webbserver (HTTP) | 4 ms | 762 ms |
| VPS i Sverige (HTTPS) | 231 ms | 2072 ms |

Jämför `Reader::open()` på samma fil: 10135 ms innan första punkten går att läsa.

### LasWriter / LazWriter — spara ut ett urval

**Läget är uttryckligt: `WriteMode::Lite` eller `WriteMode::Full`.** Det måste vara det,
eftersom läsningen bara har en utdatatyp — när allt kommer ut som en `PointView` finns
ingen andra write-funktion att bära läget implicit med.

- **LITE** (standard) — X, Y, Z och klassificering. Punktformat 6, 30 byte per post, sex
  av nio attributlager tomma. Ett leveransformat, inte en nödlösning: ska urvalet bli en
  DEM är RGB och GPS-tid dödvikt hela vägen.
- **FULL** — behåll det källan har. Utdataformatet *följer* källan, så en format 7-källa
  ger en format 7-fil med RGB, GPS-tid, intensitet och allt annat kvar, bit för bit.

FULL sätts bara via `create_like(path, source, WriteMode::Full)`. `create()` med
`mode == Full` returnerar `InvalidHeader` — utan en källa finns inget format och ingen
kvantisering att ärva, och läget hade tyst fallit tillbaka på format 6.

**Stödda källformat är 0–3, 6 och 7.** Format 4 och 5 bär ett `Wavepacket13`-item och
8–10 kräver `Byte14` eller NIR; ingen av dem finns som codec och det finns ingen testfil
att verifiera mot, så de ger `UnsupportedPointFormat`. Extra bytes likaså. En översättning
mellan formaten vore en *konvertering*, inte ett bevarande, och hör inte hemma bakom ett
läge som heter FULL.

**Filversionen följer punktformatet, inte källans versionsfält:** LAS 1.2 med 227-bytes
header för punktformat 0–5, LAS 1.4 med 375 för 6 och uppåt. Punktformat 6 finns inte i
1.2, och de äldre har ingen anledning att skrivas i 1.4. `global_encoding` och
`file_source_id` ärvs från källan — det förra för att bit 0 bär GPS-tidens typ, och en
nolla där gör tiderna feltolkade av den som läser. Bit 4 (WKT) läggs till för 1.4.

De två formatfamiljerna har också **olika chunkstruktur**, och det är där risken låg:
punktformat 6–10 använder compressor 3 med nio lager och ett punktantal i chunken, medan
0–5 använder compressor 2 — seed-posten rått följd av EN aritmetisk ström där alla items
kodas efter varandra per punkt. Ingen lagerindelning, inget punktantal; antalet står i
chunk-tabellen. Formatkunskapen som båda skrivarna delar ligger i
[point_encode.h](src/write/point_encode.h), eftersom `LasWriter` byggs även utan LAZ.

`write(const PointView&)` är vägen in. `write(doubles)` finns kvar men är alltid LITE och
returnerar fel om skrivaren står i FULL.

Mätt på hela testfilen, 1 777 643 punkter i punktformat 7:

| | tid | storlek | per punkt |
|---|---|---|---|
| LITE | 202 ms | 7 094 742 byte | 3,99 B |
| FULL | 311 ms | 14 679 681 byte | 8,26 B |

LITE är alltså 48 % av FULL och 1,54x snabbare.

Och på hela `autzen.laz`, 10 653 336 punkter i punktformat 3:

| | tid | storlek | per punkt |
|---|---|---|---|
| LITE | 1148 ms | 29 773 303 byte | 2,79 B |
| FULL | 1504 ms | 56 349 793 byte | 5,29 B |

LITE är 53 % av FULL och 1,31x snabbare. Kvoten är en annan än för format 7 därför att
det som LITE kastar är ett annat: här RGB och GPS-tid ur en 34-bytespost, där RGB ur en
36-bytespost som redan bar GPS-tid i sitt eget lager.

Skillnaden är inte att lager hoppas över: `classification`, `flags`, `intensity` och
`user_data` kodas för varje punkt även i LITE. Tre av de fyra lagren blir sedan tomma
eftersom fälten är konstant noll — men **`classification`-lagret gör det inte**, för LITE
bevarar klassningen och den varierar. Det LITE slipper är RGB-itemet helt, plus arbetet i
de villkorliga lagren för scanvinkel, point source och GPS-tid.

**För format 7-filen** blir FULL 0,6 % större än originalet. Orsaken är **chunkgränserna**,
inte antalet chunkar: COPC-filen har en chunk per octree-nod, alltså spatialt sammanhållna
block, medan vi skriver fasta 50000-block. Med originalets gränser producerar kodaren exakt
originalets byte. För `autzen.laz` sammanfaller gränserna redan, och där blir vår fil i
stället 1195 byte *mindre* — se tabellen längre ned.

Biblioteket är i övrigt läsare-bara. Skrivarna finns för att kunna spara ett urval som en
fil andra verktyg läser.

- `create(path[, Options])` / `create_like(path, source_header)` — den senare ärver skala och
  offset, vilket ger bit-identisk kvantisering.
- `write(x, y, z, classification, count)` — får anropas flera gånger; `classification` får
  vara `nullptr`.
- `close()` — **måste** anropas, den skriver om headern med verkligt antal och utbredning.

LITE skriver LAS 1.4 punktformat 6 (30 B/pt). Inte 1.2 format 0: där delar klassificeringen
byte med flaggbitar, så klasser över 31 förvanskas. FULL skriver källans format — 0–3, 6
eller 7 — och filversionen följer formatet. Utbredningen i headern beskriver de
*lagrade* koordinaterna, alltså efter kvantisering.

`LazWriter` har samma API och skriver samma format, komprimerat. Kodningen är **vår egen**
sedan 1.1.1 — samma aritmetiska kodare och samma Point14-modeller som läsaren, spegelvända
([src/core/laz/arithmetic_encoder.h](src/core/laz/arithmetic_encoder.h),
[point14_encoder.h](src/core/laz/point14_encoder.h)). Filen ägs av
[src/write/laz_writer.cpp](src/write/laz_writer.cpp): header, LAZ-VLR, chunkning om
50000 punkter, och chunk-tabellen sist. Följer `HLAS_ENABLE_LAZ` precis som läsningen.

**`Point14Encoder` har två ingångar men är EN kodare.** `encode(Point14Fields)` är
LITE-vägen: fyra fält, och sex av nio attributlager blir tomma eftersom de andra fälten
inte *går* att uttrycka. `encode_item(const uint8_t*)` tar hela 30-bytesposten och kodar
allt källan bär. Båda går genom samma kod — två separata kodare vore samma fel som
`IntegerCompressor` skyddar mot genom att dela klass mellan läsning och skrivning.

Tillståndet är `Point14Context`, samma struct som avkodaren använder, med fyra kontexter
(en per scannerkanal). Fältoffseten ligger i
[point14_item.h](src/core/laz/point14_item.h) och delas också.

**Lagerpolicyn är avläst ur referensen, inte antagen.** `classification`, `flags`,
`intensity` och `user_data` kodas för *varje* punkt men lagret skrivs bara om något
ändrats — kodar man selektivt hamnar modellerna ur fas med avkodarens. `scan_angle` och
`point_source` kodas bara vid ändring, eftersom de har egna bitar i `changed_values`.
`channel_returns_XY` och `Z` skrivs alltid.

`RGB14Encoder` är motsvarande spegel för färgitemet i punktformat 7.
[chunk_layers.h](src/core/laz/chunk_layers.h) sätter ihop chunken: formatet
kräver *alla* items lagerlängder först och sedan alla deras data, så ett item går
inte att skriva färdigt i taget. Funktionen är allt-eller-inget — felar RGB men
inte Point14 blir längden noll medan Point14 skriver som vanligt, och chunken
avkodas då utan fel men med fel färger.

**GPS-tiden måste välja, den kan inte spegla.** Avkodningen läser ett multi-värde och gör
vad det säger; kodningen måste *välja* det — vilket av fyra fack tiden hör till, och vilken
multiplikator som beskriver steget. Det valet går inte att härleda ur avkodaren, så
`encode_gps_time` är skriven för att välja precis som referensens kodare gör — annars
uteblir byte-identiteten.

Två fällor i den, båda dokumenterade i koden:

- `multi = round(diff / (float)last_diff)` räknas i **enkel precision**. I double väljs
  ibland ett annat multi — filen avkodas fortfarande rätt, men byte-identiteten uteblir.
- `init_context` fyller bara **GPS-fack 0** med seed-tiden och lämnar de tre andra
  nollställda — i *både* kodare och avkodare. Fyller man alla fyra tar `findSeq` ett
  annat fack än referensen och byte-identiteten uteblir, men det är inte hela skälet:
  en fil vars seed-tid är 1000,0 och vars nästa tid är 0,0 får en konform kodare att
  välja ett nollställt fack, eftersom differensen dit är noll. En avkodare som fyllt
  facket med seed-tiden räknar då från fel värde och ger fel punkter.

**RGB14 har en egen egendomlighet, och den måste speglas.** Vid byte till en scannerkanal
som *redan* använts pekar formatet fortfarande på föregående kanals färg — både för
prediktionen och för tillbakaskrivningen — medan modellerna hämtas ur den nya kanalen.
Andra läsare tolkar filerna så. HLas gjorde "rätt" fram till 1.1.2 och avkodade därför
flerkanaliga format 7-filer med fel färger.

Uppmätt mot referensen på 10 653 336 punkter ur autzen.las, samma skala och offset:
utdatan är **byte-identisk** så när som på 35 byte, alla i header och VLR — 29 av dem ren
identifikation, resten två ställen där vi följer LAS 1.4 och referensen inte gör det (legacy
point count ska vara noll för format 6; returtabellen ska summera till antalet). Skrivningen
tog 895–1091 ms mot referensens 1338–1692 ms, alltså 1,5x, och det beror inte på bättre
kodning utan på de tre lager per punkt vi hoppar över.

**`close()` måste anropas och dess svar kontrolleras** — den skriver chunk-tabellen och den
slutgiltiga headern. Misslyckas den kan filen vara delvis skriven och ska kastas.

COPC-utskrift finns inte: den kräver att octreet byggs. Se TODO.md.

### Felrapportering
- `Reader::last_error()` — `const std::string&`, tom när senaste anropet lyckades.
- `open()` sätter `last_error` till `status_message(status)` vid fel, och rensar den vid `Status::Ok`.
- Varje punktdata-funktion börjar med `Impl::begin_call()` i [src/manager/reader.cpp](src/manager/reader.cpp) (och motsvarande i `copc_reader.cpp`), som rensar `last_error`. Lägg till nya punktdata-funktioner via den, så att ett gammalt fel inte står kvar efter ett lyckat anrop.
- `get_classification_histogram(_polygon)` nollställer `counts[256]` först, så anroparens buffer aldrig lämnas oinitierad.
- Trådsäkerhet: `last_error` är `mutable` och skrivs av const-metoder. Samtidiga anrop på *samma* Reader-instans från flera trådar är inte längre säkert; en Reader per tråd gäller.

## Nätverk och SSRF
`CopcReader::open_url()` begränsar till http/https, även genom omdirigeringar, och följer
högst fem hopp. Uppgifter för HTTP Basic skickas bara över https och följer inte med till
en annan värd vid omdirigering.

**Värdpolicyn är anroparens ansvar.** Biblioteket kan inte veta vilka adresser som är
känsliga i anroparens nät. Kommer en URL från användare, konfiguration eller ett API-svar
måste den valideras innan den skickas in — annars går det att styra hämtningen mot
`169.254.169.254`, loopback eller interna intervall. Kontrollera den *upplösta* adressen,
inte bara värdnamnet.

### `NetworkSettings` — nätet där programmet körs

Av samma skäl kan biblioteket inte veta hur *nätet* ser ut: företagsproxyer, TLS-inspektion
och var CA-lagret ligger på just den maskinen. Före 1.1.2 var hela uppsättningen hårdkodad,
och en Windows-jobbdator där libcurls inkompilerade CA-sökväg inte fanns fick
`CURLE_SSL_CACERT_BADFILE` redan på första förfrågan — ett "CA-lagret gick inte att läsa in
alls", inte ett certifikatfel, och omöjligt att göra något åt utifrån.

`CopcReader::set_network_settings()` tar en struct med **rena värden**, inte ett
HTTP-handtag: den publika headern känner medvetet inte till libcurl. **Ett förvalt objekt
ändrar ingenting** — fälten är valda så att de återger beteendet från före de fanns.

Fyra fällor. Alla fyra prövas som BESLUT i
[test_net_settings.cpp](tests/test_net_settings.cpp) — testet granskar planen, inte de
faktiska anropen mot libcurl, och `clone()` går inte att pröva utan en server:

- **`ProxyMode::None` och `System` är motsatser.** `None` sätter proxyn till tom sträng,
  vilket stänger av den *och* ignorerar `http_proxy`/`https_proxy`. `System` sätter
  ingenting alls, och det är frånvaron som får libcurl att läsa dem.
- **Namnkontrollen följer kedjekontrollen**, 1/2 eller 0/0. Att verifiera namnet men inte
  kedjan låter försiktigare, men på ett nät med TLS-inspektion är det just namnkontrollen
  som fäller anslutningen — då hade "stäng av verifieringen" inte gjort det anroparen bad
  om. `VERIFYHOST` är 2, aldrig 1: värdet 1 är borttaget ur libcurl.
- **Tom `ca_file` betyder "sätt ingenting"**, inte "leta rätt på en".
- **Komprimering av sätts uttryckligen till `nullptr`**, inte genom att hoppa över anropet.
  Handtaget återanvänds över alla range-läsningar, och där är "hoppa över" inte samma sak
  som "av". Tom sträng vore dessutom motsatsen — den betyder "allt libcurl kan".

Proxyns TLS **härleds** ur samma fält, hela vägen: `Manual` sätter
`PROXY_SSL_VERIFYPEER/HOST` till samma par som servern, och `PROXY_CAINFO` respektive
`PROXY_SSL_OPTIONS` till samma CA-val. Det antar en gemensam CA-rot, vilket stämmer på ett
nät med TLS-inspektion; skulle de behöva skilja sig krävs egna fält. Det spelar bara roll
när proxy-URL:en själv är `https` — en vanlig `http`-proxy gör CONNECT, och då är TLS:et
mot servern.

⛔ Proxyns par sätts **uttryckligen även när verifieringen är av**. Att hoppa över anropen
hade lämnat libcurls förval 1/2 kvar, och proxy-benet hade fortsatt verifiera trots att
anroparen bett om motsatsen — samma fel som hela inställningen finns för att rätta, ett steg
längre ut i kedjan.

⚠️ **Inställningarna följer med i `clone()`.** Med `thread_count > 1` kopplar varje tråd upp
på nytt; en klon som fick förvalen hade felat mitt under en hämtning, på en annan tråd, med
ett fel som inte liknar det som fungerade vid `open_url()`.

`timeout_scale` skalar **både** uppkopplingen och varje enskild läsning. Det är den senare
som failar först på en långsam förbindelse, eftersom `TIMEOUT` gäller varje begäran på
handtaget och inte bara den första. Noll och negativa tal tolkas som 100, och resultatet
kläms nedåt till en sekund — noll betyder "ingen gräns" i libcurl, alltså en hängd
uppkoppling som aldrig återvänder.

Besluten ligger i `make_curl_plan()` i [src/net/curl_plan.h](src/net/curl_plan.h), skilda
från anropen som sätter dem. Fällorna sitter i besluten, och som ett värde går de att pröva
utan nät, utan server och utan libcurl-handtag.

Proxy-uppgifter har **ingen** motsvarighet till spärren som vägrar skicka origin-uppgifter
över icke-https. En företagsproxy är typiskt `http` med Basic, och en spärr där hade
blockerat precis det fall inställningarna finns för. Anroparen har konfigurerat proxyn
medvetet.

## Sökvägar
Sökvägar är **UTF-8 på alla plattformar**. På Windows går filöppningen via
`CreateFileW`/`_wfopen`/`MoveFileExW` med konvertering i
[src/core/path_utf8.h](src/core/path_utf8.h) — A-varianterna tolkar byten i den aktiva
ANSI-kodsidan, och en sökväg som `C:\Projekt\Sträcka Öst\` gav då `FileNotFound`, alltså
samma symptom som en flyttad fil.

Raderingen och namnbytet har samma fälla: `std::remove()` och `std::rename()` går
också via A-varianterna. Använd `detail::remove_utf8()` och
`detail::rename_utf8()` i stället — båda ligger i samma header och gäller båda
plattformarna. `rename_utf8()` bär dessutom en semantisk skillnad som är lätt att
missa: `std::rename()` får inte skriva över en befintlig fil på Windows, medan
POSIX `rename()` gör det, så `MOVEFILE_REPLACE_EXISTING` är vad som gör
`.tmp`-till-slutgiltig-steget lika på båda.

## Plattformar

Linux, Windows och macOS. **Linux är plattformen allt mäts på.** Windows provas
med MSVC (VS 2026, `cl` 19.50) i fem konfigurationer: Release, Debug,
`HLAS_ENABLE_LAZ=OFF`, `BUILD_SHARED_LIBS=ON` och ASan. ⚠️ **macOS är oprövat** —
källan bär inget Linux-specifikt, men det är ett antagande och inte en mätning.

Sviten körs också med `TMPDIR` satt till en katalog med mellanslag och `äöåÅÖ`.
Det är den sökväg [src/core/path_utf8.h](src/core/path_utf8.h) finns för, och den
prövades aldrig end-to-end innan 1.1.4:s Windows-rättelse. Körningen ska lämna
**noll** filer kvar i katalogen — en kvarlämnad fil betyder att något håller den
öppen, se `FILE_SHARE_DELETE` nedan.
⚠️ **`HLAS_ENABLE_NET=ON` är inte provat på Windows** — libcurl finns inte
installerat på maskinen som verifierade resten, och `find_package(CURL REQUIRED)`
faller därför redan i konfigureringen. Det är ett saknat beroende och inte ett
fel i koden, men det betyder att `src/net/` inte har kompilerats med MSVC.


Sex fällor som Windows har och Linux inte. Alla sex fällde 1.1.4:s första tagg,
och alla sex är lätta att återinföra:

- ⛔ **`windows.h` dras in på EXAKT ett ställe** —
  [src/core/path_utf8.h](src/core/path_utf8.h) — och den definierar `NOMINMAX`
  och `WIN32_LEAN_AND_MEAN` först. Utan det blir varje `std::min(a, b)` i varje
  fil som når hit `std::(a, b)`, och felet pekar på std::min-raden i den filen,
  inte på headern. Lägg inte till ett andra ställe. Och en `#define NOMINMAX`
  **efter** en include som redan dragit in windows.h gör ingenting — det var
  precis vad `file_io.cpp` gjorde.
- ⛔ **Stacken är 1 MB, inte 8.** En `double buf[10000]` är 80 kB, och ramen
  allokeras vid funktionens **ingång** — alltså också för block som hoppas över.
  `test_integration()` låg på drygt 2 MB och dödades med `0xC0000409`, mitt i en
  helt orelaterad rad. Stora buffertar hör på heapen. Biblioteket självt har inga.
- ⛔ **`fopen()` och `std::remove()` tolkar byten i ANSI-kodsidan.** Se Sökvägar
  ovan.
- ⚠️ **`TMPDIR` finns inte** — Windows har `TEMP` och `TMP`, och de måste läsas
  med `_wgetenv`: `getenv` ger byten i ANSI-kodsidan, alltså inte UTF-8. Testerna
  går genom `testdata::tmp_dir()` i [tests/test_data.h](tests/test_data.h), som
  provar TMPDIR först så en Linux-körning beter sig som förr.
- ⛔ **En mappad fil går inte att radera** om `CreateFileW` öppnade utan
  `FILE_SHARE_DELETE`. POSIX tillåter det och låter mappningen leva vidare på det
  gamla innehållet; Windows vägrade, och `BuildSidecar()` misslyckades därför med
  `FileOpenFailed` om någon hade den gamla `.hlz`-filen öppen. Flaggan sitter i
  [file_io.cpp](src/core/file_io.cpp) och ska stanna där.
- ⚠️ **`std::fopen`, `std::remove` och `std::ifstream` är alla ANSI-vägen.**
  Testerna går genom `testdata::fopen_utf8`, `remove_utf8` och `ifstream_utf8`.
  En ännu öppen `ifstream` blockerar dessutom raderingen — den öppnar utan
  `FILE_SHARE_DELETE` och har ingen flagga för det, så strömmen måste stängas
  först.

Ett `BUILD_SHARED_LIBS=ON`-bygge kräver dessutom två saker som Linux får gratis,
båda i rot-CMakeLists: `CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS` — utan exporter ur en
DLL skapas inget importbibliotek att länka mot — och `hlas_stage_runtime_dlls()`,
som kopierar DLL:erna bredvid varje körbar eftersom Windows saknar RPATH.

## Planering
Öppna punkter ligger i [TODO.md](TODO.md): kända buggar först, sedan det som är medvetet
uppskjutet och `1.1.x`. Avklarat arbete flyttas till [doc/CHANGELOG.md](doc/CHANGELOG.md)
med samma struktur — TODO ska bara innehålla det som är kvar.

Hela `doc/` är spårad. [doc/plans/](doc/plans/) håller utredningar som ännu inte
blivit kod — en plan vars mätningar tog timmar att ta fram ska inte ligga bara på
en maskin.

## Conventions
- C++20
- Namespace: `HLas`
- Focus: speed and simplicity
- Läsare i första hand. Skrivarna finns för att spara ett urval som en fil andra
  verktyg läser — inte för att vara ett allmänt skriv-API
- Static library by default, `-DBUILD_SHARED_LIBS=ON` for shared
- No external dependencies
- Swedish commit messages, short format: "Fas N: beskrivning"
- No Co-Authored-By in commits

## Gyllene vektorer — facit utifrån

[tests/golden_ref.h](tests/golden_ref.h) är genererad och innehåller
chunk-byte producerade av **referensimplementationen** — en öppen LAZ-kodare som
vi inte skrivit — och inte av oss. Allt annat LAZ-test i sviten är rundgång, och rundgång kan inte upptäcka ett *delat* missförstånd — när kodare
och avkodare är eniga om något som ändå är fel. `readCorrector` kortslöt på
`k >= bits_` i stället för `k == 32` och vår egen rundgång var lycklig hela tiden.

Vektorerna täcker kantfall testfilerna saknar: extrema GPS-hopp; en ström som
blandar oförändrad tid, små steg och sekvensbyten för att nå 0diff-grenen; alla
fyra scannerkanaler, och ett fall där *seed-punkten* ligger i kanal 2; 15 returer
per puls; klass spridd i 32–255; flaggbyten spridda över värdeområdet; allt utom
koordinaterna konstant (som ger sju tomma lager); INT32_MIN/MAX i koordinaterna;
RGB och grå RGB.

Varje falls verkliga lagerlängder står som kommentar i den genererade filen —
avlästa ur facit, inte påstådda. `tests/test_golden.cpp` driver
`Point14Decoder`/`RGB14Decoder` direkt och testar alltså algoritmerna, inte
VLR-tolkningen eller item-dispatchen i `LazReader::decompress_chunk`; de täcks
av de filbaserade testerna.

[tests/golden_ref10.h](tests/golden_ref10.h) är samma sak för punktformat
0–5, som har ett **annat filupplägg**: compressor 2 har ingen lagerindelning alls.
Chunken är seed-posten rått följd av EN aritmetisk ström där varje punkts items
kodas efter varandra. Det finns därför inga lagerlängder att skriva ut — bara
strömmens storlek, som står över varje fall. `tests/test_golden10.cpp` driver
`Point10Decoder`/`Point10Encoder`, `GPSTime11Decoder`/`GPSTime11Encoder` och
`RGB12Decoder`/`RGB12Encoder` direkt.

`p10_gps_frames` finns för en **läsarbugg den hittade**: referensen håller TVÅ
fackräknare, `last` som flyttas av ett sekvensbyte och `next` som pekar ut facket
som tas i bruk vid nästa fulla värde. `GPSTime11Decoder` räknade fram det nya
facket ur `last`, vilket är rätt ända tills ett sekvensbyte skilt dem åt — och
FÖRST då, vid nästa fulla värde, hamnar avkodaren i fel fack och ger fel
GPS-tider för resten av chunken. Det rör punktformat 1, 3, 4 och 5.
Point14-lagret hade det rätt sedan tidigare. Måttet på hur skarpa vektorerna är:
med buggen kvar föll **tre av de fyra GPS-bärande fallen** — de tre som är
byggda för att tvinga fram fulla värden och sekvensbyten — och med den borta
klarar alla fjorton. `p10_full` bär också GPS men dess tider når varken fulla
värden eller fackbyten.

Samma avkodare hade **en bugg till**, funnen av granskningen och inte av
vektorerna: prediktionen till `IntegerCompressor` räknades i int64 i stället för
int32 med wrappning. Kodaren bildar korrektorn mot det wrappade värdet, så det
var inte bara en avvikelse från referensen utan en oenighet mellan vår egen
kodare och avkodare — som slår till först när `multi * delta` spiller över int32.
Point14 har alltid gjort det rätt. Vektorerna hade inte fångat den, eftersom
referensens kodare inte väljer multiplar som spiller över på de här indata.

`p10_returns` täcker **alla 64 rutor** i `kReturnMap` och `kReturnLevel`,
inklusive de "omöjliga" kombinationerna tabellerna finns för. `p10_intensity` är
fallet med tänder: intensiteten jämförs inte med föregående punkt utan med den
senaste intensiteten för samma retur-klass, och en kodare som gör det
uppenbara divergerar redan på punkt fyra. Kontrollerat genom att medvetet
ändra jämförelsen — då och bara då faller det fallet, de tretton övriga står kvar.

Generatorn ligger **utanför repot** och följer inte med: den länkar
referensimplementationen, och det beroendet ska inte tillbaka in i bygget. Den
har fem lägen, och proven nedan hänvisar till dem:

- `golden` och `golden10` — skriver vektorerna i de två genererade filerna.
- `verify` — packar upp varje COPC-nod med referensen och kodar om den.
- `records` — dumpar referensens uppackade poster för en hel fil.
- `readlaz` — packar upp en fil som HLas skrivit och dumpar posterna.

**Punktformat 0–3 har ett eget, starkare prov.** `tests/test_reencode_legacy.cpp` gör
samma sak med hela `las/autzen.laz` — punktformat 3, 10,6 miljoner punkter, skriven av
ett tredjepartsverktyg. Där ligger chunkgränserna dessutom redan rätt, eftersom filen
använder samma chunkstorlek som vi: **214 av 214 hela chunkar byte-identiska.**

Hela filen går att göra räkenskap för. Vår utdata är 1195 byte mindre än originalet, och
varje byte av skillnaden är mätt:

| | original | vår | |
|---|---|---|---|
| punktdata | 56 348 978 | 56 348 978 | identisk |
| chunk-tabell | 474 | 474 | **byte-identisk** |
| VLR:er | 5 | 1 | −1163 byte, tre projektions-VLR:er och en verktygs-VLR |
| efter chunk-tabellen | 32 | 0 | data vi inte skriver |

**Och referensen läser tillbaka det vi skrev.** Generatorns `readlaz` packar upp en fil
HLas producerat och dumpar posterna: 10 653 336 poster, **noll avvikelser** mot vad som
matades in. Att vår egen avkodare klarar vår egen kodares utdata bevisar bara att de är
eniga; det här är skrivningen prövad av en implementation vi inte skrivit.

**Punktformat 6–7 provades på samma sätt** mot en format 7-COPC-fil skriven av ett
tredjepartsverktyg: uppackad med *våra* avkodare, omkodad med *våra* kodare och jämförd
mot filens egna byte. **75 av 75 HELA noder byte-identiska, 1 777 643 punkter** —
seed-post, punktantal, lagerhuvuden och lagerdata. ⚠️ Provet ligger inte i sviten: filen
är inte publik. Att göra om det mot autzens COPC-fil står i TODO.md.

Det visar bitströmskompatibilitet men inte att posterna *betyder* rätt: ett ömsesidigt
inverst fel i vår egen avkodare och kodare hade överlevt. Därför finns `records` i
generatorn, som dumpar referensens uppackade poster för hela filen att jämföra mot:
**0 avvikande poster av 1 777 643.** Två oberoende avkodare ger alltså samma innehåll,
inte bara samma byteström.

Kvar som osäkerhet: ett fel som HLas och referensen DELAR skulle överleva båda proven.

`verify` i generatorn packar upp varje COPC-nod med referensen och kodar om den. Mätt på
tredjepartsfilen: **75 av 75 noder byte-identiska**. Det gör byte-identitet till
ett rimligt mål för vår egen kodare — en kodare som återskapar originalfilens byte
finns, och lagervalen i just den här filen är förutsägbara.

Men beviset är svagare än det låter: avkodaren och kodaren är samma kodbas, så
ett fel referensen gör konsekvent åt båda hållen ger identitet ändå. Att posterna
verkligen är de verktyget som skrev filen avsåg stöds av att *vår* avkodare håller med
referensen om alla gyllene vektorer och läser rimliga fältvärden ur samma fil. Det är
plausibilitet, inte bevis — ett riktigt tvåimplementationsbevis kräver en
tredje avkodare, oberoende av både oss och referensen.

## Test Data
Inga testfiler i repot (`las/` är gitignorerad). Sökvägar, förväntade värden och
nedladdningsinstruktioner ligger samlade i [tests/test_data.h](tests/test_data.h) —
byt testdata där, inte i de enskilda testfilerna.

Saknas en fil skrivs `SKIP` och testet hoppas över; testsviten kraschar inte och
returnerar fortfarande 0.

En okomprimerad `.las` genereras ur en LAZ med `./build/tools/HLas_make_las <in.laz> <ut.las>`.

Tester som läser hela punktposten (RGB, `point_record_length`, råa byte-offsets) måste
öppna med `Cache::Off` — en cache-backad läsare lämnar inte ut råa poster alls.
