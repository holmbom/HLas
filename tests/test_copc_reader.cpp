// Det publika CopcReader-API:t.
//
// Testet jämför mot Reader på samma fil: samma rektangel ska ge samma punkter,
// oavsett om de kom ur en full uppackning eller ur ett partiellt urval.

#include "HLas.h"
#include "test_data.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <atomic>
#include <vector>

// Läsningen lämnar ut RÅA POSTER — hela punkten, alla fält. De här hjälparna
// packar upp dem till arrayer så att jämförelserna nedan kan se ut som förut.
// Att de är två rader är själva poängen: det anroparen förlorade före 1.1.2 var
// inte bekvämlighet, det var data.
namespace {

struct Sel {
    std::vector<uint8_t> recs;
    std::vector<double>  x, y, z;
    std::vector<uint8_t> c;
    uint64_t n = 0;

    Sel(const HLas::CopcReader& r, uint64_t max_points)
        : recs(static_cast<size_t>(max_points) * r.header().point_record_length) {}

    void unpack(const HLas::CopcReader& r, uint64_t count) {
        n = count;
        const auto v = r.view(recs.data(), n);
        x.resize(n); y.resize(n); z.resize(n); c.resize(n);
        v.xyz(0, n, x.data(), y.data(), z.data());
        for (uint64_t i = 0; i < n; ++i) c[i] = v.classification(i);
    }
};

// Samlar upp en strömmande läsning på samma form.
struct Collected {
    std::vector<double>  x, y, z;
    std::vector<uint8_t> c;
    uint64_t calls = 0, max_batch = 0;

    void add(const HLas::PointView& v) {
        ++calls;
        const uint64_t n = v.count();
        if (n > max_batch) max_batch = n;
        const size_t base = x.size();
        x.resize(base + n); y.resize(base + n); z.resize(base + n); c.resize(base + n);
        v.xyz(0, n, x.data() + base, y.data() + base, z.data() + base);
        for (uint64_t i = 0; i < n; ++i) c[base + i] = v.classification(i);
    }
};

} // namespace

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

int test_copc_reader() {
    int fails = 0;
    printf("test_copc_reader:\n");

    // Felstigar utan testdata
    {
        HLas::CopcReader r;
        CHECK(!r.is_open());
        CHECK(r.open("finns-inte.copc.laz") == HLas::Status::FileNotFound);
        CHECK(!r.last_error().empty());
        CHECK(r.open(nullptr) == HLas::Status::FileNotFound);
        CHECK(r.node_count() == 0);
    }

    SKIP_IF_MISSING(testdata::kCopc);

#ifndef HLAS_HAVE_LAZ
    // COPC-noder är LAZ-chunkar. Utan avkodare går indexet att läsa (test_copc
    // gör det) men inte punkterna, och CopcReader nekar öppningen.
    {
        HLas::CopcReader nr;
        CHECK(nr.open(testdata::kCopc) != HLas::Status::Ok);
        CHECK(!nr.is_open());
        CHECK(!nr.last_error().empty());
    }
    printf("  SKIP (resten) — byggt utan LAZ\n");
    return fails;
#else

    // En vanlig LAZ är inte COPC och ska nekas — men vara läsbar med Reader
    if (testdata::exists(testdata::kLaz)) {
        HLas::CopcReader r;
        CHECK(r.open(testdata::kLaz) != HLas::Status::Ok);
        CHECK(!r.is_open());
        CHECK(!r.last_error().empty());
    }

    HLas::CopcReader r;
    CHECK(r.open(testdata::kCopc) == HLas::Status::Ok);
    CHECK(r.is_open());
    CHECK(r.last_error().empty());

    const auto& h = r.header();
    CHECK(h.point_count == testdata::kPointCount);
    CHECK(h.point_format == 7);
    CHECK(h.point_record_length == testdata::kRecLen14);
    CHECK(r.node_count() == testdata::kCopcRootHierNodes);
    CHECK(r.max_level() == 5);

    // Frågan
    const double qx0 = h.min_x + (h.max_x - h.min_x) * 0.30;
    const double qx1 = h.min_x + (h.max_x - h.min_x) * 0.45;
    const double qy0 = h.min_y + (h.max_y - h.min_y) * 0.55;
    const double qy1 = h.min_y + (h.max_y - h.min_y) * 0.70;

    auto est = r.estimate(qx0, qy0, qx1, qy1);
    CHECK(est.node_count > 0);
    CHECK(est.node_count < r.node_count());
    CHECK(est.point_count > 0);
    CHECK(est.byte_size > 0);

    Sel sel(r, est.point_count);

    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    uint64_t n = r.read_region(qx0, qy0, qx1, qy1, sel.recs.data(), est.point_count);
    double copc_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    sel.unpack(r, n);
    const auto& xs = sel.x; const auto& ys = sel.y;
    const auto& zs = sel.z; const auto& cls = sel.c;

    // Estimatet är en övre gräns — aldrig för litet
    CHECK(n > 0);
    CHECK(n <= est.point_count);

    // Alla returnerade punkter måste ligga i rektangeln
    uint64_t outside = 0;
    for (uint64_t i = 0; i < n; ++i)
        if (xs[i] < qx0 || xs[i] > qx1 || ys[i] < qy0 || ys[i] > qy1) ++outside;
    CHECK(outside == 0);

    // Facit: samma rektangel genom Reader på hela filen
    HLas::Reader full;
    t0 = Clock::now();
    CHECK(full.open(testdata::kCopc) == HLas::Status::Ok);
    double open_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

    double px[] = { qx0, qx1, qx1, qx0 };
    double py[] = { qy0, qy0, qy1, qy1 };
    std::vector<double> rx(n + 1024), ry(n + 1024), rz(n + 1024);
    std::vector<uint8_t> rc(n + 1024);
    uint64_t ref = full.get_xyzc_polygon(0, full.header().point_count,
                                          px, py, 4,
                                          rx.data(), ry.data(), rz.data(), rc.data());
    CHECK(ref == n);

    // Samma punkter, inte bara samma antal. Ordningen skiljer (nodordning mot
    // filordning), så jämför sorterat.
    if (ref == n) {
        std::vector<double> a(xs.begin(), xs.begin() + n), b(rx.begin(), rx.begin() + n);
        std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
        CHECK(a == b);
        std::vector<double> az(zs.begin(), zs.begin() + n), bz(rz.begin(), rz.begin() + n);
        std::sort(az.begin(), az.end()); std::sort(bz.begin(), bz.end());
        CHECK(az == bz);
    }

    printf("  copc_reader: %llu punkter, %u av %u noder, %llu byte lästa\n",
           (unsigned long long)n, est.node_count, r.node_count(),
           (unsigned long long)est.byte_size);
    printf("    partiellt %.0f ms  |  Reader::open på hela filen %.0f ms\n",
           copc_ms, open_ms);

    // Polygon: en triangel inuti rektangeln ska ge färre punkter, alla innanför
    {
        double tx[] = { qx0, qx1, (qx0 + qx1) / 2 };
        double ty[] = { qy0, qy0, qy1 };
        auto e2 = r.estimate(qx0, qy0, qx1, qy1);
        Sel tri(r, e2.point_count);
        uint64_t m = r.read_polygon(tx, ty, 3, tri.recs.data(), e2.point_count);
        CHECK(m > 0);
        CHECK(m < n);
        printf("  copc_polygon: %llu punkter i triangeln (av %llu i rektangeln)\n",
               (unsigned long long)m, (unsigned long long)n);
    }

    // LOD: nivåtak ger färre punkter, och noll nivåer under noll
    {
        auto e0 = r.estimate(qx0, qy0, qx1, qy1, 0);
        CHECK(e0.node_count <= 1);
        auto e2 = r.estimate(qx0, qy0, qx1, qy1, 2);
        CHECK(e2.node_count <= est.node_count);
        CHECK(e2.point_count <= est.point_count);
    }

    // För liten buffert ska fylla den och sluta, inte skriva utanför
    {
        Sel small(r, 10);
        uint64_t m = r.read_region(qx0, qy0, qx1, qy1, small.recs.data(), 10);
        CHECK(m == 10);
    }

    // Strömmande läsning ska ge exakt samma punkter som den buffrade — den
    // finns för att slippa allokera för estimate(), inte för att ge ett annat
    // svar.
    {
        Collected got;
        auto cb = [](void* u, const HLas::PointView& v) -> bool {
            static_cast<Collected*>(u)->add(v);
            return true;
        };

        uint64_t m = r.read_region_streaming(qx0, qy0, qx1, qy1, cb, &got);
        CHECK(m == n);
        CHECK(got.x.size() == n);
        CHECK(got.calls > 1);            // levererat nod för nod
        CHECK(got.max_batch < n);        // ingen enskild batch är hela resultatet

        // Samma punkter, samma ordning som collect() ger
        uint64_t diffs = 0;
        for (uint64_t i = 0; i < n && i < got.x.size(); ++i)
            if (got.x[i] != xs[i] || got.y[i] != ys[i] ||
                got.z[i] != zs[i] || got.c[i] != cls[i]) ++diffs;
        CHECK(diffs == 0);

        printf("  streaming: %llu punkter i %llu batchar, största %llu, %llu avvikelser\n",
               (unsigned long long)m, (unsigned long long)got.calls,
               (unsigned long long)got.max_batch, (unsigned long long)diffs);

        // Avbrott: sink returnerar false efter första batchen
        struct Stop { uint64_t calls = 0; } st;
        auto stop_cb = [](void* u, const HLas::PointView&) -> bool {
            ++static_cast<Stop*>(u)->calls;
            return false;
        };
        uint64_t partial = r.read_region_streaming(qx0, qy0, qx1, qy1, stop_cb, &st);
        CHECK(st.calls == 1);
        CHECK(partial > 0);
        CHECK(partial < n);
    }

    // Klassfilter — samma mening som Reader::get_xyzc_polygon
    {
        bool ground[256] = {};
        ground[2] = true;
        Sel gsel(r, est.point_count);
        uint64_t g = r.read_region(qx0, qy0, qx1, qy1, gsel.recs.data(),
                                   est.point_count, ground);
        gsel.unpack(r, g);
        const auto& gc = gsel.c;
        CHECK(g > 0);
        CHECK(g < n);
        bool only2 = true;
        for (uint64_t i = 0; i < g; ++i) if (gc[i] != 2) only2 = false;
        CHECK(only2);

        // Filtrera i efterhand ur det ofiltrerade resultatet — måste bli lika
        uint64_t manual = 0;
        for (uint64_t i = 0; i < n; ++i) if (cls[i] == 2) ++manual;
        CHECK(manual == g);

        // Strömmande med filter ger samma antal
        struct C { uint64_t n = 0; bool bad = false; } cnt;
        auto cb2 = [](void* u, const HLas::PointView& v) -> bool {
            auto* s = static_cast<C*>(u);
            s->n += v.count();
            for (uint64_t i = 0; i < v.count(); ++i)
                if (v.classification(i) != 2) s->bad = true;
            return true;
        };
        CHECK(r.read_region_streaming(qx0, qy0, qx1, qy1, cb2, &cnt, ground) == g);
        CHECK(!cnt.bad);
        printf("  accepted[]: %llu markpunkter av %llu (%.1f%%)\n",
               (unsigned long long)g, (unsigned long long)n, 100.0*g/n);
    }

    // DEM direkt ur COPC — ska ge samma rutnät som Reader::generate_dem på
    // hela filen, för samma polygon och samma cellstorlek
    {
        double px2[] = { qx0, qx1, qx1, qx0 };
        double py2[] = { qy0, qy0, qy1, qy1 };
        bool ground[256] = {}; ground[2] = true;

        auto a = r.generate_dem(px2, py2, 4, 2.0, HLas::DemFill::None, ground);
        CHECK(a.cols > 0 && a.rows > 0);

        auto b = full.generate_dem(px2, py2, 4, 2.0, HLas::DemFill::None, ground);
        CHECK(a.cols == b.cols);
        CHECK(a.rows == b.rows);
        uint64_t cell_diffs = 0, filled = 0;
        for (size_t i = 0; i < a.elevation.size() && i < b.elevation.size(); ++i) {
            const bool na = std::isnan(a.elevation[i]), nb = std::isnan(b.elevation[i]);
            if (na != nb || (!na && a.elevation[i] != b.elevation[i])) ++cell_diffs;
            if (!na) ++filled;
        }
        CHECK(cell_diffs == 0);
        printf("  copc_dem: %ux%u celler, %llu fyllda, %llu avvikelser mot full fil\n",
               a.cols, a.rows, (unsigned long long)filled,
               (unsigned long long)cell_diffs);
    }

    // Parallellt måste ge samma svar som seriellt. Ordningen är obestämd, så
    // jämför som mängd — men antal, summor och innehåll ska stämma exakt.
    {
        CHECK(r.max_connections() == 4);        // standardvärdet
        r.set_max_connections(2);
        CHECK(r.max_connections() == 2);

        for (int t : {2, 4, 8}) {
            Sel par(r, est.point_count);
            uint64_t m = r.read_region(qx0, qy0, qx1, qy1, par.recs.data(),
                                       est.point_count, nullptr, -1, t);
            CHECK(m == n);
            if (m == n) {
                par.unpack(r, m);
                std::vector<double> a(xs.begin(), xs.begin() + n), b(par.x);
                std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
                CHECK(a == b);
                std::vector<double> az(zs.begin(), zs.begin() + n), bz(par.z);
                std::sort(az.begin(), az.end()); std::sort(bz.begin(), bz.end());
                CHECK(az == bz);
            }

            // Strömmande med flera trådar: callbacken får aldrig anropas
            // samtidigt, och summan ska stämma
            struct S {
                std::vector<double> x;
                uint64_t calls = 0;
                std::atomic<int> inside{0};
                bool overlapped = false;
            } got;
            auto cb = [](void* u, const HLas::PointView& v) -> bool {
                auto* s = static_cast<S*>(u);
                if (s->inside.fetch_add(1) != 0) s->overlapped = true;
                ++s->calls;
                const size_t base = s->x.size();
                s->x.resize(base + v.count());
                for (uint64_t i = 0; i < v.count(); ++i) s->x[base + i] = v.x(i);
                s->inside.fetch_sub(1);
                return true;
            };
            uint64_t sm = r.read_region_streaming(qx0, qy0, qx1, qy1, cb, &got,
                                                  nullptr, -1, t);
            CHECK(sm == n);
            CHECK(!got.overlapped);             // aldrig två trådar i callbacken
            CHECK(got.x.size() == n);
            if (got.x.size() == n) {
                std::vector<double> a(xs.begin(), xs.begin() + n), b(got.x);
                std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
                CHECK(a == b);
            }

            // DEM parallellt = DEM seriellt, cell för cell
            double dpx[] = { qx0, qx1, qx1, qx0 };
            double dpy[] = { qy0, qy0, qy1, qy1 };
            bool ground[256] = {}; ground[2] = true;
            auto d1 = r.generate_dem(dpx, dpy, 4, 4.0, HLas::DemFill::None, ground, -1, 1);
            auto dt = r.generate_dem(dpx, dpy, 4, 4.0, HLas::DemFill::None, ground, -1, t);
            CHECK(d1.cols == dt.cols && d1.rows == dt.rows);
            uint64_t cd = 0;
            for (size_t i = 0; i < d1.elevation.size() && i < dt.elevation.size(); ++i) {
                bool na = std::isnan(d1.elevation[i]), nb = std::isnan(dt.elevation[i]);
                if (na != nb || (!na && d1.elevation[i] != dt.elevation[i])) ++cd;
            }
            CHECK(cd == 0);
        }

        // För liten buffert parallellt: fyll den och stanna, aldrig skriva utanför
        Sel tiny(r, 50);
        uint64_t k = r.read_region(qx0, qy0, qx1, qy1, tiny.recs.data(),
                                   50, nullptr, -1, 4);
        CHECK(k <= 50);

        // Avbrott parallellt: callbacken får inte anropas efter att den sagt nej
        {
            struct A {
                std::atomic<int> calls{0};
                std::atomic<bool> said_no{false};
                std::atomic<int> after_no{0};
            } ab;
            auto stop_cb = [](void* u, const HLas::PointView&) -> bool {
                auto* a = static_cast<A*>(u);
                if (a->said_no.load()) a->after_no.fetch_add(1);
                a->calls.fetch_add(1);
                a->said_no.store(true);
                return false;
            };
            r.read_region_streaming(qx0, qy0, qx1, qy1, stop_cb, &ab, nullptr, -1, 8);
            CHECK(ab.calls.load() == 1);
            CHECK(ab.after_no.load() == 0);
        }

        r.set_max_connections(4);
        printf("  parallellt: 2/4/8 trådar ger identiskt resultat, callbacken serialiserad\n");
    }

    // En flyttad-från läsare får inte krascha. Det gällde inte tidigare — och
    // gäller nu även för Reader, som haft samma lucka sedan början.
    {
        HLas::CopcReader a;
        CHECK(a.open(testdata::kCopc) == HLas::Status::Ok);
        HLas::CopcReader b(std::move(a));
        CHECK(b.is_open());
        CHECK(b.node_count() == testdata::kCopcRootHierNodes);

        // a är flyttad-från: allt ska svara tomt, inget får dereferera null
        CHECK(!a.is_open());
        CHECK(a.node_count() == 0);
        CHECK(a.max_level() == -1);
        CHECK(a.header().point_count == 0);
        CHECK(a.estimate(0, 0, 1, 1).node_count == 0);
        uint8_t q[64];
        CHECK(a.read_region(0, 0, 1e9, 1e9, q, 1) == 0);
        a.close();
        CHECK(a.open(testdata::kCopc) != HLas::Status::Ok);

        HLas::Reader ra;
        CHECK(ra.open(testdata::kCopc) == HLas::Status::Ok);
        HLas::Reader rb(std::move(ra));
        CHECK(rb.is_open());
        CHECK(!ra.is_open());
        CHECK(ra.header().point_count == 0);
        CHECK(ra.vlr_count() == 0);
        CHECK(ra.point_data(0) == nullptr);
        double x, y, z;
        ra.get_xyz(0, x, y, z);
        CHECK(ra.get_xyz_batch(0, 1, &x, &y, &z) == 0);
        CHECK(ra.get_classification(0) == 0);
    }

    // Misslyckad open får inte lämna kvar index från försöket
    {
        HLas::CopcReader a;
        CHECK(a.open(testdata::kCopc) == HLas::Status::Ok);
        CHECK(a.node_count() > 0);
        CHECK(a.open("finns-inte.copc.laz") != HLas::Status::Ok);
        CHECK(a.node_count() == 0);
        CHECK(a.max_level() == -1);
        CHECK(!a.is_open());
    }

    if (fails == 0)
        printf("  All passed.\n");

    return fails;
#endif // HLAS_HAVE_LAZ
}
