#pragma once
#include "las_error.h"
#include "file_io.h"
#include <cstdint>
#include <string>
#include <vector>

namespace HLas::detail {

// .hlz version 4 — LITE: X, Y, Z och klassificering, inget annat.
//
// ⛔ **RUTNÄT, inte Hilbert-block.** v3 sorterade punkterna längs en
// Hilbert-kurva och klippte dem i klumpar om 256 eller 512 stycken. v4 lägger i
// stället ett rutnät över filens utbredning och sorterar punkterna i celler.
//
// Skälet är mätt, i doc/plans/sidecar-las.md. v3:s form hade två problem som
// båda försvinner med ett rutnät:
//
//   1. FRÅGEFORMEN. Frågan gäller KORRIDORER — en buffrad polyline, en
//      orm med tiotalet hörn. v3 måste läsa ALLA blockhuvuden för att veta vilka
//      som berörs. Ett rutnät RASTRERAR polygonen: per cellrad räknas stråkets
//      x-utbredning fram, och ingen cell utanför rörs.
//   2. UPPSKATTNINGEN. Ett block bar exakt N punkter men okänd yta; en cell bär
//      känd yta och sitt antal följer av offseterna. Budget-läget i LOD kan
//      därför räkna fram gallringen exakt i stället för att gissa.
//
// Mätt på autzen, 10,65 M punkter, mot v3:
//
//   storlek     5,35 -> 4,92 B/pt   (och 5,43 -> 4,93 på en GLES fil)
//   bygge       9,5 s -> 3,2 s      (hinksortering slår jämförelsesortering)
//   korridor    2,29 ms -> 0,47 ms  (orm 12 m x 300 m)
//
// ⛔ **CELLINDEXET ÄR GLEST, och det är inte valfritt.** Bara celler som har
// punkter lagras, plus ett radindex. Med en tät celltabell blir en klippt
// korridorleverans — ett tunt stråk i en stor bbox — 79 % STÖRRE än v3, eftersom
// 96 % av cellerna är tomma och betalas ändå. Med glest index blir den mindre.
//
// ⛔ **`nx` och `ny` följer av bounds**, så cellerna blir kvadratiska i
// världsenheter och en avlång fil får ett avlångt rutnät.
//
// ⚠️ **Punkterna ligger INTE i källans ordning.** Cellordningen är hela poängen,
// och index `i` här är därför en annan punkt än index `i` i källfilen. Cachen
// lovar ingen indexmotsvarighet; det är designat, inte glömt.

inline constexpr char     HLZ_MAGIC[4]     = { 'H','L','Z','4' };
inline constexpr uint16_t HLZ_VERSION      = 4;
inline constexpr uint16_t HLZ_CONTENT_LITE = 0;

// Headern rymmer histogrammet: 128 byte fält plus uint32[256].
inline constexpr size_t HLZ_HIST_OFFSET = 128;
inline constexpr size_t HLZ_HEADER_SIZE = HLZ_HIST_OFFSET + 256 * 4;   // 1152

// Målbeläggning: punkter per cell.
//
// ⛔ Cellen får INTE ha en fast meterstorlek. Ett första utkast satte 10 m, och
// det blev 0,96 B/pt DYRARE än v3 på en fil med skala 0,001 — där blir en
// 10 m-cell 10 000 enheter, alltså 14 bitar för x och y, medan v3:s blockvisa
// frame-of-reference klarade sig på 11 i tät data.
//
// Optimum följer av att de två kostnaderna drar åt olika håll. Med C celler och
// n punkter är kostnaden per punkt
//
//     log2(ex * ey) - log2(C) + bz + 8  +  216 * C / n
//     ^^^^^^^^^^^^^^^^^^^^^^^             ^^^^^^^^^^^^
//     bitar för x och y: färre ju          celltabellen: 27 byte = 216 bitar
//     mindre cellen är                     per cell, dyrare ju fler
//
// Derivatan är noll vid C = n / (216 * ln2) ≈ n / 150. Alltså drygt hundra
// punkter per cell — OBEROENDE av skala och utbredning, vilket är precis det en
// fast meterstorlek inte är.
//
// Mätt bekräftar det: på autzen (skala 0,01) landar det nära de 10 m som mätte
// bäst i labbet, och på en fil med skala 0,001 blir cellen mindre i stället för
// att slösa bitar.
inline constexpr uint64_t HLZ_POINTS_PER_CELL = 100;

// Bygger cachens sökväg genom att LÄGGA TILL ".hlz".
//
// ⛔ Ändelsen läggs till, den ersätter inte. `foo.las` och `foo.laz` skiljer sig
// på en enda bokstav, och en ersättning hade kollapsat båda till samma fil — de
// hade sedan byggt om varandras cache i all evighet. `tools/make_las.cpp`
// producerar precis det filparet.
//
// Skiftlägesokänsligt: en `FOO.LAS` på Windows ska inte tyst bli utan cache.
std::string sidecar_path_for(const char* source_path);

// Filens storlek i byte, 0 om den inte går att läsa.
//
// Egen funktion därför att std::fseek/ftell tar `long`, som är 32 bitar på
// Windows — en fil över 2 GB hade gett 0 eller ett trunkerat tal, och storleken
// är det som skiljer två filer med samma punktantal åt.
uint64_t file_size_64(const char* path);

struct SidecarHeader {
    uint64_t point_count = 0;
    uint64_t cell_count  = 0;      // OCKUPERADE celler, inte nx*ny
    uint32_t nx = 0, ny = 0;
    double   x_scale = 1, y_scale = 1, z_scale = 1;
    double   x_offset = 0, y_offset = 0, z_offset = 0;
    int32_t  grid_x0 = 0, grid_y0 = 0;   // rutnätets ursprung, heltalsenheter
    int64_t  cell_w = 1, cell_h = 1;     // cellstorlek, heltalsenheter
    // Identifierar källan. Utan dem accepteras en gammal cache för en ny fil med
    // samma punktantal.
    uint64_t source_size = 0;
    uint8_t  source_format = 0;
    // Ett stickprov ur källans BYTE. Storlek, antal, format och kvantisering kan
    // alla vara oförändrade medan punkterna är andra — en omklassad fil som
    // kodas om till samma längd är inte otänkbar.
    uint64_t source_fingerprint = 0;
};

// FNV-1a över 32 fönster ur filen plus dess längd. Källan är redan mmap:ad, så
// det kostar en handfull sidfel — inte en genomläsning.
//
// ⚠️ Ett STICKPROV, inte en checksumma: en ändring som helt ligger mellan två
// fönster passerar. Se TODO.md.
uint64_t source_fingerprint_of(const uint8_t* data, size_t size);

// Läser en färdig .hlz. Äger mmap:en; vyer in i den gäller så länge den lever.
class SidecarReader {
public:
    ErrorCode open(const char* path, const SidecarHeader& expect);
    void close();
    bool is_open() const { return file_.is_open(); }

    uint64_t count() const { return h_.point_count; }
    const SidecarHeader& header() const { return h_; }

    // Klassräkningen som den låg när cachen byggdes. O(1) — det är hela poängen:
    // en skanning kostar 141 ms på autzen och 1,8 s på en nationell laserruta,
    // och den är dessutom LÅNGSAMMARE än att skanna källan, eftersom punkten är
    // bitpackad med koordinaterna interfolierade med klassen.
    const uint32_t* histogram() const { return hist_; }

    // En ockuperad cell. `cx`/`cy` är dess plats i rutnätet, `n` dess punkter.
    // ⛔ Cellen bär sin EGEN frame-of-reference, inte bara rutnätets position.
    //
    // Rutnätet ger indexet — rastreringen, sållningen, den exakta uppskattningen.
    // Men om bitbredden bestäms av CELLENS storlek kan den inte vara liten i tät
    // data och stor i gles, vilket ett Hilbert-block kunde. Mätt kostade det
    // 0,29 B/pt på en fil med ojämn täthet.
    //
    // Därför lagrar cellen `dx0`/`dy0` — punkternas faktiska min inom cellen —
    // och sina egna `bx`/`by`. Elva byte per cell, alltså 0,11 B/pt vid hundra
    // punkter per cell, och cellen blir lika anpassningsbar som ett v3-block.
    //
    // dx0/dy0 är uint32 och inte uint16: en fil som spänner hela int32 får celler
    // bredare än 65535 enheter, och ett uint16 hade tvingat fram en klämning som
    // kolliderade med taket på antalet celler.
    struct Cell {
        uint32_t cx, cy;
        uint64_t bitpos;
        uint32_t dx0, dy0;         // punkternas min INOM cellen
        int32_t  min_z;
        uint8_t  bx, by, bz;
        uint32_t n;
        uint64_t first;            // globalt index för cellens första punkt
    };
    uint64_t cell_count() const { return h_.cell_count; }
    Cell cell(uint64_t i) const;

    // Cellens hörn i heltalsenheter. Följer av rutnätet, lagras inte.
    void cell_bounds(const Cell& c, int64_t& x0, int64_t& y0,
                     int64_t& x1, int64_t& y1) const;

    // Första ockuperade cellen i rad `cy` med `cx >= from_cx`, eller cell_count.
    uint64_t row_lower_bound(uint32_t cy, uint32_t from_cx) const;
    uint64_t row_end(uint32_t cy) const;

    // Punkt `k` i cell `c`. Utan gränskontroll — anroparen har cellen.
    void point_in_cell(const Cell& c, uint32_t k,
                       int32_t& x, int32_t& y, int32_t& z, uint8_t& cl) const;

    // BARA klassen. En bitread i stället för fyra.
    //
    // Finns för LOD: gallringen avgörs av klassen, och en punkt som ska hoppas
    // över ska inte kosta tre koordinatuppackningar först. Samma väg vore värd
    // att använda för accepted[256] — se TODO.
    uint8_t class_in_cell(const Cell& c, uint32_t k) const;

    // Punkt `i` i cachens egen ordning. Slår upp cellen med binärsökning.
    bool point(uint64_t i, int32_t& x, int32_t& y, int32_t& z, uint8_t& cl) const;

    // Cellen som innehåller punkt `i`. O(log celler) — finns för att en
    // paginerad läsning ska kunna hoppa direkt till sin startcell i stället för
    // att svepa fram till den.
    uint64_t cell_of_point(uint64_t i) const;

private:
    // Skild från open() så att den kan omslutas av bad_alloc-hanteringen.
    ErrorCode open_impl(const char* path, const SidecarHeader& expect);

    MappedFile     file_;
    SidecarHeader  h_{};
    const uint32_t* hist_     = nullptr;
    const uint32_t* row_start_ = nullptr;   // ny + 1
    const uint32_t* cell_x_   = nullptr;    // cell_count
    const uint64_t* cell_off_ = nullptr;    // cell_count + 1, bitpositioner
    const int32_t*  cell_mnz_ = nullptr;    // cell_count
    const uint32_t* cell_dx_  = nullptr;    // cell_count
    const uint32_t* cell_dy_  = nullptr;    // cell_count
    const uint8_t*  cell_bx_  = nullptr;    // cell_count
    const uint8_t*  cell_by_  = nullptr;    // cell_count
    const uint8_t*  cell_bz_  = nullptr;    // cell_count
    const uint8_t*  data_     = nullptr;
    std::vector<uint32_t> cell_row_;        // cell -> rad, räknas vid open
    std::vector<uint64_t> first_;           // prefixsumma av punktantal
};

// Bygger en .hlz ur punkter i godtycklig ordning. Skriver atomiskt: temporärfil
// plus rename, så en avbruten körning aldrig lämnar en halv cache som ser hel ut.
struct SidecarPoint { int32_t x, y, z; uint8_t c; };

// Allokerar för hinksorteringen och för den packade utdatan. Minnesbrist kommer
// tillbaka som ErrorCode::FileOpenFailed, inte som ett kastat std::bad_alloc —
// resten av modulen rapporterar med ErrorCode, och en anropare som fångar
// felkoder men inte undantag hade annars fått processen nedlagd.
ErrorCode write_sidecar(const std::string& path, const SidecarHeader& meta,
                        std::vector<SidecarPoint>& pts);

} // namespace HLas::detail
