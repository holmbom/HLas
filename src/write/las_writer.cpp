#include "HLas.h"
#include "point_encode.h"
#include "path_utf8.h"
#include <cmath>
#include <cstdio>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace HLas {

namespace {

// Kortar filen till exakt `len` byte.
//
// Behövs för att create() skriver en LAS 1.4-header om 375 byte innan formatet
// är känt, och create_like() med en format 0-3-källa sedan skriver om den till
// 227. Utan trunkering blir en fil med färre än fem punkter längre än sitt
// innehåll och bär rester av den första headern efter sista posten.
bool truncate_to(std::FILE* f, long long len) {
    if (std::fflush(f) != 0) return false;
#ifdef _WIN32
    return _chsize_s(_fileno(f), len) == 0;
#else
    return ftruncate(fileno(f), static_cast<off_t>(len)) == 0;
#endif
}

constexpr uint16_t kHeaderMaxSize = 375;   // LAS 1.4; 1.2 är 227
constexpr uint16_t kRecordLen  = detail::kWriteRecordLen;
constexpr uint8_t  kPointFormat = detail::kWritePointFormat;

template <typename T>
void put(uint8_t* p, T v) { std::memcpy(p, &v, sizeof(T)); }

} // namespace

struct LasWriter::Impl {
    ~Impl() {
        // Utan den här läcker en flyttilldelning över en öppen skrivare både
        // filhandtaget och den ofärdiga headern — unique_ptr river Impl, men
        // Impl ägde ingenting som stängde filen.
        if (f) { write_header(true); std::fclose(f); f = nullptr; }
    }

    std::FILE*  f = nullptr;
    std::string path;
    LasWriter::Options opt{};
    // Punktformatet filen skriver. LITE är alltid 6; FULL ärver källans.
    uint8_t     format = kPointFormat;
    uint16_t    rec_len = kRecordLen;
    uint64_t    by_return[15] = {};
    // Ärvs från källan i FULL, av samma skäl som i LazWriter.
    uint16_t    src_global_encoding = 0;
    uint16_t    src_file_source_id = 0;
    bool        offset_fixed = false;
    uint64_t    count = 0;
    double      min_x = 0, min_y = 0, min_z = 0;
    double      max_x = 0, max_y = 0, max_z = 0;
    std::string last_error;
    std::vector<uint8_t> buf;
    bool        failed = false;   // efter en trasig skrivning är filen inte att lita på

    bool write_header(bool final_pass);
    bool mode_full() const { return opt.mode == WriteMode::Full; }
};

bool LasWriter::Impl::write_header(bool final_pass) {
    // Efter ett skrivfel skrivs INGEN slutgiltig header. Filen kan då innehålla
    // ett prefix av posterna, och en header som påstår ett antal och en
    // utbredning hade fått den att se komplett ut. Utan omskrivningen står
    // antalet kvar på noll från create(), vilket är vad den är: ofärdig.
    if (final_pass && failed)
        return false;

    // Filversionen följer punktformatet: LAS 1.2 för 0-5, LAS 1.4 för 6 och
    // uppåt. Se las_header_size() i point_encode.h för varför.
    const bool legacy = detail::legacy_point_format(format);
    const uint16_t hdr_size = detail::las_header_size(format);

    // LAS 1.2 räknar punkter i uint32. Fler än så går inte att beskriva, och ett
    // tyst trunkerat antal vore värre än ett fel: filen hade sett komplett ut
    // och lästs som en bråkdel.
    if (final_pass && legacy && count > 0xFFFFFFFFull) {
        last_error = "too many points for a LAS 1.2 header";
        return false;
    }

    uint8_t h[kHeaderMaxSize];
    std::memset(h, 0, sizeof h);
    std::memcpy(h, "LASF", 4);
    put<uint16_t>(h + 4, src_file_source_id);
    // global_encoding bär GPS-tidens typ i bit 0. Skrivs den som noll tolkas
    // tiderna fel av den som läser, så den ärvs från källan i FULL. I LAS 1.2 är
    // bara bit 0 definierad och 1-15 reserverade, så resten maskas bort; bit 4
    // (WKT) krävs däremot av LAS 1.4 för punktformat 6 och uppåt.
    put<uint16_t>(h + 6, legacy ? static_cast<uint16_t>(src_global_encoding & 0x0001u)
                                : static_cast<uint16_t>(src_global_encoding | (1u << 4)));
    h[24] = 1; h[25] = legacy ? 2 : 4;
    std::snprintf(reinterpret_cast<char*>(h + 58), 32, "HLas LasWriter");
    put<uint16_t>(h + 94, hdr_size);
    put<uint32_t>(h + 96, hdr_size);                       // offset to point data
    put<uint32_t>(h + 100, 0);                             // inga VLR:er
    h[104] = format;
    put<uint16_t>(h + 105, rec_len);

    put<double>(h + 131, opt.x_scale);
    put<double>(h + 139, opt.y_scale);
    put<double>(h + 147, opt.z_scale);
    put<double>(h + 155, opt.x_offset);
    put<double>(h + 163, opt.y_offset);
    put<double>(h + 171, opt.z_offset);

    if (final_pass && count) {
        put<double>(h + 179, max_x); put<double>(h + 187, min_x);
        put<double>(h + 195, max_y); put<double>(h + 203, min_y);
        put<double>(h + 211, max_z); put<double>(h + 219, min_z);
    }

    if (legacy) {
        // LAS 1.2: antalet är uint32 vid 107 och det finns bara FEM returfack.
        // Returnummer 6 och 7 förekommer i punktformat 0-5 men har inget fack —
        // formatets begränsning, inte vår.
        put<uint32_t>(h + 107, final_pass ? static_cast<uint32_t>(count) : 0u);
        if (final_pass) {
            if (mode_full()) {
                for (int i = 0; i < 5; ++i)
                    put<uint32_t>(h + 111 + i * 4, static_cast<uint32_t>(by_return[i]));
            } else {
                put<uint32_t>(h + 111, static_cast<uint32_t>(count));
            }
        }
    } else {
        // Legacy-fälten (1.2-kompatibilitet) ska vara noll för format 6 och uppåt.
        put<uint32_t>(h + 107, 0);

        // 227 = start of waveform, 235 = start of first EVLR, 243 = antal EVLR:er.
        // Alla ska vara noll och är det redan via nollställningen ovan.
        put<uint64_t>(h + 247, final_pass ? count : 0);     // 1.4 point count
        if (final_pass) {
            // Tabellen ska summera till antalet. I LITE är varje punkt retur 1
            // av 1; i FULL kommer fördelningen ur källan och räknas medan
            // posterna skrivs.
            if (mode_full()) {
                for (int i = 0; i < 15; ++i)
                    put<uint64_t>(h + 255 + i * 8, by_return[i]);
            } else {
                put<uint64_t>(h + 255, count);
            }
        }
    }

    if (std::fseek(f, 0, SEEK_SET) != 0)
        return false;
    if (std::fwrite(h, 1, hdr_size, f) != hdr_size)
        return false;

    // Filen ska vara exakt header + poster. Se truncate_to() för varför den
    // annars kan vara längre. Görs bara i sista passet — dessförinnan skrivs
    // posterna fortfarande.
    if (final_pass) {
        const long long want = static_cast<long long>(hdr_size)
                             + static_cast<long long>(count) * rec_len;
        if (!truncate_to(f, want))
            return false;
    }
    return true;
}

LasWriter::LasWriter() : impl_(std::make_unique<Impl>()) {}
LasWriter::~LasWriter() { if (impl_ && impl_->f) close(); }
LasWriter::LasWriter(LasWriter&&) noexcept = default;
LasWriter& LasWriter::operator=(LasWriter&&) noexcept = default;

Status LasWriter::create(const char* path) { return create(path, Options{}); }

Status LasWriter::create(const char* path, const Options& opt) {
    if (!impl_) return Status::FileOpenFailed;
    if (impl_->f) close();
    if (!path || !*path) {
        impl_->last_error = "empty path";
        return Status::FileNotFound;
    }
    // NaN faller på jämförelsen, oändligheter måste fångas separat: ett
    // oändligt skalvärde gör varje koordinat till heltalsnoll och skriver
    // dessutom en ogiltig header.
    if (!(opt.x_scale > 0) || !(opt.y_scale > 0) || !(opt.z_scale > 0) ||
        !std::isfinite(opt.x_scale) || !std::isfinite(opt.y_scale) ||
        !std::isfinite(opt.z_scale)) {
        impl_->last_error = "scale must be positive and finite";
        return Status::InvalidHeader;
    }
    if (!opt.auto_offset &&
        (!std::isfinite(opt.x_offset) || !std::isfinite(opt.y_offset) ||
         !std::isfinite(opt.z_offset))) {
        impl_->last_error = "offset must be finite";
        return Status::InvalidHeader;
    }

#ifdef _WIN32
    const std::wstring wpath = detail::utf8_to_wide(path);
    if (wpath.empty()) {
        impl_->last_error = "invalid UTF-8 path";
        return Status::FileNotFound;
    }
    impl_->f = _wfopen(wpath.c_str(), L"wb");
#else
    impl_->f = std::fopen(path, "wb");
#endif
    if (!impl_->f) {
        impl_->last_error = "could not create file";
        return Status::FileOpenFailed;
    }
    if (opt.mode == WriteMode::Full) {
        // FULL kräver en källa att ärva format och kvantisering ur.
        std::fclose(impl_->f);
        impl_->f = nullptr;
        impl_->last_error = "FULL requires create_like()";
        return Status::InvalidHeader;
    }
    impl_->path = path;
    impl_->opt = opt;
    impl_->offset_fixed = !opt.auto_offset;
    impl_->count = 0;
    impl_->failed = false;
    impl_->last_error.clear();
    // MÅSTE nollställas: samma skrivarobjekt kan återanvändas efter close(),
    // och ett kvarlämnat format 7 hade fått headern att annonsera 36 byte per
    // post medan LITE skriver 30.
    impl_->format = kPointFormat;
    impl_->rec_len = kRecordLen;
    std::memset(impl_->by_return, 0, sizeof impl_->by_return);
    impl_->src_global_encoding = 0;
    impl_->src_file_source_id = 0;
    if (!impl_->write_header(false)) {
        impl_->last_error = "could not write header";
        std::fclose(impl_->f);
        impl_->f = nullptr;
        return Status::FileOpenFailed;
    }
    return Status::Ok;
}

Status LasWriter::create_like(const char* path, const Header& source,
                              WriteMode mode) {
    Options o;
    o.x_scale = source.x_scale;  o.y_scale = source.y_scale;  o.z_scale = source.z_scale;
    o.x_offset = source.x_offset; o.y_offset = source.y_offset; o.z_offset = source.z_offset;
    o.auto_offset = false;

    if (mode == WriteMode::Full) {
        // Samma gräns som LazWriter, och av samma skäl: FULL skriver posterna
        // oförändrade, och en post vi inte kan beskriva i headern kan vi inte
        // skriva. Att tyst falla tillbaka på format 6 vore att tappa fält bakom
        // ett läge som lovar motsatsen.
        if (!detail::writer_supported_format(source.point_format)) {
            if (impl_) impl_->last_error =
                "FULL supports source point format 0-3, 6 and 7";
            return Status::UnsupportedPointFormat;
        }
        if (source.point_record_length != detail::las_record_len(source.point_format)) {
            if (impl_) impl_->last_error = "FULL does not support extra bytes";
            return Status::UnsupportedPointFormat;
        }
    }

    const Status st = create(path, o);
    if (st == Status::Ok && impl_ && mode == WriteMode::Full) {
        impl_->opt.mode = WriteMode::Full;
        impl_->format   = source.point_format;
        impl_->rec_len  = source.point_record_length;
        impl_->src_global_encoding = source.global_encoding;
        impl_->src_file_source_id  = source.file_source_id;
        // Headern skrevs i create() med LITE-formatet. Skriv om den nu när
        // formatet är känt, annars pekar filen på fel postlängd tills close().
        if (!impl_->write_header(false)) {
            impl_->failed = true;
            return Status::FileOpenFailed;
        }
    }
    return st;
}

// Skriver posterna ur en vy. I FULL går de igenom OFÖRÄNDRADE.
Status LasWriter::write(const PointView& points) {
    if (!impl_ || !impl_->f) return Status::FileOpenFailed;
    auto& I = *impl_;
    if (I.failed) {
        I.last_error = "writer is in a failed state";
        return Status::FileOpenFailed;
    }
    if (points.count() == 0) return Status::Ok;
    if (!points.valid()) {
        I.last_error = "invalid point view";
        return Status::InvalidHeader;
    }

    if (!I.mode_full()) {
        // LITE ur en vy: plocka ut koordinater och klass och gå den beprövade
        // write()-vägen, i block så en stor vy inte kräver stora temporärer.
        constexpr uint64_t kBlock = 65536;
        std::vector<double>  bx, by, bz;
        std::vector<uint8_t> bc;
        try {
            bx.resize(kBlock); by.resize(kBlock); bz.resize(kBlock); bc.resize(kBlock);
        } catch (const std::bad_alloc&) {
            I.last_error = "out of memory";
            return Status::InvalidHeader;
        }
        for (uint64_t start = 0; start < points.count(); start += kBlock) {
            const uint64_t n = points.xyz(start, kBlock, bx.data(), by.data(), bz.data());
            if (n == 0) break;
            for (uint64_t i = 0; i < n; ++i)
                bc[static_cast<size_t>(i)] = points.classification(start + i);
            const Status st = write(bx.data(), by.data(), bz.data(), bc.data(), n);
            if (st != Status::Ok) return st;
        }
        return Status::Ok;
    }

    if (points.point_format() != I.format) {
        I.last_error = "view point format does not match the writer";
        return Status::UnsupportedPointFormat;
    }
    if (points.record_length() != I.rec_len) {
        I.last_error = "view record length does not match the writer";
        return Status::InvalidPointRecordLength;
    }
    if (points.x_scale() != I.opt.x_scale || points.y_scale() != I.opt.y_scale ||
        points.z_scale() != I.opt.z_scale || points.x_offset() != I.opt.x_offset ||
        points.y_offset() != I.opt.y_offset || points.z_offset() != I.opt.z_offset) {
        I.last_error = "view scale/offset does not match the writer";
        return Status::InvalidHeader;
    }
    if (points.count() > (std::numeric_limits<uint64_t>::max)() - I.count) {
        I.last_error = "too many points";
        return Status::InvalidHeader;
    }

    double bx0 = 0, by0 = 0, bz0 = 0, bx1 = 0, by1 = 0, bz1 = 0;
    for (uint64_t i = 0; i < points.count(); ++i) {
        const uint8_t* rec = points.record(i);
        if (!rec) break;
        if (std::fwrite(rec, 1, I.rec_len, I.f) != I.rec_len) {
            I.last_error = "write failed";
            I.failed = true;
            return Status::FileOpenFailed;
        }
        // Returnumret ligger olika i de två postlayouterna — tre bitar i
        // punktformat 0-5, fyra i 6 och uppåt — så det plockas ur vyn, som är
        // formatmedveten, i stället för ur en rå offset.
        const uint8_t rn = points.return_number(i);
        if (rn >= 1 && rn <= 15) ++I.by_return[rn - 1];

        const double sx = points.x(i), sy = points.y(i), sz = points.z(i);
        if (i == 0) { bx0 = bx1 = sx; by0 = by1 = sy; bz0 = bz1 = sz; }
        else {
            if (sx < bx0) bx0 = sx; else if (sx > bx1) bx1 = sx;
            if (sy < by0) by0 = sy; else if (sy > by1) by1 = sy;
            if (sz < bz0) bz0 = sz; else if (sz > bz1) bz1 = sz;
        }
    }

    if (I.count == 0) {
        I.min_x = bx0; I.max_x = bx1;
        I.min_y = by0; I.max_y = by1;
        I.min_z = bz0; I.max_z = bz1;
    } else {
        if (bx0 < I.min_x) I.min_x = bx0;  if (bx1 > I.max_x) I.max_x = bx1;
        if (by0 < I.min_y) I.min_y = by0;  if (by1 > I.max_y) I.max_y = by1;
        if (bz0 < I.min_z) I.min_z = bz0;  if (bz1 > I.max_z) I.max_z = bz1;
    }
    I.offset_fixed = true;
    I.count += points.count();
    return Status::Ok;
}

Status LasWriter::write(const double* x, const double* y, const double* z,
                        const uint8_t* classification, uint64_t count) {
    if (!impl_ || !impl_->f) return Status::FileOpenFailed;
    auto& I = *impl_;
    if (I.failed) {
        I.last_error = "writer is in a failed state";
        return Status::FileOpenFailed;
    }
    if (count == 0) return Status::Ok;
    if (I.mode_full()) {
        // Fyra doubles kan inte bära det FULL lovar. Att tyst skriva en
        // reducerad fil ur en skrivare som sagt FULL vore precis det läget
        // finns för att förhindra.
        I.last_error = "writer is in FULL mode; use write(const PointView&)";
        return Status::InvalidHeader;
    }
    if (!x || !y || !z) {
        I.last_error = "null coordinate array";
        return Status::InvalidHeader;
    }
    if (count > (std::numeric_limits<size_t>::max)() / kRecordLen) {
        I.last_error = "batch too large";
        return Status::InvalidHeader;
    }

    // Offset härleds ur första punkten i första lyckade skrivningen. Härleds
    // den ur en batch som sedan avvisas ligger den ändå fast, och nästa giltiga
    // batch kan då nekas i onödan — därför bara ett förslag tills vi vet.
    const bool derive_offset = !I.offset_fixed;
    double ox = I.opt.x_offset, oy = I.opt.y_offset, oz = I.opt.z_offset;
    if (derive_offset) {
        if (!std::isfinite(x[0]) || !std::isfinite(y[0]) || !std::isfinite(z[0])) {
            I.last_error = "non-finite coordinate";
            return Status::InvalidHeader;
        }
        ox = detail::floor_1000(x[0]);
        oy = detail::floor_1000(y[0]);
        oz = detail::floor_1000(z[0]);
    }

    I.buf.resize(static_cast<size_t>(count) * kRecordLen);
    uint8_t* p = I.buf.data();

    // Utbredningen räknas på de LAGRADE koordinaterna, alltså efter
    // kvantiseringen. LAS-headern beskriver filens punkter, inte de doubles
    // anroparen råkade skicka in — annars kan min/max ligga en halv skala fel.
    double bmin_x = 0, bmin_y = 0, bmin_z = 0, bmax_x = 0, bmax_y = 0, bmax_z = 0;

    detail::QuantOptions q{ I.opt.x_scale, I.opt.y_scale, I.opt.z_scale, ox, oy, oz };

    for (uint64_t i = 0; i < count; ++i, p += kRecordLen) {
        // Delad kodning med LazWriter. Samma punkter måste ge samma byte i
        // båda formaten — annars är en LAZ-fil inte samma data som en LAS-fil.
        double sx, sy, sz;
        if (!detail::encode_point6(q, x[i], y[i], z[i],
                                   classification ? classification[i] : 0,
                                   p, sx, sy, sz)) {
            I.last_error = "point does not fit the chosen scale/offset";
            return Status::InvalidHeader;
        }

        if (i == 0) {
            bmin_x = bmax_x = sx; bmin_y = bmax_y = sy; bmin_z = bmax_z = sz;
        } else {
            if (sx < bmin_x) bmin_x = sx;  if (sx > bmax_x) bmax_x = sx;
            if (sy < bmin_y) bmin_y = sy;  if (sy > bmax_y) bmax_y = sy;
            if (sz < bmin_z) bmin_z = sz;  if (sz > bmax_z) bmax_z = sz;
        }
    }

    if (std::fseek(I.f, 0, SEEK_END) != 0) {
        I.failed = true;
        I.last_error = "seek failed";
        return Status::FileOpenFailed;
    }
    const size_t wrote = std::fwrite(I.buf.data(), kRecordLen,
                                     static_cast<size_t>(count), I.f);
    if (wrote != count) {
        // Filen innehåller nu ett okänt antal poster. Att fortsätta skriva
        // skulle ge en header som ljuger om innehållet.
        I.failed = true;
        I.last_error = "short write";
        return Status::FileOpenFailed;
    }

    // Först här är batchen verklighet — nu, och inte tidigare, ändras tillståndet.
    if (derive_offset) {
        I.opt.x_offset = ox; I.opt.y_offset = oy; I.opt.z_offset = oz;
        I.offset_fixed = true;
    }
    if (I.count == 0) {
        I.min_x = bmin_x; I.max_x = bmax_x;
        I.min_y = bmin_y; I.max_y = bmax_y;
        I.min_z = bmin_z; I.max_z = bmax_z;
    } else {
        if (bmin_x < I.min_x) I.min_x = bmin_x;  if (bmax_x > I.max_x) I.max_x = bmax_x;
        if (bmin_y < I.min_y) I.min_y = bmin_y;  if (bmax_y > I.max_y) I.max_y = bmax_y;
        if (bmin_z < I.min_z) I.min_z = bmin_z;  if (bmax_z > I.max_z) I.max_z = bmax_z;
    }
    I.count += count;
    return Status::Ok;
}

Status LasWriter::close() {
    if (!impl_ || !impl_->f) return Status::Ok;
    const bool header_ok = impl_->write_header(true);
    const bool closed_ok = std::fclose(impl_->f) == 0;
    impl_->f = nullptr;
    if (!header_ok) {
        impl_->last_error = "could not rewrite header";
        return Status::FileOpenFailed;
    }
    if (!closed_ok) {
        impl_->last_error = "close failed";
        return Status::FileOpenFailed;
    }
    return impl_->failed ? Status::FileOpenFailed : Status::Ok;
}

uint64_t LasWriter::point_count() const { return impl_ ? impl_->count : 0; }

const std::string& LasWriter::last_error() const {
    static const std::string empty;
    return impl_ ? impl_->last_error : empty;
}

} // namespace HLas
