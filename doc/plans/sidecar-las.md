# Sidecar 1.1.4 — `.las`, histogram, LOD, och ett nytt format

**Status: GENOMFÖRD — släppt som 1.1.4.** A–F är byggda, granskade och mätta med
den byggda koden; se [doc/CHANGELOG.md](../CHANGELOG.md) för utfallet.

⚠️ **Två avvikelser mot planen nedan, båda medvetna:**

- **LOD blev bara `stride`, ingen budget.** En budget beror på frågans
  utsträckning och hade brutit zoom-stabiliteten, som är hela poängen med att
  gallra per cell.
- **Den dubbla källan för `.las` ströks helt.** Cache och källa samtidigt gav
  `get_xyz(i)` ur cellordningen och `points().x(i)` ur filens — två ordningar
  bakom ett index. Cachen äger ordningen för båda källformerna.

⚠️ **Storlekssiffrorna nedan är labbets, mätta på autzen i minnet.** De rätta
siffrorna för den byggda koden står i CHANGELOG:en — v4 är 1–3 % mindre än v3 på
de två stora filerna och 6 % större på den lilla med skala 0,001, vilket labbet
inte kunde se eftersom det bara mätte autzen.

Allt nedan är mätt där det står "mätt", och märkt när det är räknat eller gissat.

Labben: `tests/bench_las_vs_cache.cpp`, `bench_hist_lod.cpp`, `bench_corridor.cpp`,
`bench_grid.cpp`.

---

## Sammanfattning

Fyra delar, i den ordning de kom fram:

| del | avgjort? |
|---|---|
| 1. Sidecar för `.las` | **byggt** — `.hlz` tillagd som ändelse |
| 2. Histogram i headern | **byggt** — `uint32[256]`, API oförändrat |
| 3. LOD — stride eller budget per klass | formen avgjord, **inte byggd** |
| 4. **Rutnät i stället för Hilbert-block** | **byggt** — se förbehållet om storlek |

Och ett fynd i 1.1.3 som gäller oavsett:

⛔ **Sållningen jämför blockens bbox mot FRÅGANS BBOX, inte mot polygonen.** För
en diagonal korridor täcker den bboxen hela filen, så ingenting sållas bort.
Cachen blir då **långsammare än ingen cache alls** — 568 ms mot en platt fils 524.
Det är en ren algoritmbugg, ingen formatfråga, och den ska rättas oavsett vad som
väljs i del 4.

---

## Del 1 — sidecar för `.las`

Sidecaren är utestängd för `.las` på tre ställen: `sidecar_path_for_laz()`,
`BuildSidecar()` och `Impl::cached()` (som kräver `parsed.is_laz`).

Motivet höll till och med 1.1.2: cachen fanns för att slippa uppackningen, och en
`.las` har ingen. **Sedan 1.1.3 bär cachen ett spatialt index, och det hjälper
oavsett om källan är komprimerad.**

### Mätt: 43–190x på selektiva frågor

`bench_las_vs_cache.cpp`, autzen. `.las` är 34 B/post och skannas O(N):

| ruta | `.las` idag | med cache | kvot |
|---|---|---|---|
| 10 × 10 m | 60,1 ms | 0,316 ms | **190x** |
| 50 × 50 m | 60,5 ms | 0,408 ms | 148x |
| 200 × 200 m | 62,7 ms | 1,47 ms | 43x |
| 1000 × 1000 m | 74,8 ms | 24,5 ms | 3x |

`.las`-kolumnen är **platt ~60 ms oavsett rutstorlek** — full skanning, ~5,6 ns
per punkt, även när frågan träffar 66 punkter. Öppningen ger inget: `.las`
mmap:as på 0,026 ms mot cachens 0,461.

⚠️ Mätfel i första utkastet, rättat: `count` är BÅDE skanningsintervall och
utdatatak, så 2 000 000 lät båda vägarna läsa bara sina första två miljoner
punkter — olika punkter, eftersom ordningarna skiljer sig. Labbet avbryter nu med
fel när antalen inte stämmer.

### ⛔ Den dubbla källan är UPPSKJUTEN, och skälet är värt att minnas

Tanken var: för en `.laz` betyder "använd cachen" att källan inte packas upp, så
råa poster saknas. För en `.las` är källan mmap:ad ändå — alltså kunde `points()`
lämna ut alla fält MEDAN frågorna sållade på cellerna, och cachens största
nackdel försvinna.

**Halvbyggd blev den farlig.** `cached()` delades i `har_index()` och
`råa_poster_saknas()`, men den senare kopplades aldrig hela vägen. Följden:

- `get_xyz(i)` läste ur cachens cellordning
- `points().x(i)` läste ur filens ordning
- vyn märktes som `kSidecarFormat` fast bufferten bar riktiga LAS-poster, så
  `points().classification(i)` plockade klassen ur fel byte
- RGB, GPS och intensitet rapporterades som saknade fast de fanns

Två ordningar bakom ett index ger tyst fel data. **Cachen äger nu ordningen för
båda källformerna**, precis som i v3. Den dubbla källan kan byggas färdigt senare,
men då som en hel sak: index måste betyda samma sak i varje API.

⚠️ Buggen överlevde för att **inget test byggde en cache för en `.las`**. Det
finns nu.

### Ändelsen: `.hlz`, TILLAGD

```
foo.las  ->  foo.las.hlz
foo.laz  ->  foo.laz.hlz
```

Ersätt-semantiken kollapsade `foo.las` och `foo.laz` till samma `foo.hlaz`, och
de hade byggt om varandras cache i all evighet — `tools/make_las.cpp` producerar
precis det filparet. `.hlz` för att formatets magi redan är `H`,`L`,`Z`,`3`;
versionen bor i magin, inte i ändelsen.

Ändelsen är upptagen av två utdöda program, ingetdera nära punktmoln. Medvetet
val — vi identifierar på magin.

---

## Del 2 — histogram i headern

### Mätt: cachen är i dag LÅNGSAMMARE än ingen cache

| fil | utan cache | med cache | |
|---|---|---|---|
| autzen (10,6 M) | 49,8 ms | **140,8 ms** | 2,8x långsammare |
| laserruta (140,3 M) | 655 ms | **1835 ms** | 2,8x långsammare |

Orsaken: punkten är bitpackad med `x`, `y`, `z` och klassen **interfolierade**, så
ett histogram som bara vill ha klassen packar ändå upp tre koordinater. Den platta
vägen läser en byte per punkt.

Headern **rättar alltså en försämring**, den är inte bara en optimering.

### Formen

`uint32[256]` räcker — `BuildSidecar` avvisar redan fler än `UINT32_MAX` punkter.
1 KB. Räknas på de MASKADE klasserna, samma värden som lagras.

**API:t ändras inte.** `get_classification_histogram(uint64_t counts[256])` finns
redan och fungerar för LAS, LAZ och cache. Den hämtar hädanefter ur **cachen
först, annars genom att skanna** — och anroparen får inte veta vilket.

⛔ Just därför MÅSTE de ge samma svar. Det gör den lagrade räkningen till data som
kan ljuga utan att någon märker det. Provet som jämför cachens histogram mot
källans är garantin, och det är ännu ett argument för checksumman i TODO.

⚠️ Ingen `histogram_is_instant()`. Skillnaden är 0 mot 1,8 s, vilket vore värt att
veta för en UI — men svaret är identiskt, så det vore att läcka mekanik.
Medvetet avfärdat.

**Fynd på köpet:** en klass-först-väg som hoppar över koordinatuppackningen gör
VARJE klassfiltrerad fråga billigare. `accepted[256]` kan läsa klassen med en
`bitread` och bara packa upp x/y/z för de som passerar.

---

## Del 3 — LOD

### Formen

Per klass **antingen stride eller budget**, läget valt per fråga. Tom klasslista
= alla klasser, och då har värdelistan ett element. Listan är **också filtret**.

```
[2,5], [10,4]        stride per klass
[2,5], [300k,100k]   budget per klass
[],    [10]          stride, alla klasser
[],    [100k]        budget, alla klasser
[],    []            allt
```

⛔ Allt räknas **inom det frågade området**. ⛔ Budgeten är ett **tak, inte ett
mål** — finns bara 50k markpunkter får du 50k.

### Stride kostar ingenting

```
ta punkt k i cellen om (k % stride[cls]) == 0
```

Tillståndslöst, sprider urvalet jämnt över ytan, och **säkert när `start`/`count`
delar en cell mitt itu** — vilket en global räknare inte är: den hade gett olika
urval beroende på hur anroparen delade upp läsningen.

### Mätt tak

`tid = 0,304 ms + punkter × 28,1 ns` på autzen med Hilbert-block. Golvet är
sållningen och överlever varje stride, så gallring lönar sig bara där punkttermen
dominerar — stora områden, alltså 3D-vyn. För en 5 × 5 m-ruta finns ingenting att
hämta.

⚠️ Härlett ur golv och lutning, inte uppmätt — funktionen finns inte.

### Budget-uppskattningen — och varför rutnätet löser den

Med Hilbert-block är uppskattningen `antal_block × block_size`, och det är
**katastrofalt fel för en korridor**: en diagonal korridors bbox täcker hela
filen, så uppskattningen blir 14–136x för hög. Ber du om 100k punkter i ett 20 m
stråk får du några hundra.

| korridor | i polygon | i dess bbox | faktor |
|---|---|---|---|
| diagonal 20 m | 78 487 | 10 653 336 | **136x** |
| diagonal 50 m | 195 868 | 10 653 336 | 54x |
| diagonal 200 m | 767 194 | 10 653 336 | 14x |
| axelparallell 50 m | 115 860 | 115 860 | 1,0x |

⛔ **Med ett rutnät är uppskattningen exakt per cell.** Cellernas punktantal följer
av offseterna, och bara randceller bidrar med fel — vid 10 m-celler är det litet.
Budget-läget blir därmed användbart, vilket det inte är med dagens form.

Klassandelarna inne i en korridor mot hela filen, mätt: −2,5 % till +9,6 % på
autzen. Men autzen har bara två klasser — svagt underlag.

---

## Del 4 — rutnät i stället för Hilbert-block

`bench_grid.cpp` bygger båda formerna i minnet och kör samma frågor mot dem.
**Båda får korrekt polygonsållning**, så det som jämförs är strukturerna och inte
1.1.3:s bugg.

Rutnätet har **fysisk cellstorlek**, inte ett antal: `nx` och `ny` följer av
bounds, så cellerna blir kvadratiska i världsenheter och en avlång fil får ett
avlångt rutnät.

### Storlek — autzen, 10,65 M punkter

| | MB | B/pt |
|---|---|---|
| `.las` källa | 345,4 | 34 |
| rått i minnet | 132,1 | 13 |
| `.laz` källa | 53,7 | 5,3 |
| `.hlaz` v3 (1.1.3) | 54,4 | 5,35 |
| **rutnät 10 m, glest index** | **50,0** | **4,92** |

Rutnätet slipper lagra `min_x`, `min_y`, `max_x`, `max_y` per block — de följer av
cellens position — och bitbredden för x och y blir konstant ur cellstorleken.
Bygget i labbet: **3,2 s mot Hilberts 9,5**, eftersom hinksortering på cellindex
slår jämförelsesortering på Hilbert-nycklar.

### Fart — medel per fråga, 12 slumpvis placerade ormar

Frågeformen är en **buffrad polyline** — en kraftledning svänger, och offseten ger
korridoren. 8–18 hörn, inte fyra.

| fråga | träffar | platt | `.hlaz` v3 idag | + polygonsåll | **rutnät 10 m** |
|---|---|---|---|---|---|
| orm 12 m × 300 m | 2 202 | 18,2 ms | 2,29 | 1,24 | **0,47** |
| orm 50 m × 300 m | 9 447 | 19,1 | 2,93 | 1,95 | **1,22** |
| orm 50 m × 1 km | 27 683 | 41,7 | 26,3 | 7,49 | **4,36** |
| orm 50 m × 3 km | 66 527 | 222 | 211 | 25,7 | **12,9** |
| orm 200 m × 1 km | 110 908 | 50,9 | 35,3 | 17,3 | **14,6** |
| rak diagonal 50 m | 195 868 | 524 | **568** | 33,4 | **14,2** |

⛔ **STORLEKEN VANN INTE ÖVERALLT, och labbet sa fel.** Labbet mätte bara autzen.
Mot den riktiga testfilen (1,78 M punkter, skala 0,001) är v4 **6,09 B/pt mot
v3:s 5,75** — alltså 6 % STÖRRE. Orsaken är att en cell inte kan vara liten i tät
data och stor i gles, vilket ett Hilbert-block kunde, och att celltabellen kostar
27 byte per ~100 punkter mot v3:s 23 per 256. Att ge cellen egen
frame-of-reference provades och hjälpte inte (6,04 → 6,09).

Avvägningen är alltså **6 % storlek mot 15–1362x fart**, inte en ren vinst.

**Uppmätt med den BYGGDA koden**, medel per fråga över 12 slumpvis placerade ormar:

| fråga | autzen ocachad | autzen v4 | laser ocachad | laser v4 |
|---|---|---|---|---|
| orm 12 m × 300 m | 62,0 ms | **0,234** | 800,9 ms | **0,588** |
| orm 50 m × 1 km | 67,9 | 1,554 | 955,7 | 5,940 |
| orm 200 m × 1 km | 68,5 | 4,636 | 833,7 | 18,144 |

Vinsten VÄXER med filen: den ocachade vägen är O(N), medan v4:s tid följer
träffarna. Uppackningen av laserrutan tog 113 s innan första punkten gick att läsa.

Två avläsningar ur labbet:

1. **Polygonsållningen ensam ger ~10x** — 568 → 33 ms. Ingen formatändring.
2. **Rutnätet ger 1,2–2,8x till.** Marginalen är störst för smala korridorer, där
   sållningen betyder mest, och minst för 200 m breda, där man ändå packar upp det
   man vill ha.

Rutnätet packar upp mindre i onödan: 2,2x mot träffarna för en 12 m-orm, mot
Hilberts 6,6x.

### ⛔ Glesprovet — och varför indexet MÅSTE vara glest

autzen klippt till ett 100 m brett stråk: 389 362 punkter, 3,7 % av filen, bbox
oförändrad. Alltså en klippt korridorleverans.

| | B/pt |
|---|---|
| hilbert 512 | 5,43 |
| rutnät 10 m, **tät** celltabell | **9,72** |
| rutnät 10 m, **glest** index | **4,93** |

Med en tät tabell blir rutnätet **79 % större** — 96 % av cellerna är tomma och
betalas ändå. Hilbert-blocken betalar per PUNKT och bryr sig inte om täckningen.

Med glest index — bara ockuperade celler, sorterade på (rad, x), plus ett
radindex — försvinner straffet helt. Och på tät data kostar det nästan ingenting:
4,92 mot 4,88.

**Använd alltid den glesa formen.** Inom 1 % på tät data, dubbelt så bra på gles.

### Formen

```
rad-index:           ny + 1 st uint32                     -> radens första cell
per ockuperad cell:  cx (4) + offset (8) + min_z (4) + bz (1)  = 17 B
bx, by:              konstanta, ur cellstorleken
antal i cellen:      följer av nästa cells offset
```

Frågan rastrerar: för varje cellrad räknas polygonens x-utbredning i just den
raden, binärsökning ger radens första cell, sedan framåt. Ingen cell utanför
stråket rörs.

---

## Granskningen — sju blockerande fynd

Codex granskade v4 och hittade sju. Alla åtgärdade:

1. **`.las` med cache blandade två punktordningar** — se ovan. Den halvbyggda
   dubbla källan är borttagen.
2. **Nollskala gav olika svar ur cache och källa.** Skrivaren behandlade den som
   1,0 och läsvägen likaså, men det är ingen giltig invers: alla punkter får
   samma världskoordinat medan rutnätet ligger kvar på de råa talen, och hela
   rader sållas bort. Nollskala avvisas nu vid bygget.
3. **NaN- eller oändlig skala gav odefinierat beteende** — `side / sx` blir NaN,
   och NaN till int64 är UB. Avvisas nu, och klämningen sker före konverteringen.
4. **Rutnätets aritmetik kontrollerades inte mot int64-spill.** En fientlig cache
   med `cell_w = INT64_MAX` och positiv `grid_x0` hade spillt över i
   `cell_bounds()`, `point_in_cell()` och radbandsberäkningen.
5. **`row_start[0] == 0` kontrollerades inte**, så celler före första radstarten
   accepterades men nåddes aldrig. Och `cell_x` verifierades inte vara STRIKT
   STIGANDE inom raden, fast `row_lower_bound()` binärsöker — en osorterad rad
   hade tappat celler tyst.
6. **Minnesbrist vid `resize()` kunde kasta genom det publika `Reader::open()`**,
   och `close()` frigjorde inte kapaciteten, så en avvisad cache lämnade kvar
   filstyrda allokeringar.
7. **Histogrammets summa validerades inte.** Den måste vara punktantalet — annars
   accepteras en cache som bevisligen ger fel svar. Billig invariant, inte en
   garanti; för det behövs checksumman i TODO.

Plus: `each_point()` svepte fram till startcellen i stället för att binärsöka,
alltså kvadratiskt över en paginerad läsning. Och kostnadshärledningen i headern
angav 17 byte per cell när tabellen kostar 27.

## Vad som INTE är mätt

- **Bara autzen.** Inte laserrutan (140 M), inte en riktig `.las`, inte en fil i ren
  skannordning.
- **Bara två klasser** i autzen — proxyfelet för budget är svagt underlagt.
- **Labbet är inte biblioteket.** Ingen fil-I/O, ingen mmap,
  ingen `start`/`count`. Siffrorna är strukturernas, inte den färdiga kodens.
- **Kall kontra varm sidcache.** Allt mätt varmt. Cachen är ~7x mindre än en
  `.las` och bör vinna mer när det är kallt — hypotes.
- **LOD är inte byggt**, så stride-tabellen är härledd.
- **Byggtiderna i labbet är inte `BuildSidecar()`s.** Verklig: 16,0 s för autzen
  och **223,5 s för en laserruta** — cirka 1,5 µs per punkt, alltså ~10x labbets
  siffror, eftersom uppackning och filskrivning tillkommer. Att bygga cachen för en
  hämtad laserruta tar alltså närmare fyra minuter.

---

## Arbetet

**A. Polygonsållningen** — gäller oavsett del 4, och rättar att cachen i dag är
sämre än ingen cache. Blockets eller cellens rektangel mot polygonen, inte mot
polygonens bbox.

**B. Formatet v4** — rutnät med glest cellindex, `.hlz` som tillagd ändelse,
histogram i headern.

**C. `.las`-vägen** — släpp `.laz`-kravet i `BuildSidecar`, generalisera
routningen i `Reader::open()` (cachen sitter i `CachedLazSource` som bara nås när
`is_laz`), och dela `cached()` i `har_index()` och `råa_poster_saknas()`.

**D. LOD** — `accepted`/stride/budget, med uppskattningen ur cellernas antal.

**E. Skiftläget** — ändelsematchningen är skiftlägeskänslig och tar bara gemena
`.laz`. En `FOO.LAS` på Windows får tyst ingen cache.

---

## Utanför scope

- `CUSTOM` och `FULL` som cache-innehåll
- Tvånivåindex — rutnätets rastrering gör det mindre angeläget
- Cachens egen integritetskontroll (men histogrammet skärper argumentet)
