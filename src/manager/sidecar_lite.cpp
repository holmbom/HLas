#include "sidecar_lite.h"
#include "path_utf8.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <new>

namespace HLas::detail {

namespace {

int bits_for(uint64_t range) { int b = 1; while (b < 64 && (range >> b)) ++b; return b; }

template <typename T> void put(uint8_t* p, T v) { std::memcpy(p, &v, sizeof(T)); }
template <typename T> T get(const uint8_t* p) { T v{}; std::memcpy(&v, p, sizeof(T)); return v; }

struct BitWriter {
    std::vector<uint8_t>& out;
    uint64_t acc = 0;
    int      n = 0;
    explicit BitWriter(std::vector<uint8_t>& o) : out(o) {}
    void put_bits(uint64_t v, int bits) {
        if (bits <= 0) return;
        acc |= (v & ((bits >= 64) ? ~0ull : ((1ull << bits) - 1))) << n;
        n += bits;
        while (n >= 8) { out.push_back(uint8_t(acc & 0xFF)); acc >>= 8; n -= 8; }
    }
    void flush() { if (n > 0) { out.push_back(uint8_t(acc & 0xFF)); acc = 0; n = 0; } }
};

bool ends_with_ci(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char a = static_cast<unsigned char>(s[s.size() - n + i]);
        if (std::tolower(a) != static_cast<unsigned char>(suffix[i])) return false;
    }
    return true;
}

} // namespace

// Läser upp till 57 bitar. Hämtar alltid åtta byte, så bufferten måste ha slack
// på slutet — skrivaren lägger dit den, och open() kontrollerar att den finns.
inline uint64_t bitread(const uint8_t* p, uint64_t bitpos, int bits) {
    if (bits <= 0 || bits > 57) return 0;
    uint64_t w;
    std::memcpy(&w, p + (bitpos >> 3), 8);
    return (w >> (bitpos & 7)) & ((1ull << bits) - 1);
}

// ---- Sökvägar ---------------------------------------------------------------

std::string sidecar_path_for(const char* source_path) {
    if (!source_path) return {};
    std::string s = source_path;
    // ⛔ LÄGG TILL, ersätt inte: foo.las och foo.laz skiljer sig på en bokstav,
    // och en ersättning hade gett dem samma cachefil att slåss om.
    if (!ends_with_ci(s, ".las") && !ends_with_ci(s, ".laz")) return {};
    return s + ".hlz";
}

uint64_t file_size_64(const char* path) {
    if (!path) return 0;
#ifdef _WIN32
    const std::wstring w = utf8_to_wide(path);
    if (w.empty()) return 0;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
#else
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return 0;
    uint64_t n = 0;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long long t = std::ftell(f);
        if (t > 0) n = uint64_t(t);
    }
    std::fclose(f);
    return n;
#endif
}

uint64_t source_fingerprint_of(const uint8_t* data, size_t size) {
    uint64_t h = 1469598103934665603ull;                 // FNV-1a offset basis
    auto mix = [&h](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    };
    // Skiftas som uint64, inte som size_t. På ett 32-bitarsmål är `size >> 32`
    // odefinierat beteende, och biblioteket byggs för sådana mål.
    const uint64_t n64 = uint64_t(size);
    const uint8_t sz[8] = {
        uint8_t(n64), uint8_t(n64 >> 8), uint8_t(n64 >> 16), uint8_t(n64 >> 24),
        uint8_t(n64 >> 32), uint8_t(n64 >> 40), uint8_t(n64 >> 48), uint8_t(n64 >> 56)
    };
    mix(sz, 8);
    if (!data || size == 0) return h;

    // ⚠️ Ett STICKPROV, inte en checksumma. Det fångar det som ändrar filen brett
    // — en omkodning, en annan fil av samma längd — men en ändring som helt
    // ligger MELLAN två fönster passerar. Ett fullt hashvärde vore ett löfte men
    // kostar en genomläsning vid varje open(), alltså hela poängen med en cache
    // som öppnas på 0 ms. Se TODO.md.
    constexpr size_t kWindow = 4096;
    for (int i = 1; i <= 31; ++i) {
        const size_t at = size_t((double(size) * i) / 32.0);
        const size_t n  = at >= size ? 0 : (size - at < kWindow ? size - at : kWindow);
        mix(data + at, n);
    }
    const size_t tail = size < kWindow ? size : kWindow;
    mix(data + (size - tail), tail);
    return h;
}

// ---- Skrivning --------------------------------------------------------------

static ErrorCode write_sidecar_impl(const std::string& path,
                                    const SidecarHeader& meta,
                                    std::vector<SidecarPoint>& pts);

ErrorCode write_sidecar(const std::string& path, const SidecarHeader& meta,
                        std::vector<SidecarPoint>& pts) {
    // Se headern: minnesbrist blir en felkod, inte ett undantag.
    try {
        return write_sidecar_impl(path, meta, pts);
    } catch (const std::bad_alloc&) {
        remove_utf8(path + ".tmp");
        return ErrorCode::FileOpenFailed;
    }
}

static ErrorCode write_sidecar_impl(const std::string& path,
                                    const SidecarHeader& meta,
                                    std::vector<SidecarPoint>& pts) {
    const uint64_t n = pts.size();
    if (n == 0) return ErrorCode::InvalidHeaderSize;
    // Cellens punktantal är uint32, och globala index likaså.
    if (n > 0xFFFFFFFFull) return ErrorCode::InvalidHeaderSize;

    // --- Rutnätet ur bounds ---
    int32_t gx0 = pts[0].x, gy0 = pts[0].y, hx = gx0, hy = gy0;
    int32_t mnz = pts[0].z, mxz = mnz;
    for (const auto& p : pts) {
        gx0 = std::min(gx0, p.x); hx = std::max(hx, p.x);
        gy0 = std::min(gy0, p.y); hy = std::max(hy, p.y);
        mnz = std::min(mnz, p.z); mxz = std::max(mxz, p.z);
    }
    (void)mnz; (void)mxz;

    // Cellstorleken följer MÅLBELÄGGNINGEN, inte en fast meterstorlek — se
    // HLZ_POINTS_PER_CELL i headern för härledningen.
    //
    // Cellen görs kvadratisk i VÄRLDSENHETER, inte i heltalsenheter: x och y kan
    // ha olika skala, och en cell som är kvadratisk i enheter vore då rektangulär
    // i meter. Sållningen mot en korridor bryr sig om metrar.
    // ⛔ Skalan måste vara ändlig och skild från noll, och det kontrolleras HÄR
    // och inte längre fram.
    //
    // Nollskala: skrivaren hade behandlat den som 1,0 och läsvägen likaså, men
    // det är ingen giltig invers — alla punkter får samma världskoordinat medan
    // rutnätet ligger kvar på de råa talen, och hela rader sållas bort. Cachen
    // hade då gett andra svar än källan.
    //
    // NaN eller oändlighet: `side / sx` blir NaN, och att konvertera NaN till
    // int64 är odefinierat beteende.
    auto finite_nonzero = [](double v) {
        return v == v && v != 0.0 && v < 1e300 && v > -1e300;
    };
    if (!finite_nonzero(meta.x_scale) || !finite_nonzero(meta.y_scale) ||
        !finite_nonzero(meta.z_scale))
        return ErrorCode::InvalidHeaderSize;
    const double sx = meta.x_scale < 0 ? -meta.x_scale : meta.x_scale;
    const double sy = meta.y_scale < 0 ? -meta.y_scale : meta.y_scale;
    const double wx = double(uint64_t(int64_t(hx) - gx0)) * sx;   // utbredning i meter
    const double wy = double(uint64_t(int64_t(hy) - gy0)) * sy;
    const uint64_t want_cells = n / HLZ_POINTS_PER_CELL + 1;
    double side = 1.0;
    if (wx > 0.0 && wy > 0.0)
        side = std::sqrt(wx * wy / double(want_cells));
    else
        side = (wx > 0.0 ? wx : (wy > 0.0 ? wy : 1.0)) / double(want_cells) + 1.0;
    if (!(side > 0.0)) side = 1.0;

    // Klämning FÖRE konverteringen: en int64-konvertering av ett flyttal utanför
    // typens område är odefinierat beteende, inte ett stort tal.
    const double dcw = side / sx, dch = side / sy;
    if (!(dcw == dcw) || !(dch == dch)) return ErrorCode::InvalidHeaderSize;
    constexpr double kMaxCell = 4.0e18;
    int64_t cw = dcw >= kMaxCell ? int64_t(kMaxCell) : (dcw < 1.0 ? 1 : int64_t(dcw));
    int64_t ch = dch >= kMaxCell ? int64_t(kMaxCell) : (dch < 1.0 ? 1 : int64_t(dch));

    // ⛔ ANTALET CELLER MÅSTE HA ETT TAK, och det är inte kosmetiskt.
    //
    // Hinksorteringen behöver en räknare per cell i rutnätet — nx * ny stycken,
    // inte bara de ockuperade. En fil vars punkter spänner hela int32 ger med
    // 10 m-celler ett rutnät på 838 861 x 838 861, alltså 7 * 10^11 räknare och
    // ett par terabyte. Provet "int32:ans ändar" träffade precis det.
    //
    // Taket är punktantalet: fler celler än punkter är meningslöst, eftersom de
    // flesta då är tomma. Räknearrayen blir därmed lika stor som punkterna, inte
    // som ytan. Cellen växer tills rutnätet ryms.
    const uint64_t span_x = uint64_t(int64_t(hx) - gx0);
    const uint64_t span_y = uint64_t(int64_t(hy) - gy0);
    auto axis = [](uint64_t span, int64_t c) { return span / uint64_t(c) + 1; };
    while (true) {
        const uint64_t ax = axis(span_x, cw), ay = axis(span_y, ch);
        if (ax <= n / (ay ? ay : 1) || (ax <= 1 && ay <= 1)) break;
        // Väx den längre axeln, så cellerna förblir ungefär kvadratiska.
        if (ax >= ay) { if (cw > (INT64_MAX >> 1)) break; cw *= 2; }
        else          { if (ch > (INT64_MAX >> 1)) break; ch *= 2; }
    }

    const uint64_t nx = axis(span_x, cw);
    const uint64_t ny = axis(span_y, ch);
    if (nx == 0 || ny == 0) return ErrorCode::InvalidHeaderSize;
    if (nx > 0xFFFFFFFFull || ny > 0xFFFFFFFFull) return ErrorCode::InvalidHeaderSize;
    if (nx > SIZE_MAX / ny) return ErrorCode::InvalidHeaderSize;
    const size_t total_cells = size_t(nx) * size_t(ny);
    // Cellindexet lagras som uint32 i hinksorteringen.
    if (uint64_t(total_cells) > 0xFFFFFFFFull) return ErrorCode::InvalidHeaderSize;


    // --- Hinksortering på cellindex ---
    //
    // Snabbare än att jämförelsesortera: 3,2 s mot 9,5 för autzen. Cellindexet
    // är känt direkt ur koordinaten, så ingen nyckel behöver räknas fram.
    std::vector<uint32_t> cell_of(static_cast<size_t>(n));
    std::vector<uint32_t> cnt(total_cells + 1, 0);
    for (size_t i = 0; i < size_t(n); ++i) {
        const uint64_t cx = uint64_t(int64_t(pts[i].x) - gx0) / uint64_t(cw);
        const uint64_t cy = uint64_t(int64_t(pts[i].y) - gy0) / uint64_t(ch);
        cell_of[i] = uint32_t(cy * nx + cx);
        ++cnt[cell_of[i] + 1];
    }
    for (size_t i = 1; i <= total_cells; ++i) cnt[i] += cnt[i - 1];
    std::vector<uint32_t> ord(static_cast<size_t>(n));
    {
        std::vector<uint32_t> at(cnt.begin(), cnt.end() - 1);
        for (size_t i = 0; i < size_t(n); ++i) ord[at[cell_of[i]]++] = uint32_t(i);
    }
    cell_of.clear(); cell_of.shrink_to_fit();

    // --- Glest cellindex ---
    //
    // ⛔ Bara ockuperade celler lagras. Med en tät tabell blir en gles fil — ett
    // tunt stråk i en stor bbox — 79 % större, eftersom 96 % av cellerna är
    // tomma och ändå betalas.
    std::vector<uint32_t> occ_x, row_start;
    std::vector<int32_t>  occ_mnz;
    std::vector<uint32_t> occ_dx, occ_dy;
    std::vector<uint8_t>  occ_bx, occ_by, occ_bz;
    std::vector<uint64_t> occ_off;
    row_start.reserve(size_t(ny) + 1);

    std::vector<uint8_t> data;
    data.reserve(size_t(n) * 2);
    BitWriter w(data);
    uint64_t bitpos = 0;
    uint32_t hist[256] = {};

    for (uint64_t cy = 0; cy < ny; ++cy) {
        row_start.push_back(uint32_t(occ_x.size()));
        for (uint64_t cx = 0; cx < nx; ++cx) {
            const size_t ci = size_t(cy) * size_t(nx) + size_t(cx);
            const uint32_t s = cnt[ci], e = cnt[ci + 1];
            if (s == e) continue;

            const int64_t ox = gx0 + int64_t(cx) * cw;
            const int64_t oy = gy0 + int64_t(cy) * ch;

            // Cellens EGEN utbredning, inte rutnätets. Ligger punkterna i ett
            // hörn av cellen räcker färre bitar — det är den anpassning ett
            // rent rutnät saknar.
            int32_t cmn = pts[ord[s]].z, cmx = cmn;
            uint64_t dxlo = uint64_t(int64_t(pts[ord[s]].x) - ox), dxhi = dxlo;
            uint64_t dylo = uint64_t(int64_t(pts[ord[s]].y) - oy), dyhi = dylo;
            for (uint32_t i = s; i < e; ++i) {
                const auto& p = pts[ord[i]];
                cmn = std::min(cmn, p.z); cmx = std::max(cmx, p.z);
                const uint64_t dx = uint64_t(int64_t(p.x) - ox);
                const uint64_t dy = uint64_t(int64_t(p.y) - oy);
                dxlo = std::min(dxlo, dx); dxhi = std::max(dxhi, dx);
                dylo = std::min(dylo, dy); dyhi = std::max(dyhi, dy);
            }
            const int bz = bits_for(uint64_t(cmx) - uint64_t(cmn));
            const int cbx = bits_for(dxhi - dxlo);
            const int cby = bits_for(dyhi - dylo);

            occ_x.push_back(uint32_t(cx));
            occ_mnz.push_back(cmn);
            occ_dx.push_back(uint32_t(dxlo));
            occ_dy.push_back(uint32_t(dylo));
            occ_bx.push_back(uint8_t(cbx));
            occ_by.push_back(uint8_t(cby));
            occ_bz.push_back(uint8_t(bz));
            occ_off.push_back(bitpos);

            for (uint32_t i = s; i < e; ++i) {
                const auto& p = pts[ord[i]];
                w.put_bits(uint64_t(int64_t(p.x) - ox) - dxlo, cbx);
                w.put_bits(uint64_t(int64_t(p.y) - oy) - dylo, cby);
                w.put_bits(uint64_t(int64_t(p.z) - cmn), bz);
                w.put_bits(p.c, 8);
                ++hist[p.c];
            }
            bitpos += uint64_t(e - s) * uint64_t(cbx + cby + bz + 8);
        }
    }
    row_start.push_back(uint32_t(occ_x.size()));
    occ_off.push_back(bitpos);           // vaktpost: sista cellens slut
    w.flush();
    // Slack: bitread hämtar alltid åtta byte.
    data.insert(data.end(), 8, 0);

    const uint64_t cells = occ_x.size();
    if (cells == 0) return ErrorCode::InvalidHeaderSize;

    // --- Filen ---
    const std::string tmp = path + ".tmp";
#ifdef _WIN32
    const std::wstring wtmp = utf8_to_wide(tmp);
    std::FILE* f = wtmp.empty() ? nullptr : _wfopen(wtmp.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
#endif
    if (!f) return ErrorCode::FileOpenFailed;

    uint8_t hdr[HLZ_HEADER_SIZE];
    std::memset(hdr, 0, sizeof hdr);
    std::memcpy(hdr, HLZ_MAGIC, 4);
    put<uint16_t>(hdr + 4, HLZ_VERSION);
    put<uint16_t>(hdr + 6, HLZ_CONTENT_LITE);
    put<uint64_t>(hdr + 8,  n);
    put<uint64_t>(hdr + 16, cells);
    put<uint32_t>(hdr + 24, uint32_t(nx));
    put<uint32_t>(hdr + 28, uint32_t(ny));
    put<double>(hdr + 32, meta.x_scale);  put<double>(hdr + 40, meta.y_scale);
    put<double>(hdr + 48, meta.z_scale);  put<double>(hdr + 56, meta.x_offset);
    put<double>(hdr + 64, meta.y_offset); put<double>(hdr + 72, meta.z_offset);
    put<uint64_t>(hdr + 80, meta.source_size);
    hdr[88] = meta.source_format;
    put<uint64_t>(hdr + 96, meta.source_fingerprint);
    put<int32_t>(hdr + 104, gx0);
    put<int32_t>(hdr + 108, gy0);
    put<int64_t>(hdr + 112, cw);
    put<int64_t>(hdr + 120, ch);
    std::memcpy(hdr + HLZ_HIST_OFFSET, hist, sizeof hist);

    auto fail = [&]() {
        std::fclose(f);
        remove_utf8(tmp);
        return ErrorCode::FileOpenFailed;
    };
    auto wr = [&](const void* p, size_t bytes) {
        return bytes == 0 || std::fwrite(p, 1, bytes, f) == bytes;
    };
    if (!wr(hdr, sizeof hdr) ||
        !wr(row_start.data(), row_start.size() * 4) ||
        !wr(occ_x.data(),     occ_x.size() * 4) ||
        !wr(occ_off.data(),   occ_off.size() * 8) ||
        !wr(occ_mnz.data(),   occ_mnz.size() * 4) ||
        !wr(occ_dx.data(),    occ_dx.size() * 4) ||
        !wr(occ_dy.data(),    occ_dy.size() * 4) ||
        !wr(occ_bx.data(),    occ_bx.size()) ||
        !wr(occ_by.data(),    occ_by.size()) ||
        !wr(occ_bz.data(),    occ_bz.size()) ||
        !wr(data.data(),      data.size()))
        return fail();
    if (std::fclose(f) != 0) { remove_utf8(tmp); return ErrorCode::FileOpenFailed; }

    // Atomiskt: .tmp skrivs färdig och byter namn sist, så en avbruten skrivning
    // aldrig lämnar en halv cache under det riktiga namnet.
    if (!rename_utf8(tmp, path)) {
        remove_utf8(tmp);
        return ErrorCode::FileOpenFailed;
    }
    return ErrorCode::Ok;
}

// ---- Läsning ----------------------------------------------------------------


ErrorCode SidecarReader::open(const char* path, const SidecarHeader& expect) {
    // Tabellernas storlek styrs av FILEN. En orimlig men formellt giltig cache
    // kan begära mer minne än som finns, och resten av modulen rapporterar med
    // ErrorCode — en anropare som fångar felkoder men inte undantag hade annars
    // fått processen nedlagd.
    try {
        return open_impl(path, expect);
    } catch (const std::bad_alloc&) {
        close();
        return ErrorCode::FileOpenFailed;
    }
}

ErrorCode SidecarReader::open_impl(const char* path, const SidecarHeader& expect) {
    close();
    ErrorCode ec = file_.open(path);
    if (ec != ErrorCode::Ok) return ec;

    const uint8_t* p = file_.data();
    const size_t   size = file_.size();
    if (size < HLZ_HEADER_SIZE || std::memcmp(p, HLZ_MAGIC, 4) != 0) {
        close(); return ErrorCode::InvalidSignature;
    }
    if (get<uint16_t>(p + 4) != HLZ_VERSION || get<uint16_t>(p + 6) != HLZ_CONTENT_LITE) {
        close(); return ErrorCode::UnsupportedVersion;
    }

    h_.point_count = get<uint64_t>(p + 8);
    h_.cell_count  = get<uint64_t>(p + 16);
    h_.nx = get<uint32_t>(p + 24);
    h_.ny = get<uint32_t>(p + 28);
    h_.x_scale = get<double>(p + 32); h_.y_scale = get<double>(p + 40);
    h_.z_scale = get<double>(p + 48); h_.x_offset = get<double>(p + 56);
    h_.y_offset = get<double>(p + 64); h_.z_offset = get<double>(p + 72);
    h_.source_size = get<uint64_t>(p + 80);
    h_.source_format = p[88];
    h_.source_fingerprint = get<uint64_t>(p + 96);
    h_.grid_x0 = get<int32_t>(p + 104);
    h_.grid_y0 = get<int32_t>(p + 108);
    h_.cell_w  = get<int64_t>(p + 112);
    h_.cell_h  = get<int64_t>(p + 120);

    // Cachen måste höra till DEN HÄR filen.
    if (h_.point_count != expect.point_count ||
        h_.source_size != expect.source_size ||
        h_.source_format != expect.source_format ||
        h_.source_fingerprint != expect.source_fingerprint ||
        h_.x_scale != expect.x_scale || h_.y_scale != expect.y_scale ||
        h_.z_scale != expect.z_scale || h_.x_offset != expect.x_offset ||
        h_.y_offset != expect.y_offset || h_.z_offset != expect.z_offset) {
        close(); return ErrorCode::InvalidSignature;
    }

    // Filen kommer utifrån och styr både allokeringar och hur många bitar som
    // läses. Varje fält kontrolleras innan det används.
    if (h_.point_count == 0 || h_.point_count > 0xFFFFFFFFull) {
        close(); return ErrorCode::InvalidHeaderSize;
    }
    if (h_.cell_count == 0 || h_.cell_count > h_.point_count) {
        close(); return ErrorCode::InvalidHeaderSize;
    }
    if (h_.nx == 0 || h_.ny == 0) { close(); return ErrorCode::InvalidHeaderSize; }
    if (uint64_t(h_.nx) * h_.ny < h_.cell_count) {
        close(); return ErrorCode::InvalidHeaderSize;
    }
    if (h_.cell_w <= 0 || h_.cell_h <= 0) { close(); return ErrorCode::InvalidHeaderSize; }
    // ⛔ Rutnätets yttersta hörn måste rymmas i int64. En fientlig cache med
    // cell_w = INT64_MAX och positiv grid_x0 hade annars spillt över i
    // cell_bounds(), point_in_cell() och radbandsberäkningen — signerat
    // spill, alltså odefinierat beteende.
    {
        const int64_t maxc = (INT64_MAX - 1) / 2;
        if (h_.cell_w > maxc / int64_t(h_.nx) || h_.cell_h > maxc / int64_t(h_.ny)) {
            close(); return ErrorCode::InvalidHeaderSize;
        }
        const int64_t ex = int64_t(h_.grid_x0) + int64_t(h_.nx) * h_.cell_w;
        const int64_t ey = int64_t(h_.grid_y0) + int64_t(h_.ny) * h_.cell_h;
        if (ex < int64_t(h_.grid_x0) || ey < int64_t(h_.grid_y0)) {
            close(); return ErrorCode::InvalidHeaderSize;
        }
    }

    // Tabellernas storlek, med division före varje multiplikation.
    const uint64_t rows = uint64_t(h_.ny) + 1;
    const uint64_t cc   = h_.cell_count;
    auto too_big = [&](uint64_t count, uint64_t elem, uint64_t have) {
        return count > have / elem;
    };
    uint64_t need = HLZ_HEADER_SIZE;
    if (too_big(rows, 4, SIZE_MAX - need)) { close(); return ErrorCode::InvalidHeaderSize; }
    need += rows * 4;
    if (too_big(cc, 4, SIZE_MAX - need)) { close(); return ErrorCode::InvalidHeaderSize; }
    need += cc * 4;
    if (too_big(cc + 1, 8, SIZE_MAX - need)) { close(); return ErrorCode::InvalidHeaderSize; }
    need += (cc + 1) * 8;
    if (too_big(cc, 4, SIZE_MAX - need)) { close(); return ErrorCode::InvalidHeaderSize; }
    need += cc * 4;                                   // min_z
    if (too_big(cc, 4, SIZE_MAX - need)) { close(); return ErrorCode::InvalidHeaderSize; }
    need += cc * 4;                                   // dx0
    if (too_big(cc, 4, SIZE_MAX - need)) { close(); return ErrorCode::InvalidHeaderSize; }
    need += cc * 4;                                   // dy0
    if (cc > (SIZE_MAX - need) / 3) { close(); return ErrorCode::InvalidHeaderSize; }
    need += cc * 3;                                   // bx, by, bz
    if (size < need + 8) { close(); return ErrorCode::InvalidHeaderSize; }

    const uint8_t* q = p + HLZ_HIST_OFFSET;
    hist_ = reinterpret_cast<const uint32_t*>(q);
    q = p + HLZ_HEADER_SIZE;
    row_start_ = reinterpret_cast<const uint32_t*>(q);        q += rows * 4;
    cell_x_    = reinterpret_cast<const uint32_t*>(q);        q += cc * 4;
    cell_off_  = reinterpret_cast<const uint64_t*>(q);        q += (cc + 1) * 8;
    cell_mnz_  = reinterpret_cast<const int32_t*>(q);         q += cc * 4;
    cell_dx_   = reinterpret_cast<const uint32_t*>(q);        q += cc * 4;
    cell_dy_   = reinterpret_cast<const uint32_t*>(q);        q += cc * 4;
    cell_bx_   = q;                                           q += cc;
    cell_by_   = q;                                           q += cc;
    cell_bz_   = q;                                           q += cc;
    data_      = q;

    // Radindexet måste börja på noll, vara växande och sluta på cell_count.
    // Utan nollan accepteras celler som ligger FÖRE första radstarten — de nås
    // då aldrig av en polygonfråga, och punkter försvinner tyst.
    if (get<uint32_t>(reinterpret_cast<const uint8_t*>(row_start_)) != 0) {
        close(); return ErrorCode::InvalidHeaderSize;
    }
    for (uint64_t r = 0; r < rows; ++r) {
        const uint32_t v = get<uint32_t>(reinterpret_cast<const uint8_t*>(row_start_ + r));
        if (v > cc) { close(); return ErrorCode::InvalidHeaderSize; }
        if (r > 0) {
            const uint32_t prev =
                get<uint32_t>(reinterpret_cast<const uint8_t*>(row_start_ + r - 1));
            if (v < prev) { close(); return ErrorCode::InvalidHeaderSize; }
        }
    }
    if (get<uint32_t>(reinterpret_cast<const uint8_t*>(row_start_ + h_.ny)) != cc) {
        close(); return ErrorCode::InvalidHeaderSize;
    }

    // Bitpositionerna måste vara växande, och den sista får inte peka utanför.
    const uint64_t data_bits = uint64_t(size - size_t(data_ - p) - 8) * 8;
    cell_row_.resize(size_t(cc));
    first_.resize(size_t(cc) + 1);
    uint64_t acc = 0;
    for (uint64_t i = 0; i < cc; ++i) {
        const uint64_t o0 = get<uint64_t>(reinterpret_cast<const uint8_t*>(cell_off_ + i));
        const uint64_t o1 = get<uint64_t>(reinterpret_cast<const uint8_t*>(cell_off_ + i + 1));
        if (o1 < o0 || o1 > data_bits) { close(); return ErrorCode::InvalidHeaderSize; }
        const uint8_t bx = cell_bx_[i], by = cell_by_[i], bz = cell_bz_[i];
        // Bitbredderna kommer utifrån och styr hur många bitar som läses.
        if (bx == 0 || bx > 32 || by == 0 || by > 32 || bz == 0 || bz > 32) {
            close(); return ErrorCode::InvalidHeaderSize;
        }
        const uint64_t stride = uint64_t(bx) + by + bz + 8;
        if ((o1 - o0) % stride != 0) { close(); return ErrorCode::InvalidHeaderSize; }
        const uint64_t cnt = (o1 - o0) / stride;
        if (cnt == 0 || cnt > h_.point_count) { close(); return ErrorCode::InvalidHeaderSize; }
        if (cell_x_[i] >= h_.nx) { close(); return ErrorCode::InvalidHeaderSize; }
        first_[size_t(i)] = acc;
        acc += cnt;
        if (acc > h_.point_count) { close(); return ErrorCode::InvalidHeaderSize; }
    }
    if (acc != h_.point_count) { close(); return ErrorCode::InvalidHeaderSize; }
    first_[size_t(cc)] = acc;

    // Cellens rad, så cell(i) kan svara utan att söka.
    //
    // ⛔ Samtidigt kontrolleras att cell_x är STRIKT STIGANDE inom varje rad.
    // row_lower_bound() binärsöker, och en osorterad rad hade fått den att
    // hoppa över celler — polygonfrågan tappar då punkter, tyst.
    for (uint64_t r = 0; r < h_.ny; ++r) {
        const uint32_t a = get<uint32_t>(reinterpret_cast<const uint8_t*>(row_start_ + r));
        const uint32_t b = get<uint32_t>(reinterpret_cast<const uint8_t*>(row_start_ + r + 1));
        for (uint32_t i = a; i < b; ++i) {
            if (i > a && cell_x_[i] <= cell_x_[i - 1]) {
                close(); return ErrorCode::InvalidHeaderSize;
            }
            cell_row_[i] = uint32_t(r);
        }
    }

    // ⛔ Histogrammet ANVÄNDS av get_classification_histogram(), som svarar med
    // det utan att skanna. Summan måste därför stämma med punktantalet — annars
    // accepteras en cache som bevisligen ger fel svar. Det är en billig
    // invariant, inte en garanti att fördelningen är rätt; för det behövs
    // checksumman som ligger i TODO.
    {
        uint64_t sum = 0;
        for (int i = 0; i < 256; ++i)
            sum += get<uint32_t>(reinterpret_cast<const uint8_t*>(hist_ + i));
        if (sum != h_.point_count) { close(); return ErrorCode::InvalidHeaderSize; }
    }
    return ErrorCode::Ok;
}

void SidecarReader::close() {
    file_.close();
    h_ = SidecarHeader{};
    hist_ = nullptr; row_start_ = nullptr; cell_x_ = nullptr;
    cell_off_ = nullptr; cell_mnz_ = nullptr; cell_bz_ = nullptr; data_ = nullptr;
    cell_dx_ = nullptr; cell_dy_ = nullptr; cell_bx_ = nullptr; cell_by_ = nullptr;
    // shrink_to_fit, inte bara clear: tabellerna dimensioneras av filen, och en
    // avvisad cache ska inte lämna kvar hundratals megabyte reserverade.
    cell_row_.clear(); cell_row_.shrink_to_fit();
    first_.clear();    first_.shrink_to_fit();
}

SidecarReader::Cell SidecarReader::cell(uint64_t i) const {
    Cell c{};
    if (i >= h_.cell_count) return c;
    c.cx = cell_x_[i];
    c.cy = cell_row_[size_t(i)];
    c.bitpos = get<uint64_t>(reinterpret_cast<const uint8_t*>(cell_off_ + i));
    c.min_z = cell_mnz_[i];
    c.dx0 = cell_dx_[i];
    c.dy0 = cell_dy_[i];
    c.bx = cell_bx_[i];
    c.by = cell_by_[i];
    c.bz = cell_bz_[i];
    c.first = first_[size_t(i)];
    c.n = uint32_t(first_[size_t(i) + 1] - first_[size_t(i)]);
    return c;
}

void SidecarReader::cell_bounds(const Cell& c, int64_t& x0, int64_t& y0,
                                int64_t& x1, int64_t& y1) const {
    x0 = int64_t(h_.grid_x0) + int64_t(c.cx) * h_.cell_w;
    y0 = int64_t(h_.grid_y0) + int64_t(c.cy) * h_.cell_h;
    x1 = x0 + h_.cell_w;
    y1 = y0 + h_.cell_h;
}

uint64_t SidecarReader::row_end(uint32_t cy) const {
    if (cy >= h_.ny) return h_.cell_count;
    return get<uint32_t>(reinterpret_cast<const uint8_t*>(row_start_ + cy + 1));
}

uint64_t SidecarReader::row_lower_bound(uint32_t cy, uint32_t from_cx) const {
    if (cy >= h_.ny) return h_.cell_count;
    const uint32_t a = get<uint32_t>(reinterpret_cast<const uint8_t*>(row_start_ + cy));
    const uint32_t b = get<uint32_t>(reinterpret_cast<const uint8_t*>(row_start_ + cy + 1));
    const uint32_t* lo = std::lower_bound(cell_x_ + a, cell_x_ + b, from_cx);
    return uint64_t(lo - cell_x_);
}

void SidecarReader::point_in_cell(const Cell& c, uint32_t k,
                                  int32_t& x, int32_t& y, int32_t& z,
                                  uint8_t& cl) const {
    const int stride = int(c.bx) + c.by + c.bz + 8;
    const uint64_t at = c.bitpos + uint64_t(k) * uint64_t(stride);
    const int64_t ox = int64_t(h_.grid_x0) + int64_t(c.cx) * h_.cell_w + c.dx0;
    const int64_t oy = int64_t(h_.grid_y0) + int64_t(c.cy) * h_.cell_h + c.dy0;
    x = int32_t(int64_t(bitread(data_, at, c.bx)) + ox);
    y = int32_t(int64_t(bitread(data_, at + c.bx, c.by)) + oy);
    z = int32_t(int64_t(bitread(data_, at + c.bx + c.by, c.bz)) + c.min_z);
    cl = uint8_t(bitread(data_, at + c.bx + c.by + c.bz, 8));
}

uint64_t SidecarReader::cell_of_point(uint64_t i) const {
    if (first_.empty() || i >= h_.point_count) return h_.cell_count;
    const auto it = std::upper_bound(first_.begin(), first_.end(), i);
    return uint64_t(it - first_.begin()) - 1;
}

uint8_t SidecarReader::class_in_cell(const Cell& c, uint32_t k) const {
    const int stride = int(c.bx) + c.by + c.bz + 8;
    const uint64_t at = c.bitpos + uint64_t(k) * uint64_t(stride);
    return uint8_t(bitread(data_, at + c.bx + c.by + c.bz, 8));
}

bool SidecarReader::point(uint64_t i, int32_t& x, int32_t& y, int32_t& z,
                          uint8_t& cl) const {
    if (i >= h_.point_count) return false;
    const Cell c = cell(cell_of_point(i));
    point_in_cell(c, uint32_t(i - c.first), x, y, z, cl);
    return true;
}

} // namespace HLas::detail
