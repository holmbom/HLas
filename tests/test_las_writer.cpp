// LasWriter — skriver ut ett urval och läser tillbaka det.
//
// Rundgången är hela testet: skriver vi något Reader inte kan läsa, eller
// tappar precision i kvantiseringen, syns det direkt.

#include "HLas.h"
#include "test_data.h"
#include "laz/arithmetic.h"
#include "laz/integer_compressor.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

static std::string tmp_path(const char* name) {
    return testdata::tmp_path(name);
}

int test_las_writer() {
    int fails = 0;
    printf("test_las_writer:\n");

    // Felstigar utan testdata
    {
        HLas::LasWriter w;
        CHECK(w.create(nullptr) == HLas::Status::FileNotFound);
        CHECK(w.create("/finns/inte/alls/x.las") == HLas::Status::FileOpenFailed);
        CHECK(w.point_count() == 0);
        double q[2] = {0, 0};
        // write utan create ska nekas, inte krascha
        CHECK(w.write(q, q, q, nullptr, 2) == HLas::Status::FileOpenFailed);
        CHECK(w.close() == HLas::Status::Ok);

        HLas::LasWriter::Options bad;
        bad.x_scale = 0.0;
        CHECK(w.create(tmp_path("hlas_bad.las").c_str(), bad) == HLas::Status::InvalidHeader);
    }

    // Syntetiskt: skriv kända punkter, läs tillbaka, jämför exakt
    {
        const std::string path = tmp_path("hlas_synth.las");
        const uint64_t N = 5000;
        std::vector<double> x(N), y(N), z(N);
        std::vector<uint8_t> c(N);
        for (uint64_t i = 0; i < N; ++i) {
            x[i] = 650000.0 + (i % 100) * 0.37;
            y[i] = 6640000.0 + (i / 100) * 0.41;
            z[i] = 35.0 + std::sin(double(i) * 0.01) * 20.0;
            c[i] = static_cast<uint8_t>(i % 256);   // hela klassintervallet
        }

        HLas::LasWriter w;
        CHECK(w.create(path.c_str()) == HLas::Status::Ok);
        // Två skrivningar — strömningen ska ge samma resultat som en
        CHECK(w.write(x.data(), y.data(), z.data(), c.data(), 3000) == HLas::Status::Ok);
        CHECK(w.write(x.data() + 3000, y.data() + 3000, z.data() + 3000,
                      c.data() + 3000, N - 3000) == HLas::Status::Ok);
        CHECK(w.point_count() == N);
        CHECK(w.close() == HLas::Status::Ok);

        HLas::Reader r;
        CHECK(r.open(path.c_str()) == HLas::Status::Ok);
        const auto& h = r.header();
        CHECK(h.version_major == 1 && h.version_minor == 4);
        CHECK(h.point_format == 6);
        CHECK(h.point_record_length == 30);
        CHECK(h.point_count == N);

        // Utbredningen i headern måste stämma med punkterna
        CHECK(std::fabs(h.min_x - *std::min_element(x.begin(), x.end())) < 0.01);
        CHECK(std::fabs(h.max_x - *std::max_element(x.begin(), x.end())) < 0.01);
        CHECK(std::fabs(h.min_z - *std::min_element(z.begin(), z.end())) < 0.01);

        uint64_t bad_xy = 0, bad_cls = 0;
        double worst = 0.0;
        for (uint64_t i = 0; i < N; ++i) {
            double rx, ry, rz;
            r.get_xyz(i, rx, ry, rz);
            // Kvantiseringen är 0.01 m, så avvikelsen får vara högst en halv skala
            const double d = std::max({ std::fabs(rx - x[i]), std::fabs(ry - y[i]),
                                        std::fabs(rz - z[i]) });
            if (d > worst) worst = d;
            if (d > 0.005001) ++bad_xy;
            if (r.get_classification(i) != c[i]) ++bad_cls;
        }
        CHECK(bad_xy == 0);
        CHECK(bad_cls == 0);
        printf("  round_trip: %llu punkter, största avvikelse %.4f m, klasser 0-255 OK\n",
               (unsigned long long)N, worst);
        testdata::remove_utf8(path);
    }

    // Utbredningen ska beskriva de LAGRADE koordinaterna, inte anroparens
    // doubles — annars kan min/max ligga en halv skala fel.
    {
        const std::string path = tmp_path("hlas_bounds.las");
        HLas::LasWriter::Options o;
        o.auto_offset = false;
        o.x_offset = o.y_offset = o.z_offset = 0.0;
        o.x_scale = o.y_scale = o.z_scale = 1.0;   // grov skala gör felet synligt
        double x[] = { 10.4, 20.6 }, y[] = { 5.5, 7.4 }, z[] = { 1.2, 3.9 };
        HLas::LasWriter w;
        CHECK(w.create(path.c_str(), o) == HLas::Status::Ok);
        CHECK(w.write(x, y, z, nullptr, 2) == HLas::Status::Ok);
        CHECK(w.close() == HLas::Status::Ok);

        HLas::Reader r;
        CHECK(r.open(path.c_str()) == HLas::Status::Ok);
        // Lagrat blir 10 och 21 — headern måste säga det, inte 10.4 och 20.6
        CHECK(r.header().min_x == 10.0);
        CHECK(r.header().max_x == 21.0);
        double px, py, pz;
        r.get_xyz(0, px, py, pz);
        CHECK(px == r.header().min_x);
        testdata::remove_utf8(path);
    }

    // En avvisad batch får inte förorena utbredningen för de punkter som
    // faktiskt skrevs
    {
        const std::string path = tmp_path("hlas_reject.las");
        HLas::LasWriter::Options o;
        o.auto_offset = false;
        o.x_offset = o.y_offset = o.z_offset = 0.0;
        HLas::LasWriter w;
        CHECK(w.create(path.c_str(), o) == HLas::Status::Ok);
        double gx[] = { 100.0, 200.0 }, gy[] = { 100.0, 200.0 }, gz[] = { 1.0, 2.0 };
        CHECK(w.write(gx, gy, gz, nullptr, 2) == HLas::Status::Ok);
        // Andra punkten ryms inte i int32 med skala 0.01
        double bx[] = { 300.0, 1e12 }, by[] = { 300.0, 300.0 }, bz[] = { 3.0, 3.0 };
        CHECK(w.write(bx, by, bz, nullptr, 2) == HLas::Status::InvalidHeader);
        CHECK(w.point_count() == 2);
        CHECK(w.close() == HLas::Status::Ok);

        HLas::Reader r;
        CHECK(r.open(path.c_str()) == HLas::Status::Ok);
        CHECK(r.header().point_count == 2);
        CHECK(r.header().max_x == 200.0);   // inte 300, och absolut inte 1e12
        testdata::remove_utf8(path);
    }

    // Icke-ändliga skalor och offset ska nekas
    {
        HLas::LasWriter w;
        HLas::LasWriter::Options o;
        o.x_scale = std::numeric_limits<double>::infinity();
        CHECK(w.create(tmp_path("hlas_inf.las").c_str(), o) == HLas::Status::InvalidHeader);
        o.x_scale = 0.01; o.auto_offset = false;
        o.y_offset = std::nan("");
        CHECK(w.create(tmp_path("hlas_nan.las").c_str(), o) == HLas::Status::InvalidHeader);
    }

    // Flyttilldelning över en öppen skrivare får inte läcka handtaget
    {
        const std::string p1 = tmp_path("hlas_mv1.las"), p2 = tmp_path("hlas_mv2.las");
        HLas::LasWriter a, b;
        CHECK(a.create(p1.c_str()) == HLas::Status::Ok);
        double q[] = { 1.0, 2.0 };
        CHECK(a.write(q, q, q, nullptr, 2) == HLas::Status::Ok);
        CHECK(b.create(p2.c_str()) == HLas::Status::Ok);
        CHECK(b.write(q, q, q, nullptr, 2) == HLas::Status::Ok);
        b = std::move(a);           // b:s gamla fil måste färdigställas här
        CHECK(b.close() == HLas::Status::Ok);

        HLas::Reader r1, r2;
        CHECK(r1.open(p1.c_str()) == HLas::Status::Ok);
        CHECK(r1.header().point_count == 2);
        CHECK(r2.open(p2.c_str()) == HLas::Status::Ok);
        CHECK(r2.header().point_count == 2);   // headern skrevs om trots flytten
        testdata::remove_utf8(p1); testdata::remove_utf8(p2);
    }

    SKIP_IF_MISSING(testdata::kCopc);

#ifdef HLAS_HAVE_LAZ
    // Skarpt fall: hämta en korridor ur COPC-filen och spara den
    {
        HLas::CopcReader cr;
        CHECK(cr.open(testdata::kCopc) == HLas::Status::Ok);
        if (!cr.is_open()) return fails;
        const auto& h = cr.header();

        // Diagonal korridor, ±12 m — det fall polygonurvalet finns för
        const double buf = 12.0;
        const double ax = h.min_x + (h.max_x - h.min_x) * 0.1;
        const double ay = h.min_y + (h.max_y - h.min_y) * 0.1;
        const double bx = h.min_x + (h.max_x - h.min_x) * 0.9;
        const double by = h.min_y + (h.max_y - h.min_y) * 0.9;
        const double dx = bx - ax, dy = by - ay;
        const double len = std::sqrt(dx * dx + dy * dy);
        const double nx = -dy / len * buf, ny = dx / len * buf;
        double px[] = { ax + nx, bx + nx, bx - nx, ax - nx };
        double py[] = { ay + ny, by + ny, by - ny, ay - ny };

        auto est_poly = cr.estimate_polygon(px, py, 4);
        auto est_rect = cr.estimate(std::min({px[0],px[1],px[2],px[3]}),
                                    std::min({py[0],py[1],py[2],py[3]}),
                                    std::max({px[0],px[1],px[2],px[3]}),
                                    std::max({py[0],py[1],py[2],py[3]}));
        // Polygonurvalet måste vara strikt bättre för en korridor
        CHECK(est_poly.node_count < est_rect.node_count);
        CHECK(est_poly.byte_size < est_rect.byte_size);

        const uint16_t rl = cr.header().point_record_length;
        std::vector<uint8_t> recs(static_cast<size_t>(est_poly.point_count) * rl);
        uint64_t n = cr.read_polygon(px, py, 4, recs.data(), est_poly.point_count);
        CHECK(n > 0);
        const auto pv = cr.view(recs.data(), n);
        std::vector<double> x(n), y(n), z(n);
        std::vector<uint8_t> c(n);
        pv.xyz(0, n, x.data(), y.data(), z.data());
        for (uint64_t i = 0; i < n; ++i) c[i] = pv.classification(i);

        const std::string path = tmp_path("hlas_corridor.las");
        HLas::LasWriter w;
        // create_like ärver källans skala och offset, så kvantiseringen blir
        // exakt densamma som i originalet
        CHECK(w.create_like(path.c_str(), h) == HLas::Status::Ok);
        CHECK(w.write(x.data(), y.data(), z.data(), c.data(), n) == HLas::Status::Ok);
        CHECK(w.close() == HLas::Status::Ok);

        HLas::Reader back;
        CHECK(back.open(path.c_str()) == HLas::Status::Ok);
        CHECK(back.header().point_count == n);

        // Med ärvd skala och offset ska koordinaterna vara BIT-identiska
        uint64_t diffs = 0;
        for (uint64_t i = 0; i < n; ++i) {
            double rx, ry, rz;
            back.get_xyz(i, rx, ry, rz);
            if (rx != x[i] || ry != y[i] || rz != z[i]) ++diffs;
            if (back.get_classification(i) != c[i]) ++diffs;
        }
        CHECK(diffs == 0);

        printf("  korridor: %llu punkter ur %u/%u noder (%.0f%% av filen mot %.0f%% med AABB)\n",
               (unsigned long long)n, est_poly.node_count, cr.node_count(),
               100.0 * est_poly.byte_size / 81111042.0,
               100.0 * est_rect.byte_size / 81111042.0);
        printf("  sparad som LAS 1.4 fmt 6, återläst: %llu avvikelser\n",
               (unsigned long long)diffs);
        testdata::remove_utf8(path);
    }
#endif

#ifdef HLAS_HAVE_LAZ
    // En avvisad första batch får inte låsa offset eller lämna en fil efter sig
    {
        const std::string zp = tmp_path("hlas_reject.laz");
        testdata::remove_utf8(zp);
        HLas::LazWriter zw;
        CHECK(zw.create(zp.c_str()) == HLas::Status::Ok);
        // Andra punkten ryms inte i int32 med skala 0.01
        double bx[] = { 100.0, 1e12 }, by[] = { 100.0, 100.0 }, bz[] = { 1.0, 1.0 };
        CHECK(zw.write(bx, by, bz, nullptr, 2) == HLas::Status::InvalidHeader);
        CHECK(zw.point_count() == 0);
        CHECK(!testdata::exists(zp.c_str()));   // ingen fil skapad ännu

        // Nästa giltiga batch ska få härleda sin egen offset
        double gx[] = { 500000.0, 500001.0 }, gy[] = { 600000.0, 600001.0 };
        double gz[] = { 5.0, 6.0 };
        CHECK(zw.write(gx, gy, gz, nullptr, 2) == HLas::Status::Ok);
        CHECK(zw.close() == HLas::Status::Ok);

        HLas::Reader rr;
        CHECK(rr.open(zp.c_str()) == HLas::Status::Ok);
        CHECK(rr.header().point_count == 2);
        double px2, py2, pz2;
        rr.get_xyz(0, px2, py2, pz2);
        CHECK(std::fabs(px2 - 500000.0) < 0.02);
        testdata::remove_utf8(zp);
    }

    // LAS och LAZ måste ge IDENTISKA koordinater ur samma punkter — de delar
    // kodning, och det här är testet som håller dem ihop
    {
        const std::string lp = tmp_path("hlas_eq.las"), zp = tmp_path("hlas_eq.laz");
        const uint64_t N = 3000;
        std::vector<double> x(N), y(N), z(N);
        std::vector<uint8_t> c(N);
        for (uint64_t i = 0; i < N; ++i) {
            x[i] = 650000.0 + i * 0.137;
            y[i] = 6640000.0 - i * 0.211;
            z[i] = -30.0 + std::cos(double(i) * 0.02) * 90.0;  // även negativa Z
            c[i] = static_cast<uint8_t>(i % 256);
        }
        HLas::LasWriter lw;  HLas::LazWriter zw;
        CHECK(lw.create(lp.c_str()) == HLas::Status::Ok);
        CHECK(zw.create(zp.c_str()) == HLas::Status::Ok);
        CHECK(lw.write(x.data(), y.data(), z.data(), c.data(), N) == HLas::Status::Ok);
        CHECK(zw.write(x.data(), y.data(), z.data(), c.data(), N) == HLas::Status::Ok);
        CHECK(lw.close() == HLas::Status::Ok);
        CHECK(zw.close() == HLas::Status::Ok);

        HLas::Reader rl, rz;
        CHECK(rl.open(lp.c_str()) == HLas::Status::Ok);
        CHECK(rz.open(zp.c_str()) == HLas::Status::Ok);
        CHECK(rl.header().point_count == rz.header().point_count);
        CHECK(rl.header().min_x == rz.header().min_x);
        CHECK(rl.header().max_z == rz.header().max_z);
        uint64_t d = 0;
        for (uint64_t i = 0; i < N; ++i) {
            double ax, ay, az, bx2, by2, bz2;
            rl.get_xyz(i, ax, ay, az);
            rz.get_xyz(i, bx2, by2, bz2);
            if (ax != bx2 || ay != by2 || az != bz2) ++d;
            if (rl.get_classification(i) != rz.get_classification(i)) ++d;
        }
        CHECK(d == 0);
        printf("  las_laz_equal: %llu punkter, %llu avvikelser mellan formaten\n",
               (unsigned long long)N, (unsigned long long)d);
        testdata::remove_utf8(lp); testdata::remove_utf8(zp);
    }

    // Chunkning. En chunk rymmer 50000 punkter, och gränserna är där fel bor:
    // sista chunken är nästan alltid ofullständig, och write()-batcherna följer
    // inte chunkarna. Testet skriver dessutom i medvetet obalanserade batcher.
    {
        struct Case { uint64_t n; int batches; const char* vad; };
        const Case cases[] = {
            {      1, 1, "1 punkt" },
            {      2, 1, "2 punkter" },
            {  49999, 1, "49999, strax under gränsen" },
            {  50000, 1, "50000, exakt en chunk" },
            {  50001, 1, "50001, två chunkar" },
            { 100001, 1, "100001, tre chunkar" },
            { 120000, 7, "120000 i 7 obalanserade batcher" },
        };
        int chunk_fails = 0;
        for (const Case& c : cases) {
            const std::string zp = tmp_path("hlas_chunk.laz");
            testdata::remove_utf8(zp);

            std::vector<double> X(c.n), Y(c.n), Z(c.n);
            std::vector<uint8_t> C(c.n);
            uint64_t st = c.n * 7919ull + 13ull;
            auto next = [&st]() { st ^= st << 13; st ^= st >> 7; st ^= st << 17;
                                  return static_cast<uint32_t>(st >> 32); };
            for (uint64_t i = 0; i < c.n; ++i) {
                X[i] = 650000.0 + double(i) * 0.137 + double(next() % 100) * 0.01;
                Y[i] = 6640000.0 - double(i) * 0.211;
                Z[i] = -30.0 + std::cos(double(i) * 0.02) * 90.0;
                C[i] = static_cast<uint8_t>(next() % 256);
            }

            HLas::LazWriter w;
            if (w.create(zp.c_str()) != HLas::Status::Ok) { ++chunk_fails; continue; }
            const uint64_t step = c.batches > 1 ? (c.n / c.batches) + 7 : c.n;
            uint64_t off = 0;
            bool wrote = true;
            while (off < c.n) {
                const uint64_t n = (off + step <= c.n) ? step : (c.n - off);
                if (w.write(X.data()+off, Y.data()+off, Z.data()+off, C.data()+off, n)
                    != HLas::Status::Ok) { wrote = false; break; }
                off += n;
            }
            if (!wrote || w.close() != HLas::Status::Ok) {
                printf("  FAIL: chunkning %s: skrivning misslyckades (%s)\n",
                       c.vad, w.last_error().c_str());
                ++chunk_fails; testdata::remove_utf8(zp); continue;
            }

            HLas::Reader rr;
            if (rr.open(zp.c_str()) != HLas::Status::Ok ||
                rr.header().point_count != c.n) {
                printf("  FAIL: chunkning %s: återläsning\n", c.vad);
                ++chunk_fails; testdata::remove_utf8(zp); continue;
            }
            uint64_t d = 0;
            for (uint64_t i = 0; i < c.n; ++i) {
                double ax, ay, az;
                rr.get_xyz(i, ax, ay, az);
                if (std::fabs(ax-X[i]) > 0.011 || std::fabs(ay-Y[i]) > 0.011 ||
                    std::fabs(az-Z[i]) > 0.011) ++d;
                if (rr.get_classification(i) != C[i]) ++d;
            }
            if (d) {
                printf("  FAIL: chunkning %s: %llu avvikelser\n",
                       c.vad, (unsigned long long)d);
                ++chunk_fails;
            }
            testdata::remove_utf8(zp);
        }
        fails += chunk_fails;
        if (chunk_fails == 0)
            printf("  laz_chunkning: 7 fall kring 50000-gränsen, 0 avvikelser\n");
    }

    // close() ska minnas sitt resultat. Ett andra anrop får inte svara Ok efter
    // att det första misslyckats — den som stänger rutinmässigt en gång till
    // skulle då tro att filen blev bra.
    {
        const std::string zp = tmp_path("hlas_close.laz");
        testdata::remove_utf8(zp);
        HLas::LazWriter w;
        CHECK(w.create(zp.c_str()) == HLas::Status::Ok);
        double x[] = { 100.0 }, y[] = { 200.0 }, z[] = { 3.0 };
        CHECK(w.write(x, y, z, nullptr, 1) == HLas::Status::Ok);
        const HLas::Status first = w.close();
        CHECK(first == HLas::Status::Ok);
        CHECK(w.close() == first);          // samma svar, inte ett nytt Ok
        testdata::remove_utf8(zp);
    }

    // Och samma sak när det första close() MISSLYCKAS — det var den regressionen
    // som fanns, och ett test som bara upprepar Ok bevisar ingenting om den.
    //
    // /dev/full går att öppna och skriva till, men varje verklig utskrivning ger
    // ENOSPC. Headern ryms i stdio:s buffert, så felet kommer först när något
    // verkligen spolas ut — precis det fall vi vill åt.
    //
    // create() öppnar ingen fil: med auto_offset är origo inte känt förrän
    // första punkten setts, så filen skapas i write(). Grenen nedan är alltså
    // inte ett öppningstest utan bara en väg att hoppa över det här på en
    // plattform utan /dev/full.
#ifndef _WIN32
    {
        // Kontrollera /dev/full direkt. create() svarar Ok oavsett, eftersom
        // den inte öppnar något — utan den här kontrollen skulle testet FALLA
        // på en plattform som saknar enheten i stället för att hoppas över.
        bool have_dev_full = false;
        if (std::FILE* probe = std::fopen("/dev/full", "wb")) {
            have_dev_full = true;
            std::fclose(probe);
        }

        HLas::LazWriter w;
        if (have_dev_full && w.create("/dev/full") == HLas::Status::Ok) {
            std::vector<double> X(200000), Y(200000), Z(200000);
            for (size_t i = 0; i < X.size(); ++i) {
                X[i] = 500000.0 + double(i) * 0.1;
                Y[i] = 6000000.0 + double(i) * 0.1;
                Z[i] = double(i % 100) * 0.1;
            }
            // Skrivningen kan misslyckas redan här eller först vid close —
            // båda är riktiga utfall. Det som räknas är att close() svarar
            // samma sak två gånger och aldrig Ok.
            w.write(X.data(), Y.data(), Z.data(), nullptr, X.size());
            const HLas::Status a = w.close();
            const HLas::Status b = w.close();
            CHECK(a != HLas::Status::Ok);
            CHECK(b == a);
            CHECK(!w.last_error().empty());
            printf("  laz_close_fel: /dev/full ger %s två gånger (\"%s\")\n",
                   HLas::status_message(a), w.last_error().c_str());
        } else {
            printf("  laz_close_fel: SKIP — /dev/full finns inte\n");
        }
    }
#endif

    // Strukturell validering av den skrivna filen. Rundgången ovan går genom vår
    // egen läsare, och den kan vara överseende med samma sak som vår skrivare gör
    // fel. Det här läser råa byte och kräver att varje fält är vad LAZ-formatet
    // säger — det är den kontroll som blir kvar när referensen inte längre finns
    // i bygget för att säga emot oss.
    {
        const std::string zp = tmp_path("hlas_struct.laz");
        testdata::remove_utf8(zp);
        const uint64_t N = 60000;                 // två chunkar
        std::vector<double> X(N), Y(N), Z(N);
        for (uint64_t i = 0; i < N; ++i) {
            X[i] = 500000.0 + double(i) * 0.1;
            Y[i] = 6000000.0 + double(i) * 0.1;
            Z[i] = double(i % 1000) * 0.1;
        }
        HLas::LazWriter w;
        CHECK(w.create(zp.c_str()) == HLas::Status::Ok);
        CHECK(w.write(X.data(), Y.data(), Z.data(), nullptr, N) == HLas::Status::Ok);
        CHECK(w.close() == HLas::Status::Ok);

        std::vector<uint8_t> raw;
        {
            std::FILE* f = testdata::fopen_utf8(zp, "rb");
            CHECK(f != nullptr);
            if (f) {
                std::fseek(f, 0, SEEK_END);
                raw.resize(static_cast<size_t>(std::ftell(f)));
                std::fseek(f, 0, SEEK_SET);
                CHECK(std::fread(raw.data(), 1, raw.size(), f) == raw.size());
                std::fclose(f);
            }
        }

        auto u16 = [&raw](size_t o) { uint16_t v; std::memcpy(&v, raw.data()+o, 2); return v; };
        auto u32 = [&raw](size_t o) { uint32_t v; std::memcpy(&v, raw.data()+o, 4); return v; };
        auto u64 = [&raw](size_t o) { uint64_t v; std::memcpy(&v, raw.data()+o, 8); return v; };
        auto i64 = [&raw](size_t o) { int64_t v; std::memcpy(&v, raw.data()+o, 8); return v; };

        if (raw.size() > 600) {
            // --- Header ---
            CHECK(std::memcmp(raw.data(), "LASF", 4) == 0);
            CHECK((u16(6) & (1u << 4)) != 0);      // WKT-biten, krav för format 6
            CHECK(raw[24] == 1 && raw[25] == 4);   // LAS 1.4
            CHECK(u16(94) == 375);                 // header_size
            CHECK(u32(96) == 469);                 // offset_to_point_data, förbi VLR:n
            CHECK(u32(100) == 1);                  // en VLR
            CHECK(raw[104] == (6 | 0x80));         // format 6 med LAZ-biten
            CHECK(u16(105) == 30);
            CHECK(u32(107) == 0);                  // legacy-antal ska vara noll
            CHECK(u64(247) == N);                  // 1.4-antalet
            // Returtabellen: alla punkter är retur 1 av 1, så [0] bär allt och
            // resten är noll. Summan MÅSTE vara totalantalet — en tabell som
            // inte går ihop med punktantalet är intern motsägelse i headern.
            uint64_t by_return = 0;
            for (int i = 0; i < 15; ++i) by_return += u64(255 + i * 8);
            CHECK(by_return == N);
            CHECK(u64(255) == N);
            CHECK(u64(235) == 0 && u32(243) == 0); // inga EVLR:er

            // --- LAZ-VLR ---
            CHECK(u16(375 + 0) == 0);                                  // reserved
            CHECK(std::memcmp(raw.data()+375+2, "laszip encoded", 14) == 0);
            CHECK(u16(375 + 18) == 22204);
            CHECK(u16(375 + 20) == 40);                                // payload-längd
            const size_t pl = 375 + 54;
            CHECK(u16(pl + 0) == 3);            // LayerChunked
            CHECK(u16(pl + 2) == 0);            // aritmetisk kodare
            CHECK(raw[pl + 4] == 3 && raw[pl + 5] == 4);
            CHECK(u32(pl + 12) == 50000);       // chunk_size
            CHECK(i64(pl + 16) == -1 && i64(pl + 24) == -1);
            CHECK(u16(pl + 32) == 1);           // ett item
            CHECK(u16(pl + 34) == 10);          // Point14
            CHECK(u16(pl + 36) == 30);
            CHECK(u16(pl + 38) == 3);           // version 3

            // --- Chunk-tabellen ---
            const int64_t tpos = i64(469);
            // Den färdiga filen ska aldrig innehålla platshållaren -1: den är
            // till för icke-sökbar utdata, och vi söker tillbaka och patchar.
            CHECK(tpos > 0);
            CHECK(static_cast<uint64_t>(tpos) + 8 <= raw.size());
            if (tpos > 0 && static_cast<uint64_t>(tpos) + 8 <= raw.size()) {
                CHECK(u32(static_cast<size_t>(tpos)) == 0);          // version
                const uint32_t nchunks = u32(static_cast<size_t>(tpos) + 4);
                CHECK(nchunks == 2);                                  // 60000 / 50000
                // Första chunken börjar direkt efter tabellpekaren, och
                // tabellen ligger efter all punktdata.
                CHECK(static_cast<uint64_t>(tpos) > 477);

                // Och tabellens INNEHÅLL, inte bara dess huvud: avkoda längderna
                // och kräv att de summerar exakt till tabellens position. Går de
                // inte ihop pekar chunk-positionerna fel, och det är just den
                // sortens fel som en rundgång genom vår egen läsare kan vara
                // överseende med.
                using namespace HLas::detail::laz;
                ArithmeticDecoder dec;
                dec.init(raw.data() + tpos + 8,
                         raw.size() - static_cast<size_t>(tpos) - 8);
                IntegerCompressor ic;
                ic.init(dec, 32, 2);
                uint64_t sum = 0;
                int32_t pred = 0;
                for (uint32_t i = 0; i < nchunks; ++i) {
                    const int32_t len = ic.decompress(pred, 1);
                    pred = len;
                    sum += static_cast<uint32_t>(len);
                }
                CHECK(477 + sum == static_cast<uint64_t>(tpos));
            }
            printf("  laz_struktur: header, LAZ-VLR och chunk-tabell "
                   "stämmer byte för byte\n");
        }
        testdata::remove_utf8(zp);
    }

    // LAZ-utskrift: samma punkter, samma bounds, mindre fil. Kodningen är
    // numera vår egen, så det här är rundgång genom eget folk hela vägen —
    // interoperabiliteten bevisas av de gyllene vektorerna i test_laz_encoder.
    {
        HLas::CopcReader cr;
        CHECK(cr.open(testdata::kCopc) == HLas::Status::Ok);
        if (!cr.is_open()) return fails;
        const auto& ch = cr.header();

        const double cx = (ch.min_x + ch.max_x) / 2, cy = (ch.min_y + ch.max_y) / 2;
        // 500 m och inte 200: urvalet ska korsa flera chunkgränser. Chunkningen
        // testas med syntetiska punkter i laz_chunkning, men verkliga
        // punktfördelningar belastar modellerna annorlunda — och det är just
        // över chunkgränsen som tillstånd kan läcka mellan chunkar.
        double px[] = { cx, cx + 500, cx + 500, cx };
        double py[] = { cy, cy, cy + 500, cy + 500 };
        auto e = cr.estimate_polygon(px, py, 4);
        const uint16_t rl2 = cr.header().point_record_length;
        std::vector<uint8_t> recs2(static_cast<size_t>(e.point_count) * rl2);
        uint64_t n = cr.read_polygon(px, py, 4, recs2.data(), e.point_count);
        CHECK(n > 0);
        if (n == 0) return fails;
        const auto pv2 = cr.view(recs2.data(), n);
        std::vector<double> X(n), Y(n), Z(n);
        std::vector<uint8_t> C(n);
        pv2.xyz(0, n, X.data(), Y.data(), Z.data());
        for (uint64_t i = 0; i < n; ++i) C[i] = pv2.classification(i);

        double mnx = X[0], mxx = X[0], mny = Y[0], mxy = Y[0];
        for (uint64_t i = 1; i < n; ++i) {
            mnx = std::min(mnx, X[i]); mxx = std::max(mxx, X[i]);
            mny = std::min(mny, Y[i]); mxy = std::max(mxy, Y[i]);
        }

        const std::string lp = tmp_path("hlas_rt.las"), zp = tmp_path("hlas_rt.laz");
        HLas::LasWriter lw;
        CHECK(lw.create_like(lp.c_str(), ch) == HLas::Status::Ok);
        CHECK(lw.write(X.data(), Y.data(), Z.data(), C.data(), n) == HLas::Status::Ok);
        CHECK(lw.close() == HLas::Status::Ok);

        HLas::LazWriter zw;
        CHECK(zw.create_like(zp.c_str(), ch) == HLas::Status::Ok);
        // Två skrivningar — chunkningen ska inte bero på batchgränserna
        CHECK(zw.write(X.data(), Y.data(), Z.data(), C.data(), n / 2) == HLas::Status::Ok);
        CHECK(zw.write(X.data() + n/2, Y.data() + n/2, Z.data() + n/2,
                       C.data() + n/2, n - n/2) == HLas::Status::Ok);
        CHECK(zw.point_count() == n);
        CHECK(zw.close() == HLas::Status::Ok);

        // Vår egen avkodare läser tillbaka
        HLas::Reader r;
        CHECK(r.open(zp.c_str()) == HLas::Status::Ok);
        const auto& b = r.header();
        CHECK(b.point_count == n);
        CHECK(b.is_laz);
        CHECK(b.point_format == 6);

        // Bounds ur URVALET, inte ur källfilen — annars blir varje DEM gigantisk
        CHECK(std::fabs(b.min_x - mnx) < 0.02);
        CHECK(std::fabs(b.max_x - mxx) < 0.02);
        CHECK(std::fabs(b.min_y - mny) < 0.02);
        CHECK(std::fabs(b.max_y - mxy) < 0.02);
        CHECK(b.max_x < ch.max_x);      // strikt mindre än källan
        CHECK(b.min_x > ch.min_x);

        uint64_t diffs = 0;
        for (uint64_t i = 0; i < n && i < b.point_count; ++i) {
            double rx, ry, rz;
            r.get_xyz(i, rx, ry, rz);
            if (rx != X[i] || ry != Y[i] || rz != Z[i] ||
                r.get_classification(i) != C[i]) ++diffs;
        }
        CHECK(diffs == 0);

        // LAZ ska vara väsentligt mindre
        //
        // Strömmarna ligger i ett EGET block: en ifstream öppnar utan
        // FILE_SHARE_DELETE, och Windows vägrar radera en fil som någon har
        // öppen — så en ännu levande ström hade lämnat kvar filerna i TEMP.
        // Läsaren ovan går bra, den mappar med FILE_SHARE_DELETE.
        std::streampos sl, sz;
        {
            std::ifstream fl = testdata::ifstream_utf8(lp, std::ios::binary | std::ios::ate);
            std::ifstream fz = testdata::ifstream_utf8(zp, std::ios::binary | std::ios::ate);
            sl = fl.tellg(); sz = fz.tellg();
        }
        CHECK(sz < sl / 2);
        printf("  laz_roundtrip: %llu punkter (%llu chunkar), las %lld -> laz %lld byte "
               "(%.0f%%), %llu avvikelser\n",
               (unsigned long long)n, (unsigned long long)((n + 49999) / 50000),
               (long long)sl, (long long)sz,
               100.0 * double(sz) / double(sl), (unsigned long long)diffs);

        testdata::remove_utf8(lp); testdata::remove_utf8(zp);
    }
#else
    // Utan flaggan ska API:t finnas men neka, inte försvinna
    {
        HLas::LazWriter zw;
        CHECK(zw.create(tmp_path("hlas_none.laz").c_str()) == HLas::Status::LazNotSupported);
        CHECK(!zw.last_error().empty());
    }
#endif

    if (fails == 0)
        printf("  All passed.\n");

    return fails;
}

// FULL: allt källan bär ska överleva vägen ut och tillbaka.
//
// Det här är hela poängen med 1.1.2. Testet läser en riktig format 7-fil,
// skriver ett urval i FULL, läser tillbaka det och jämför FÄLT FÖR FÄLT — inte
// bara koordinaterna, som var allt som överlevde före den här versionen.
int test_write_full() {
    int fails = 0;
    printf("test_write_full:\n");
    SKIP_IF_MISSING(testdata::kCopc);

#ifndef HLAS_HAVE_LAZ
    printf("  SKIP — byggt utan LAZ\n");
    return 0;
#else
    HLas::CopcReader cr;
    CHECK(cr.open(testdata::kCopc) == HLas::Status::Ok);
    if (!cr.is_open()) return fails;

    const auto& ch = cr.header();
    const double qx0 = ch.min_x + (ch.max_x - ch.min_x) * 0.40;
    const double qx1 = ch.min_x + (ch.max_x - ch.min_x) * 0.55;
    const double qy0 = ch.min_y + (ch.max_y - ch.min_y) * 0.40;
    const double qy1 = ch.min_y + (ch.max_y - ch.min_y) * 0.55;

    const auto est = cr.estimate(qx0, qy0, qx1, qy1);
    if (est.point_count == 0) { printf("  SKIP — tomt urval\n"); return fails; }

    std::vector<uint8_t> recs(static_cast<size_t>(est.point_count) * ch.point_record_length);
    const uint64_t n = cr.read_region(qx0, qy0, qx1, qy1, recs.data(), est.point_count);
    CHECK(n > 0);
    if (n == 0) return fails;
    const auto src = cr.view(recs.data(), n);

    // --- FULL ---
    const std::string full_path = tmp_path("hlas_full.laz");
    {
        HLas::LazWriter w;
        CHECK(w.create_like(full_path.c_str(), ch, HLas::WriteMode::Full) == HLas::Status::Ok);

        // Fyra doubles kan inte bära FULL, och skrivaren ska säga det i stället
        // för att tyst skriva en reducerad fil.
        double d = 0;
        CHECK(w.write(&d, &d, &d, nullptr, 1) != HLas::Status::Ok);

        CHECK(w.write(src) == HLas::Status::Ok);
        CHECK(w.close() == HLas::Status::Ok);
    }

    HLas::Reader r;
    CHECK(r.open(full_path.c_str()) == HLas::Status::Ok);
    if (!r.is_open()) { testdata::remove_utf8(full_path); return fails; }

    const auto& rh = r.header();
    CHECK(rh.point_format == ch.point_format);          // format 7 bevarat
    CHECK(rh.point_record_length == ch.point_record_length);
    CHECK(rh.point_count == n);
    CHECK(rh.x_scale == ch.x_scale && rh.y_scale == ch.y_scale);
    CHECK(rh.x_offset == ch.x_offset && rh.y_offset == ch.y_offset);

    // points_by_return ska summera till antalet.
    {
        uint64_t sum = 0;
        for (int i = 0; i < 15; ++i) sum += rh.points_by_return[i];
        CHECK(sum == n);
    }

    const auto back = r.points();
    CHECK(back.count() == n);
    CHECK(back.has_rgb() && back.has_gps_time() && back.has_intensity());

    // Fält för fält, varje punkt. Rå post räcker som jämförelse — men räkna
    // också per fält, så ett fel pekar ut VILKET fält som tappats i stället för
    // att bara säga att bytena skiljer.
    uint64_t bad_raw = 0, bad_xyz = 0, bad_rgb = 0, bad_gps = 0, bad_int = 0,
             bad_cls = 0, bad_ang = 0, bad_ret = 0, bad_ud = 0, bad_ps = 0;
    for (uint64_t i = 0; i < n && i < back.count(); ++i) {
        if (std::memcmp(src.record(i), back.record(i), ch.point_record_length) != 0)
            ++bad_raw;
        if (src.raw_x(i) != back.raw_x(i) || src.raw_y(i) != back.raw_y(i) ||
            src.raw_z(i) != back.raw_z(i)) ++bad_xyz;
        uint16_t r0, g0, b0, r1, g1, b1;
        src.rgb(i, r0, g0, b0); back.rgb(i, r1, g1, b1);
        if (r0 != r1 || g0 != g1 || b0 != b1) ++bad_rgb;
        if (src.gps_time(i) != back.gps_time(i)) ++bad_gps;
        if (src.intensity(i) != back.intensity(i)) ++bad_int;
        if (src.classification(i) != back.classification(i)) ++bad_cls;
        if (src.scan_angle(i) != back.scan_angle(i)) ++bad_ang;
        if (src.return_number(i) != back.return_number(i) ||
            src.number_of_returns(i) != back.number_of_returns(i)) ++bad_ret;
        if (src.user_data(i) != back.user_data(i)) ++bad_ud;
        if (src.point_source_id(i) != back.point_source_id(i)) ++bad_ps;
    }
    CHECK(bad_raw == 0);
    CHECK(bad_xyz == 0 && bad_rgb == 0 && bad_gps == 0 && bad_int == 0);
    CHECK(bad_cls == 0 && bad_ang == 0 && bad_ret == 0 && bad_ud == 0 && bad_ps == 0);
    printf("  FULL: %llu punkter, format %u, %llu avvikande råposter\n",
           (unsigned long long)n, rh.point_format, (unsigned long long)bad_raw);
    r.close();

    // --- LITE ur samma vy ---
    const std::string lite_path = tmp_path("hlas_lite.laz");
    {
        HLas::LazWriter w;
        CHECK(w.create_like(lite_path.c_str(), ch) == HLas::Status::Ok);
        CHECK(w.write(src) == HLas::Status::Ok);
        CHECK(w.close() == HLas::Status::Ok);
    }
    HLas::Reader lr;
    CHECK(lr.open(lite_path.c_str()) == HLas::Status::Ok);
    if (lr.is_open()) {
        const auto lv = lr.points();
        CHECK(lr.header().point_format == 6);
        CHECK(lv.count() == n);
        CHECK(!lv.has_rgb());
        // Koordinaterna och klassen ska ändå stämma exakt — create_like ärver
        // kvantiseringen, så inget avrundningsfel införs.
        uint64_t bad = 0;
        for (uint64_t i = 0; i < n && i < lv.count(); ++i)
            if (lv.raw_x(i) != src.raw_x(i) || lv.raw_y(i) != src.raw_y(i) ||
                lv.raw_z(i) != src.raw_z(i) ||
                lv.classification(i) != src.classification(i)) ++bad;
        CHECK(bad == 0);
        lr.close();
    }

    // LITE ska vara märkbart mindre — det är hela skälet att läget finns kvar.
    {
        std::ifstream fa = testdata::ifstream_utf8(full_path, std::ios::binary | std::ios::ate);
        std::ifstream fl = testdata::ifstream_utf8(lite_path, std::ios::binary | std::ios::ate);
        const long long a = fa ? (long long)fa.tellg() : 0;
        const long long l = fl ? (long long)fl.tellg() : 0;
        CHECK(a > 0 && l > 0);
        CHECK(l < a);
        if (a > 0)
            printf("  FULL %lld byte, LITE %lld byte (%.0f%% av FULL)\n",
                   a, l, 100.0 * (double)l / (double)a);
    }

    testdata::remove_utf8(full_path);
    testdata::remove_utf8(lite_path);

    // --- Avvisade källor ---
    {
        // Punktformat 4 och 5 kräver Wavepacket13, som inte finns som codec.
        // Format 0-3 STÖDS sedan fas 11 och prövas i test_write_full_legacy.
        HLas::Header wave = ch;
        wave.point_format = 5;
        wave.point_record_length = 63;
        HLas::LazWriter w;
        CHECK(w.create_like(tmp_path("hlas_bad_full.laz").c_str(), wave,
                            HLas::WriteMode::Full) == HLas::Status::UnsupportedPointFormat);

        HLas::Header extra = ch;
        extra.point_record_length = 40;          // extra bytes kräver Byte14
        HLas::LazWriter w2;
        CHECK(w2.create_like(tmp_path("hlas_bad_eb.laz").c_str(), extra,
                             HLas::WriteMode::Full) == HLas::Status::UnsupportedPointFormat);
        testdata::remove_utf8(tmp_path("hlas_bad_full.laz"));
        testdata::remove_utf8(tmp_path("hlas_bad_eb.laz"));
    }

    // --- Samma sak okomprimerat ---
    // LasWriter har en egen väg för FULL, och den skriver posterna rakt ut utan
    // kodare. Att den ger samma resultat är inte självklart: headern måste
    // skrivas om när formatet blir känt, och postlängden styr allt efter det.
    {
        const std::string las_path = tmp_path("hlas_full.las");
        HLas::LasWriter w;
        CHECK(w.create_like(las_path.c_str(), ch, HLas::WriteMode::Full) == HLas::Status::Ok);
        CHECK(w.write(src) == HLas::Status::Ok);
        CHECK(w.close() == HLas::Status::Ok);

        HLas::Reader lr;
        CHECK(lr.open(las_path.c_str()) == HLas::Status::Ok);
        if (lr.is_open()) {
            CHECK(lr.header().point_format == 7);
            CHECK(lr.header().point_record_length == 36);
            CHECK(lr.header().point_count == n);
            const auto lv = lr.points();
            uint64_t bad = 0;
            for (uint64_t i = 0; i < n && i < lv.count(); ++i)
                if (std::memcmp(src.record(i), lv.record(i), 36) != 0) ++bad;
            CHECK(bad == 0);
            printf("  LasWriter FULL: %llu punkter, %llu avvikande råposter\n",
                   (unsigned long long)n, (unsigned long long)bad);
            lr.close();
        }
        testdata::remove_utf8(las_path);
    }

    // --- Återanvänd skrivare ---
    // Samma objekt efter close(): FULL-tillståndet MÅSTE nollställas av
    // create(). Gör det inte det annonserar headern format 7 medan LITE skriver
    // 30-bytesposter, och LAZ-vägen läser dessutom RGB förbi postens slut.
    {
        const std::string reuse = tmp_path("hlas_reuse.laz");
        HLas::LazWriter w;
        CHECK(w.create_like(reuse.c_str(), ch, HLas::WriteMode::Full) == HLas::Status::Ok);
        CHECK(w.write(src) == HLas::Status::Ok);
        CHECK(w.close() == HLas::Status::Ok);

        // Samma objekt igen, nu LITE.
        CHECK(w.create_like(reuse.c_str(), ch) == HLas::Status::Ok);
        double xs[3] = { ch.min_x, ch.min_x + 1, ch.min_x + 2 };
        double ys[3] = { ch.min_y, ch.min_y + 1, ch.min_y + 2 };
        double zs[3] = { 10, 11, 12 };
        uint8_t cs[3] = { 2, 2, 5 };
        CHECK(w.write(xs, ys, zs, cs, 3) == HLas::Status::Ok);
        CHECK(w.close() == HLas::Status::Ok);

        HLas::Reader rr;
        CHECK(rr.open(reuse.c_str()) == HLas::Status::Ok);
        if (rr.is_open()) {
            CHECK(rr.header().point_format == 6);
            CHECK(rr.header().point_record_length == 30);
            CHECK(rr.header().point_count == 3);
            uint64_t sum = 0;
            for (int i = 0; i < 15; ++i) sum += rr.header().points_by_return[i];
            CHECK(sum == 3);        // inte förorenad av FULL-skrivningen innan
            rr.close();
        }
        testdata::remove_utf8(reuse);
    }

    // FULL utan källa går inte: create() har inget format att ärva.
    {
        HLas::LasWriter::Options o;
        o.mode = HLas::WriteMode::Full;
        HLas::LazWriter w;
        CHECK(w.create(tmp_path("hlas_nosrc.laz").c_str(), o) != HLas::Status::Ok);
        HLas::LasWriter w2;
        CHECK(w2.create(tmp_path("hlas_nosrc.las").c_str(), o) != HLas::Status::Ok);
        testdata::remove_utf8(tmp_path("hlas_nosrc.laz"));
        testdata::remove_utf8(tmp_path("hlas_nosrc.las"));
    }

    printf("  format 3 och extra bytes avvisas i FULL, FULL kräver create_like,\n"
           "    och en återanvänd skrivare nollställs\n");
    return fails;
#endif
}

// FULL ur en KÄLLA I PUNKTFORMAT 0-3 — de gamla formatens hela väg.
//
// Ett annat filupplägg än format 7 i varje led: LAS 1.2-header om 227 byte,
// LAZ-VLR med compressor 2 och en itemlista som växer med formatet, och
// chunkar utan lagerindelning där alla items skriver i samma aritmetiska ström.
// Går något av det fel blir filen antingen oläsbar eller — värre — läsbar med
// fel innehåll, så testet jämför både filens BYTE på de ställen som styr
// tolkningen och varje post.
//
// Källan är den OKOMPRIMERADE autzen.las, som är punktformat 3 och mmap:as
// direkt. Att gå via .laz hade kostat en full uppackning av 10,6 miljoner
// punkter för samma jämförelse. Ur den byggs poster i format 0, 1 och 2 genom
// att plocka bort GPS-tid respektive färg — så itemlistan prövas för alla fyra
// formaten och inte bara det källan råkar ha.
namespace {

uint16_t le16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
uint32_t le32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

// Läser filens första `n` byte. Headern och VLR:n ligger där, och de går inte
// att kontrollera genom Reader — den tolkar dem, och ett fel i tolkningen skulle
// då dölja ett fel i skrivningen.
bool read_head(const std::string& path, std::vector<uint8_t>& out, size_t n) {
    out.assign(n, 0);
    std::FILE* f = testdata::fopen_utf8(path, "rb");
    if (!f) return false;
    const size_t got = std::fread(out.data(), 1, n, f);
    std::fclose(f);
    out.resize(got);
    return got == n;
}

} // namespace

int test_write_full_legacy() {
    int fails = 0;
    printf("test_write_full_legacy:\n");
    SKIP_IF_MISSING(testdata::kLas);

    HLas::Reader r;
    CHECK(r.open(testdata::kLas) == HLas::Status::Ok);
    if (!r.is_open()) return fails;

    const auto& sh = r.header();
    if (sh.point_format != 3) {
        printf("  SKIP — testfilen är punktformat %u, inte 3\n", sh.point_format);
        return fails;
    }

    // Ett urval, inte hela filen. Antalet är MEDVETET inte en multipel av
    // chunkstorleken 50000: sista chunken blir partiell, vilket är en egen
    // kodväg både i skrivaren och i chunk-tabellen.
    constexpr uint64_t kN = 250123;
    const uint64_t n = (sh.point_count < kN) ? sh.point_count : kN;
    std::vector<uint8_t> src3(static_cast<size_t>(n) * sh.point_record_length);
    CHECK(r.read_points(0, n, src3.data()) == n);

    struct Fmt {
        uint8_t  format;
        uint16_t rec_len;
        bool     gps;
        bool     rgb;
        uint16_t items;      // förväntad itemlista i LAZ-VLR:n
    };
    const Fmt formats[] = {
        { 0, 20, false, false, 1 },
        { 1, 28, true,  false, 2 },
        { 2, 26, false, true,  2 },
        { 3, 34, true,  true,  3 },
    };

    for (const Fmt& fm : formats) {
        // Bygg posterna i målformatet ur format 3-posten: kärnan är alltid de
        // första 20 byten, GPS-tiden ligger vid 20 och färgen vid 28.
        std::vector<uint8_t> recs(static_cast<size_t>(n) * fm.rec_len);
        for (uint64_t i = 0; i < n; ++i) {
            const uint8_t* s = src3.data() + static_cast<size_t>(i) * 34;
            uint8_t* d = recs.data() + static_cast<size_t>(i) * fm.rec_len;
            std::memcpy(d, s, 20);
            uint16_t off = 20;
            if (fm.gps) { std::memcpy(d + off, s + 20, 8); off += 8; }
            if (fm.rgb) std::memcpy(d + off, s + 28, 6);
        }

        HLas::Header src_hdr = sh;
        src_hdr.point_format = fm.format;
        src_hdr.point_record_length = fm.rec_len;
        // Sätts för att pröva att de ÄRVS. Bit 0 är GPS-tidens typ och ska
        // följa med; bit 4 är WKT, som inte finns i LAS 1.2 och ska maskas bort.
        src_hdr.global_encoding = 0x0011;
        src_hdr.file_source_id = 4711;

        const HLas::PointView src(recs.data(), n, fm.rec_len, fm.format,
                                  sh.x_scale, sh.y_scale, sh.z_scale,
                                  sh.x_offset, sh.y_offset, sh.z_offset);
        CHECK(src.valid());
        CHECK(src.has_gps_time() == fm.gps);
        CHECK(src.has_rgb() == fm.rgb);

        for (int compressed = 0; compressed < 2; ++compressed) {
#ifndef HLAS_HAVE_LAZ
            // LazWriter finns inte i ett bygge utan LAZ och svarar
            // LazNotSupported. Den okomprimerade vägen prövas ändå.
            if (compressed) continue;
#endif
            char name[64];
            std::snprintf(name, sizeof name, "hlas_full%u.%s",
                          fm.format, compressed ? "laz" : "las");
            const std::string path = tmp_path(name);

            if (compressed) {
                HLas::LazWriter w;
                const HLas::Status st =
                    w.create_like(path.c_str(), src_hdr, HLas::WriteMode::Full);
                CHECK(st == HLas::Status::Ok);
                if (st != HLas::Status::Ok) continue;
                CHECK(w.write(src) == HLas::Status::Ok);
                CHECK(w.close() == HLas::Status::Ok);
            } else {
                HLas::LasWriter w;
                const HLas::Status st =
                    w.create_like(path.c_str(), src_hdr, HLas::WriteMode::Full);
                CHECK(st == HLas::Status::Ok);
                if (st != HLas::Status::Ok) continue;
                CHECK(w.write(src) == HLas::Status::Ok);
                CHECK(w.close() == HLas::Status::Ok);
            }

            // --- Filens egna byte, inte Readers tolkning av dem ---
            {
                std::vector<uint8_t> head;
                CHECK(read_head(path, head, 400));
                if (head.size() == 400) {
                    CHECK(std::memcmp(head.data(), "LASF", 4) == 0);
                    CHECK(le16(head.data() + 4) == 4711);        // file_source_id ärvd
                    CHECK(le16(head.data() + 6) == 0x0001);      // bit 0 kvar, bit 4 maskad
                    CHECK(head[24] == 1 && head[25] == 2);       // LAS 1.2
                    CHECK(le16(head.data() + 94) == 227);        // headerstorlek
                    CHECK(head[104] == (compressed ? (fm.format | 0x80) : fm.format));
                    CHECK(le16(head.data() + 105) == fm.rec_len);
                    CHECK(le32(head.data() + 107) == static_cast<uint32_t>(n));
                    if (compressed) {
                        CHECK(le32(head.data() + 100) == 1);     // en VLR
                        const uint16_t payload =
                            static_cast<uint16_t>(34 + 6 * fm.items);
                        CHECK(le32(head.data() + 96) == 227u + 54u + payload);
                        const uint8_t* v = head.data() + 227;
                        CHECK(le16(v + 18) == 22204);            // LAZ-VLR
                        CHECK(le16(v + 20) == payload);
                        const uint8_t* pl = v + 54;
                        CHECK(le16(pl + 0) == 2);                // compressor: Chunked
                        CHECK(le16(pl + 32) == fm.items);
                        // Itemlistan i kodningsordning: Point10, GPSTime11, RGB12.
                        int k = 0;
                        CHECK(le16(pl + 34 + 6 * k) == 6 && le16(pl + 36 + 6 * k) == 20 &&
                              le16(pl + 38 + 6 * k) == 2);
                        ++k;
                        if (fm.gps) {
                            CHECK(le16(pl + 34 + 6 * k) == 7 && le16(pl + 36 + 6 * k) == 8 &&
                                  le16(pl + 38 + 6 * k) == 2);
                            ++k;
                        }
                        if (fm.rgb) {
                            CHECK(le16(pl + 34 + 6 * k) == 8 && le16(pl + 36 + 6 * k) == 6 &&
                                  le16(pl + 38 + 6 * k) == 2);
                        }
                    } else {
                        CHECK(le32(head.data() + 100) == 0);     // inga VLR:er
                        CHECK(le32(head.data() + 96) == 227);
                    }
                }
            }

            // Den okomprimerade filen ska vara EXAKT header + poster. Se
            // truncate_to() i las_writer.cpp — utan trunkering bär den rester av
            // den 375-bytes header som create() skrev innan formatet var känt.
            if (!compressed) {
                std::FILE* f = testdata::fopen_utf8(path, "rb");
                if (f) {
                    std::fseek(f, 0, SEEK_END);
                    const long long len = std::ftell(f);
                    std::fclose(f);
                    CHECK(len == 227LL + static_cast<long long>(n) * fm.rec_len);
                }
            }

            // --- Posterna ---
            HLas::Reader back;
            CHECK(back.open(path.c_str()) == HLas::Status::Ok);
            if (!back.is_open()) { testdata::remove_utf8(path); continue; }

            const auto& bh = back.header();
            CHECK(bh.point_format == fm.format);
            CHECK(bh.point_record_length == fm.rec_len);
            CHECK(bh.point_count == n);
            CHECK(bh.x_scale == sh.x_scale && bh.x_offset == sh.x_offset);

            const auto bv = back.points();
            CHECK(bv.count() == n);
            uint64_t bad_raw = 0;
            for (uint64_t i = 0; i < n && i < bv.count(); ++i)
                if (std::memcmp(src.record(i), bv.record(i), fm.rec_len) != 0)
                    ++bad_raw;
            CHECK(bad_raw == 0);
            printf("  format %u %s: %llu punkter, %u items, %llu avvikande råposter\n",
                   fm.format, compressed ? "LAZ" : "LAS",
                   (unsigned long long)n, fm.items, (unsigned long long)bad_raw);
            back.close();
            testdata::remove_utf8(path);
        }
    }

    // Punktformat 4 och 5 kräver Wavepacket13 och ska AVVISAS, inte tyst
    // reduceras. Samma för 8-10.
    {
        HLas::Header h4 = sh;
        h4.point_format = 4;
        h4.point_record_length = 57;
        HLas::LasWriter lw;
        CHECK(lw.create_like(tmp_path("hlas_fmt4.las").c_str(), h4, HLas::WriteMode::Full)
              == HLas::Status::UnsupportedPointFormat);
#ifdef HLAS_HAVE_LAZ
        HLas::LazWriter w;
        CHECK(w.create_like(tmp_path("hlas_fmt4.laz").c_str(), h4, HLas::WriteMode::Full)
              == HLas::Status::UnsupportedPointFormat);
#endif
    }

    return fails;
}
