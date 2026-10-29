#include "HLas.h"
#include "point_encode.h"
#ifdef HLAS_HAVE_LAZ
#include "laz_header.h"
#include "laz_chunk_table.h"
#include "laz/point14_encoder.h"
#include "laz/rgb14_encoder.h"
#include "laz/chunk_layers.h"
#include "laz/point14_item.h"
#include "laz/point10_encoder.h"
#include "laz/point10_item.h"
#include "laz/gpstime11_encoder.h"
#include "laz/rgb12_encoder.h"
#endif
#include "path_utf8.h"
#include <cmath>
#include <new>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace HLas {

#ifdef HLAS_HAVE_LAZ

namespace {

// 64-bitars sökning. Naken std::fseek tar long, som är 32 bitar på Windows —
// en LAZ-fil kan mycket väl passera 2 GB, och då hade headerpatchningen på
// slutet skrivit fel ställe i stället för att fela.
bool seek64(std::FILE* f, int64_t off) {
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET) == 0;
#else
    // fseeko är bara 64-bitars om byggmiljön ger ett 64-bitars off_t. Är den
    // inte det tystnar problemet till en trunkerad sökning vid 2 GB — alltså
    // en headerpatchning som skriver på fel ställe.
    static_assert(sizeof(off_t) >= 8,
                  "64-bitars filoffset krävs; bygg med _FILE_OFFSET_BITS=64");
    return fseeko(f, static_cast<off_t>(off), SEEK_SET) == 0;
#endif
}

int64_t tell64(std::FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return static_cast<int64_t>(ftello(f));
#endif
}

} // namespace
#endif

struct LazWriter::Impl {
    ~Impl() {
#ifdef HLAS_HAVE_LAZ
        // Utan den här läcker en flyttilldelning över en öppen skrivare filen
        // med en oskriven header. Samma resonemang som i LasWriter.
        //
        // finalize() allokerar (chunk-tabellen), och ett undantag ur en
        // destruktor betyder std::terminate. Att riva processen för att en
        // avslutning misslyckades vore fel proportion — filen är förlorad,
        // programmet behöver inte vara det.
        if (f) {
            try { finalize(); }
            catch (...) { if (f) { std::fclose(f); f = nullptr; } failed = true; }
        }
#endif
    }

    LasWriter::Options    opt{};
    detail::QuantOptions  q{};
    // Punktformatet skrivaren producerar. LITE är alltid 6; FULL ärver källans.
    uint8_t     format = detail::kWritePointFormat;
    uint16_t    rec_len = detail::kWriteRecordLen;
    // points_by_return. I LITE är allt retur 1 av 1 och tabellen härleds ur
    // antalet; i FULL måste den räknas, eftersom fördelningen kommer ur källan.
    uint64_t    by_return[15] = {};
    // Ärvs från källan i FULL. global_encoding bär GPS-tidens typ i bit 0, och
    // skrivs den som noll tolkas tiderna fel av den som läser — det är punktdata
    // i allt utom namnet.
    uint16_t    src_global_encoding = 0;
    uint16_t    src_file_source_id = 0;

    bool mode_full() const { return opt.mode == WriteMode::Full; }
    bool        offset_fixed = false;
    bool        open_ = false;
    bool        failed = false;
    // Resultatet av close(). Ett andra close() ska inte kunna svara Ok efter
    // att det första misslyckats — den som stänger rutinmässigt en gång till
    // skulle då tro att filen blev bra.
    bool        closed = false;
    Status      close_status = Status::Ok;
    uint64_t    count = 0;
    std::string last_error;
    std::string path_;

#ifdef HLAS_HAVE_LAZ
    std::FILE*  f = nullptr;
    double      min_x = 0, min_y = 0, min_z = 0;
    double      max_x = 0, max_y = 0, max_z = 0;
    bool        have_bounds = false;

    // Punktformat 6-7: nio lager med var sin ström, plus RGB:s.
    detail::laz::Point14Encoder p14;
    detail::laz::RGB14Encoder   rgb;

    // Punktformat 0-3: EN gemensam ström som alla items skriver i, i den
    // ordning de står i LAZ-VLR:n. Kodarna äger den därför inte själva.
    detail::laz::ArithmeticEncoder enc2;
    detail::laz::Point10Encoder    p10;
    detail::laz::GPSTime11Encoder  gps11;
    detail::laz::RGB12Encoder      rgb12;

    bool legacy() const   { return detail::legacy_point_format(format); }
    bool has_gps() const  { return format == 1 || format == 3; }
    bool has_rgb12() const { return format == 2 || format == 3; }
    bool                  chunk_open = false;
    uint32_t              chunk_points = 0;
    std::vector<uint8_t>  chunk_bytes;    // seed + antal + lager, hela chunken
    std::vector<uint32_t> chunk_sizes;

    bool create_file();
    // Chunkarna arbetar på RÅA poster, inte på Point14Fields. LITE bygger sin
    // 30-bytespost ur fyra doubles och går sedan samma väg som FULL — en
    // kodningsväg, inte två.
    bool start_chunk(const uint8_t* rec);
    bool close_chunk();
    bool add_point(const uint8_t* rec);
    bool finalize();
#endif
};

#ifdef HLAS_HAVE_LAZ

bool LazWriter::Impl::create_file() {
#ifdef _WIN32
    const std::wstring wpath = detail::utf8_to_wide(path_);
    if (wpath.empty()) { last_error = "invalid UTF-8 path"; return false; }
    f = _wfopen(wpath.c_str(), L"wb");
#else
    f = std::fopen(path_.c_str(), "wb");
#endif
    if (!f) { last_error = "could not create file"; return false; }

    // Header och VLR skrivs som platshållare — antal och utbredning är inte
    // kända förrän close(). Chunk-tabellens position likaså.
    uint8_t hdr[detail::kLazHeaderMaxSize];
    detail::LazHeaderInfo info;
    info.q = q;
    info.format = format;
    info.global_encoding = src_global_encoding;
    info.file_source_id = src_file_source_id;
    detail::build_laz_header(info, hdr);
    const size_t hdr_len = detail::las_header_size(format);

    // Payloaden växer med antalet items, så båda buffertarna dimensioneras för
    // det största formatet och bara den använda delen skrivs. Punktformat 3 har
    // tre items, alltså 34 + 18 byte payload.
    uint8_t vlr[detail::kLazVlrHeaderSize + 52];
    detail::build_laz_vlr(vlr, format);
    const size_t vlr_len = detail::kLazVlrHeaderSize + detail::laz_vlr_payload(format);

    const int64_t placeholder = 0;
    if (std::fwrite(hdr, 1, hdr_len, f) != hdr_len ||
        std::fwrite(vlr, 1, vlr_len, f) != vlr_len ||
        std::fwrite(&placeholder, 1, 8, f) != 8) {
        last_error = "could not write header";
        std::fclose(f);
        f = nullptr;
        return false;
    }
    return true;
}

bool LazWriter::Impl::start_chunk(const uint8_t* rec) {
    chunk_bytes.clear();

    // Seed-posten ligger RÅ först i chunken i BÅDA upplägg. Skillnaden kommer
    // sedan: compressor 3 har ett punktantal och nio lagerlängder efter den,
    // compressor 2 har ingenting alls — punktantalet står i chunk-tabellen.
    chunk_bytes.insert(chunk_bytes.end(), rec, rec + rec_len);

    if (legacy()) {
        enc2.init();
        p10.init(enc2);
        p10.start_chunk(rec);
        uint16_t off = detail::laz::kPoint10Size;
        if (has_gps()) {
            gps11.init(enc2);
            gps11.start_chunk(rec + off);
            off += 8;
        }
        if (has_rgb12()) {
            rgb12.init(enc2);
            rgb12.start_chunk(rec + off);
        }
        if (p10.failed() || (has_gps() && gps11.failed()) ||
            (has_rgb12() && rgb12.failed())) {
            last_error = "point encoder failed";
            return false;
        }
    } else {
        chunk_bytes.insert(chunk_bytes.end(), 4, 0);   // platshållare för antalet

        p14.start_chunk_item(rec);
        if (format == 7)
            rgb.start_chunk(rec + 30, detail::laz::get_scanner_channel(rec));
        if (p14.failed() || (format == 7 && rgb.failed())) {
            last_error = "point encoder failed";
            return false;
        }
    }

    chunk_open = true;
    chunk_points = 1;                       // seed räknas med
    return true;
}

bool LazWriter::Impl::close_chunk() {
    if (!chunk_open) return true;
    chunk_open = false;

    if (legacy()) {
        // EN ström för alla items. Att avsluta den är allt som behövs — det
        // finns inga lagerlängder att skriva och inget punktantal i chunken.
        enc2.done();
        if (p10.failed() || (has_gps() && gps11.failed()) ||
            (has_rgb12() && rgb12.failed()) || enc2.failed()) {
            last_error = "point encoder failed";
            return false;
        }
        chunk_bytes.insert(chunk_bytes.end(), enc2.bytes().begin(), enc2.bytes().end());
    } else {
        // Allt eller inget: felar ett item skrivs ingen del av lagren, eftersom
        // en halv chunk avkodas utan fel men ger fel data.
        if (!detail::laz::append_chunk_layers(chunk_bytes, p14,
                                              format == 7 ? &rgb : nullptr)) {
            last_error = "point encoder failed";
            return false;
        }

        // Patcha punktantalet direkt efter seed-posten.
        std::memcpy(chunk_bytes.data() + rec_len, &chunk_points, 4);
    }

    if (chunk_bytes.size() > 0xFFFFFFFFull) {
        last_error = "chunk too large";
        return false;
    }

    // Först nu rör vi filen: chunken är komplett och giltig innan ett enda byte
    // skrivs, så en chunk som kodaren avvisat kommer aldrig ut.
    //
    // Ett kort fwrite kan däremot lämna ett prefix på disk — det finns inget
    // fil-API som appenderar godtyckligt många byte atomiskt. Vad som GARANTERAS
    // är att en sådan chunk aldrig hamnar i chunk-tabellen och att filen aldrig
    // färdigställs: close() rapporterar fel och filen ska kastas. Vill man ha en
    // verkligt atomisk fil krävs skrivning till en temporärfil och rename, vilket
    // är anroparens beslut och inte skrivarens.
    if (std::fwrite(chunk_bytes.data(), 1, chunk_bytes.size(), f) != chunk_bytes.size()) {
        last_error = "write failed";
        return false;
    }
    chunk_sizes.push_back(static_cast<uint32_t>(chunk_bytes.size()));
    chunk_bytes.clear();
    return true;
}

bool LazWriter::Impl::add_point(const uint8_t* rec) {
    // Returtabellen räknas för VARJE punkt, även seed:en. Returnummer 0 är inte
    // giltig LAS-data men kan komma ur en trasig källa; den hamnar då utanför
    // tabellen i stället för att skriva före den.
    {
        // Returnumret ligger olika i de två postlayouterna: tre bitar i
        // punktformat 0-5, fyra i 6 och uppåt.
        const uint8_t rn = legacy() ? detail::laz::get10_return_number(rec)
                                    : detail::laz::get_return_number(rec);
        if (rn >= 1 && rn <= 15) ++by_return[rn - 1];
    }

    if (!chunk_open)
        return start_chunk(rec);          // punkten blir chunkens seed

    if (legacy()) {
        uint16_t off = detail::laz::kPoint10Size;
        p10.encode(rec);
        if (has_gps()) { gps11.encode(rec + off); off += 8; }
        if (has_rgb12()) rgb12.encode(rec + off);
        if (p10.failed() || (has_gps() && gps11.failed()) ||
            (has_rgb12() && rgb12.failed())) {
            last_error = "point encoder failed";
            return false;
        }
    } else {
        p14.encode_item(rec);
        if (format == 7)
            rgb.encode(detail::laz::get_scanner_channel(rec), rec + 30);
        if (p14.failed() || (format == 7 && rgb.failed())) {
            last_error = "point encoder failed";
            return false;
        }
    }
    ++chunk_points;

    if (chunk_points >= detail::kLazChunkSize)
        return close_chunk();
    return true;
}

// Stänger sista chunken, skriver chunk-tabellen och patchar header och
// tabellposition. Allt eller inget: går något fel bevaras felet och close()
// rapporterar det.
bool LazWriter::Impl::finalize() {
    if (!f) return true;
    bool ok = !failed;

    if (ok) ok = close_chunk();

    if (ok) {
        const int64_t table_pos = tell64(f);
        if (table_pos < 0) { last_error = "tell failed"; ok = false; }
        else {
            std::vector<uint8_t> table;
            if (!detail::write_laz_chunk_table(chunk_sizes, table)) {
                last_error = "could not build chunk table";
                ok = false;
            } else if (std::fwrite(table.data(), 1, table.size(), f) != table.size()) {
                last_error = "write failed";
                ok = false;
            } else if (!seek64(f, detail::laz_point_data_offset(format)) ||
                       std::fwrite(&table_pos, 1, 8, f) != 8) {
                // Platshållaren vid 469 ersätts med den verkliga positionen, så
                // den färdiga filen aldrig innehåller ett -1 som en läsare måste
                // leta upp i filens sista åtta byte.
                last_error = "could not patch chunk table offset";
                ok = false;
            }
        }
    }

    // LAS 1.2 räknar punkter i uint32. Fler än så GÅR inte att beskriva, och att
    // tyst skriva ett trunkerat antal vore värre än att fela: filen hade sett
    // komplett ut och lästs som en bråkdel.
    if (ok && !detail::laz_count_fits(format, count)) {
        last_error = "too many points for a LAS 1.2 header";
        ok = false;
    }

    if (ok) {
        uint8_t hdr[detail::kLazHeaderMaxSize];
        detail::LazHeaderInfo info;
        info.q = q;
        info.format = format;
        info.count = count;
        info.have_bounds = have_bounds;
        info.global_encoding = src_global_encoding;
        info.file_source_id = src_file_source_id;
        if (mode_full()) {
            std::memcpy(info.by_return, by_return, sizeof by_return);
            info.by_return_valid = true;
        }
        info.min_x = min_x; info.min_y = min_y; info.min_z = min_z;
        info.max_x = max_x; info.max_y = max_y; info.max_z = max_z;
        detail::build_laz_header(info, hdr);
        const size_t hdr_len = detail::las_header_size(format);
        if (!seek64(f, 0) || std::fwrite(hdr, 1, hdr_len, f) != hdr_len) {
            last_error = "could not rewrite header";
            ok = false;
        }
    }

    if (std::fclose(f) != 0 && ok) { last_error = "close failed"; ok = false; }
    f = nullptr;
    if (!ok) failed = true;
    return ok;
}

#endif // HLAS_HAVE_LAZ

LazWriter::LazWriter() : impl_(std::make_unique<Impl>()) {}
LazWriter::~LazWriter() { if (impl_ && impl_->open_) close(); }
LazWriter::LazWriter(LazWriter&&) noexcept = default;
LazWriter& LazWriter::operator=(LazWriter&&) noexcept = default;

Status LazWriter::create(const char* path) {
    return create(path, LasWriter::Options{});
}

Status LazWriter::create(const char* path, const LasWriter::Options& opt) {
#ifndef HLAS_HAVE_LAZ
    (void)path; (void)opt;
    if (impl_) impl_->last_error =
        "LAZ support not enabled in this build (HLAS_ENABLE_LAZ)";
    return Status::LazNotSupported;
#else
    if (!impl_) return Status::FileOpenFailed;
    // En misslyckad färdigställning av föregående fil får inte tystas av att
    // en ny öppnas — anroparen skulle tro att båda gick bra.
    if (impl_->open_) {
        const Status prev = close();
        if (prev != Status::Ok)
            return prev;
    }
    if (!path || !*path) {
        impl_->last_error = "empty path";
        return Status::FileNotFound;
    }
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

    // FULL kräver en källa att ärva format och kvantisering ur. Utan den skulle
    // läget tyst falla tillbaka på punktformat 6, alltså precis den reduktion
    // det finns för att förhindra.
    if (opt.mode == WriteMode::Full) {
        impl_->last_error = "FULL requires create_like()";
        return Status::InvalidHeader;
    }

    auto& I = *impl_;
    I.opt = opt;
    I.q = detail::QuantOptions{ opt.x_scale, opt.y_scale, opt.z_scale,
                                opt.x_offset, opt.y_offset, opt.z_offset };
    // MÅSTE nollställas: samma skrivarobjekt kan återanvändas efter close(),
    // och ett kvarlämnat format 7 hade fått LITE att kopiera 36 byte ur en
    // 30-bytespost och RGB-kodaren att läsa förbi den.
    I.format = detail::kWritePointFormat;
    I.rec_len = detail::kWriteRecordLen;
    std::memset(I.by_return, 0, sizeof I.by_return);
    I.src_global_encoding = 0;
    I.src_file_source_id = 0;
    I.offset_fixed = !opt.auto_offset;
    I.count = 0;
    I.failed = false;
    I.closed = false;
    I.close_status = Status::Ok;
    I.last_error.clear();
    I.path_ = path;
    I.have_bounds = false;
    I.chunk_open = false;
    I.chunk_points = 0;
    I.chunk_bytes.clear();
    I.chunk_sizes.clear();

    // Filen skapas först vid första write(): med auto_offset vet vi inte origo
    // förrän vi sett en punkt, och offset står i headern.
    I.open_ = true;
    return Status::Ok;
#endif
}

Status LazWriter::create_like(const char* path, const Header& source,
                              WriteMode mode) {
    LasWriter::Options o;
    o.x_scale = source.x_scale;  o.y_scale = source.y_scale;  o.z_scale = source.z_scale;
    o.x_offset = source.x_offset; o.y_offset = source.y_offset; o.z_offset = source.z_offset;
    o.auto_offset = false;

    if (mode == WriteMode::Full) {
        // FULL skriver posterna oförändrade. Då måste vi kunna KODA källans
        // format. Punktformat 0-3, 6 och 7 har vi kodare för; 4 och 5 kräver
        // Wavepacket13 och 8-10 kräver Byte14 eller NIR, och inget av det finns
        // — varken som kodare eller avkodare. Att tyst falla tillbaka på 6 vore
        // att tappa fält bakom ett läge som lovar motsatsen.
        if (!detail::writer_supported_format(source.point_format)) {
            if (impl_) impl_->last_error =
                "FULL supports source point format 0-3, 6 and 7";
            return Status::UnsupportedPointFormat;
        }
        // Extra bytes kräver en Byte-codec, som inte finns. Samma resonemang.
        if (source.point_record_length != detail::las_record_len(source.point_format)) {
            if (impl_) impl_->last_error = "FULL does not support extra bytes";
            return Status::UnsupportedPointFormat;
        }
    }

    // create() nollställer allt och avvisar FULL utan källa, så läget sätts
    // EFTER den — här finns källan.
    const Status st = create(path, o);
    if (st == Status::Ok && impl_ && mode == WriteMode::Full) {
        impl_->opt.mode = WriteMode::Full;
        impl_->format   = source.point_format;
        impl_->rec_len  = source.point_record_length;
        impl_->src_global_encoding = source.global_encoding;
        impl_->src_file_source_id  = source.file_source_id;
    }
    return st;
}

// Skriver posterna ur en vy.
//
// I FULL går de igenom OFÖRÄNDRADE — det är hela poängen, och därför krävs
// create_like(): posterna är heltal relativt källans skala och offset.
// I LITE plockas X, Y, Z och klass ut och resten lämnas.
Status LazWriter::write(const PointView& points) {
#ifndef HLAS_HAVE_LAZ
    (void)points;
    return Status::LazNotSupported;
#else
    if (!impl_ || !impl_->open_) return Status::FileOpenFailed;
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
    if (points.count() > (std::numeric_limits<uint64_t>::max)() - I.count) {
        I.last_error = "too many points";
        return Status::InvalidHeader;
    }

    if (!I.mode_full()) {
        // LITE ur en vy: plocka ut koordinater och klass och gå den redan
        // beprövade write()-vägen. Att koda direkt ur vyns heltal vore snabbare
        // men bara korrekt när vyns kvantisering råkar vara filens, och den
        // genvägen är inte värd en andra kodningsväg att hålla i synk.
        //
        // I block, så en vy med hundra miljoner punkter inte kräver 2,5 GB
        // temporära arrayer.
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

    // FULL kräver att vyn matchar det create_like() ärvde. En vy med annat
    // format eller annan postlängd skulle skrivas in i en header som påstår
    // något annat, och filen bli obegriplig.
    if (points.point_format() != I.format) {
        I.last_error = "view point format does not match the writer";
        return Status::UnsupportedPointFormat;
    }
    if (points.record_length() != I.rec_len) {
        I.last_error = "view record length does not match the writer";
        return Status::InvalidPointRecordLength;
    }
    // Kvantiseringen måste också stämma, annars ligger punkterna fel i rummet
    // trots att bytena är korrekta.
    if (points.x_scale() != I.q.x_scale || points.y_scale() != I.q.y_scale ||
        points.z_scale() != I.q.z_scale || points.x_offset() != I.q.x_offset ||
        points.y_offset() != I.q.y_offset || points.z_offset() != I.q.z_offset) {
        I.last_error = "view scale/offset does not match the writer";
        return Status::InvalidHeader;
    }

    if (!I.f && !I.create_file()) {
        I.failed = true;
        return Status::FileOpenFailed;
    }

    double bx0 = 0, by0 = 0, bz0 = 0, bx1 = 0, by1 = 0, bz1 = 0;
    try {
        for (uint64_t i = 0; i < points.count(); ++i) {
            const uint8_t* rec = points.record(i);
            if (!rec) break;
            if (!I.add_point(rec)) {
                I.failed = true;
                return Status::FileOpenFailed;
            }
            const double sx = points.x(i), sy = points.y(i), sz = points.z(i);
            if (i == 0) { bx0 = bx1 = sx; by0 = by1 = sy; bz0 = bz1 = sz; }
            else {
                if (sx < bx0) bx0 = sx; else if (sx > bx1) bx1 = sx;
                if (sy < by0) by0 = sy; else if (sy > by1) by1 = sy;
                if (sz < bz0) bz0 = sz; else if (sz > bz1) bz1 = sz;
            }
        }
    } catch (const std::bad_alloc&) {
        I.failed = true;
        I.last_error = "out of memory";
        return Status::FileOpenFailed;
    }

    if (!I.have_bounds) {
        I.min_x = bx0; I.max_x = bx1;
        I.min_y = by0; I.max_y = by1;
        I.min_z = bz0; I.max_z = bz1;
        I.have_bounds = true;
    } else {
        if (bx0 < I.min_x) I.min_x = bx0;  if (bx1 > I.max_x) I.max_x = bx1;
        if (by0 < I.min_y) I.min_y = by0;  if (by1 > I.max_y) I.max_y = by1;
        if (bz0 < I.min_z) I.min_z = bz0;  if (bz1 > I.max_z) I.max_z = bz1;
    }

    I.offset_fixed = true;
    I.count += points.count();
    return Status::Ok;
#endif
}

Status LazWriter::write(const double* x, const double* y, const double* z,
                        const uint8_t* classification, uint64_t count) {
#ifndef HLAS_HAVE_LAZ
    (void)x; (void)y; (void)z; (void)classification; (void)count;
    return Status::LazNotSupported;
#else
    if (!impl_ || !impl_->open_) return Status::FileOpenFailed;
    auto& I = *impl_;
    if (I.failed) {
        I.last_error = "writer is in a failed state";
        return Status::FileOpenFailed;
    }
    if (count == 0) return Status::Ok;
    if (!x || !y || !z) {
        I.last_error = "null coordinate array";
        return Status::InvalidHeader;
    }
    if (I.mode_full()) {
        // Fyra doubles kan inte bära det FULL lovar. Att tyst skriva en
        // reducerad fil ur en skrivare som sagt FULL vore precis det läget
        // finns för att förhindra.
        I.last_error = "writer is in FULL mode; use write(const PointView&)";
        return Status::InvalidHeader;
    }
    if (count > (std::numeric_limits<size_t>::max)() / detail::kWriteRecordLen) {
        I.last_error = "batch too large";
        return Status::InvalidHeader;
    }
    // Antalet står som uint64 i headern; summan får inte wrappa dit.
    if (count > (std::numeric_limits<uint64_t>::max)() - I.count) {
        I.last_error = "too many points";
        return Status::InvalidHeader;
    }

    // Offset härleds ur första punkten, men bara PROVISORISKT: låses den innan
    // batchen validerats, och batchen sedan avvisas, måste nästa giltiga batch
    // använda en offset härledd ur data som aldrig skrevs.
    detail::QuantOptions q = I.q;
    const bool derive_offset = !I.offset_fixed;
    if (derive_offset) {
        if (!std::isfinite(x[0]) || !std::isfinite(y[0]) || !std::isfinite(z[0])) {
            I.last_error = "non-finite coordinate";
            return Status::InvalidHeader;
        }
        q.x_offset = detail::floor_1000(x[0]);
        q.y_offset = detail::floor_1000(y[0]);
        q.z_offset = detail::floor_1000(z[0]);
    }

    // Kvantisera hela batchen först. En punkt som inte ryms ska avvisa batchen
    // innan filen ens skapas — annars ligger en tom eller halv fil kvar på disk.
    std::vector<uint8_t> recs;
    try {
        recs.resize(static_cast<size_t>(count) * detail::kWriteRecordLen);
    } catch (const std::bad_alloc&) {
        I.last_error = "out of memory";
        return Status::InvalidHeader;
    }
    double bx0 = 0, by0 = 0, bz0 = 0, bx1 = 0, by1 = 0, bz1 = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint8_t rec[detail::kWriteRecordLen];
        double sx, sy, sz;
        if (!detail::encode_point6(q, x[i], y[i], z[i],
                                   classification ? classification[i] : 0,
                                   rec, sx, sy, sz)) {
            I.last_error = "point does not fit the chosen scale/offset";
            return Status::InvalidHeader;
        }
        std::memcpy(recs.data() + static_cast<size_t>(i) * detail::kWriteRecordLen,
                    rec, detail::kWriteRecordLen);

        if (i == 0) { bx0 = bx1 = sx; by0 = by1 = sy; bz0 = bz1 = sz; }
        else {
            if (sx < bx0) bx0 = sx; else if (sx > bx1) bx1 = sx;
            if (sy < by0) by0 = sy; else if (sy > by1) by1 = sy;
            if (sz < bz0) bz0 = sz; else if (sz > bz1) bz1 = sz;
        }
    }

    // Först nu är batchen verklighet.
    if (!I.f && !I.create_file()) {
        I.failed = true;
        return Status::FileOpenFailed;
    }

    // Kodarens buffertar växer under matningen. Ett allokeringsfel här får inte
    // kasta ut ur write() — skrivaren har en öppen fil och en halv chunk, och
    // det tillståndet måste bli ett rapporterat fel, inte ett undantag.
    try {
        for (uint64_t i = 0; i < count; ++i) {
            if (!I.add_point(recs.data() + static_cast<size_t>(i) * detail::kWriteRecordLen)) {
                I.failed = true;
                return Status::FileOpenFailed;
            }
        }
    } catch (const std::bad_alloc&) {
        I.failed = true;
        I.last_error = "out of memory";
        return Status::FileOpenFailed;
    }

    if (!I.have_bounds) {
        I.min_x = bx0; I.max_x = bx1;
        I.min_y = by0; I.max_y = by1;
        I.min_z = bz0; I.max_z = bz1;
        I.have_bounds = true;
    } else {
        if (bx0 < I.min_x) I.min_x = bx0;  if (bx1 > I.max_x) I.max_x = bx1;
        if (by0 < I.min_y) I.min_y = by0;  if (by1 > I.max_y) I.max_y = by1;
        if (bz0 < I.min_z) I.min_z = bz0;  if (bz1 > I.max_z) I.max_z = bz1;
    }

    if (derive_offset) {
        I.q = q;
        I.offset_fixed = true;
    }
    I.count += count;                     // kontrollerad ovan, kan inte wrappa
    return Status::Ok;
#endif
}

Status LazWriter::close() {
#ifndef HLAS_HAVE_LAZ
    return Status::Ok;
#else
    if (!impl_) return Status::Ok;
    auto& I = *impl_;
    if (!I.open_)
        return I.closed ? I.close_status : Status::Ok;
    I.open_ = false;
    I.closed = true;
    if (!I.f) { I.close_status = Status::Ok; return Status::Ok; }  // inga punkter skrevs

    // finalize() allokerar — chunk-tabellen och kodarens buffertar. Den här
    // catchen är inte pynt: ~LazWriter() anropar close(), destruktorn är
    // implicit noexcept, och ett undantag som tar sig ut hit river processen
    // innan Impl:s egen catch ens hinner se det.
    try {
        I.close_status = I.finalize() ? Status::Ok : Status::FileOpenFailed;
    } catch (...) {
        if (I.f) { std::fclose(I.f); I.f = nullptr; }
        I.failed = true;
        I.close_status = Status::FileOpenFailed;
        // Feltexten är best-effort och ligger SIST. En std::string-tilldelning
        // allokerar, och var ursprungsfelet bad_alloc kastar den sannolikt igen
        // — rakt ut ur en noexcept-destruktor. Statusen är redan satt, så en
        // saknad text kostar bara läsbarhet.
        try { I.last_error = "failed while finalizing"; } catch (...) {}
    }
    return I.close_status;
#endif
}

uint64_t LazWriter::point_count() const { return impl_ ? impl_->count : 0; }

const std::string& LazWriter::last_error() const {
    static const std::string empty;
    return impl_ ? impl_->last_error : empty;
}

} // namespace HLas
