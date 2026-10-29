[English](README.md) · **Svenska**

# HLas - Holmbom LAS

Ett snabbt C++20-bibliotek utan beroenden för att läsa punktmoln i LAS och LAZ.

## Funktioner

- Minnesmappad fil-I/O för högsta läsfart
- Egen LAZ-avkodning **och -kodning** (inget externt beroende)
- Partiell COPC-läsning ur lokala filer eller via HTTP range requests — bara de
  octree-noder en fråga berör hämtas
- Valfri `.hlz`-cache för både `.las` och `.laz`, som byggs uttryckligen — ett
  rutnät över filens utbredning, bitpackat, så att den öppnas på ~0 ms i stället
  för en hel uppackning, och dess celler är samtidigt ett spatialt index. Cirka
  5 byte per punkt mot 13 lagrade rått, och korridorfrågor går 4–19x snabbare än
  med v3
- Detaljnivå: `stride[256]` per klassificering på polygonfrågorna. Gallringen
  sker per rutnätscell, så en stride på 10 ger *samma* punkter utzoomad som
  inzoomad — en vy som panorerar flimrar aldrig
- XYZ i omgångar, med valfritt filter på klassificering
- Spatialt polygonfilter (2D, XY-planet) med AABB-förfilter
- Klasshistogram (hela filen eller inom en polygon)
- DEM med fyllnadsmetoder (Nearest, IDW, TIN), flertrådad
- `accepted[256]`-filter för att välja flera klasser effektivt
- `PointView` — en icke-ägande, formatmedveten vy över de råa posterna, så
  ingenting som källan bär kastas
- LAS- och LAZ-skrivning i två lägen: `Lite` (XYZ + klassificering) och `Full`
  (källans poster oförändrade)
- Konfigurerbart nätverkslager (`NetworkSettings`) — proxy, CA-källa, tidsgränser
- Fel tystas aldrig — `Status` från `open()`, `last_error()` efter varje anrop
- En enda publik header (`HLas.h`)

## Format som stöds

### LAS-versioner
- LAS 1.1, 1.2, 1.3, 1.4

### Punktformat
- Format 0 — XYZ + intensitet + flaggor (20 byte)
- Format 1 — format 0 + GPS-tid (28 byte)
- Format 2 — format 0 + RGB (26 byte)
- Format 3 — format 0 + GPS-tid + RGB (34 byte)
- Format 4 — format 1 + vågformspaket (57 byte)
- Format 5 — format 3 + vågformspaket (63 byte)
- Format 6–10 — LAS 1.4-formaten (30+ byte, utökade returnummer, scannerkanal)

Format 4, 5, 9 och 10 bär vågformspaket, och format 8 och 10 bär NIR.
Okomprimerade filer i de formaten går att läsa; **komprimerade** gör det inte —
codecarna `Wavepacket13`/`Wavepacket14` finns inte här, och inte heller någon
referensimplementation att verifiera dem mot. Sådana filer avvisas med
`Status::LazNotSupported` i stället för att läsas fel i tysthet.

### LAZ-komprimering
- Egen LAZ-**avkodning och -kodning** — inga externa beroenden
- **Chunked** (LAS 1.2, format 0–3): Point10, GPSTime11, RGB12 — läsning och skrivning
- **LayerChunked** (LAS 1.4, format 6–7): Point14, RGB14 — läsning och skrivning
- RGBNIR14 och Byte14 avkodas; Pointwise (en enda chunk) avkodas
- Aritmetisk kodare (LAZ-kompatibel)

Verifierad byte för byte mot filer skrivna av tredje part: varje chunk i en
LAS 1.2 format 3-fil med 10,6 M punkter och varje nod i en COPC format 7-fil med
1,8 M punkter kodas om till exakt de byte som ligger på disk.

### COPC
`CopcReader` läser octree-indexet och packar bara upp de noder en fråga berör —
lokalt eller via HTTP range requests. I en fil med 278 noder tog en fråga som
täcker 20 noder 4 ms att öppna och 739 ms att besvara, mot 10,1 s för att packa
upp hela filen. Kräver en server som stödjer range requests; `open_url()` kräver
`-DHLAS_ENABLE_NET=ON`.

Punktordningen är obestämd med fler än en tråd — punkterna hör ihop radvis, men
vilken nod som kommer först avgörs av trådarna.

## Bygga

```bash
cmake -B build -G Ninja
cmake --build build
```

Flaggor:
- `-DHLAS_ENABLE_LAZ=OFF` — stäng av LAZ-stödet
- `-DHLAS_ENABLE_NET=ON` — slå på HTTP range requests (kräver libcurl)
- `-DBUILD_SHARED_LIBS=ON` — bygg som delat bibliotek
- `-DHLAS_BUILD_TESTS=OFF` — hoppa över tester, mätprogram och verktyg. Den är
  på när HLas byggs för sig själv och av när ett annat projekt drar in det med
  FetchContent eller `add_subdirectory`
- `-DHLAS_INSTALL=OFF` — inga installationsregler. Samma förval som ovan

### Använda HLas i ett projekt

Ur källan, med FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(HLas
    GIT_REPOSITORY https://github.com/holmbom/HLas.git
    GIT_TAG        main)   # eller en release-tagg, t.ex. vX.Y.Z
FetchContent_MakeAvailable(HLas)

target_link_libraries(myapp PRIVATE HLas::HLas)
```

Eller med de färdigbyggda biblioteken. Varje release har Release-bibliotek för
Windows (MSVC och MinGW) och Linux (GCC), så biblioteket inte behöver kompileras
i ditt projekt. Packa upp ett och peka CMake på det:

```cmake
find_package(HLas 1 CONFIG REQUIRED)   # -DCMAKE_PREFIX_PATH=sökväg/till/HLas-vX.Y.Z-…
target_link_libraries(myapp PRIVATE HLas::HLas)
```

Välj paketet för din kompilator: statiska C++-bibliotek går inte att blanda
mellan MSVC, MinGW och GCC. MinGW- och Linux-paketen finns också som
`-net`-varianter med HTTP range requests; de kräver samma libcurl i ditt bygge
(`mingw-w64-ucrt-x86_64-curl` i MSYS2, `libcurl4-openssl-dev` på Debian/Ubuntu).
Alla paket är Release-byggen. Ett Debug-bygge av din app med MSVC, eller
nätstöd med MSVC, kräver källan.

### Plattformar

Linux, Windows och macOS. Linux är plattformen varje mätning i den här filen är
gjord på. På Windows körs samma två kommandon i en *x64 Native Tools*-prompt
(eller efter `vcvars64.bat`); MSVC 19.50 / VS 2026 är det som provas, i fem
konfigurationer — Release, Debug, `HLAS_ENABLE_LAZ=OFF`, `BUILD_SHARED_LIBS=ON`
och ASan. LAZ-kodaren är byte för byte lika på båda: omkodningen av `autzen.laz`
ger 214 av 214 hela chunkar identiska med filen på båda plattformarna. Sviten
körs också med `TMPDIR` pekande på en katalog med mellanslag och `äöåÅÖ` —
sökvägar är UTF-8 på alla plattformar, och på Windows är det vad
`CreateFileW`/`_wfopen` finns där för.

`-DHLAS_ENABLE_NET=ON` är byggt och provat på Windows med MinGW (MSYS2 UCRT64,
libcurl 8.22), men **inte** med MSVC: där finns inget libcurl att bygga mot, så
`src/net/` har inte kompilerats med MSVC.

macOS är **oprövat**. Källan bär inget Linux-specifikt, men ingen har byggt den
på en Mac — det är ett antagande, inte en mätning.

## Användning

```cpp
#include "HLas.h"

// Öppna en LAS- eller LAZ-fil (formatet känns igen automatiskt).
// Ingen cache används eller skapas om du inte ber om det. Se "Cache" nedan.
HLas::Reader reader;
HLas::Status status = reader.open("pointcloud.laz");
if (status != HLas::Status::Ok) {
    // hantera felet: HLas::status_message(status) eller reader.last_error()
}

// Headern
const HLas::Header& h = reader.header();
uint64_t count = h.point_count;

// XYZ för en punkt
double x, y, z;
reader.get_xyz(0, x, y, z);

// I omgångar (snabbt, ~120 Mpts/s)
double xs[10000], ys[10000], zs[10000];
uint64_t n = reader.get_xyz_batch(0, 10000, xs, ys, zs);

// Filtrera på klassificering (t.ex. mark = 2)
uint64_t ground = reader.get_xyz_filtered(0, 10000, 2, xs, ys, zs);

// Spatialt polygonfilter.
//
// `count` är BÅDE intervallet som söks och taket för antalet skrivna punkter, så
// utbuffertarna måste rymma `count` element. Hela filens antal tillsammans med
// en array på 10000 element spiller över så fort 10001 punkter träffar.
double poly_x[] = {350000, 351000, 351000, 350000};
double poly_y[] = {6509000, 6509000, 6510000, 6510000};

std::vector<double> px(count), py(count), pz(count);
uint64_t inside = reader.get_xyz_polygon(0, count, poly_x, poly_y, 4,
                                          px.data(), py.data(), pz.data());

// Eller läs i sidor, så stora som du vill hålla i minnet:
std::vector<double> qx(100000), qy(100000), qz(100000);
for (uint64_t s = 0; s < count; s += 100000) {
    uint64_t got = reader.get_xyz_polygon(s, 100000, poly_x, poly_y, 4,
                                           qx.data(), qy.data(), qz.data());
    /* ... använd de första `got` elementen ... */
}

// Polygonfilter med klassificering per punkt
std::vector<uint8_t> cls(count);
uint64_t all = reader.get_xyzc_polygon(0, count, poly_x, poly_y, 4,
                                        px.data(), py.data(), pz.data(),
                                        cls.data());

// Filtrera på flera godkända klasser
bool accepted[256] = {};
accepted[2] = true;  // mark
accepted[9] = true;  // vatten
uint64_t filtered = reader.get_xyzc_polygon(0, count, poly_x, poly_y, 4,
                                             px.data(), py.data(), pz.data(),
                                             cls.data(), -1, accepted);

// Klasshistogram — antal punkter per klass
uint64_t counts[256] = {};
reader.get_classification_histogram(counts);
// counts[2] = antal markpunkter, osv.

// Histogram inom en polygon
reader.get_classification_histogram_polygon(counts, poly_x, poly_y, 4);

// DEM (flertrådad, med godkända klasser)
bool dem_classes[256] = {};
dem_classes[2] = true;  // mark
dem_classes[9] = true;  // vatten
HLas::DemResult dem = reader.generate_dem(poly_x, poly_y, 4,
                                           1.0,                  // cellstorlek (m)
                                           HLas::DemFill::Tin,   // fyllnadsmetod
                                           dem_classes,           // accepted[256]
                                           4);                    // antal trådar
// dem.elevation[row * dem.cols + col] — NaN = inget värde

// Rå punktdata
const uint8_t* raw = reader.point_data(0);

// Felrapportering — tystas aldrig.
// Ett punktdataanrop som inte kan svara ger ett tomt resultat OCH sätter
// last_error() — med en cache i bruk finns till exempel inga råa poster.
// last_error() rensas av nästa anrop som lyckas, så läs den direkt efter
// anropet som felade.
if (reader.get_xyz_batch(0, 10000, xs, ys, zs) == 0)
    printf("%s\n", reader.last_error().c_str());
```

### Alla fält filen bär

`get_xyz*` ovan är bekvämlighet. `points()` ger en `PointView` över de råa
posterna med alla fält källan har — ingen konvertering, ingenting kastat:

```cpp
const HLas::PointView pts = reader.points();      // kräver Cache::Off
for (uint64_t i = 0; i < pts.count(); ++i) {
    uint16_t r, g, b;
    if (pts.rgb(i, r, g, b)) { /* ... */ }
    double t = pts.gps_time(i);
    uint8_t cls = pts.classification(i);          // maskad i format 0-5
}
```

Accessorerna är formatmedvetna: `scan_angle()` ger grader oavsett om källan
lagrar int8-grader (format 0-5) eller int16-steg om 0,006° (format 6+), och fält
som formatet saknar svarar `has_*() == false` i stället för att hittas på.

### Partiell COPC-läsning

```cpp
HLas::CopcReader c;
c.open_url("https://example.com/tile.copc.laz");

auto est = c.estimate(x0, y0, x1, y1);            // övre gräns
std::vector<uint8_t> recs(est.point_count * c.header().point_record_length);
uint64_t n = c.read_region(x0, y0, x1, y1, recs.data(), est.point_count);
auto v = c.view(recs.data(), n);
```

För långa korridorer: använd `read_region_streaming()` /
`read_polygon_streaming()` — ingenting större än en nod materialiseras.

### Spara ett urval

```cpp
HLas::LazWriter w;
w.create_like("out.laz", reader.header(), HLas::WriteMode::Full);  // behåll allt
w.write(pts);
if (w.close() != HLas::Status::Ok) { /* filen är ofullständig; kasta den */ }
```

`WriteMode::Lite` (förvalet) skriver X, Y, Z och klassificering som punktformat
6 — ungefär halva storleken, och rätt val när urvalet ska bli en DEM.
`WriteMode::Full` behåller källans poster oförändrade, och utdataformatet följer
källan. **`close()` måste anropas och dess svar kontrolleras** — den skriver
chunk-tabellen och den slutgiltiga headern.

### Nätverksinställningar

Biblioteket kan inte veta hur nätet ser ut där det körs. Ange det du vet innan
du kopplar upp:

```cpp
HLas::NetworkSettings ns;
ns.ca_file = "/etc/pki/tls/certs/ca-bundle.crt";  // eller ns.use_native_ca = true
ns.proxy_mode = HLas::NetworkSettings::ProxyMode::Manual;
ns.proxy_url = "http://proxy.corp:3128";
ns.timeout_scale = 300;                            // långsam förbindelse
c.set_network_settings(ns);
```

Ett förvalt `NetworkSettings` ändrar ingenting. Observera att
`ProxyMode::None` och `ProxyMode::System` är motsatser: `None` stänger av proxyn
och ignorerar `http_proxy`/`https_proxy`, `System` låter dem läsas.

### Cache

En `.las` eller `.laz` kan ha en `.hlz`-cache bredvid sig med X, Y, Z och
klassificering. Ändelsen **läggs till**, den ersätter inte — `foo.las.hlz`,
`foo.laz.hlz` — eftersom `.las` och `.laz` skiljer sig på en bokstav och annars
hade kollapsat till samma cachefil. Den **byggs uttryckligen**; läsning skapar
aldrig någon:

```cpp
HLas::BuildSidecar("pointcloud.laz");             // en gång, kostar en uppackning

HLas::Reader r;
r.open("pointcloud.laz", HLas::Cache::Fast);      // ~0 ms i stället för sekunder
```

Cachen är ett **rutnät** över filens utbredning, bitpackat med frame-of-reference
per cell: varje cell lagrar sitt eget minimum och bara de bitar dess egen
utbredning behöver. Minimum och maximum tillsammans *är* cellens bbox, så det
spatiala indexet faller ut ur komprimeringen i stället för att betalas separat.

Cellindexet är **glest** — bara upptagna celler lagras, plus ett radindex, med
cellerna sorterade på `(rad, x)`. Det är inte valfritt: med en tät celltabell
växer en klippt korridorleverans med 79 %, eftersom 96 % av cellerna är tomma
och ändå betalas. Cellstorleken följer en **målbeläggning** på ~100 punkter i
stället för en fast meterstorlek; en fast 10 m-cell kostade 0,96 B/pt extra på
en fil med skala 0,001.

Den kostar cirka **5 byte per punkt**, mot 13 lagrade rått som tre int32 plus en
klassbyte:

| fil | punkter | v3-cache | **v4-cache** | källan som `.laz` |
|---|---|---|---|---|
| nationell laserruta | 140,3 M | 674,9 MB — 5,04 B/pt | **666,0 MB — 4,98** | 689,4 MB — 5,15 |
| `autzen` | 10,65 M | 54,4 MB — 5,35 | **53,0 MB — 5,22** | 53,7 MB — 5,29 |
| liten format 7-ruta | 1,78 M | 9,8 MB — 5,75 | **10,3 MB — 6,09** | 13,9 MB — 8,21 |

⚠️ **v4 är inte mindre än v3 överallt.** Den är 1–3 % mindre på de två stora
filerna och 6 % större på den lilla. En cell kan inte vara liten i tät data och
stor i gles, vilket ett Hilbert-block kunde, och det slår igenom på en fil med
fin skala och liten utbredning.

⚠️ **"Mindre än källan" gäller mot COPC, inte mot en mager LAZ.** En COPC-fil bär
punktformat 6 eller 7 med långt mer per punkt än de fyra fält cachen håller. Mot
en vanlig LAS 1.2 format 3-`.laz` är det i praktiken jämnt. Vinsten som är sann
överallt är ~13 B/pt ner till ~5.

Alla uppmätta filer var COPC och låg redan spatialt ordnade. En fil i ren
skannordning är oprövad (se [TODO.md](TODO.md)).

**Frågorna sållas mot polygonen, inte mot dess bbox.** Den skillnaden är hela
poängen: en diagonal korridors bbox täcker hela filen, så under v3 sållades
ingenting bort och cachen var *långsammare än ingen cache alls*. Rutnätet
rastreras rad för rad, och ingen cell utanför formen rörs. Mätt som medel över
12 slumpvis placerade *ormar* — buffrade polylines med 8–18 hörn, den form en
kraftledningskorridor faktiskt har:

| fråga | `autzen` v3 | `autzen` **v4** | laserruta v3 | laserruta **v4** |
|---|---|---|---|---|
| 12 m × 300 m | 0,413 ms | **0,101** | 1,951 ms | **0,243** |
| 50 m × 1 km | 3,840 | **0,727** | 16,940 | **2,517** |
| 50 m × 3 km | 32,133 | **2,138** | 162,522 | **8,528** |
| 200 m × 1 km | 5,307 | **2,194** | 23,574 | **8,750** |
| histogram | 43,9 | **0,000** | 581,0 | **0,000** |

Båda versionerna ger samma träffantal på varje rad. Histogrammet är en
`uint32[256]` i headern, så `get_classification_histogram()` är ett
minnesuppslag.

**Detaljnivå.** Båda polygonfrågorna tar en valfri `stride[256]`:
`stride[c] == N` behåller var N:te punkt av klass `c`, och 0 eller 1 betyder full
täthet. Gallringen styrs av punktens plats **i sin rutnätscell**, aldrig av en
löpande räknare över träffarna, så urvalet beror inte på frågan — en vy som
zoomar eller panorerar får samma punkter och flimrar inte. Utan cache faller
stride tillbaka på det globala punktindexet: samma andel, andra punkter, och inte
spatialt jämnt.

⛔ **Den lagrar bara X, Y, Z och klassificering, och inte i filens ordning.**
Gränsen går vid **råa poster**, inte vid index: `points()`, `point_data()` och
`read_points()` svarar tomt *och sätter `last_error()`*, eftersom cachen inte
har några poster att lämna ut. Allt annat fungerar, anrop som tar ett index
inräknade — ett index är då en position i *den* läsaren, och cachen är en annan
ordning av samma mängd punkter.

⛔ **Enumet `Cache` skapar ingenting.** Alla tre lägena avgör bara om en
*befintlig* cache får användas; `BuildSidecar()` är det enda som skapar en.
Öppnar du med `Fast` utan att ha byggt något finns ingen cache att ta, och
källan packas upp som vanligt.

- `Fast` — använd cachen om den finns och hör till filen
- `Smart` — samma, men hoppa över den för en COPC-fil, som redan bär ett octree.
  Bygger du ändå en för en COPC-fil ignorerar `Smart` den — välj `Fast`
- `Off` — rör aldrig någon (förvalet)

`BuildSidecar()` bygger för vilken `.laz` som helst, COPC inräknad. Vill du inte
cacha dina COPC-filer låter du bli att anropa den för dem.

En enda header; länka mot biblioteksmålet `HLas::HLas`.

## Prestanda

Provat med filer på 6 M punkter (LAS 1.2 + LAZ 1.4 COPC):
- XYZ i omgångar: ~120 Mpts/s (~3 GB/s)
- Filtrerad läsning: ~240 Mpts/s
- DEM 270x407 (1 m-celler): ~24 ms med 4 trådar, ~62 ms med en
- LAZ-uppackning: transparent, samma API som LAS

Cache, Release-bygge:
- Bygg en gång (uppackning + rutnätssortering): 3,5 s för 10,65 M punkter, 44,9 s
  för 140,3 M
- Varje öppning därefter (mmap): ≈ 0 ms
- Storlek: 5,22 B/pt på filen med 10,65 M och 4,98 på den med 140 M — mot 13 rått
  (XYZ int32 + klassbyte), alltså ungefär 2,6x mindre

## Licens

[MIT](LICENSE) © 2026 Erik Holmbom

Material från tredje part (GPS-tidens kodning i LAZ-kodaren) står i
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
