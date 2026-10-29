#include "HLas.h"
#include "file_io.h"
#include "header_parser.h"
#include "vlr_parser.h"
#include "point_source.h"
#include "geometry.h"
#include "point_reader.h"
#include "cached_laz_source.h"
#include "dem.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <thread>
#include <atomic>
#include <vector>
#include <type_traits>

namespace HLas {

// Map internal ErrorCode to public Status.
// Inte static: copc_reader.cpp använder samma avbildning.
Status to_status(detail::ErrorCode ec) {
    switch (ec) {
        case detail::ErrorCode::Ok:                     return Status::Ok;
        case detail::ErrorCode::FileNotFound:           return Status::FileNotFound;
        case detail::ErrorCode::FileOpenFailed:         return Status::FileOpenFailed;
        case detail::ErrorCode::MmapFailed:             return Status::MmapFailed;
        case detail::ErrorCode::InvalidSignature:       return Status::InvalidSignature;
        case detail::ErrorCode::UnsupportedVersion:     return Status::UnsupportedVersion;
        case detail::ErrorCode::InvalidHeaderSize:      return Status::InvalidHeader;
        case detail::ErrorCode::UnsupportedPointFormat: return Status::UnsupportedPointFormat;
        case detail::ErrorCode::InvalidPointRecordLength: return Status::InvalidPointRecordLength;
        case detail::ErrorCode::CorruptVlr:             return Status::CorruptFile;
        case detail::ErrorCode::CorruptEvlr:            return Status::CorruptFile;
        case detail::ErrorCode::ReadPastEnd:            return Status::CorruptFile;
        case detail::ErrorCode::LazNotSupported:        return Status::LazNotSupported;
        // Intern än så länge — COPC-indexet exponeras inte publikt förrän Fas 15 är klar.
        case detail::ErrorCode::NotCopc:                return Status::CorruptFile;
        case detail::ErrorCode::CorruptChunk:           return Status::CorruptFile;
    }
    return Status::CorruptFile;
}

const char* version() {
    return HLAS_VERSION_STRING;
}

const char* status_message(Status s) {
    switch (s) {
        case Status::Ok:                     return "OK";
        case Status::FileNotFound:           return "File not found";
        case Status::FileOpenFailed:         return "Failed to open file";
        case Status::MmapFailed:             return "Memory mapping failed";
        case Status::InvalidSignature:       return "Invalid file signature";
        case Status::UnsupportedVersion:     return "Unsupported LAS version";
        case Status::InvalidHeader:          return "Invalid header";
        case Status::UnsupportedPointFormat: return "Unsupported point format";
        case Status::InvalidPointRecordLength: return "Invalid point record length";
        case Status::CorruptFile:            return "Corrupt file";
        case Status::LazNotSupported:        return "LAZ not supported in this build";
    }
    return "Unknown error";
}

struct Reader::Impl {
    detail::MappedFile           file;
    detail::ParsedHeader         parsed{};
    Header                       pub_header{};
    std::vector<detail::Vlr>     vlrs;
    std::vector<detail::Vlr>     evlrs;
    detail::PointReader          point_reader;   // rå, okomprimerad LAS
    detail::LazSource            laz_source;     // enbart LAZ-dekompression
    bool                         is_open = false;
    mutable std::string          last_error;

    // Varje punktdatafunktion börjar här, så att last_error() är tom efter ett
    // lyckat anrop och inte bär kvar ett fel från ett tidigare.
    void begin_call() const { last_error.clear(); }

    // Vy över punktbufferten, satt en gång i open_internal(). Backendarna ovan
    // äger minnet; all läsning går genom den här. Det är sömmen som gör att
    // DEM, polygonfilter och histogram kan sluta bry sig om vilken backend det är.
    detail::PointSource source;

    // Cachen, om en togs. Ligger HÄR och inte i LazSource, eftersom den sedan
    // 1.1.4 gäller även en .las — och en .las går aldrig genom LAZ-vägen.
    detail::SidecarReader sidecar;
    bool using_sidecar = false;

    // ⛔ EN cache betyder EN ordning, oavsett källform.
    //
    // Ett utkast lät en .las behålla sin mmap:ade källa och svara på points()
    // ur den MEDAN frågorna sållade på cellerna. Idén var god — källan kostar
    // ingenting att behålla — men halvbyggd blev den farlig: get_xyz(i) läste
    // ur cachens cellordning medan points().x(i) läste ur filens, vyn märktes
    // som cacheformat fast bufferten bar riktiga LAS-poster, och
    // points().classification(i) plockade klassen ur fel byte.
    //
    // Två ordningar bakom ett index ger tyst fel data. Cachen äger ordningen
    // för BÅDA källformerna tills den delade semantiken går att bygga färdig;
    // se doc/plans/sidecar-las.md.
    bool has_index() const { return using_sidecar; }

    // Antalet punkter, oavsett var de kommer ifrån.
    uint64_t point_count() const {
        return has_index() ? sidecar.count() : source.count;
    }

    // De tre API:er som lämnar ut RÅA POSTER går inte att svara på när punkterna
    // bara finns i cachen: den är bitpackad, cellsorterad och bär fyra fält. Att
    // svara med något som SER ut som poster vore det värsta av allt — rimliga
    // värden ur fel punkt.
    //
    bool raw_records_unavailable() const {
        if (!has_index()) return false;
        last_error = "cachen bär bara X, Y, Z och klass i cellordning; "
                     "öppna med Cache::Off för råa poster";
        return true;
    }

    // ITERATIONEN ÖVER PUNKTER, en gång för alla.
    //
    // Grenen mellan platt buffert och cache tas HÄR, en gång per anrop — inte
    // en gång per punkt. Lambdan inlinas i respektive loop, så den heta koden
    // förblir monomorf. Det är hela skälet att PointSource är icke-virtuell,
    // och det skälet gäller fortfarande.
    //
    // `f(index, ix, iy, iz, klass)` med RÅA heltal; skalningen hör hemma hos
    // anroparen som ändå har headern.
    template <typename F>
    uint64_t each_point(uint64_t start, uint64_t n, F&& f) const {
        const uint64_t total = point_count();
        if (start >= total) return 0;
        if (n > total - start) n = total - start;

        if (!has_index()) {
            const uint8_t* p = source.raw_point(start);
            if (!p) return 0;
            const uint16_t stride = source.stride;
            const uint16_t coff = source.class_offset;
            const uint8_t  mask = source.class_mask;
            for (uint64_t i = 0; i < n; ++i, p += stride) {
                int32_t ix, iy, iz;
                std::memcpy(&ix, p,     4);
                std::memcpy(&iy, p + 4, 4);
                std::memcpy(&iz, p + 8, 4);
                if (!call_point(f, start + i, ix, iy, iz, uint8_t(p[coff] & mask)))
                    return i + 1;
            }
            return n;
        }

        // Cachen: cellvis. Cellen hämtas EN gång per cell, inte per punkt —
        // point(i) binärsöker fram sin cell varje anrop och är långt dyrare.
        //
        // Startcellen slås upp med binärsökning i stället för att svepas fram.
        // Ett svep gjorde en paginerad läsning O(celler) per anrop bara för att
        // hitta början, alltså kvadratiskt över hela filen.
        uint64_t done = 0;
        for (uint64_t ci = sidecar.cell_of_point(start);
             ci < sidecar.cell_count() && done < n; ++ci) {
            const auto cell = sidecar.cell(ci);
            if (cell.n == 0) continue;
            if (cell.first + cell.n <= start) continue;
            uint32_t k = cell.first < start ? uint32_t(start - cell.first) : 0;
            for (; k < cell.n && done < n; ++k, ++done) {
                int32_t ix, iy, iz; uint8_t cl;
                sidecar.point_in_cell(cell, k, ix, iy, iz, cl);
                if (!call_point(f, cell.first + k, ix, iy, iz, cl)) return done + 1;
            }
        }
        return done;
    }

    // ITERATION ÖVER EN POLYGON — den som faktiskt använder indexet.
    //
    // ⛔ SÅLLNINGEN GÅR MOT POLYGONEN, INTE MOT DESS BBOX. 1.1.3 jämförde
    // blockens bbox mot FRÅGANS BBOX, och för en diagonal korridor täcker den
    // bboxen hela filen — så ingenting sållades bort. Cachen blev då LÅNGSAMMARE
    // än ingen cache alls: 568 ms mot en platt fils 524 på autzen.
    //
    // Rutnätet rastreras rad för rad: `polygon_row_span` ger stråkets
    // x-utbredning i just den raden, radindexet ger radens första cell, och
    // ingen cell utanför stråket rörs. Mätt på en orm 12 m x 300 m: 2,29 ms för
    // 1.1.3 mot 0,47 ms.
    //
    // `start`/`count` gäller ÄVEN här. Ett index är en position i DEN HÄR
    // läsaren; utan det får en anropare som läser i omgångar samma punkter om
    // och om igen, och taket på antalet skrivna punkter försvinner.
    // ⛔ `stride[256]` GALLRAR, och urvalet styrs av punktens plats i CELLEN.
    //
    // `(k % stride[klass]) == 0` beror bara på cellen, inte på frågan. Alltså
    // ger varje cell SAMMA punkter varje gång — stride 10 ser likadant ut
    // utzoomad som inzoomad, och en vy som panorerar flimrar inte.
    //
    // En global räknare hade brutit det: urvalet hade då berott på hur anroparen
    // delade upp läsningen, och två bildrutor över samma yta gett olika punkter.
    //
    // Klassen läses för sig — en bitread i stället för fyra — så en överhoppad
    // punkt aldrig kostar tre koordinatuppackningar.
    // `accepted` och `classification` sållar på KLASS och hör därför hemma här,
    // inte i anroparens lambda: klassen går att läsa för sig, och en punkt som
    // ändå ska bort ska inte kosta tre koordinatuppackningar först.
    struct ClassFilter {
        const bool*    accepted = nullptr;
        const uint8_t* stride   = nullptr;
        int            only     = -1;        // enskild klass, -1 = alla
        bool none() const { return !accepted && !stride && only < 0; }
        // `k` är uint64: på den ocachade vägen är det det GLOBALA punktindexet,
        // och en LAS 1.4-fil får ha fler än 2^32 punkter. En trunkering till
        // uint32 hade startat om gallringsfasen vid 4294967296 och behållit en
        // punkt som skulle avvisats.
        bool keep(uint8_t cl, uint64_t k) const {
            if (only >= 0 && cl != uint8_t(only)) return false;
            if (accepted && !accepted[cl]) return false;
            if (stride && stride[cl] > 1 && (k % stride[cl]) != 0) return false;
            return true;
        }
    };

    template <typename F>
    void each_point_in_poly(uint64_t start, uint64_t n,
                            const double* vx, const double* vy, uint32_t nv,
                            const ClassFilter& filt, F&& f) const {
        if (!has_index() || nv < 3) {
            // Platt buffert: ingen cell att räkna k i, så gallringen går på det
            // globala indexet. Samma andel, men urvalet följer filens ordning
            // och är därför inte spatialt jämnt.
            each_point(start, n, [&](uint64_t i, int32_t ix, int32_t iy,
                                     int32_t iz, uint8_t cl) {
                if (!filt.keep(cl, i)) return true;
                return call_point(f, i, ix, iy, iz, cl);
            });
            return;
        }

        const uint64_t total = point_count();
        if (start >= total) return;
        if (n > total - start) n = total - start;
        const uint64_t stop = start + n;                 // exklusiv

        const auto& h = sidecar.header();

        // Cellens hörn i världskoordinater, med samma uttryck som punktvägen:
        // `i * scale + offset`. En negativ skala vänder ordningen, därför min/max.
        auto span = [](int64_t lo, int64_t hi, double scale, double off,
                       double& out_lo, double& out_hi) {
            const double a = double(lo) * scale + off;
            const double b = double(hi) * scale + off;
            out_lo = a < b ? a : b;
            out_hi = a < b ? b : a;
        };

        for (uint32_t cy = 0; cy < h.ny; ++cy) {
            // Radens band i världskoordinater.
            const int64_t iy0 = int64_t(h.grid_y0) + int64_t(cy) * h.cell_h;
            double wy0, wy1;
            span(iy0, iy0 + h.cell_h, parsed.y_scale, parsed.y_offset, wy0, wy1);

            double xa, xb;
            if (!detail::polygon_row_span(vx, vy, nv, wy0, wy1, xa, xb)) continue;

            // Från världs-x till kolumn. Skalans tecken avgör riktningen, så
            // båda ändarna räknas om och min/max tas efteråt.
            const double sxs = parsed.x_scale != 0.0 ? parsed.x_scale : 1.0;
            const double ia = (xa - parsed.x_offset) / sxs;
            const double ib = (xb - parsed.x_offset) / sxs;
            double lo = ia < ib ? ia : ib, hi = ia < ib ? ib : ia;
            // Marginal på en cell åt vardera hållet — avrundningen får inte
            // klippa bort en cell som polygonen nätt och jämnt når.
            lo -= double(h.cell_w); hi += double(h.cell_w);
            if (!(lo == lo) || !(hi == hi)) continue;        // NaN
            const double flo = (lo - double(h.grid_x0)) / double(h.cell_w);
            const double fhi = (hi - double(h.grid_x0)) / double(h.cell_w);
            if (fhi < 0.0 || flo > double(h.nx)) continue;
            const uint32_t cx0 = flo <= 0.0 ? 0u : uint32_t(flo);
            const uint32_t cx1 = fhi >= double(h.nx) ? h.nx - 1 : uint32_t(fhi);

            const uint64_t row_stop = sidecar.row_end(cy);
            for (uint64_t ci = sidecar.row_lower_bound(cy, cx0); ci < row_stop; ++ci) {
                const auto cell = sidecar.cell(ci);
                if (cell.cx > cx1) break;
                if (cell.n == 0) continue;
                if (cell.first >= stop) return;              // celler i indexordning
                if (cell.first + cell.n <= start) continue;

                int64_t bx0, by0, bx1, by1;
                sidecar.cell_bounds(cell, bx0, by0, bx1, by1);
                double wx0, wx1, cy0d, cy1d;
                span(bx0, bx1, parsed.x_scale, parsed.x_offset, wx0, wx1);
                span(by0, by1, parsed.y_scale, parsed.y_offset, cy0d, cy1d);
                if (!detail::rect_intersects_polygon(wx0, cy0d, wx1, cy1d, vx, vy, nv))
                    continue;

                const uint32_t k0 = cell.first < start
                                  ? uint32_t(start - cell.first) : 0u;
                const uint32_t k1 = cell.first + cell.n > stop
                                  ? uint32_t(stop - cell.first) : cell.n;
                for (uint32_t k = k0; k < k1; ++k) {
                    // Klassen först — en bitread i stället för fyra. En punkt
                    // som filtret avvisar packas aldrig upp.
                    if (!filt.none() &&
                        !filt.keep(sidecar.class_in_cell(cell, k), k)) continue;
                    int32_t ix, iy, iz; uint8_t cl;
                    sidecar.point_in_cell(cell, k, ix, iy, iz, cl);
                    if (!call_point(f, cell.first + k, ix, iy, iz, cl)) return;
                }
            }
        }
    }

    // Callbacken får returnera bool för att avbryta, eller void för att inte
    // kunna det. En skrivande anropare MÅSTE kunna avbryta: dess buffert rymmer
    // `count` punkter, och cachens ordning är inte filens — utan taket skriver
    // en polygonfråga så många punkter som ryms i polygonen, inte så många som
    // ryms i bufferten.
    template <typename F>
    static bool call_point(F&& f, uint64_t i, int32_t x, int32_t y, int32_t z, uint8_t c) {
        if constexpr (std::is_same_v<decltype(f(i, x, y, z, c)), bool>)
            return f(i, x, y, z, c);
        else { f(i, x, y, z, c); return true; }
    }

    // Punktformatet som en vy ska rapportera. Cachen är inte ett LAS-format och
    // får inte utge sig för att vara det.
    uint8_t view_format() const {
        return has_index() ? PointView::kSidecarFormat : pub_header.point_format;
    }

    void bind_source() {
        // Cachen kan INTE beskrivas som base + i * stride — den är bitpackad med
        // olika bitbredd per cell. En vy här hade pekat på bitar tolkade som byte.
        source = using_sidecar ? detail::PointSource{}
               : (parsed.is_laz ? laz_source.point_source()
                                : point_reader.point_source());

        source.class_mask =
            detail::PointSource::class_mask_for_format(parsed.point_format_id);

        // Backendarna känner bara till byten. Skala och offset är headerdata.
        source.x_scale  = parsed.x_scale;   source.x_offset = parsed.x_offset;
        source.y_scale  = parsed.y_scale;   source.y_offset = parsed.y_offset;
        source.z_scale  = parsed.z_scale;   source.z_offset = parsed.z_offset;
    }
};

Reader::Reader() : impl_(std::make_unique<Impl>()) {}
Reader::~Reader() = default;
Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;

Status Reader::open(const char* path, Cache cache) {
    if (!impl_) return Status::FileOpenFailed;   // flyttad-från
    Status s = open_internal(path, cache);
    if (s == Status::Ok)
        impl_->last_error.clear();
    else
        impl_->last_error = status_message(s);
    return s;
}

Status Reader::open_internal(const char* path, Cache cache) {
    close();

    // 1. Memory-map
    auto ec = impl_->file.open(path);
    if (ec != detail::ErrorCode::Ok)
        return to_status(ec);

    // 2. Parse header
    ec = detail::parse_header(impl_->file.data(), impl_->file.size(),
                              impl_->parsed);
    if (ec != detail::ErrorCode::Ok) {
        impl_->file.close();
        return to_status(ec);
    }

    // 3. Parse VLRs (needed before LAZ init)
    ec = detail::parse_vlrs(impl_->file.data(), impl_->file.size(),
                            impl_->parsed.header_size,
                            impl_->parsed.num_vlrs,
                            impl_->vlrs);
    if (ec != detail::ErrorCode::Ok) {
        impl_->file.close();
        return to_status(ec);
    }

    // 4. Parse EVLRs (1.4+)
    if (impl_->parsed.version_minor >= 4 && impl_->parsed.num_evlrs > 0) {
        ec = detail::parse_evlrs(impl_->file.data(), impl_->file.size(),
                                 impl_->parsed.start_of_first_evlr,
                                 impl_->parsed.num_evlrs,
                                 impl_->evlrs);
        if (ec != detail::ErrorCode::Ok) {
            impl_->file.close();
            return to_status(ec);
        }
    }

    // 5. Init punktkälla. Cachen ligger här i Impl sedan 1.1.4, eftersom den
    //    gäller båda källformerna och en .las aldrig går genom LAZ-vägen.
    //
    //    Smart hoppar över cachen för en COPC-fil: den har redan ett octree, och
    //    CopcReader når det. En .copc.laz slutar på .laz och hade annars fått en
    //    cache bredvid en fil som inte behöver någon.
    bool use_cache = (cache == Cache::Fast);
    if (cache == Cache::Smart)
        use_cache = !detail::find_vlr(impl_->vlrs, "copc", 1);
    // Cachen först, och den gäller BÅDA källformerna sedan 1.1.4. Saknad, trasig
    // eller för en annan fil faller igenom tyst — den som vill ha en bygger den
    // med BuildSidecar().
    if (use_cache) {
        const std::string sp = detail::sidecar_path_for(path);
        if (!sp.empty() &&
            impl_->sidecar.open(sp.c_str(),
                                detail::sidecar_expectation(path, impl_->parsed,
                                                            impl_->file.data(),
                                                            impl_->file.size()))
                == detail::ErrorCode::Ok) {
            impl_->using_sidecar = true;
        } else {
            impl_->sidecar.close();
        }
    }

    // Togs cachen behövs källan inte alls — för en .laz är det hela
    // besparingen, och för en .las hålls ordningen då enhetlig.
    if (impl_->using_sidecar) {
        ec = detail::ErrorCode::Ok;
    } else if (impl_->parsed.is_laz) {
        ec = impl_->laz_source.open(impl_->parsed, impl_->file.data(),
                                    impl_->file.size(), impl_->vlrs);
    } else {
        ec = impl_->point_reader.init(impl_->parsed,
                                      impl_->file.data(), impl_->file.size());
    }
    if (ec != detail::ErrorCode::Ok) {
        impl_->file.close();
        return to_status(ec);
    }

    // 6. Bind vyn till den backend som faktiskt valdes. Efter det här bryr sig
    //    ingen läsande kod om huruvida det är sidecar, LAZ eller rå LAS.
    impl_->bind_source();

    // 7. Populate public header
    auto& h = impl_->pub_header;
    auto& p = impl_->parsed;
    h.version_major = p.version_major;
    h.version_minor = p.version_minor;
    h.file_source_id = p.file_source_id;
    h.global_encoding = p.global_encoding;
    std::memcpy(h.system_id, p.system_id, 32);
    std::memcpy(h.generating_software, p.generating_software, 32);
    h.creation_day = p.creation_day;
    h.creation_year = p.creation_year;
    h.point_format = p.point_format_id;
    // Postlängden är KÄLLANS, alltid. Cachen har ingen postlängd — den är
    // bitpackad — och en nolla här hade sett ut som en trasig fil i stället för
    // som "cachen bär inga poster". De API:er som lämnar ut poster säger det
    // själva, via last_error().
    h.point_record_length = p.point_record_length;
    h.point_count = p.point_count;
    std::memcpy(h.points_by_return, p.points_by_return, sizeof(h.points_by_return));
    h.x_scale = p.x_scale;   h.y_scale = p.y_scale;   h.z_scale = p.z_scale;
    h.x_offset = p.x_offset; h.y_offset = p.y_offset; h.z_offset = p.z_offset;
    h.max_x = p.max_x;  h.min_x = p.min_x;
    h.max_y = p.max_y;  h.min_y = p.min_y;
    h.max_z = p.max_z;  h.min_z = p.min_z;
    h.is_laz = p.is_laz;
    h.num_vlrs = p.num_vlrs;
    h.num_evlrs = p.num_evlrs;

    impl_->is_open = true;
    return Status::Ok;
}

void Reader::close() {
    if (impl_) {
        impl_->file.close();
        impl_->laz_source.close();
        impl_->sidecar.close();
        impl_->using_sidecar = false;
        impl_->source = {};
        impl_->vlrs.clear();
        impl_->evlrs.clear();
        impl_->is_open = false;
    }
}

bool Reader::is_open() const {
    return impl_ && impl_->is_open;
}

const std::string& Reader::last_error() const {
    static const std::string empty;
    return impl_ ? impl_->last_error : empty;
}

const Header& Reader::header() const {
    static const Header empty{};
    return impl_ ? impl_->pub_header : empty;   // flyttad-från
}

uint32_t Reader::vlr_count() const {
    return impl_ ? static_cast<uint32_t>(impl_->vlrs.size()) : 0;
}

VlrInfo Reader::vlr(uint32_t index) const {
    VlrInfo info{};
    if (!impl_) return info;   // flyttad-från
    if (index < impl_->vlrs.size()) {
        const auto& v = impl_->vlrs[index];
        std::memcpy(info.user_id, v.user_id, 16);
        info.record_id = v.record_id;
        info.data = v.data;
        info.data_size = v.data_size;
    }
    return info;
}

const uint8_t* Reader::point_data(uint64_t index) const {
    if (!impl_) return nullptr;
    impl_->begin_call();
    if (impl_->raw_records_unavailable()) return nullptr;
    return impl_->source.raw_point(index);
}

uint64_t Reader::read_points(uint64_t start, uint64_t count, uint8_t* dest) const {
    if (!impl_) return 0;
    impl_->begin_call();
    if (impl_->raw_records_unavailable()) return 0;
    return impl_->source.read_raw(start, count, dest);
}


void Reader::get_xyz(uint64_t index, double& x, double& y, double& z) const {
    x = y = z = 0.0;
    if (!impl_) return;
    impl_->begin_call();
    const auto& h = impl_->pub_header;
    impl_->each_point(index, 1, [&](uint64_t, int32_t ix, int32_t iy, int32_t iz, uint8_t) {
        x = ix * h.x_scale + h.x_offset;
        y = iy * h.y_scale + h.y_offset;
        z = iz * h.z_scale + h.z_offset;
    });
}

PointView Reader::points() const {
    if (!impl_) return PointView{};
    impl_->begin_call();
    // En vy är base + i * stride. Cachen är bitpackad med olika bitbredd per
    // cell och har ingen sådan form — och dess ordning är inte filens.
    if (impl_->raw_records_unavailable()) return PointView{};
    const auto& s = impl_->source;
    const auto& h = impl_->pub_header;
    return PointView(s.base, s.count, s.stride, impl_->view_format(),
                     h.x_scale, h.y_scale, h.z_scale,
                     h.x_offset, h.y_offset, h.z_offset);
}

uint64_t Reader::get_xyz_batch(uint64_t start, uint64_t count,
                               double* x, double* y, double* z) const {
    if (!impl_) return 0;
    impl_->begin_call();
    const auto& h = impl_->pub_header;
    return impl_->each_point(start, count,
        [&](uint64_t i, int32_t ix, int32_t iy, int32_t iz, uint8_t) {
            const uint64_t k = i - start;
            x[k] = ix * h.x_scale + h.x_offset;
            y[k] = iy * h.y_scale + h.y_offset;
            z[k] = iz * h.z_scale + h.z_offset;
        });
}

uint8_t Reader::get_classification(uint64_t index) const {
    if (!impl_) return 0;
    impl_->begin_call();
    uint8_t out = 0;
    impl_->each_point(index, 1,
        [&](uint64_t, int32_t, int32_t, int32_t, uint8_t cl) { out = cl; });
    return out;
}

uint64_t Reader::get_xyz_filtered(uint64_t start, uint64_t count,
                                  uint8_t classification,
                                  double* x, double* y, double* z) const {
    if (!impl_) return 0;
    impl_->begin_call();
    const auto& h = impl_->pub_header;
    uint64_t out = 0;
    impl_->each_point(start, count,
        [&](uint64_t, int32_t ix, int32_t iy, int32_t iz, uint8_t cl) {
            if (cl != classification) return;
            x[out] = ix * h.x_scale + h.x_offset;
            y[out] = iy * h.y_scale + h.y_offset;
            z[out] = iz * h.z_scale + h.z_offset;
            ++out;
        });
    return out;
}

uint64_t Reader::get_xyz_polygon(uint64_t start, uint64_t count,
                                 const double* poly_x, const double* poly_y,
                                 uint32_t num_vertices,
                                 double* out_x, double* out_y, double* out_z,
                                 int classification,
                                 const uint8_t* stride) const {
    if (!impl_) return 0;
    impl_->begin_call();
    if (num_vertices < 3) return 0;
    const auto& h = impl_->pub_header;

    // Polygonens omslutande rektangel först: en billig jämförelse som avvisar de
    // flesta punkter innan den dyra point_in_polygon körs.
    double bbox_min_x = poly_x[0], bbox_max_x = poly_x[0];
    double bbox_min_y = poly_y[0], bbox_max_y = poly_y[0];
    for (uint32_t v = 1; v < num_vertices; ++v) {
        if (poly_x[v] < bbox_min_x) bbox_min_x = poly_x[v];
        if (poly_x[v] > bbox_max_x) bbox_max_x = poly_x[v];
        if (poly_y[v] < bbox_min_y) bbox_min_y = poly_y[v];
        if (poly_y[v] > bbox_max_y) bbox_max_y = poly_y[v];
    }

    // Ur cachen rastreras polygonen: bara celler stråket passerar rörs, och
    // ingen punkt utanför packas upp. Ur en platt buffert finns inget att sålla
    // på och allt gås igenom.
    // Taket är `count` i båda vägarna, och det följer av att BÅDA bara går
    // igenom `count` punkter — cachevägen sållar block men begränsar sedan till
    // indexintervallet. `out >= count` nedan är därför ett bälte till hängslena,
    // inte det som håller anroparens buffert inom sina gränser.
    uint64_t out = 0;
    auto body = [&](uint64_t, int32_t ix, int32_t iy, int32_t iz, uint8_t cl) -> bool {
        if (out >= count) return false;
        const double x = ix * h.x_scale + h.x_offset;
        const double y = iy * h.y_scale + h.y_offset;
        if (x < bbox_min_x || x > bbox_max_x ||
            y < bbox_min_y || y > bbox_max_y) return true;
        if (!detail::point_in_polygon(x, y, poly_x, poly_y, num_vertices)) return true;
        out_x[out] = x;
        out_y[out] = y;
        out_z[out] = iz * h.z_scale + h.z_offset;
        ++out;
        return true;
    };
    Impl::ClassFilter filt;
    filt.only = classification;
    filt.stride = stride;
    impl_->each_point_in_poly(start, count, poly_x, poly_y, num_vertices,
                              filt, body);
    return out;
}

uint64_t Reader::get_xyzc_polygon(uint64_t start, uint64_t count,
                                   const double* poly_x, const double* poly_y,
                                   uint32_t num_vertices,
                                   double* out_x, double* out_y, double* out_z,
                                   uint8_t* out_classification,
                                   int classification,
                                   const bool* accepted,
                                   const uint8_t* stride) const {
    if (!impl_) return 0;
    impl_->begin_call();
    if (num_vertices < 3) return 0;
    const auto& h = impl_->pub_header;

    double bbox_min_x = poly_x[0], bbox_max_x = poly_x[0];
    double bbox_min_y = poly_y[0], bbox_max_y = poly_y[0];
    for (uint32_t v = 1; v < num_vertices; ++v) {
        if (poly_x[v] < bbox_min_x) bbox_min_x = poly_x[v];
        if (poly_x[v] > bbox_max_x) bbox_max_x = poly_x[v];
        if (poly_y[v] < bbox_min_y) bbox_min_y = poly_y[v];
        if (poly_y[v] > bbox_max_y) bbox_max_y = poly_y[v];
    }

    // Samma tak som i get_xyz_polygon, och av samma skäl.
    uint64_t out = 0;
    auto body = [&](uint64_t, int32_t ix, int32_t iy, int32_t iz, uint8_t cl) -> bool {
        if (out >= count) return false;
        const double x = ix * h.x_scale + h.x_offset;
        const double y = iy * h.y_scale + h.y_offset;
        if (x < bbox_min_x || x > bbox_max_x ||
            y < bbox_min_y || y > bbox_max_y) return true;
        if (!detail::point_in_polygon(x, y, poly_x, poly_y, num_vertices)) return true;
        out_x[out] = x;
        out_y[out] = y;
        out_z[out] = iz * h.z_scale + h.z_offset;
        out_classification[out] = cl;
        ++out;
        return true;
    };
    Impl::ClassFilter filt;
    filt.only = classification;
    filt.accepted = accepted;
    filt.stride = stride;
    impl_->each_point_in_poly(start, count, poly_x, poly_y, num_vertices,
                              filt, body);
    return out;
}

Status Reader::get_classification_histogram(uint64_t counts[256]) const {
    std::memset(counts, 0, 256 * sizeof(uint64_t));
    if (!impl_) return Status::FileOpenFailed;   // flyttad-från
    impl_->begin_call();

    // ⛔ CACHEN FÖRST. Räkningen ligger i .hlz-headern sedan v4, gjord en gång
    // när cachen byggdes — så det här är ett minnesuppslag i stället för en
    // skanning. Skillnaden är 141 ms mot 0 på autzen och 1,8 s mot 0 på en
    // nationell laserruta.
    //
    // Och skanningen den ersätter var LÅNGSAMMARE med cache än utan: punkten är
    // bitpackad med koordinaterna interfolierade med klassen, så ett histogram
    // packade upp tre koordinater det inte ville ha. Headern rättar alltså en
    // försämring, inte bara en långsamhet.
    //
    // ⚠️ Anroparen får INTE veta vilken väg som svarade — därför måste de ge
    // samma svar. Provet som jämför cachens histogram mot källans är garantin.
    if (impl_->has_index()) {
        const uint32_t* h = impl_->sidecar.histogram();
        if (h) {
            for (int i = 0; i < 256; ++i) counts[i] = h[i];
            return Status::Ok;
        }
    }

    impl_->each_point(0, impl_->point_count(),
        [&](uint64_t, int32_t, int32_t, int32_t, uint8_t cl) { ++counts[cl]; });
    return Status::Ok;
}

Status Reader::get_classification_histogram_polygon(uint64_t counts[256],
                                                     const double* poly_x,
                                                     const double* poly_y,
                                                     uint32_t num_verts) const {
    std::memset(counts, 0, 256 * sizeof(uint64_t));
    if (!impl_) return Status::FileOpenFailed;   // flyttad-från
    impl_->begin_call();
    if (num_verts < 3) return Status::Ok;

    const auto& h = impl_->pub_header;
    double bx0 = poly_x[0], bx1 = poly_x[0], by0 = poly_y[0], by1 = poly_y[0];
    for (uint32_t v = 1; v < num_verts; ++v) {
        if (poly_x[v] < bx0) bx0 = poly_x[v];
        if (poly_x[v] > bx1) bx1 = poly_x[v];
        if (poly_y[v] < by0) by0 = poly_y[v];
        if (poly_y[v] > by1) by1 = poly_y[v];
    }

    impl_->each_point_in_poly(0, impl_->point_count(), poly_x, poly_y, num_verts,
                              Impl::ClassFilter{},
        [&](uint64_t, int32_t ix, int32_t iy, int32_t, uint8_t cl) {
            const double x = ix * h.x_scale + h.x_offset;
            const double y = iy * h.y_scale + h.y_offset;
            if (x < bx0 || x > bx1 || y < by0 || y > by1) return;
            if (detail::point_in_polygon(x, y, poly_x, poly_y, num_verts))
                ++counts[cl];
        });
    return Status::Ok;
}

DemResult Reader::generate_dem(const double* poly_x, const double* poly_y,
                               uint32_t num_vertices,
                               double cell_size,
                               DemFill fill,
                               const bool* accepted,
                               int thread_count) const {
    if (!impl_) return DemResult{};
    impl_->begin_call();
    if (!impl_->has_index())
        return detail::generate_dem(impl_->source, poly_x, poly_y, num_vertices,
                                    cell_size, fill, accepted, thread_count);

    // Ur cachen matas punkterna i omgångar in i samma rutnät — samma väg som
    // COPC redan använder, så binningen finns bara på ett ställe. Alternativet
    // hade varit att skriva om cellberäkningen och min-skrivningen här.
    //
    // Och celler som polygonen inte passerar packas aldrig upp:
    // en DEM över en korridor ska inte kosta hela filen.
    DemResult grid = detail::dem_make_grid(poly_x, poly_y, num_vertices, cell_size);
    if (grid.cols == 0) return grid;   // taket i dem_make_grid slog till

    double bx0 = poly_x[0], bx1 = poly_x[0], by0 = poly_y[0], by1 = poly_y[0];
    for (uint32_t v = 1; v < num_vertices; ++v) {
        if (poly_x[v] < bx0) bx0 = poly_x[v];
        if (poly_x[v] > bx1) bx1 = poly_x[v];
        if (poly_y[v] < by0) by0 = poly_y[v];
        if (poly_y[v] > by1) by1 = poly_y[v];
    }

    constexpr uint64_t kChunk = 65536;
    constexpr uint16_t kRec = 13;      // int32 X, Y, Z + klassbyte
    std::vector<uint8_t> buf(size_t(kChunk) * kRec);
    uint64_t n = 0;

    detail::PointSource chunk;
    chunk.base = buf.data();
    chunk.stride = kRec;
    chunk.class_offset = 12;
    chunk.class_mask = 0xFF;           // redan maskad när cachen byggdes
    chunk.x_scale = impl_->parsed.x_scale;  chunk.x_offset = impl_->parsed.x_offset;
    chunk.y_scale = impl_->parsed.y_scale;  chunk.y_offset = impl_->parsed.y_offset;
    chunk.z_scale = impl_->parsed.z_scale;  chunk.z_offset = impl_->parsed.z_offset;

    auto flush = [&]() {
        if (n == 0) return;
        chunk.count = n;
        detail::dem_accumulate(grid, chunk, poly_x, poly_y, num_vertices,
                               accepted, thread_count);
        n = 0;
    };

    impl_->each_point_in_poly(0, impl_->point_count(), poly_x, poly_y, num_vertices,
                              Impl::ClassFilter{},
        [&](uint64_t, int32_t ix, int32_t iy, int32_t iz, uint8_t cl) {
            uint8_t* d = buf.data() + size_t(n) * kRec;
            std::memcpy(d,     &ix, 4);
            std::memcpy(d + 4, &iy, 4);
            std::memcpy(d + 8, &iz, 4);
            d[12] = cl;
            if (++n == kChunk) flush();
        });
    flush();
    detail::dem_fill(grid, fill, thread_count);
    return grid;
}

// ---- Att bygga cachen --------------------------------------------------------

std::string SidecarPathFor(const char* laz_path) {
    return detail::sidecar_path_for(laz_path);
}

Status BuildSidecar(const char* path) {
    const std::string out = detail::sidecar_path_for(path);
    if (out.empty()) return Status::InvalidHeader;

    // Källan läses UTAN cache — annars hade en befintlig cache byggt nästa,
    // och ett fel i den hade förökat sig i stället för att rättas.
    Reader r;
    const Status st = r.open(path, Cache::Off);
    if (st != Status::Ok) return st;

    const Header& h = r.header();
    const uint64_t n = h.point_count;
    if (n == 0) return Status::InvalidHeader;

    // Formatets tak kontrolleras HÄR, före allokeringen. write_sidecar()
    // avvisar också, men först efter att vi hunnit allokera och fylla n poster
    // — på ett 32-bitarsmål trunkeras storleken i resize() och loopen skriver
    // då utanför vektorn.
    if (n > 0xFFFFFFFFull || n > uint64_t(SIZE_MAX) / sizeof(detail::SidecarPoint))
        return Status::InvalidHeader;

    std::vector<detail::SidecarPoint> pts;
    try {
        pts.resize(static_cast<size_t>(n));
    } catch (const std::bad_alloc&) {
        return Status::FileOpenFailed;
    }

    // Klassen maskas HÄR, en gång, när punkterna plockas ur källan. I
    // punktformat 0-5 delar klassbyten plats med tre flaggbitar, och en
    // markpunkt med synthetic satt hade annars lagrats som klass 34.
    const PointView v = r.points();
    for (uint64_t i = 0; i < n; ++i) {
        pts[static_cast<size_t>(i)] = { v.raw_x(i), v.raw_y(i), v.raw_z(i),
                                        v.classification(i) };
    }

    // Metadatan byggs ur den PUBLIKA headern, inte ur den interna — allt som
    // behövs finns där, och en fri funktion ska inte behöva vara vän med Impl.
    // Den måste stämma exakt med det SidecarLiteReader::open jämför mot, annars
    // avvisas cachen direkt efter att den skrivits.
    detail::SidecarHeader meta;
    meta.point_count = n;
    meta.x_scale = h.x_scale; meta.y_scale = h.y_scale; meta.z_scale = h.z_scale;
    meta.x_offset = h.x_offset; meta.y_offset = h.y_offset; meta.z_offset = h.z_offset;
    meta.source_format = h.point_format;
    meta.source_size = detail::file_size_64(path);

    // Fingeravtrycket måste tas med SAMMA funktion som läsvägen jämför med,
    // annars avvisas cachen direkt efter att den skrivits. Källan mmap:as en
    // gång till för det — ett stickprov, inte en genomläsning.
    {
        detail::MappedFile src;
        if (src.open(path) != detail::ErrorCode::Ok) return Status::FileOpenFailed;
        meta.source_fingerprint =
            detail::source_fingerprint_of(src.data(), src.size());
    }

    const detail::ErrorCode ec = detail::write_sidecar(out, meta, pts);
    return ec == detail::ErrorCode::Ok ? Status::Ok : to_status(ec);
}

} // namespace HLas
