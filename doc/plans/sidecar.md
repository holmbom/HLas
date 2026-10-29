# Sidecar-cachen — plan

Status: **utredning**, inget byggt. Mätningarna nedan är gjorda 2026-08-28 på
den här maskinen och är skälet till att planen ser ut som den gör.

## Problemet

`.hlaz`-sidecaren är en cache som skapas tyst vid första `Reader::open()` av en
`.laz`. Den bär 13 byte per punkt — X, Y, Z som int32 plus en maskad klassbyte —
och ingenting annat.

Tre saker skaver:

1. **Den är förstörande, och det är förvalet.** Öppnar man en fil med RGB och
   GPS-tid en andra gång är fälten borta. Hela premissen för 1.1.2 var att
   läsningen ska bära hela punkten; standardvägen gör motsatsen.
2. **Den är större än filen den cachar.** Okomprimerad, 13 B/pt.
3. **Den skapas som en bieffekt av att läsa**, bredvid källan, och misslyckas
   tyst på en skrivskyddad katalog.

Och en fjärde, som är ren olycka: `.copc.laz` slutar på `.laz`, så en COPC-fil
får en sidecar trots att den redan har ett octree.

## Vad mätningarna visar

Alla siffror från `las/autzen-classified.copc.laz`, 10 653 336 punkter,
punktformat 7, utbredning 3426 × 4656 m.

### Storlek

| | |
|---|---|
| `autzen.laz` (LAS 1.2 format 3) | 53,7 MB |
| samma data som COPC | 77,4 MB |
| **`.hlaz`-sidecar** | **132,1 MB** |

Cachen är alltså störst av alla tre trots att den innehåller minst.

### Läsa hela filen

| väg | öppna | läsa | totalt | fält |
|---|---|---|---|---|
| `Reader` + sidecar | 0,1 ms | 25,7 ms | **25,8 ms** | bara XYZ + klass |
| `Reader` utan sidecar | 4092,6 ms | 23,1 ms | 4115,7 ms | alla |
| `CopcReader` strömmande | 0,0 ms | 4102,3 ms | 4102,4 ms | alla |

**Sidecaren är 160x snabbare.** Och `Reader` utan cache kostar lika mycket som
`CopcReader`: när man läser allt köper octreet ingenting. Skillnaden är att
`CopcReader` aldrig har mer än en nod i minnet, medan `Reader` materialiserar
383 MB.

### Små utspridda frågor

20 slumpmässiga rutor, var och en 0,04 % av ytan:

| väg | per fråga | punkter |
|---|---|---|
| `Reader` + sidecar | **8,0 ms** | 100 316 |
| `CopcReader` | 111,5 ms | 100 316 |

**Sidecaren är 14x snabbare trots att den saknar spatialt index och skannar hela
filen för varje fråga.** COPC packade upp 4 977 045 punkter för att hitta
100 316 — 50x överhämtning.

Förklaringen är hela nyckeln till den här planen:

> Uppackning kostar ~400 ns per punkt. En skanning i okomprimerat minne kostar
> under 1 ns. Att läsa hundra gånger fler punkter ur en okomprimerad cache slår
> att packa upp femtio gånger färre.

### Slumpvis åtkomst på index

200 000 punkter på 4,3 ms — **21 ns per punkt**. `CopcReader` har ingen
motsvarighet; dess API är spatialt.

### Spatial lokalitet i en vanlig LAZ

För ett tänkt index med bbox per chunk, mätt på `autzen.laz`:

| fråga (andel av ytan) | måste packas upp |
|---|---|
| 0,04 % | 4,3 % av filen |
| 1 % | 6,9 % |
| 6,25 % | 17,4 % |

Chunkens bbox är i median 2,28 % av filens yta — filen ligger inte i
skannordning. Motsvarande för COPC är 2,0 / 5,3 / 13,8 %, alltså bara 1,3–2x
bättre. **Octreet är inte magiskt; det har ett eget golv eftersom dess grova
nivåer täcker hela utbredningen.**

## Slutsatser som mätningarna tvingar fram

1. **Ett rent index ("SMART") är inte värt det.** Att packa upp 4 % av filen är
   ~180 ms; att skanna cachen är 8 ms. Indexet sparar disk, inte tid.
2. **Komprimera inte cachen med zstd eller lz4.** Cachen vinner *för att* den är
   okomprimerad. Generisk komprimering handlar tillbaka precis den egenskapen.
3. **COPC:s värde ligger över nätet**, där alternativet är att hämta hela filen,
   och i 3D-vyn (se nedan). Lokalt och för hela filer vinner cachen.

## Planen

### 1. Skapandet skiljs från läsningen

`Reader::open()` ska **inte** skapa någon sidecar. Ett eget kommando gör det,
med inställningar. Det fixar den tysta skrivningen bredvid användarens data, det
tysta misslyckandet på en skrivskyddad katalog, och att första öppningen är
långsam utan att någon förstår varför.

### 2. Läsningen väljer

```cpp
enum class Cache { Fast, Smart, Off };
Status open(const char* path, Cache cache = ...);
```

`Fast` använder cachen om den finns. `Off` rör den aldrig. Anroparen kan alltså
välja per anrop: ibland alla fält ur källan, ibland bara XYZ+klass ur cachen.

⛔ **Det förutsätter att index `i` betyder samma punkt i båda.** Se sorteringen
nedan.

Namnet `Lite` är avsiktligt undviket: `WriteMode::Lite` finns redan och betyder
något annat — ett val om vilken *fil du producerar*, inte om vad du kan *läsa
ur en fil du inte rört*.

**Öppen fråga:** ska förvalet vara `Fast` eller `Smart`? `Fast` är ett inbjudande
namn på ett läge som tyst tappar RGB och GPS-tid.

### 3. Innehållet väljs vid skapandet

`LITE` (XYZ + klass), `CUSTOM` (valda fält), `FULL` (allt källan bär).

Uppskattade storlekar för autzen, kolumnvis och bitpackat:

| | fält | storlek |
|---|---|---|
| LITE | X, Y, Z, klass | ~81 MB |
| CUSTOM | + intensitet, retur | ~110 MB |
| FULL | allt i format 7 | ~380 MB |

### 4. Bitpackning mot filens egen utbredning

Autzen sträcker sig 342 593 × 465 551 × 20 912 heltalsenheter, vilket kräver
**19 + 19 + 15 bitar**, plus 8 för klassen = 61 bitar. Det ryms i **8 byte i
stället för 13**: 132 MB → 81 MB.

Förlustfritt — exakt samma heltal, bara utan de tomma höga bitarna — och
avkodningen är ett skift och en addition.

### 5. Kolumnlagring i stället för poster

Gör `CUSTOM` naturligt och ger varje fält sin egen bitbredd. Gissningen att det
skulle ta frågan från 8 till ~3 ms höll inte — mätt blev den 6,77 ms mot 6,47 för
dagens postvisa form, alltså i praktiken oförändrad. Vinsten ligger i storleken
och i den gallrade läsningen, där kolumnvis är 2–3x snabbare.

### 6. Sortering — AVGJORD: sortera

Skrevs först som en öppen fråga att skjuta upp. Mätningen sa motsatsen.

Morton-sortering med frame-of-reference per block om 256 ger **54,2 MB och
0,049 ms per fråga** — 132x snabbare än skanningen, och den enda formen som
klarar en vandrande 3D-vy. Se de två körningarna nedan.

Priset är att **index `i` inte betyder samma punkt i cachen som i källan**. Att
köpa tillbaka det löftet kostar 30 MB, alltså mer än sorteringen sparade — och
Anroparen för inte index mellan två läsningar, så löftet är oanvänt. Det ska
därför designas bort, inte dokumenteras bort.

## 3D-vyn — det fall som ändrar bilden

Programmet har en 3D-vy där man går omkring. Den saknar två saker:

- **Strömma det som är framför kameran** medan man rör sig.
- **Gallra** — var n:te punkt, eller riktig detaljnivå efter avstånd.

Det är det användningsfall som avgjorde formen, och det är mätt: se *Tredje
körningen* nedan. Kort: **dagens cache klarar det inte** — 14,3 ms i snitt och
16,9 ms som värst per bildruta, alltså hela budgeten innan en punkt ritats.
Morton FOR tar 0,094 ms.

I Morton-ordning ger dessutom var n:te punkt en jämnt utspridd gallring över
området, inte en godtycklig delmängd. Det är vad en vandrande vy vill ha för sitt
glesare moln, och det faller ut av sorteringen utan extra arbete.

## Labbet

`tests/bench_sidecar.cpp` bygger olika cache-varianter och mäter dem mot
`CopcReader`. Det är ett laboratorium, inte ett test som går sönder — inga
påståenden, bara siffror.

Varianter som finns:

- postvis 13 B — dagens
- postvis bitpackat
- kolumnvis bitpackat
- Morton-sorterat med frame-of-reference per block

Åtkomstmönster som mäts:

- små utspridda rutor, full täthet
- samma rutor gallrade, steg 1 / 4 / 16
- en vandrande vy: 200 bildrutor, tre avståndsband

⚠️ **Tre mätfel har hittats i labbet självt** och rättats: tidtagning utan
uppvärmning gjorde tabellerna ojämförbara; gallringsvägen saknade den tidiga
utgången på X som rektangelvägen hade, vilket missgynnade kolumnvis; och en loop
vars resultat kastades med `(void)` ströks helt av kompilatorn, så tabellen visade
0,000 ms. Ett labb kan ljuga lika gärna som ett test.
- **gallrad läsning** — var n:te punkt ur ett område

Mått: byggtid, storlek på disk, frågetid, minne.

### Första körningen

`autzen-classified.copc.laz`, 10 653 336 punkter. Bitbredderna ur filens
utbredning blev X 19, Y 19, Z 15, klass 8 = 61 bitar.

| variant | bygg | storlek | per punkt | fråga (0,04 % av ytan) |
|---|---|---|---|---|
| postvis 13 B (dagens) | 58 ms | 132,1 MB | 13,00 B | 11,13 ms |
| postvis bitpackat | 115 ms | 77,5 MB | 7,63 B | 11,63 ms |
| kolumnvis bitpackat | 131 ms | 77,5 MB | 7,63 B | **6,75 ms** |

**Bitpackningen ger disk, inte fart** — postvis blir den till och med aningen
långsammare, eftersom bituppackningen kostar. **Kolumnvis ger båda:** samma 41 %
mindre *och* 1,65x snabbare, eftersom den läser X först och rör Y bara för de
punkter som passerar.

Gallrad läsning ur samma rutor:

| variant | steg 1 | steg 4 | steg 16 |
|---|---|---|---|
| postvis 13 B | 6,75 ms | 5,54 ms | 2,59 ms |
| postvis bitpackat | 12,60 ms | 5,12 ms | 3,29 ms |
| kolumnvis bitpackat | 12,68 ms | 3,48 ms | **1,83 ms** |

**1,83 ms för var 16:e punkt ur en ruta.** Med 16 ms per bildruta ryms flera
sådana frågor — utan octree och utan sortering.

⚠️ **Två fel i labbet som gör siffrorna missvisande.** De ska rättas innan något
byggs på dem:

1. **Tabellerna går inte att jämföra med varandra.** Gallringstestet kör efter
   det första, med data redan i cachen — därför är "postvis 13 B, steg 1"
   6,75 ms där men 11,13 ms ovan, för samma arbete. Jämförelser *inom* en tabell
   är giltiga, mellan dem inte.
2. **Gallringsvägen missgynnar kolumnvis.** Den läser alltid både X och Y, medan
   rektangelvägen hoppar över Y när X faller utanför.

### Andra körningen — Morton-sorterat med frame-of-reference

De två mätfelen ovan är rättade: uppvärmning före tidtagning, och summan används
så att kompilatorn inte kan stryka loopen. (Den *hade* strukit den — gallrings-
tabellen visade 0,000 ms.)

| variant | bygg | storlek | per punkt | fråga | rört |
|---|---|---|---|---|---|
| postvis 13 B (dagens) | 49 ms | 132,1 MB | 13,00 B | 6,473 ms | alla 10,6 M |
| postvis bitpackat | 142 ms | 77,5 MB | 7,63 B | 7,766 ms | alla 10,6 M |
| kolumnvis bitpackat | 110 ms | 77,5 MB | 7,63 B | 6,771 ms | alla 10,6 M |
| **morton FOR block 256** | 685 ms | **54,2 MB** | **5,34 B** | **0,049 ms** | **42 av 41 615 block, 10 905 punkter** |

**132x snabbare än dagens skanning, 2275x snabbare än `CopcReader`** (111,5 ms på
samma frågor). Indexet byggs inte vid sidan om — det är blockets min och max, som
FOR ändå måste lagra för att packa bitarna.

Gallrad läsning, medel per fråga:

| variant | steg 1 | steg 4 | steg 16 |
|---|---|---|---|
| kolumnvis bitpackat | 6,641 ms | 2,011 ms | 0,860 ms |
| **morton FOR** | **0,045 ms** | **0,029 ms** | **0,026 ms** |

26 mikrosekunder för ett glesat område — en vandrande vy ryms med marginal.

**Storleken** är 54,2 MB mot `autzen.laz`:s 53,7 MB, alltså i praktiken jämnt.
Blockhuvudet är 44 byte med utfyllnad; `first` går att räkna fram och `bitpos`
att härleda, så ~24 byte är rimligt och sparar 0,8 MB. Då hamnar cachen **under**
källan.

### Tredje körningen — den vandrande vyn

200 bildrutor längs en 3468 m färdväg, tre avståndsband framför kameran: nära
0–50 m i full täthet, mellan var fjärde punkt, fjärran var sextonde. Det är
3D-vyns verkliga mönster, till skillnad från utspridda rutor.

| variant | medel/bildruta | värsta bildrutan | punkter/bildruta |
|---|---|---|---|
| postvis 13 B (dagens) | 14,263 ms | **16,900 ms** | 5 490 |
| postvis bitpackat | 14,311 ms | 16,038 ms | 5 490 |
| kolumnvis bitpackat | 9,682 ms | 11,827 ms | 5 490 |
| **morton FOR block 256** | **0,094 ms** | **0,211 ms** | 5 490 |

**Dagens sidecar klarar inte en vandrande vy.** 14,3 ms i snitt och 16,9 ms som
värst är hela 16 ms-budgeten uppäten på att *hitta* punkterna, innan en enda
ritas.

**Morton FOR använder 1,2 % av budgeten** — 150 gångers marginal, och den värsta
bildrutan ligger 80x under. `CopcReader` mättes tidigare till 111,5 ms för en
enda ruta av den storleken, alltså sju hela bildrutebudgetar.

**90 % av blocken är samma som i föregående bildruta.** En riktig vy skulle kunna
hålla avkodade block kvar mellan bildrutor — men vid 0,094 ms behöver den inte.

Det gör 3D-vyn till en **förmåga som redesignen låser upp**, inte en optimering
av något som redan fungerar.

### Fjärde körningen — Hilbert mot Morton, och blockstorleken

Blockhuvudet räknas nu som det kostar **på disk**: `first` och `n` följer av
blockstorleken, och `bitpos` är en prefixsumma som räknas fram vid öppning. Kvar
blir min x/y/z, max x/y och tre bitbredder = **23 byte**, mot 44 i minnet.

| variant | bygg | storlek | B/pt | fråga | block rörda | punkter |
|---|---|---|---|---|---|---|
| morton 128 | 668 ms | 52,5 MB | 5,16 | 0,073 ms | 70 av 83 230 | 9 030 |
| morton 256 | 670 ms | 53,5 MB | 5,27 | 0,045 ms | 42 av 41 615 | 10 905 |
| morton 512 | 679 ms | 55,1 MB | 5,42 | 0,030 ms | 27 av 20 808 | 14 003 |
| **hilbert 128** | 1434 ms | **51,8 MB** | **5,10** | 0,071 ms | 61 av 83 230 | 7 865 |
| **hilbert 256** | 1443 ms | 52,8 MB | 5,20 | 0,041 ms | 35 av 41 615 | 8 985 |
| **hilbert 512** | 1477 ms | 54,4 MB | 5,35 | **0,025 ms** | **21 av 20 808** | 10 803 |

**Hilbert vinner på varje körningsaxel.** ~1,3 % mindre, 15–22 % färre block
rörda, 13–23 % färre punkter avkodade, ~15 % snabbare frågor. Priset är byggtiden:
**2,2x** (1443 mot 670 ms). Eftersom bygget sker en gång per fil är det ett gott
byte.

**Och något som inte var uppenbart: STÖRRE block är SNABBARE.** Block 512 avkodar
30 % fler punkter än 128 men är tre gånger snabbare — därför att kostnaden domineras
av att **skanna blockhuvudena**, inte av att avkoda punkter. Vid 512 finns 20 808
huvuden att gå igenom, vid 128 finns 83 230.

Blockstorleken är alltså en ren avvägning storlek mot fart:

| | storlek | fråga |
|---|---|---|
| hilbert 128 | 51,8 MB — minst | 0,071 ms |
| **hilbert 256** | **52,8 MB — under källan** | **0,041 ms** |
| hilbert 512 | 54,4 MB — över källan | 0,025 ms — snabbast |

Källfilen `autzen.laz` är 53,7 MB. **hilbert 256 ligger under den och är samtidigt
snabb** — det ser ut som rätt val.

Den vandrande vyn, 200 bildrutor:

| variant | medel | värst |
|---|---|---|
| postvis 13 B (dagens) | 14,259 ms | 17,577 ms |
| kolumnvis bitpackat | 9,913 ms | 11,306 ms |
| morton 256 | 0,091 ms | 0,193 ms |
| **hilbert 256** | **0,087 ms** | **0,178 ms** |
| hilbert 512 | 0,049 ms | 0,095 ms |

Alla sorterade varianter ligger under en procent av bildrutebudgeten. Valet mellan
dem avgörs av disk, inte av om vyn går att bygga.

### Femte körningen — var ligger optimum för blockstorleken?

| block | storlek | fråga | vandrande vy | block rörda | punkter |
|---|---|---|---|---|---|
| 128 | 51,8 MB | 0,063 ms | 0,156 ms | 61 av 83 230 | 7 865 |
| 256 | 52,8 MB | 0,039 ms | 0,088 ms | 35 av 41 615 | 8 985 |
| **512** | 54,4 MB | 0,024 ms | 0,049 ms | 21 av 20 808 | 10 803 |
| 1024 | 56,1 MB | **0,022 ms** | **0,039 ms** | 13 av 10 404 | 13 414 |
| 2048 | 58,0 MB | 0,024 ms | 0,038 ms | 8 av 5 202 | 18 329 |
| 4096 | 59,8 MB | 0,029 ms | 0,044 ms | 6 av 2 601 | 26 214 |

**Optimum ligger vid 1024, och det är en grund skål** — 512 till 2048 ligger inom
0,022–0,024 ms. Bortom 1024 väger de extra avkodade punkterna tyngre än de färre
huvudena: 4096 rör bara sex block men avkodar 26 214 punkter mot 512:s 10 803.

Storleken växer däremot monotont. **512 ger 92 % av den uppnåeliga farten och är
1,7 MB mindre än 1024** — det är punkten där man slutar betala för något.

⚠️ **Optimum vandrar uppåt med filstorleken.** Huvudskanningen är O(N/B), så på en
tio gånger större fil har block 128 över 800 000 huvuden och skillnaden mot 512 blir
tio gånger större än här. Allt ovan är mätt på 10,6 miljoner punkter.

**Den riktiga lösningen på avvägningen, om den behövs: ett tvånivåindex.**
Gruppera block i superblock med egen bbox, så blir skanningen
O(N/B/64 + rörda) i stället för O(N/B). Då vinner små block på båda axlarna — 51,8 MB
*och* full fart. Oprövat.

⚠️ **Byggtiderna i tabellerna är LABBETS, inte `BuildSidecar()`s.** Labbet
sorterar och packar punkter som redan ligger i minnet. Verklig `BuildSidecar`
mätt i [sidecar-las.md](sidecar-las.md) är cirka **tio gånger längre** —
16,0 s för autzen och 223,5 s för laserrutan — eftersom uppackning, extraktion och
filskrivning tillkommer. Cirka 1,5 µs per punkt.

### Sjätte körningen — en RIKTIG nationell laserruta

En nationell laserruta: **140 343 151 punkter**, 689,4 MB, punktformat 6,
utbredning 10 000 × 5 000 m, skriven av **ett COPC-verktyg**. Tretton gånger autzen, och en
annan verktygskedja.

| variant | bygg | storlek | B/pt | fråga | vandrande vy (medel / värst) |
|---|---|---|---|---|---|
| postvis 13 B (dagens) | 0,9 s | 1739,9 MB | 13,00 | 86,3 ms | **190,7 / 202,3 ms** |
| postvis bitpackat | 1,4 s | 1070,7 MB | 8,00 | 97,4 ms | 191,0 / 206,3 ms |
| kolumnvis bitpackat | 1,8 s | 1070,7 MB | 8,00 | 87,5 ms | 130,8 / 147,9 ms |
| morton FOR 512 | 12,4 s | 683,7 MB | 5,11 | 0,317 ms | 0,551 / 1,209 ms |
| **hilbert FOR 256** | 21,9 s | **658,7 MB** | **4,92** | 0,678 ms | 1,748 / 2,864 ms |
| hilbert FOR 512 | 21,7 s | 674,9 MB | 5,04 | 0,317 ms | 0,645 / 1,213 ms |
| **hilbert FOR 1024** | 20,4 s | 693,3 MB | 5,18 | **0,225 ms** | **0,439 / 0,699 ms** |

**Dagens cache skulle ge 5 bilder per sekund.** 190 ms per bildruta är tolv gånger
över budgeten. Hilbert 1024 tar 0,44 ms — 2,7 % av den. Skillnaden är **434x**, och
på frågorna **383x** (86,3 → 0,225 ms).

**Storleken landar under källan igen:** 658,7 MB mot 689,4. Dagens cache är
1739,9 MB, alltså 2,5x källan.

**Förutsägelsen om blockstorleken höll.** O(N/B) för huvudskanningen betyder att
optimum vandrar uppåt med filstorleken — på autzen var 256 nästan lika bra som
1024, här är **256 tre gånger långsammare** (548 216 huvuden mot 137 054).
Blockstorleken bör alltså inte vara en konstant utan följa punktantalet.

⚠️ **Två skillnader mot autzen som inte ska slätas över:**

- **Hilberts försprång krympte.** Vid block 512 är Hilbert 1,3 % mindre än Morton
  men *långsammare* i den vandrande vyn, 0,645 mot 0,551 ms. På autzen vann
  Hilbert på varje axel. Orsaken är inte utredd.
- **81 % blocköverlapp mellan bildrutor** mot autzens 90 %, eftersom färdvägen är
  dubbelt så lång och varje bildruta flyttar sig längre.

Bygget tar 20–22 s för Hilbert mot 12 s för Morton. Labbet toppade på **19,5 GB**
minne, vilket är labbets egen kostnad — det håller alla varianter samtidigt.

⚠️ Filen är COPC skriven av ett COPC-verktyg, alltså redan i octree-ordning. **Frågan om hur
en fil i ren SKANNORDNING beter sig är fortfarande obesvarad** — men för de
nationella laserrutorna är det här det verkliga fallet.

### Sjunde körningen — blockstorlekens kurva över tre filstorlekar

Samma tre Hilbert-varianter på tre filer:

| | 1,78 M punkter | 10,65 M (autzen) | 140,3 M (laser) |
|---|---|---|---|
| hilbert 256 | 0,010 ms | 0,039 ms | 0,678 ms |
| hilbert 512 | **0,009 ms** | 0,024 ms | 0,317 ms |
| hilbert 1024 | 0,010 ms | **0,022 ms** | 0,232 ms |
| **spridning** | **1,1x** | **1,8x** | **3,0x** |

**Ju större fil, desto mer betyder blockstorleken.** På 1,78 M punkter är den
likgiltig; på 140 M är 256 tre gånger långsammare än 1024.

Och orsaken syns rakt igenom: frågetiden följer **antalet block**, inte antalet
punkter. Vid block 256 ger 6 944 block 0,010 ms, 41 615 ger 0,039 och 548 216 ger
0,678 — praktiskt taget linjärt, vilket är vad O(N/B) förutsäger.

Hela kurvan för laserrutan:

| block | storlek | fråga | vandrande vy | block rörda |
|---|---|---|---|---|
| 256 | **658,7 MB** | 0,678 ms | 1,748 ms | 312 av 548 216 |
| 512 | **674,9 MB** | 0,317 ms | 0,645 ms | 170 av 274 108 |
| 1024 | 693,3 MB | 0,232 ms | 0,301 ms | 97 av 137 054 |
| 2048 | 713,0 MB | **0,200 ms** | 0,212 ms | 56 av 68 527 |
| 4096 | 733,1 MB | 0,226 ms | 0,169 ms | 33 av 34 264 |
| 8192 | 754,3 MB | 0,235 ms | **0,162 ms** | 20 av 17 132 |

**Frågeoptimum ligger vid 2048 i en grund skål** — 1024 till 8192 inom
0,200–0,235 ms. Men **vyn fortsätter förbättras ända till 8192**, och skillnaden
har en förklaring: vyn GALLRAR. Med steg 4 och 16 sjunker kostnaden att avkoda
punkter medan huvudskanningen är oförändrad, så gallrad läsning gynnar större
block än full täthet gör.

**Storleken växer däremot monotont, och bara 256 och 512 ligger under källans
689,4 MB.** Det avgör valet: 0,317 mot 0,200 ms är båda meningslösa mot en
16 ms-budget — 272x respektive 431x snabbare än idag — medan storleken går att
använda till något.

Interpolerat: B växer ungefär som **N^¼** för ren fart, men eftersom skålen är
grund och storleken växer monotont hamnar det praktiska valet på **512 för allt
över tio miljoner punkter**, och 256 under det.

⚠️ **Metodisk brist.** Vyns siffra för block 1024 blev 0,439 ms i sjätte körningen
och 0,301 i den sjunde — 46 % skillnad, medan frågetiden bara varierade 3 %.
Skälet är rimligen att sjätte körningen höll de platta varianterna i minnet
samtidigt, 3,9 GB som konkurrerade om cachen. **Varianter som mäts sida vid sida i
samma process stör varandra.** Finare distinktioner än ~30 % i vytabellen är inte
meningsfulla; frågetabellen är stabilare.

### Beslut som mätningarna motiverar

**Sortera, och lova ingen indexmotsvarighet.** Att köpa tillbaka löftet kostar
30 MB (källans index komprimerar inte efter sortering), vilket är dyrare än att
aldrig ha sorterat:

| | storlek |
|---|---|
| sorterat, inget löfte | **48,5–54,2 MB** |
| osorterat, löftet håller | 54,7 MB |
| sorterat + källindex | 78,5 MB |

Löftet ska då inte dokumenteras bort utan **designas bort**: en cache-backad vy
lämnar inte ut något index som ser ut att gälla i källan. Samma princip som när
`PointView` rapporterar `kSidecarFormat` i stället för källans format — man får
tomt i stället för fel.

⚠️ Bekräftat att inget index förs mellan två läsningar i dag.

### Vad som återstår i labbet

- **Formen på disk.** Allt ovan är mätt i minnet; ingen serialisering finns.
- **`CUSTOM` och `FULL`** — fler fält, för att se hur storlek och skanning skalar.
- **Blockhuvudets bbox**: `max_x`/`max_y` lagras exakt idag (8 byte). Att i stället
  härleda dem ur `min + 2^bits - 1` sparar 8 byte per block men ger grövre sållning.
  Oprövad avvägning.
- **En annan fil.** Allt är mätt på autzen. En nationell laserruta i skannordning kan
  bete sig helt annorlunda — det är den enskilt viktigaste kvarvarande osäkerheten.
- CUSTOM och FULL, alltså fler fält, för att se hur storleken och skanningen
  skalar när posten växer.
- Jämförelse mot `CopcReader` i samma körning, så alla tal kommer ur samma
  maskin och samma tillfälle.
