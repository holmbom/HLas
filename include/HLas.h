#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define HLAS_VERSION_MAJOR 1
#define HLAS_VERSION_MINOR 1
#define HLAS_VERSION_PATCH 5
#define HLAS_VERSION_STRING "1.1.5"

namespace HLas {

constexpr int version_major = HLAS_VERSION_MAJOR;
constexpr int version_minor = HLAS_VERSION_MINOR;
constexpr int version_patch = HLAS_VERSION_PATCH;
constexpr const char* version_string = HLAS_VERSION_STRING;

enum class Status : uint8_t {
    Ok = 0,
    FileNotFound,
    FileOpenFailed,
    MmapFailed,
    InvalidSignature,
    UnsupportedVersion,
    InvalidHeader,
    UnsupportedPointFormat,
    InvalidPointRecordLength,
    CorruptFile,
    LazNotSupported,
};

const char* status_message(Status s);
const char* version();

struct Header {
    uint8_t  version_major;
    uint8_t  version_minor;
    uint16_t file_source_id;
    uint16_t global_encoding;
    char     system_id[32];
    char     generating_software[32];
    uint16_t creation_day;
    uint16_t creation_year;

    uint8_t  point_format;
    uint16_t point_record_length;
    uint64_t point_count;
    uint64_t points_by_return[15];

    double x_scale, y_scale, z_scale;
    double x_offset, y_offset, z_offset;
    double max_x, min_x;
    double max_y, min_y;
    double max_z, min_z;

    bool     is_laz;
    uint32_t num_vlrs;
    uint32_t num_evlrs;
};

struct VlrInfo {
    char     user_id[16];
    uint16_t record_id;
    const uint8_t* data;
    uint64_t data_size;
};

// Icke-ägande vy över punktposter — den enda formen läsningen lämnar ut data i.
//
// Vyn äger ingenting. Den pekar in i en buffert någon annan håller vid liv: en
// mmap:ad fil, en uppackad LAZ-nod, eller en buffert anroparen själv allokerat.
// Den slutar gälla när den bufferten gör det.
//
// **Varför en vy och inte arrayer.** Uppackningen ger redan hela posten. Att
// plocka ut fyra fält ur den och skala dem till doubles är extra arbete ovanpå,
// inte en snabb väg — och det som inte plockas ut är borta för gott. Vyn lämnar
// ut posterna som de ligger, och anroparen betalar bara för de fält den rör.
//
// Vyn känner sitt punktformat och svarar ärligt om vad som finns: `has_rgb()`
// är false för format 6, och `rgb()` returnerar då false utan att röra sina
// utparametrar. Ett fält som formatet saknar hittas alltså inte på, det gissas
// inte fram ur bytena.
//
// **Vyn är kopierbar, men en kopia förlänger ingenting.** Sparar du undan den
// och bufferten hinner försvinna pekar både kopian och varje `record()` i
// tomma luften. Behöver något leva längre, kopiera bytena — inte vyn.
class PointView {
public:
    // Punktformat för en .hlz-sidecar: X, Y, Z och klass, inget
    // annat. Inte ett LAS-format — sidecaren är vår egen cache.
    static constexpr uint8_t kSidecarFormat = 0xFF;

    PointView() = default;

    // `format` är 0–10 eller kSidecarFormat. `stride` är postens verkliga längd
    // och får vara större än formatets baslängd — mellanskillnaden är extra bytes.
    //
    // **`base` måste peka på minst `count * stride` läsbara byte.** Det kan vyn
    // inte kontrollera, så det är anroparens ansvar. Är posten för kort för ett
    // fält blir fältet däremot osynligt i stället för farligt: `has_*()` svarar
    // false och accessorn ger noll. Räcker den inte ens till koordinaterna är
    // hela vyn `!valid()` och varje accessor svarar tomt.
    PointView(const uint8_t* base, uint64_t count, uint16_t stride, uint8_t format,
              double x_scale, double y_scale, double z_scale,
              double x_offset, double y_offset, double z_offset)
        : base_(base), count_(count), stride_(stride), format_(format),
          sx_(x_scale), sy_(y_scale), sz_(z_scale),
          ox_(x_offset), oy_(y_offset), oz_(z_offset) {
        set_offsets(format);
        clip_offsets_to_stride();
    }

    // Giltig kräver mer än en pekare: posten måste rymma åtminstone
    // koordinaterna. En vy med stride 4 över format 7 hade annars sett giltig
    // ut och läst RGB långt utanför bufferten.
    bool valid() const { return base_ != nullptr && stride_ >= 12; }
    uint64_t count() const { return count_; }
    bool     empty() const { return count_ == 0; }

    uint8_t  point_format()  const { return format_; }
    uint16_t record_length() const { return stride_; }

    // Byte utöver vad punktformatet självt kräver. Noll för de flesta filer.
    uint16_t extra_bytes() const {
        const uint16_t b = base_length(format_);
        return (b && stride_ > b) ? static_cast<uint16_t>(stride_ - b) : 0;
    }

    double x_scale()  const { return sx_; }
    double y_scale()  const { return sy_; }
    double z_scale()  const { return sz_; }
    double x_offset() const { return ox_; }
    double y_offset() const { return oy_; }
    double z_offset() const { return oz_; }

    // Vilka fält posten bär. Fråga innan du läser om skillnaden spelar roll —
    // annars får du noll, vilket är ett giltigt värde och alltså inte går att
    // skilja från "saknas".
    bool has_intensity()    const { return off_intensity_ != kNone; }
    bool has_returns()      const { return off_returns_   != kNone; }
    bool has_scan_angle()   const { return off_scan_ang_  != kNone; }
    bool has_user_data()    const { return off_user_data_ != kNone; }
    bool has_point_source() const { return off_psid_      != kNone; }
    bool has_gps_time()     const { return off_gps_       != kNone; }
    bool has_rgb()          const { return off_rgb_       != kNone; }
    bool has_nir()          const { return off_nir_       != kNone; }

    // Rå post. nullptr utanför intervallet.
    // valid() och inte bara base_: koordinataccessorerna läser fyra byte var
    // ur offset 0, 4 och 8, alltså tolv byte, utan att fråga någon offsettabell.
    // En post kortare än så måste stoppas HÄR — clip_offsets_to_stride() skyddar
    // bara attributen.
    const uint8_t* record(uint64_t i) const {
        return (valid() && i < count_) ? base_ + i * stride_ : nullptr;
    }

    // ---- Koordinater ----
    // raw_* är heltalen som de ligger i filen, alltså samma kvantisering som
    // källan. x/y/z skalar dem till världskoordinater.
    int32_t raw_x(uint64_t i) const { return i32(i, 0); }
    int32_t raw_y(uint64_t i) const { return i32(i, 4); }
    int32_t raw_z(uint64_t i) const { return i32(i, 8); }

    double x(uint64_t i) const { return raw_x(i) * sx_ + ox_; }
    double y(uint64_t i) const { return raw_y(i) * sy_ + oy_; }
    double z(uint64_t i) const { return raw_z(i) * sz_ + oz_; }

    // Batch — samma sak men utan ett anrop per punkt och axel. Returnerar
    // antalet som faktiskt skrevs; kortare än `n` vid slutet av vyn.
    uint64_t xyz(uint64_t start, uint64_t n,
                 double* out_x, double* out_y, double* out_z) const {
        if (!valid() || start >= count_ || !out_x || !out_y || !out_z) return 0;
        const uint64_t avail = count_ - start;
        const uint64_t take  = n < avail ? n : avail;
        const uint8_t* p = base_ + start * stride_;
        for (uint64_t i = 0; i < take; ++i, p += stride_) {
            int32_t v[3];
            std::memcpy(v, p, 12);
            out_x[i] = v[0] * sx_ + ox_;
            out_y[i] = v[1] * sy_ + oy_;
            out_z[i] = v[2] * sz_ + oz_;
        }
        return take;
    }

    // ---- Attribut ----
    // Alla ger noll när formatet saknar fältet.
    // Format 0–5 delar klassbyten med tre flaggbitar, så bara bit 0–4 är
    // klassen där — en punkt med klass 2 och synthetic satt lagras som 34.
    // Vyn maskar bort flaggorna; de finns i synthetic()/key_point()/withheld().
    //
    // Hela biblioteket maskar likadant — get_classification(), accepted[256],
    // histogrammen, DEM och sidecar-skrivningen — så det finns inte två svar.
    uint8_t classification(uint64_t i) const {
        const uint8_t* p = record(i);
        if (!p || off_class_ == kNone) return 0;
        const uint8_t v = p[off_class_];
        return legacy_ ? static_cast<uint8_t>(v & 0x1F) : v;
    }

    uint16_t intensity(uint64_t i) const {
        const uint8_t* p = record(i);
        return (p && off_intensity_ != kNone) ? u16(p + off_intensity_) : 0;
    }

    // Returnummer och antal returer. Format 0–5 lägger dem i tre bitar vardera,
    // format 6–10 i fyra — därav grenen. Fyra bitar är hela poängen med de nya
    // formaten: fler än sju returer per puls går inte att uttrycka i tre.
    uint8_t return_number(uint64_t i) const {
        const uint8_t* p = record(i);
        if (!p || off_returns_ == kNone) return 0;
        return legacy_ ? static_cast<uint8_t>(p[off_returns_] & 0x07)
                       : static_cast<uint8_t>(p[off_returns_] & 0x0F);
    }

    uint8_t number_of_returns(uint64_t i) const {
        const uint8_t* p = record(i);
        if (!p || off_returns_ == kNone) return 0;
        return legacy_ ? static_cast<uint8_t>((p[off_returns_] >> 3) & 0x07)
                       : static_cast<uint8_t>((p[off_returns_] >> 4) & 0x0F);
    }

    // Bara format 6–10. Noll för de äldre, som inte har begreppet.
    uint8_t scanner_channel(uint64_t i) const {
        const uint8_t* p = record(i);
        if (!p || legacy_ || off_flags_ == kNone) return 0;
        return static_cast<uint8_t>((p[off_flags_] >> 4) & 0x03);
    }

    bool scan_direction(uint64_t i) const { return flag_bit(i, 6); }
    bool edge_of_flight_line(uint64_t i) const { return flag_bit(i, 7); }

    // Synthetic, key-point, withheld — och overlap, som bara finns i 6–10.
    bool synthetic(uint64_t i) const { return class_flag(i, 0); }
    bool key_point(uint64_t i) const { return class_flag(i, 1); }
    bool withheld(uint64_t i)  const { return class_flag(i, 2); }
    bool overlap(uint64_t i)   const { return legacy_ ? false : class_flag(i, 3); }

    // Grader. Format 0–5 lagrar hela grader i en int8, format 6–10 lagrar
    // 0,006-gradersteg i en int16 — vyn ger samma enhet oavsett, så anroparen
    // slipper veta vilket format den läser.
    double scan_angle(uint64_t i) const {
        const uint8_t* p = record(i);
        if (!p || off_scan_ang_ == kNone) return 0.0;
        if (legacy_) return static_cast<double>(static_cast<int8_t>(p[off_scan_ang_]));
        return static_cast<double>(static_cast<int16_t>(u16(p + off_scan_ang_))) * 0.006;
    }

    uint8_t user_data(uint64_t i) const {
        const uint8_t* p = record(i);
        return (p && off_user_data_ != kNone) ? p[off_user_data_] : 0;
    }

    uint16_t point_source_id(uint64_t i) const {
        const uint8_t* p = record(i);
        return (p && off_psid_ != kNone) ? u16(p + off_psid_) : 0;
    }

    double gps_time(uint64_t i) const {
        const uint8_t* p = record(i);
        if (!p || off_gps_ == kNone) return 0.0;
        double v; std::memcpy(&v, p + off_gps_, 8); return v;
    }

    // Returnerar false och rör inte r/g/b om formatet saknar RGB.
    bool rgb(uint64_t i, uint16_t& r, uint16_t& g, uint16_t& b) const {
        const uint8_t* p = record(i);
        if (!p || off_rgb_ == kNone) return false;
        r = u16(p + off_rgb_); g = u16(p + off_rgb_ + 2); b = u16(p + off_rgb_ + 4);
        return true;
    }

    uint16_t nir(uint64_t i) const {
        const uint8_t* p = record(i);
        return (p && off_nir_ != kNone) ? u16(p + off_nir_) : 0;
    }

    // Postens baslängd för ett punktformat, 0 för ett okänt format.
    static uint16_t base_length(uint8_t format) {
        switch (format) {
            case 0:  return 20;  case 1:  return 28;  case 2:  return 26;
            case 3:  return 34;  case 4:  return 57;  case 5:  return 63;
            case 6:  return 30;  case 7:  return 36;  case 8:  return 38;
            case 9:  return 59;  case 10: return 67;
            case kSidecarFormat: return 13;
            default: return 0;
        }
    }

private:
    static constexpr uint16_t kNone = 0xFFFF;

    // Byteoffsetar per punktformat.
    //
    // Det här är den enda platsen i biblioteket som känner LAS-postens layout på
    // fältnivå, och hela poängen med vyn: anroparen ska aldrig behöva veta att
    // scanvinkeln flyttade från byte 16 till 18 mellan format 5 och 6, eller att
    // den samtidigt bytte från int8-grader till int16-steg om 0,006 grader.
    void set_offsets(uint8_t f) {
        off_intensity_ = off_returns_ = off_flags_ = off_class_ = kNone;
        off_scan_ang_ = off_user_data_ = off_psid_ = off_gps_ = kNone;
        off_rgb_ = off_nir_ = kNone;
        legacy_ = false;

        if (f == kSidecarFormat) {
            // Sidecaren är vår egen 13-bytespost: X, Y, Z och klass. Inget annat
            // finns, och det är avsiktligt — den cachar geometri, inte filen.
            off_class_ = 12;
            return;
        }

        if (f <= 5) {
            // Format 0–5. Returnummer, antal returer, scanriktning och
            // edge-of-flight delar EN byte, och klassificeringen delar sin byte
            // med tre flaggbitar — det är därför klasser över 31 inte får plats,
            // och skälet till att skrivaren använder format 6.
            legacy_        = true;
            off_intensity_ = 12;
            off_returns_   = 14;
            off_flags_     = 14;   // samma byte som returerna
            off_class_     = 15;
            off_scan_ang_  = 16;   // int8, hela grader
            off_user_data_ = 17;
            off_psid_      = 18;
            switch (f) {
                case 1:  off_gps_ = 20; break;
                case 2:  off_rgb_ = 20; break;
                case 3:  off_gps_ = 20; off_rgb_ = 28; break;
                case 4:  off_gps_ = 20; break;                 // + wave packet
                case 5:  off_gps_ = 20; off_rgb_ = 28; break;  // + wave packet
                default: break;                                // 0: inget mer
            }
            return;
        }

        if (f <= 10) {
            // Format 6–10. Returerna får fyra bitar var, flaggorna en egen byte
            // med scannerkanal, och klassificeringen en hel byte för sig.
            off_intensity_ = 12;
            off_returns_   = 14;
            off_flags_     = 15;
            off_class_     = 16;
            off_user_data_ = 17;
            off_scan_ang_  = 18;   // int16, steg om 0,006 grader
            off_psid_      = 20;
            off_gps_       = 22;
            switch (f) {
                case 7:  off_rgb_ = 30; break;
                case 8:  off_rgb_ = 30; off_nir_ = 36; break;
                case 10: off_rgb_ = 30; off_nir_ = 36; break;  // + wave packet
                default: break;                                // 6, 9: ingen färg
            }
            return;
        }

        // Okänt format. Alla fält förblir kNone, så varje accessor svarar
        // "saknas" i stället för att läsa byten på måfå.
    }

    // Nollställer varje offset vars fält inte ryms i den faktiska posten.
    //
    // Poängen är att en för kort post ska göra fältet OSYNLIGT, inte farligt.
    // Vyns stride kommer från en header eller från en anropare, och en fil som
    // deklarerar format 7 med 30 byte per post ska ge has_rgb() == false — inte
    // sex byte lästa ur nästa punkt. has_*() förblir därmed ärlig: den svarar
    // på vad som går att läsa, inte på vad formatet lovar.
    void clip_offsets_to_stride() {
        auto fits = [&](uint16_t off, uint16_t size) {
            return off != kNone && off + size <= stride_;
        };
        if (!fits(off_intensity_, 2)) off_intensity_ = kNone;
        if (!fits(off_returns_,   1)) off_returns_   = kNone;
        if (!fits(off_flags_,     1)) off_flags_     = kNone;
        if (!fits(off_class_,     1)) off_class_     = kNone;
        if (!fits(off_scan_ang_, legacy_ ? 1 : 2)) off_scan_ang_ = kNone;
        if (!fits(off_user_data_, 1)) off_user_data_ = kNone;
        if (!fits(off_psid_,      2)) off_psid_      = kNone;
        if (!fits(off_gps_,       8)) off_gps_       = kNone;
        if (!fits(off_rgb_,       6)) off_rgb_       = kNone;
        if (!fits(off_nir_,       2)) off_nir_       = kNone;
    }

    static uint16_t u16(const uint8_t* p) {
        uint16_t v; std::memcpy(&v, p, 2); return v;
    }
    int32_t i32(uint64_t i, uint16_t off) const {
        const uint8_t* p = record(i);
        if (!p) return 0;
        int32_t v; std::memcpy(&v, p + off, 4); return v;
    }
    bool flag_bit(uint64_t i, int bit) const {
        const uint8_t* p = record(i);
        if (!p || off_flags_ == kNone) return false;
        return (p[off_flags_] >> bit) & 1;
    }
    bool class_flag(uint64_t i, int bit) const {
        const uint8_t* p = record(i);
        if (!p) return false;
        // Format 0–5 har flaggorna i de höga bitarna av klassificeringsbyten;
        // 6–10 har en egen flaggbyte. Det är därför klasser över 31 förvanskas
        // i de gamla formaten, och skälet till att skrivaren använder format 6.
        if (legacy_) {
            if (off_class_ == kNone) return false;
            return (p[off_class_] >> (5 + bit)) & 1;
        }
        if (off_flags_ == kNone) return false;
        return (p[off_flags_] >> bit) & 1;
    }

    const uint8_t* base_ = nullptr;
    uint64_t count_ = 0;
    uint16_t stride_ = 0;
    uint8_t  format_ = 0;
    bool     legacy_ = false;

    uint16_t off_intensity_ = kNone, off_returns_ = kNone, off_flags_ = kNone;
    uint16_t off_class_ = kNone, off_scan_ang_ = kNone, off_user_data_ = kNone;
    uint16_t off_psid_ = kNone, off_gps_ = kNone, off_rgb_ = kNone, off_nir_ = kNone;

    double sx_ = 1.0, sy_ = 1.0, sz_ = 1.0;
    double ox_ = 0.0, oy_ = 0.0, oz_ = 0.0;
};

enum class DemFill : uint8_t {
    None = 0,
    Nearest,
    Idw,
    Tin,
};

struct DemResult {
    double origin_x, origin_y;
    double cell_size;
    uint32_t cols, rows;
    std::vector<float> elevation; // row-major [row * cols + col], NaN = no data
};

// Om LÄSNINGEN ska använda en `.hlz`-cache bredvid källfilen.
//
// ⛔ **Det här enumet SKAPAR ingenting.** Alla tre lägena styr bara om en
// befintlig cache får användas. Att bygga den är `BuildSidecar()`, och det är
// två skilda halvor:
//
//     BuildSidecar(path);                     // skapar — en gång, uttryckligen
//     r.open(path, HLas::Cache::Fast);        // använder den om den finns
//
// Sedan 1.1.4 gäller cachen BÅDE `.las` och `.laz`. Ändelsen läggs till:
// `foo.las.hlz`, `foo.laz.hlz`.
//
// Före 1.1.3 skrev `open()` en cache som bieffekt, bredvid användarens data, och
// misslyckades tyst på en skrivskyddad katalog. Öppnar du med `Fast` utan att ha
// byggt något finns ingen cache att ta, och källan packas upp som vanligt.
//
// ⛔ **En cache bär bara X, Y, Z och klassificering.** RGB, GPS-tid, intensitet
// och returfälten finns inte i den. Behöver du dem, öppna med `Cache::Off`.
//
// ⛔ **Punkterna ligger inte i källans ordning.** Cachen är cellsorterad — det är
// det som gör den liten och snabb — så index `i` i en cache-backad läsare är en
// annan punkt än index `i` i filen. Cachen lovar ingen indexmotsvarighet.
//
// Det gäller ÄVEN en `.las`, vars källa vore mmap:ad ändå. Ett utkast lät den
// behålla båda vägarna, men två ordningar bakom ett index ger tyst fel data —
// se doc/plans/sidecar-las.md.
// Gränsen går vid RÅA POSTER, inte vid index: `points()`, `point_data()` och
// `read_points()` svarar tomt och sätter `last_error()`, eftersom cachen inte
// innehåller några poster. Index-tagande API:er fungerar — ett index är då en
// position i DEN läsaren, och cachen är en annan ordning av samma punktmängd.
enum class Cache : uint8_t {
    // Använd cachen om den finns och hör till filen. Saknas den läses källan.
    Fast = 0,
    // Som Fast, men rör inte cachen för en COPC-fil — den har redan ett octree,
    // och `CopcReader` når det. Bygger du ändå en cache för en COPC-fil kommer
    // Smart att ignorera den; välj Fast om du vill att den ska användas.
    Smart,
    // Rör aldrig en cache.
    Off,
};

// SKAPAR en `.hlz` bredvid `path`, som ska vara en `.las` eller `.laz`. Det här
// är den enda vägen till en cache — läsning skapar aldrig någon.
//
// Bygger för vilken `.las` eller `.laz` som helst, COPC inräknad. Vill du inte
// cacha dina COPC-filer: låt bli att anropa den för dem.
//
// Skrivs atomiskt: temporärfil plus rename, så en avbruten körning aldrig lämnar
// en halv cache som ser hel ut. Finns en cache redan ersätts den.
//
// Kostar en full uppackning av källan plus en spatial sortering — sekunder till
// tiotals sekunder. Det är hela poängen med att den inte längre sker som en
// bieffekt av att läsa.
Status BuildSidecar(const char* path);

// Sökvägen till cachen: källans namn plus `.hlz`. Tom sträng för filändelser
// som varken är `.las` eller `.laz`. Skiftlägesokänsligt.
std::string SidecarPathFor(const char* laz_path);

class Reader {
public:
    Reader();
    ~Reader();
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;

    // ⚠️ FÖRVALET ÄR ÄNDRAT i 1.1.3. Tidigare tog `open(path)` en cache i bruk
    // och skapade en om den saknades; nu rör den ingen. Skälet är att en
    // cache-backad läsare inte kan svara på allt — den bär bara X, Y, Z och
    // klass — och att ett förstörande förval är fel väg när hela poängen med
    // läsningen är att bära hela punkten.
    Status open(const char* path, Cache cache = Cache::Off);
    void close();
    bool is_open() const;

    // Message for the most recent failure, empty when the last call succeeded.
    // Set by open() and by every point-data function.
    const std::string& last_error() const;

    const Header& header() const;

    uint32_t vlr_count() const;
    VlrInfo vlr(uint32_t index) const;

    // Vy över HELA filens punkter, med alla fält källan bär.
    //
    // Det här är den fullständiga läsvägen: RGB, intensitet, GPS-tid, returer
    // och allt annat finns kvar, och anroparen nyttjar det den vill.
    // get_xyz*-funktionerna nedan är bekvämlighet ovanpå samma data — de
    // plockar ut koordinater och klass åt den som bara vill ha geometri.
    //
    // Vyn gäller så länge den Reader som just nu äger bufferten håller samma fil
    // öppen. `close()`, ett nytt `open()` eller destruktion gör den ogiltig, och
    // en kopia av vyn överlever inte heller något av det. En flyttkonstruktion
    // flyttar ägandet med sig och rör alltså inte vyn; en flyttilldelning gör
    // MÅLETS gamla vyer ogiltiga.
    //
    // ⛔ Med en cache i bruk finns inga råa poster att lämna ut — cachen bär
    // fyra fält i bitpackad form, inte LAS-poster. `points()` svarar då TOMT och
    // sätter `last_error()`. Behöver du posterna: öppna med `Cache::Off`.
    PointView points() const;

    // Råa poster. ⛔ Otillgängliga med en cache i bruk, av skälet ovan: de
    // svarar tomt och sätter `last_error()` i stället för att hitta på en post.
    const uint8_t* point_data(uint64_t index) const;
    uint64_t read_points(uint64_t start, uint64_t count, uint8_t* dest) const;

    // Scaled coordinate access
    void get_xyz(uint64_t index, double& x, double& y, double& z) const;
    uint64_t get_xyz_batch(uint64_t start, uint64_t count,
                           double* x, double* y, double* z) const;

    // Classification
    uint8_t get_classification(uint64_t index) const;
    uint64_t get_xyz_filtered(uint64_t start, uint64_t count,
                              uint8_t classification,
                              double* x, double* y, double* z) const;

    // Spatial filtering — 2D polygon (XY plane)
    // classification = -1 means all classes
    uint64_t get_xyz_polygon(uint64_t start, uint64_t count,
                             const double* poly_x, const double* poly_y,
                             uint32_t num_vertices,
                             double* out_x, double* out_y, double* out_z,
                             int classification = -1,
                             // Se get_xyzc_polygon nedan.
                             const uint8_t* stride = nullptr) const;

    uint64_t get_xyzc_polygon(uint64_t start, uint64_t count,
                               const double* poly_x, const double* poly_y,
                               uint32_t num_vertices,
                               double* out_x, double* out_y, double* out_z,
                               uint8_t* out_classification,
                               int classification = -1,
                               const bool* accepted = nullptr,
                               // GALLRING per klass: `stride[c]` betyder "ta var
                               // stride[c]:te punkt av klass c". 0 och 1 är full
                               // täthet. nullptr = ingen gallring.
                               //
                               // ⛔ Urvalet styrs av punktens plats i CACHENS
                               // CELL, inte av en räknare. Därför ger varje cell
                               // samma punkter varje gång — stride 10 ser
                               // likadant ut utzoomad som inzoomad, och en vy som
                               // panorerar flimrar inte.
                               //
                               // Utan cache går gallringen på det globala
                               // indexet i stället: samma andel, men urvalet
                               // följer filens ordning och blir inte spatialt
                               // jämnt.
                               const uint8_t* stride = nullptr) const;

    // Histogram — count points per classification (0-255)
    Status get_classification_histogram(uint64_t counts[256]) const;
    Status get_classification_histogram_polygon(uint64_t counts[256],
                                                const double* poly_x,
                                                const double* poly_y,
                                                uint32_t num_verts) const;

    // DEM generation
    // accepted[256] = which classifications to include (nullptr = all)
    DemResult generate_dem(const double* poly_x, const double* poly_y,
                           uint32_t num_vertices,
                           double cell_size,
                           DemFill fill = DemFill::None,
                           const bool* accepted = nullptr,
                           int thread_count = 1) const;

private:
    Status open_internal(const char* path, Cache cache);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Partiell läsning ur en COPC-fil (Cloud Optimized Point Cloud).
//
// Skillnaden mot Reader: Reader packar upp hela filen vid open() — 10 sekunder
// för 10,6 miljoner punkter. CopcReader läser octree-indexet, väljer bara de
// noder som berör det efterfrågade området och packar upp dem. För en fråga som
// träffar 7 % av filen är det ungefär tio gånger snabbare, och över nätverk är
// skillnaden större eftersom resten aldrig behöver hämtas.
//
// Filen kan ligga lokalt eller bakom en URL. Samma kod, samma resultat —
// `open_url()` kräver att biblioteket byggts med HLAS_ENABLE_NET.
//
// Typisk användning:
//
//     HLas::CopcReader r;
//     if (r.open("moln.copc.laz") != HLas::Status::Ok) { ... r.last_error() ... }
//
//     auto est = r.estimate(x0, y0, x1, y1);
//     std::vector<uint8_t> recs(est.point_count * r.header().point_record_length);
//     uint64_t n = r.read_region(x0, y0, x1, y1, recs.data(), est.point_count);
//
//     auto v = r.view(recs.data(), n);
//     for (uint64_t i = 0; i < v.count(); ++i) {
//         double x = v.x(i), y = v.y(i), z = v.z(i);
//         uint16_t red, green, blue;
//         if (v.rgb(i, red, green, blue)) { ... }
//     }
//
// Punkterna kommer ut som RÅA POSTER med alla fält källan bär, och `view()`
// tolkar dem. Vill du bara ha geometri kostar det inget: du läser bara x/y/z.
//
// estimate() ger en ÖVRE gräns: den räknar alla punkter i de noder som berörs,
// inklusive de som ligger utanför rektangeln. En buffert av den storleken räcker
// alltid; `n` säger hur många poster som faktiskt hamnade i den. För en korridor
// kan gränsen vara tiotals gånger för stor — använd de strömmande varianterna
// där, så materialiseras aldrig mer än en nod.
//
// Hur nätverkslagret ska bete sig när `CopcReader::open_url()` hämtar.
//
// **Biblioteket kan inte veta hur nätet ser ut där det körs.** Det avgörs av
// företagsproxyer, TLS-inspektion och var CA-lagret ligger på just den maskinen
// — sådant bara anroparen har underlag för. Före 1.1.2 var hela uppsättningen
// hårdkodad, och en Windows-jobbdator där libcurls inkompilerade CA-sökväg inte
// fanns fick `CURLE_SSL_CACERT_BADFILE` redan på första förfrågan. Det är inte
// ett certifikatfel utan ett "CA-lagret gick inte att läsa in alls", och det
// gick inte att göra något åt utifrån.
//
// Strukturen är RENA VÄRDEN och inte ett HTTP-handtag. Den publika headern
// känner medvetet inte till libcurl: beroendet finns bara i `src/net/`, och hela
// den modulen kompileras bort utan `HLAS_ENABLE_NET`. Ett handtag i det här
// API:t hade rivit den gränsen.
//
// **Ett förvalt objekt ändrar ingenting.** Fälten nedan är valda så att de
// återger exakt det beteende biblioteket hade innan de fanns, så den som inte
// bryr sig behöver inte veta att de finns.
//
// Strukturen och `set_network_settings()` finns ÄVEN i ett bygge utan
// `HLAS_ENABLE_NET`, och inställningarna lagras då som vanligt. Annars hade
// ett program inte gått att kompilera mot ett bibliotek utan nätstöd, och det vore
// ett byggfel för något som ska vara ett körningsfel. Beskedet kommer i stället
// från `open_url()`, som redan säger exakt vad som saknas.
struct NetworkSettings {
    // ⛔ `None` och `System` är MOTSATSER, inte varianter av samma sak.
    //
    // `None` sätter proxyn till tom sträng, vilket stänger av den OCH gör att
    // `http_proxy`/`https_proxy` i miljön ignoreras. `System` sätter ingenting
    // alls, och det är just frånvaron som får HTTP-lagret att läsa dem. Den som
    // läser "ingen proxy" som "hoppa över anropet" får alltså raka motsatsen.
    enum class ProxyMode : uint8_t {
        None   = 0,   // ingen proxy, och miljövariablerna ignoreras
        System = 1,   // låt HTTP-lagret läsa miljön (förval)
        Manual = 2,   // använd proxy_url
    };

    enum class HttpVersion : uint8_t {
        Default = 0,  // låt HTTP-lagret välja (förval)
        Http1_1 = 1,
        Http2   = 2,
    };

    ProxyMode   proxy_mode = ProxyMode::System;
    std::string proxy_url;
    std::string proxy_user;
    std::string proxy_password;

    // Verifiering av serverns certifikat. Namnkontrollen FÖLJER den här: båda
    // på eller båda av. Att verifiera namnet men inte kedjan låter användbart,
    // men på ett nät med TLS-inspektion är det just namnkontrollen som fäller
    // anslutningen — "stäng av verifieringen" hade då inte gjort det anroparen
    // bad om.
    bool        verify_peer = true;

    // Sökväg till en CA-bundle. ⛔ Tom betyder "sätt ingenting", inte "leta
    // rätt på en" — biblioteket hittar inte på sökvägar.
    std::string ca_file;

    // Använd plattformens egen certifikatlagring. Biblioteket avgör INTE om
    // plattformen stödjer det; den som sätter flaggan har redan avgjort det.
    bool        use_native_ca = false;

    HttpVersion http_version = HttpVersion::Default;

    // Be om komprimerad överföring. Accept-Encoding komprimerar SVARSKROPPEN,
    // och punktdata är redan komprimerad — vinsten ligger alltså i små svar,
    // inte i punkthämtningen.
    bool        compression = false;

    // Skalar bibliotekets egna timeouts, i procent. 100 lämnar dem orörda.
    // Gäller BÅDE uppkopplingen och varje enskild läsning — det är den senare
    // som failar först på en långsam förbindelse. Värden <= 0 tolkas som 100.
    int         timeout_scale = 100;

    // Spårningsutskrift till stderr. Rubriker och förlopp, ALDRIG kroppen.
    // Rader som nämner authorization, cookie eller proxy-user maskas.
    //
    // ⚠️ Maskningen är en namnlista, inte en garanti: HTTP-lagrets egen
    // spårningstext är fritext och kan formulera sig på sätt listan inte täcker.
    // Avsett för felsökning, inte för en logg som sparas.
    bool        verbose = false;

    // Tom betyder att HTTP-lagrets eget förval används.
    std::string user_agent;
};

// Trådsäkerhet: en instans per tråd, precis som Reader.
class CopcReader {
public:
    CopcReader();
    ~CopcReader();
    CopcReader(CopcReader&&) noexcept;
    CopcReader& operator=(CopcReader&&) noexcept;

    // Lokal fil. Returnerar CorruptFile om filen inte är en konform COPC-fil —
    // den går då fortfarande att läsa med Reader, bara inte partiellt.
    Status open(const char* path);

    // Fil bakom http:// eller https://. Servern måste stödja range requests;
    // utan det skulle varje läsning hämta hela filen. Returnerar
    // LazNotSupported om biblioteket byggts utan HLAS_ENABLE_NET.
    //
    // `username`/`password` för servrar med HTTP Basic, som många
    // nedladdningsservrar kräver. Uppgifter skickas bara över https, och
    // följer inte med till en annan värd vid omdirigering.
    //
    // **URL:en måste vara betrodd.** Biblioteket begränsar till http och https,
    // även genom omdirigeringar, och följer högst fem hopp. Men det kan inte
    // avgöra vilka *värdar* som är känsliga i ditt nät — den bedömningen har
    // bara du underlag för.
    //
    // Kommer URL:en från en användare, en konfigurationsfil eller ett API-svar
    // är det din uppgift att avvisa den innan du skickar hit den. Annars kan
    // biblioteket förmås att hämta från adresser som bara är nåbara inifrån:
    // molnleverantörers metadatatjänst på 169.254.169.254, loopback, eller
    // interna adressintervall som 10/8, 172.16/12 och 192.168/16. Svaret
    // hamnar visserligen inte hos angriparen — vi tolkar det som punktdata och
    // felar — men förfrågan i sig kan räcka för att läcka eller utlösa något.
    //
    // Ett rimligt skydd är en lista över tillåtna värdar, och för allt annat en
    // kontroll av den *upplösta* adressen — inte bara värdnamnet, eftersom ett
    // namn kan peka var som helst och dessutom bytas mellan kontroll och
    // hämtning.
    Status open_url(const char* url,
                    const char* username = nullptr,
                    const char* password = nullptr);

    // Inställningarna för nätverkslagret. Måste sättas FÖRE open_url() — de
    // används när uppkopplingen görs, och en ändring efteråt rör inte en redan
    // öppnad läsare.
    //
    // De följer med till varje uppkoppling läsaren gör, inte bara den första.
    // Det är inte en detalj: med thread_count > 1 kopplar varje tråd upp på
    // nytt, och inställningar som bara gällde den första hade gett ett fel mitt
    // under en hämtning i stället för vid open_url().
    void set_network_settings(const NetworkSettings& settings);
    const NetworkSettings& network_settings() const;

    void close();
    bool is_open() const;
    const std::string& last_error() const;

    const Header& header() const;

    // Octreets form
    uint32_t node_count() const;
    int      max_level() const;

    // True när octreet inte är byggt över COPC-infons kub utan över filens
    // utbredning per axel, och läsaren därför räknar nodernas läge från den.
    // Specen kräver kuben, men sammanfogade filer med rektangulär utbredning
    // finns som inte gör så. Avgörs vid öppningen med
    // seed-punkterna i de djupaste noderna — bara bevisat, aldrig gissat. Ingen
    // felsignal: filen läses rätt, men det är värt en rad i anroparens logg.
    bool     root_from_extent() const;

    // Hur många samtidiga hämtningar som får pågå mot källan.
    //
    // Skilt från `thread_count`: trådarna är din processor, uppkopplingarna är
    // någon annans server. Åtta trådar med taket 4 packar upp på åtta kärnor men
    // håller aldrig mer än fyra uppkopplingar — en tråd lånar en uppkoppling för
    // hämtningen och lämnar tillbaka den innan uppackningen.
    //
    // Standard är 4. Mot en server i Sverige planade genomströmningen ut vid
    // fyra; mer ger ingen vinst men mer last. För en lokal fil spelar det ingen
    // roll — där är "uppkopplingen" en minnesmappning.
    //
    // Måste sättas före open(); ändringar efteråt gäller nästa öppning.
    void set_max_connections(int n);
    int  max_connections() const;

    struct QueryEstimate {
        uint32_t node_count  = 0;   // noder som måste hämtas
        uint64_t point_count = 0;   // övre gräns för antalet punkter
        uint64_t byte_size   = 0;   // hur mycket som måste läsas eller hämtas
    };

    // Vad en fråga skulle kosta, utan att hämta något.
    QueryEstimate estimate(double min_x, double min_y,
                           double max_x, double max_y,
                           int max_level = -1) const;

    // Samma för en polygon. Urvalet testar nodens kub mot polygonen, inte mot
    // dess omslutande rektangel — skillnaden är avgörande för korridorer, där
    // rektangeln kan vara hundra gånger större än polygonen själv.
    QueryEstimate estimate_polygon(const double* poly_x, const double* poly_y,
                                   uint32_t num_vertices,
                                   int max_level = -1) const;

    // Anropas en gång per nod med de punkter som klarade filtren. Returnera
    // false för att avbryta läsningen — då returnerar read_*_streaming antalet
    // punkter som hunnit levereras.
    //
    // Vyn bär HELA posten, inte ett urval av fält: RGB, intensitet, GPS-tid och
    // allt annat källan har finns kvar.
    //
    // Den gäller **bara till slutet av det här anropet**, och att kopiera vyn
    // förlänger inte det: bufferten bakom den återanvänds för nästa nod. Vill du
    // spara något, kopiera bytena innan du returnerar.
    //
    // Funktionspekare och `user` i stället för std::function: den publika
    // headern hålls fri från templates, och anropet går att brygga till C.
    using PointSink = bool (*)(void* user, const PointView& points);

    // Strömmande varianter. Punkterna levereras nod för nod och inget större
    // än en nod materialiseras.
    //
    // Det här är det API man vill ha för långa stråk: `estimate()` räknar alla
    // punkter i berörda noder, vilket för en korridor kan vara tiotals gånger
    // fler än de som faktiskt hamnar innanför. Att allokera för den gränsen är
    // ofta gigabyte i onödan.
    // `thread_count > 1` hämtar och packar upp flera noder samtidigt. Varje
    // tråd får en egen uppkoppling respektive filmappning; går källan inte att
    // klona körs det seriellt i stället för att fela.
    //
    // Callbacken anropas aldrig samtidigt från två trådar, men **ordningen
    // mellan noder är då obestämd**. Behöver du bestämd ordning, låt bli.
    //
    // Uppmätt på en fråga som träffar 20 noder: lokalt 1764 -> 284 ms med
    // 8 trådar (6,2x), över en server i Sverige 3226 -> 1169 ms med 4 (2,8x)
    // och sedan platå — det är per-uppkoppling-taket, inte kärnorna.
    uint64_t read_region_streaming(double min_x, double min_y,
                                   double max_x, double max_y,
                                   PointSink sink, void* user,
                                   const bool* accepted = nullptr,
                                   int max_level = -1,
                                   int thread_count = 1) const;

    uint64_t read_polygon_streaming(const double* poly_x, const double* poly_y,
                                    uint32_t num_vertices,
                                    PointSink sink, void* user,
                                    const bool* accepted = nullptr,
                                    int max_level = -1,
                                    int thread_count = 1) const;

    // DEM direkt ur COPC-filen. Punkterna binnas nod för nod, så inte heller
    // här materialiseras mer än en nod i taget — ett stråk över flera kilometer
    // går att göra terrängmodell av utan att hålla punkterna i minnet.
    //
    // Samma fyllnadsmetoder som Reader::generate_dem().
    DemResult generate_dem(const double* poly_x, const double* poly_y,
                           uint32_t num_vertices,
                           double cell_size,
                           DemFill fill = DemFill::None,
                           const bool* accepted = nullptr,
                           int max_level = -1,
                           int thread_count = 1) const;

    // Punkter inom en rektangel, som RÅA POSTER.
    //
    // Anroparen allokerar `max_points * header().point_record_length` byte och
    // `max_points` är taket. Returnerar antalet poster som skrevs; slå `view()`
    // om dem för att komma åt fälten.
    //
    // Att posterna lämnas ut hela och inte som fyra arrayer är hela poängen:
    // uppackningen ger ändå hela posten, och det som plockas ut ur den är
    // borta för gott. Anroparen nyttjar det den vill.
    //
    // `accepted[256]`: vilka klassificeringar som tas med, nullptr = alla.
    // Samma mening som i Reader::get_xyzc_polygon().
    //
    // max_level begränsar detaljnivån: -1 är alla nivåer, 0 bara rotnoden.
    // OBS att ett nivåtak ger ett GLESARE moln, inte ett komplett moln av färre
    // noder — COPC-noder är inte redundanta, djupare noder innehåller
    // ytterligare punkter snarare än kopior.
    uint64_t read_region(double min_x, double min_y,
                         double max_x, double max_y,
                         uint8_t* out_records,
                         uint64_t max_points,
                         const bool* accepted = nullptr,
                         int max_level = -1,
                         int thread_count = 1) const;

    // Punkter inom en polygon. Noderna väljs mot polygonen och punkterna
    // filtreras sedan exakt, samma ray casting som Reader använder.
    // Dimensionera bufferten med estimate_polygon(), inte estimate().
    uint64_t read_polygon(const double* poly_x, const double* poly_y,
                          uint32_t num_vertices,
                          uint8_t* out_records,
                          uint64_t max_points,
                          const bool* accepted = nullptr,
                          int max_level = -1,
                          int thread_count = 1) const;

    // Vy över poster som read_region() eller read_polygon() just fyllt.
    // `records` måste peka på den buffert anropet skrev i, `count` vara dess
    // returvärde. Vyn gäller så länge bufferten gör det.
    PointView view(const uint8_t* records, uint64_t count) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Skriver punkter till en okomprimerad LAS-fil.
//
// Biblioteket är i övrigt läsare-bara. Den här finns för ett konkret behov: att
// spara ut ett urval — en korridor, en polygon — som en fil andra verktyg kan
// läsa. Vill man ha filen komprimerad finns `LazWriter` med samma API. Att
// skriva COPC kräver dessutom att octreet byggs, vilket inte finns; se TODO.md.
//
// I LITE är formatet LAS 1.4 punktformat 6, 30 byte per punkt. Inte 1.2
// format 0, som vore mindre: där delar klassificeringen byte med flaggbitar, så
// klasser över 31 förvanskas. Verkliga filer använder dem — Autzen har 64, 65,
// 73, 76, 77. I FULL följer formatet källan, och filversionen följer formatet:
// LAS 1.2 för punktformat 0–3, LAS 1.4 för 6 och 7.
//
// Skrivningen är strömmande: write() får anropas hur många gånger som helst,
// och headern skrivs om vid close() med verkligt antal och verklig utbredning.
// Ett korridorurval på tiotals miljoner punkter behöver alltså aldrig ligga i
// minnet samtidigt.
// Vad en skrivare bevarar.
//
// Läget måste anges uttryckligen, och det är en följd av att LÄSNINGEN bara har
// en utdatatyp: när allt kommer ut som en PointView finns ingen andra
// write-funktion att bära läget implicit med.
enum class WriteMode : uint8_t {
    // X, Y, Z och klassificering. Punktformat 6, 30 byte per post, sex av nio
    // attributlager tomma. Allt annat källan bär kastas — avsiktligt.
    //
    // Det är inte en nödlösning utan ett leveransformat: ska urvalet bli en DEM
    // är RGB och GPS-tid dödvikt hela vägen. Mätt på en nationell laserruta är LITE
    // under halva FULL.
    Lite = 0,

    // Behåll det källan har. Utdataformatet FÖLJER källan, så en format
    // 7-källa ger en format 7-fil med RGB, GPS-tid, intensitet och allt annat
    // kvar, bit för bit.
    //
    // Kräver create_like(): posterna är heltal relativt källans skala och
    // offset, så en annan kvantisering skulle ge tyst fel geometri.
    //
    // Stöder källformat 0–3, 6 och 7. Format 4 och 5 kräver Wavepacket13 och
    // 8–10 kräver RGBNIR14 eller Wavepacket14 — ingen av dem finns som kodare,
    // och det finns ingen testfil att verifiera mot. De avvisas därför med
    // UnsupportedPointFormat i stället för att tappa fält: en översättning
    // mellan formaten vore en KONVERTERING och inte ett bevarande, och den
    // frågan hör inte hemma bakom ett läge som heter FULL.
    Full,
};

class LasWriter {
public:
    LasWriter();
    ~LasWriter();
    LasWriter(LasWriter&&) noexcept;
    LasWriter& operator=(LasWriter&&) noexcept;

    struct Options {
        // Skala avgör upplösningen. 0.01 ger centimeter, vilket är vad svensk
        // laserdata och de flesta andra använder.
        double x_scale = 0.01, y_scale = 0.01, z_scale = 0.01;

        // Offset flyttar origo så koordinaterna ryms i int32. Med auto_offset
        // härleds den ur första punkten, avrundad nedåt till jämnt tusental —
        // det fungerar för alla vanliga koordinatsystem utan att anroparen
        // behöver tänka på saken.
        bool   auto_offset = true;
        double x_offset = 0.0, y_offset = 0.0, z_offset = 0.0;

        // Läget låses vid create(). FULL går bara att sätta via create_like():
        // utan en källa finns inget format och ingen kvantisering att ärva, och
        // läget skulle tyst falla tillbaka på punktformat 6 — precis den
        // reduktion det finns för att förhindra. create() med mode == Full
        // returnerar därför InvalidHeader.
        WriteMode mode = WriteMode::Lite;
    };

    // Skapar filen. Befintlig fil skrivs över.
    // (Två överlagringar i stället för ett standardargument: Options är nästlad,
    // och dess standardinitierare är inte färdiga förrän klassen är komplett.)
    Status create(const char* path);
    Status create(const char* path, const Options& opt);

    // Ärver skala och offset från en källfil, så koordinaterna kvantiseras
    // exakt likadant som i originalet och inga avrundningsfel införs.
    //
    // I FULL ärvs även punktformatet, och det är hela poängen: posterna skrivs
    // igenom oförändrade. Källformat 0–3, 6 och 7 stöds; andra ger
    // UnsupportedPointFormat, liksom extra bytes. Filversionen följer formatet:
    // LAS 1.2 för 0–3, LAS 1.4 för 6 och 7. `global_encoding` och
    // `file_source_id` ärvs från källan — det förra för att bit 0 bär GPS-tidens
    // typ, och en nolla där gör tiderna feltolkade av den som läser.
    Status create_like(const char* path, const Header& source,
                       WriteMode mode = WriteMode::Lite);

    // Skriver `count` punkter. `classification` får vara nullptr (blir 0).
    // Får anropas flera gånger. Alltid LITE — fyra doubles kan inte bära mer.
    Status write(const double* x, const double* y, const double* z,
                 const uint8_t* classification, uint64_t count);

    // Skriver punkterna ur en vy, i det läge skrivaren skapades med.
    //
    // I FULL måste vyns punktformat och postlängd stämma med det create_like()
    // ärvde; annars InvalidPointRecordLength. I LITE tas X, Y, Z och klass ur
    // vyn och resten lämnas, oavsett vad den bär.
    Status write(const PointView& points);

    // Skriver om headern med verkligt antal och utbredning, och stänger filen.
    // MÅSTE anropas — utan den är filens header ofullständig.
    Status close();

    uint64_t point_count() const;
    const std::string& last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Skriver punkter till en komprimerad LAZ-fil.
//
// Samma API som LasWriter, samma format och samma semantik för utbredningen —
// den beskriver de skrivna punkterna, inte källan. Skillnaden är bara att
// posterna komprimeras.
//
// Kompressionen är HLas egen — samma aritmetiska kodare och samma Point14-modeller
// som läsaren, spegelvända. Den följer `HLAS_ENABLE_LAZ` precis som läsningen;
// byggs biblioteket utan den returnerar `create()` `Status::LazNotSupported` i
// stället för att API:t försvinner.
//
// I LITE är filen LAS 1.4 punktformat 6, LayerChunked, 50000 punkter per chunk.
// Sex av formatets nio attributlager skrivs med längd 0, eftersom en skriven
// punkt bara bär X, Y, Z och klassificering — resten är konstant noll och
// behöver inte kodas. Det är formatets egen mekanism, inte en genväg.
//
// I FULL följer chunkstrukturen källans format, och de två familjerna är olika
// filupplägg: punktformat 6–7 använder LayerChunked (compressor 3) med nio
// lager, medan 0–3 använder Chunked (compressor 2) — seed-posten rått följd av
// EN aritmetisk ström där alla items kodas efter varandra per punkt, utan
// lagerindelning och utan punktantal i chunken.
//
// **`close()` måste anropas och dess svar kontrolleras.** Den skriver
// chunk-tabellen och den slutgiltiga headern, och först då är filen komplett.
// Misslyckas den kan filen vara delvis skriven och ska kastas.
class LazWriter {
public:
    LazWriter();
    ~LazWriter();
    LazWriter(LazWriter&&) noexcept;
    LazWriter& operator=(LazWriter&&) noexcept;

    Status create(const char* path);
    Status create(const char* path, const LasWriter::Options& opt);

    // Ärver skala och offset från en källa, så kvantiseringen blir densamma.
    // **Inte** utbredningen — den räknas ur de punkter som faktiskt skrivs.
    // Ett korridorurval ur en 10x10 km-ruta ska ha korridorens utbredning, inte
    // rutans, annars blir varje DEM över filen gigantisk.
    //
    // I FULL ärvs även punktformatet. Se WriteMode.
    Status create_like(const char* path, const Header& source,
                       WriteMode mode = WriteMode::Lite);

    // Alltid LITE — fyra doubles kan inte bära mer.
    Status write(const double* x, const double* y, const double* z,
                 const uint8_t* classification, uint64_t count);

    // Skriver punkterna ur en vy, i det läge skrivaren skapades med.
    Status write(const PointView& points);

    Status close();

    uint64_t point_count() const;
    const std::string& last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace HLas
