#ifdef HLAS_HAVE_LAZ
#include "point14_encoder.h"
#include <cassert>
#include <cmath>
#include <cstring>

namespace HLas::detail::laz {

// Tabellerna hör till avkodaren; de deklareras i point14.h och definieras i
// point14.cpp. Kodaren använder SAMMA tabeller — två kopior kan glida isär,
// en kan inte.

namespace {

// Bygger den 30-bytespost en Point14Fields motsvarar. Identisk med det
// encode_point6() i write/point_encode.h lägger i filen — retur 1 av 1, allt
// annat noll — vilket är precis varför lagren kan utelämnas i LITE.
void to_item(const Point14Fields& f, uint8_t* out) {
    std::memset(out, 0, kPoint14Size);
    set_X(out, f.x);
    set_Y(out, f.y);
    set_Z(out, f.z);
    out[OFF_RETURN_INFO]    = 0x11;   // retur 1 av 1
    out[OFF_CLASSIFICATION] = f.classification;
}

// GPS-tiden behandlas som ett 64-bitars bitmönster, inte som ett tal. Det är
// avsiktligt: differensen mellan två tider räknas på bitmönstren, vilket ger små
// tal för närliggande tider oavsett var i flyttalsområdet de ligger.
inline int64_t d2i(double v) { int64_t r; std::memcpy(&r, &v, 8); return r; }
inline uint64_t d2u(double v) { uint64_t r; std::memcpy(&r, &v, 8); return r; }

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

} // namespace

// Spegel av Point14Decoder::init_context. Samma modeller, samma bredder, samma
// ordning — bara riktningen skiljer.
void Point14Encoder::init_context(uint32_t ctx, const uint8_t* seed_item) {
    auto& c = contexts_[ctx];
    c.unused = false;

    std::memcpy(c.last_item, seed_item, kPoint14Size);

    for (auto& v : c.last_intensity) v = get_intensity(seed_item);
    for (auto& sm : c.last_X_diff_median5) sm.reset();
    for (auto& sm : c.last_Y_diff_median5) sm.reset();
    for (auto& z : c.last_Z) z = get_Z(seed_item);

    for (auto& m : c.m_changed_values) m.init(128);
    c.m_scanner_channel.init(3);
    for (auto& m : c.m_number_of_returns) m.init(16);
    c.m_return_number_gps_same.init(13);
    for (auto& m : c.m_return_number) m.init(16);

    c.ic_dX.init(enc_xy_, 32, 2);
    c.ic_dY.init(enc_xy_, 32, 22);
    c.ic_Z.init(enc_z_, 32, 20);

    for (auto& m : c.m_classification) m.init(256);
    for (auto& m : c.m_flags) m.init(64);
    c.ic_intensity.init(enc_intensity_, 16, 4);
    c.ic_scan_angle.init(enc_scan_angle_, 16, 2);
    for (auto& m : c.m_user_data) m.init(256);
    c.ic_point_source.init(enc_point_source_, 16, 1);

    c.last = 0;
    c.next = 0;
    // BARA fack 0 får seed-tiden — se motsvarande kommentar i Point14Decoder.
    // findSeq väljer det första facket vars differens ryms i en int32, så tre
    // extra fack med seed-tiden i skulle ge en giltig men annorlunda ström.
    c.last_gpstime[0].f64 = get_gps_time(seed_item);
    for (int i = 1; i < 4; ++i) c.last_gpstime[i].u64 = 0;
    for (auto& d : c.last_gpstime_diff) d = 0;
    for (auto& e : c.multi_extreme_counter) e = 0;
    c.m_gpstime_multi.init(GPSTIME_MULTI_TOTAL);
    c.m_gpstime_0diff.init(5);
    c.ic_gpstime.init(enc_gps_, 32, 9);

    c.gps_time_change = false;
}

// Väljer som referensens kodare gör — inte en spegling av avkodaren.
//
// Skillnaden är hela poängen med den här funktionen. Avkodningen LÄSER ett
// multi-värde och gör vad det säger. Kodningen måste VÄLJA det, och två val
// finns: vilken av fyra sekvenser tiden hör till, och vilken multiplikator som
// bäst beskriver steget. Inget av det går att härleda ur avkodaren — den ser
// bara resultatet.
//
// Sekvenserna finns för att en fil kan blanda flera skannerlinjer, där tiderna
// hoppar fram och tillbaka. Fyra fack räcker för att en punkt nästan alltid ska
// hitta ett fack vars differens ryms i en int32, och då blir steget litet att
// koda i stället för ett helt 64-bitarsvärde.
void Point14Encoder::encode_gps_time(Point14Context& c, double gps) {
    // Söker det första facket från `start` vars differens ryms i en int32.
    // Returnerar avståndet framåt, eller -1 om inget gör det.
    auto find_seq = [&c, gps](int start, int32_t& diff) -> int {
        for (int i = start; i < 4; ++i) {
            const int testseq = (static_cast<int>(c.last) + i) & 3;
            const int64_t diff64 = IDiff64(d2i(gps), d2i(c.last_gpstime[testseq].f64));
            diff = static_cast<int32_t>(diff64);
            if (diff64 == diff)
                return i;
        }
        return -1;
    };

    for (;;) {
        if (c.last_gpstime_diff[c.last] == 0) {
            // Föregående differens är noll: antingen är det här den andra punkten
            // i en sekvens, eller så nollställdes den av ett sekvensbyte.
            int32_t diff = 0;
            const int idx = find_seq(0, diff);

            if (idx == 0) {
                enc_gps_.encodeSymbol(c.m_gpstime_0diff, 0);
                c.ic_gpstime.compress(0, diff, 0);
                c.last_gpstime_diff[c.last] = diff;
                c.multi_extreme_counter[c.last] = 0;
            } else if (idx > 0) {
                // Ett annat fack passar bättre. Byt dit och räkna om — därav
                // loopen. Avkodaren gör samma sak rekursivt.
                enc_gps_.encodeSymbol(c.m_gpstime_0diff,
                                      static_cast<uint32_t>(idx + 1));
                c.last = (c.last + static_cast<uint32_t>(idx)) & 3;
                continue;
            } else {
                // Inget fack räcker till: skriv hela tiden som 64 bitar, i ett
                // nytt fack. Den höga halvan kodas mot föregående punkts höga
                // halva, den låga skrivs rått — den är i praktiken slumpmässig.
                enc_gps_.encodeSymbol(c.m_gpstime_0diff, 1);
                c.ic_gpstime.compress(
                    static_cast<int32_t>(d2u(c.last_gpstime[c.last].f64) >> 32),
                    static_cast<int32_t>(d2u(gps) >> 32), 8);
                enc_gps_.writeInt(static_cast<uint32_t>(d2u(gps)));
                c.next = (c.next + 1) & 3;
                c.last = c.next;
                c.last_gpstime_diff[c.last] = 0;
                c.multi_extreme_counter[c.last] = 0;
            }
            c.last_gpstime[c.last].f64 = gps;
            return;
        }

        const int64_t diff64 = IDiff64(d2i(gps), d2i(c.last_gpstime[c.last].f64));
        const int32_t diff   = static_cast<int32_t>(diff64);

        if (diff64 != diff) {
            // För stort steg för en int32. Pröva de andra facken innan vi ger
            // upp och skriver hela värdet.
            int32_t d2 = 0;
            const int idx = find_seq(1, d2);
            if (idx > 0) {
                enc_gps_.encodeSymbol(
                    c.m_gpstime_multi,
                    static_cast<uint32_t>(GPSTIME_MULTI_CODE_FULL + idx));
                c.last = (c.last + static_cast<uint32_t>(idx)) & 3;
                continue;
            }
            enc_gps_.encodeSymbol(c.m_gpstime_multi,
                                  static_cast<uint32_t>(GPSTIME_MULTI_CODE_FULL));
            c.ic_gpstime.compress(
                static_cast<int32_t>(d2u(c.last_gpstime[c.last].f64) >> 32),
                static_cast<int32_t>(d2u(gps) >> 32), 8);
            enc_gps_.writeInt(static_cast<uint32_t>(d2u(gps)));
            c.next = (c.next + 1) & 3;
            c.last = c.next;
            c.last_gpstime_diff[c.last] = 0;
            c.multi_extreme_counter[c.last] = 0;
            c.last_gpstime[c.last].f64 = gps;
            return;
        }

        // Steget ryms. Hur många gånger föregående steg är det?
        //
        // CASTET ÄR (float), ENKEL PRECISION, och det är inte en slarvighet att
        // rätta. Räknas kvoten i double väljs ibland ett annat multi, och då
        // avkodas filen fortfarande RÄTT men byten skiljer sig från referensens.
        // Byte-identitet hänger på den här raden.
        const float ratio = static_cast<float>(diff)
                          / static_cast<float>(c.last_gpstime_diff[c.last]);
        const float rounded = std::round(ratio);

        // AVSIKTLIG AVVIKELSE, den andra i kodaren. Referensen castar `rounded`
        // rakt till int32. Kvoten kan bli större än int32 rymmer — det räcker att
        // föregående steg var 1 och det här är stort — och då är castet
        // odefinierat beteende. Vi klämmer i stället.
        //
        // För varje kvot som ryms är resultatet identiskt. För de som inte ryms
        // hamnar vi i grenen multi >= GPSTIME_MULTI respektive
        // multi <= GPSTIME_MULTI_MINUS, vilket är den gren ett rimligt stort tal
        // hade hamnat i ändå.
        int32_t multi;
        if (rounded >= 2147483520.0f)       multi = INT32_MAX;
        else if (rounded <= -2147483648.0f) multi = INT32_MIN;
        else                                multi = static_cast<int32_t>(rounded);

        const int32_t last_diff = c.last_gpstime_diff[c.last];

        if (multi > 0 && multi < GPSTIME_MULTI) {
            // Vanligaste fallet vid jämnt pulsad skanner: multi == 1.
            const uint32_t tag = (multi == 1) ? 1u : (multi < 10 ? 2u : 3u);
            enc_gps_.encodeSymbol(c.m_gpstime_multi, static_cast<uint32_t>(multi));
            c.ic_gpstime.compress(IMul32(multi, last_diff), diff, tag);
            if (tag == 1)
                c.multi_extreme_counter[c.last] = 0;
        } else if (multi >= GPSTIME_MULTI) {
            enc_gps_.encodeSymbol(c.m_gpstime_multi,
                                  static_cast<uint32_t>(GPSTIME_MULTI));
            c.ic_gpstime.compress(IMul32(GPSTIME_MULTI, last_diff), diff, 4);
            if (++c.multi_extreme_counter[c.last] > 3) {
                c.multi_extreme_counter[c.last] = 0;
                c.last_gpstime_diff[c.last] = diff;
            }
        } else if (multi < 0 && multi > GPSTIME_MULTI_MINUS) {
            enc_gps_.encodeSymbol(c.m_gpstime_multi,
                                  static_cast<uint32_t>(GPSTIME_MULTI - multi));
            c.ic_gpstime.compress(IMul32(multi, last_diff), diff, 5);
        } else if (multi <= GPSTIME_MULTI_MINUS) {
            enc_gps_.encodeSymbol(
                c.m_gpstime_multi,
                static_cast<uint32_t>(GPSTIME_MULTI - GPSTIME_MULTI_MINUS));
            c.ic_gpstime.compress(IMul32(GPSTIME_MULTI_MINUS, last_diff), diff, 6);
            if (++c.multi_extreme_counter[c.last] > 3) {
                c.multi_extreme_counter[c.last] = 0;
                c.last_gpstime_diff[c.last] = diff;
            }
        } else {
            // multi == 0: steget är mycket mindre än föregående. Egen kontext,
            // eftersom värdena då ser helt annorlunda ut.
            enc_gps_.encodeSymbol(c.m_gpstime_multi, 0);
            c.ic_gpstime.compress(0, diff, 7);
            if (++c.multi_extreme_counter[c.last] > 3) {
                c.multi_extreme_counter[c.last] = 0;
                c.last_gpstime_diff[c.last] = diff;
            }
        }

        c.last_gpstime[c.last].f64 = gps;
        return;
    }
}

void Point14Encoder::start_chunk(const Point14Fields& seed) {
    uint8_t item[kPoint14Size];
    to_item(seed, item);
    start_chunk_item(item);
}

void Point14Encoder::start_chunk_item(const uint8_t* seed_item) {
    // Att starta om mitt i en chunk skulle kasta det som kodats utan att någon
    // märkte det. Kontrollen gäller även i release — skrivaren roterar chunkar
    // och har felvägar, och där är det här lätt att göra fel.
    assert(!in_chunk_);
    if (in_chunk_) { failed_ = true; return; }

    enc_xy_.init();
    enc_z_.init();
    enc_class_.init();
    enc_flags_.init();
    enc_intensity_.init();
    enc_scan_angle_.init();
    enc_user_data_.init();
    enc_point_source_.init();
    enc_gps_.init();

    for (auto& c : contexts_) c.unused = true;
    current_context_ = get_scanner_channel(seed_item);
    init_context(current_context_, seed_item);

    valid_class_ = valid_flags_ = valid_intensity_ = valid_scan_angle_ =
        valid_user_data_ = valid_point_source_ = valid_gps_ = false;

    in_chunk_ = true;
    failed_ = false;
}

void Point14Encoder::encode(const Point14Fields& p) {
    uint8_t item[kPoint14Size];
    to_item(p, item);
    encode_item(item);
}

// Spegel av Point14Decoder::decompress. Ordningen mellan prediktion, kodning och
// tillståndsuppdatering måste vara exakt densamma — inte bara samma beräkningar.
void Point14Encoder::encode_item(const uint8_t* item) {
    assert(in_chunk_);
    if (!in_chunk_ || failed_) { failed_ = true; return; }

    const uint32_t sc = get_scanner_channel(item);

    // prev är kontexten för FÖREGÅENDE punkt. Den äger changed_values-modellen
    // och kanalmodellen, även när den här punkten byter kanal.
    Point14Context& prev = contexts_[current_context_];

    // lpr: single(1) / last(2) / gps_time_change(4), räknat ur FÖREGÅENDE punkt.
    const uint8_t prev_r = get_return_number(prev.last_item);
    const uint8_t prev_n = get_number_of_returns(prev.last_item);
    int32_t lpr = (prev_r == 1 ? 1 : 0);
    lpr += (prev_r >= prev_n ? 2 : 0);
    lpr += (prev.gps_time_change ? 4 : 0);

    // "old" är den här kanalens senaste punkt om kanalen använts, annars
    // föregående kanals. Ändringsbitarna räknas mot DEN, inte mot prev — de två
    // skiljer sig bara vid ett byte till en ännu oanvänd kanal.
    const uint8_t* old_item = contexts_[sc].unused ? prev.last_item
                                                   : contexts_[sc].last_item;

    const double   old_gps  = get_gps_time(old_item);
    const uint16_t old_psid = get_point_source(old_item);
    const int16_t  old_ang  = get_scan_angle(old_item);
    const uint32_t last_n   = get_number_of_returns(old_item);
    const uint32_t last_r   = get_return_number(old_item);
    const uint32_t n        = get_number_of_returns(item);
    const uint32_t r        = get_return_number(item);

    // Jämförelsen är NUMERISK, inte på bitmönster, precis som referensens.
    // Två följder av det, båda avsiktliga:
    //
    //   NaN != NaN, så en NaN-tid räknas alltid som ändrad. Ofarligt: den kodas
    //   som ett fullt 64-bitarsvärde och kommer ut likadan.
    //
    //   +0.0 == -0.0, så ett teckenbyte på noll räknas som OFÖRÄNDRAT. Posten
    //   skulle då komma ut med fel tecken, och det fångas av memcmp-kontrollen
    //   längst ner som sätter failed_. Att i stället jämföra bitmönster hade
    //   gjort fallet kodningsbart men brutit byte-identiteten mot referensen för
    //   varje fil som innehåller det — och en GPS-tid på exakt minus noll är
    //   inte data någon menade att skriva.
    const bool gps_time_change     = (get_gps_time(item) != old_gps);
    const bool point_source_change = (get_point_source(item) != old_psid);
    const bool scan_angle_change   = (get_scan_angle(item) != old_ang);

    const bool rn_increments = (r == ((last_r + 1) % 16));
    const bool rn_decrements = (r == ((last_r + 15) % 16));
    const bool rn_misc       = (r != last_r) && !rn_increments && !rn_decrements;

    const int32_t changed_values =
          (static_cast<int32_t>(rn_increments || rn_misc) << 0)
        | (static_cast<int32_t>(rn_decrements || rn_misc) << 1)
        | (static_cast<int32_t>(n != last_n)              << 2)
        | (static_cast<int32_t>(scan_angle_change)        << 3)
        | (static_cast<int32_t>(gps_time_change)          << 4)
        | (static_cast<int32_t>(point_source_change)      << 5)
        | (static_cast<int32_t>(sc != current_context_)   << 6);

    enc_xy_.encodeSymbol(prev.m_changed_values[lpr],
                         static_cast<uint32_t>(changed_values));

    // Kanalbytet kodas som avståndet framåt modulo 4, minus ett — avkodaren
    // räknar (current + diff + 1) % 4 och kan därför aldrig få "samma kanal".
    if (sc > current_context_)
        enc_xy_.encodeSymbol(prev.m_scanner_channel, sc - current_context_ - 1);
    else if (sc < current_context_)
        enc_xy_.encodeSymbol(prev.m_scanner_channel, sc - current_context_ - 1 + 4);

    // En kanal som inte setts förut ärver föregående kanals tillstånd. Det sker
    // EFTER att changed_values och kanalsymbolen kodats, eftersom de gick till
    // prev:s modeller.
    if (contexts_[sc].unused)
        init_context(sc, prev.last_item);

    current_context_ = sc;
    Point14Context& c = contexts_[sc];
    uint8_t* last_item = c.last_item;
    set_scanner_channel(last_item, static_cast<uint8_t>(sc));

    if (n != last_n) {
        enc_xy_.encodeSymbol(c.m_number_of_returns[last_n], n);
        set_number_of_returns(last_item, static_cast<uint8_t>(n));
    }

    if (rn_increments || rn_decrements || rn_misc)
        set_return_number(last_item, static_cast<uint8_t>(r));

    if (rn_misc) {
        if (gps_time_change) {
            enc_xy_.encodeSymbol(c.m_return_number[last_r], r);
        } else {
            // Differensen kodas som symbol 0..12, vilket är varför modellen har
            // tretton symboler: hopp på 1 och -1 har egna bitar i changed_values
            // och kommer aldrig hit.
            //
            // AVSIKTLIG AVVIKELSE FRÅN REFERENSEN, och den enda i hela kodaren.
            // Uttrycket är referensens, byte för byte, INKLUSIVE dess fel: för
            // diff == -15 ger det -1, som konverterat till uint32 blir
            // UINT32_MAX. Referensen har en kommentar om att det är trasigt och
            // indexerar sedan utanför modellen — odefinierat beteende.
            //
            // Vi räknar likadant, så den kodade symbolen är identisk i alla
            // giltiga fall. Men encodeSymbol avvisar ett symbolvärde utanför
            // modellen och sätter failed_, i stället för att läsa ur minnet
            // bredvid. Att spegla UB är inte att spegla ett format.
            //
            // Grenen är dessutom OÅTKOMLIG i praktiken, vilket är värt att veta
            // innan någon försöker skriva ett test för den: diff == -15 kräver
            // last_r == 15 och r == 0, men (15 + 1) % 16 == 0, så det fallet
            // klassas som en ÖKNING och tas av bit 0 i changed_values långt
            // innan rn_misc blir sant. Referensens egen kommentar om att koden är
            // trasig gäller alltså kod som aldrig körs.
            const int32_t diff = static_cast<int32_t>(r) - static_cast<int32_t>(last_r);
            const uint32_t sym = static_cast<uint32_t>(diff > 1 ? diff - 2 : diff - 2 + 16);
            enc_xy_.encodeSymbol(c.m_return_number_gps_same, sym);
        }
    }

    const uint32_t m = number_return_map_6ctx[n][r];
    const uint32_t l = number_return_level_8ctx[n][r];
    const int32_t cpr = (r == 1 ? 2 : 0) + (r >= n ? 1 : 0);

    // === X ===
    const uint32_t xy_idx = (m << 1) | (gps_time_change ? 1u : 0u);
    int32_t median = c.last_X_diff_median5[xy_idx].median();
    int32_t diff = IDiff32(get_X(item), get_X(last_item));
    c.ic_dX.compress(median, diff, n == 1 ? 1 : 0);
    set_X(last_item, get_X(item));
    c.last_X_diff_median5[xy_idx].insert(diff);

    // === Y ===
    median = c.last_Y_diff_median5[xy_idx].median();
    const uint32_t kx = c.ic_dX.last_k();
    const uint32_t dy_ctx = (n == 1 ? 1u : 0u) + (kx < 20 ? u32_zero_bit_0(kx) : 20u);
    diff = IDiff32(get_Y(item), get_Y(last_item));
    c.ic_dY.compress(median, diff, dy_ctx);
    set_Y(last_item, get_Y(item));
    c.last_Y_diff_median5[xy_idx].insert(diff);

    // === Z ===
    const uint32_t ky = c.ic_dY.last_k();
    const uint32_t kxy = (kx + ky) / 2;
    const uint32_t z_ctx = (n == 1 ? 1u : 0u) + (kxy < 18 ? u32_zero_bit_0(kxy) : 18u);
    c.ic_Z.compress(c.last_Z[l], get_Z(item), z_ctx);
    set_Z(last_item, get_Z(item));
    c.last_Z[l] = get_Z(item);

    // === Klassificering ===
    // Kodas för varje punkt oavsett om den ändrats; det är bara utskriften av
    // lagret som är villkorlig. Kodar man selektivt hamnar modellerna ur fas med
    // avkodarens, som läser varje punkt så snart lagret finns.
    {
        const uint32_t last_cls = last_item[OFF_CLASSIFICATION];
        const int32_t ccc = static_cast<int32_t>((last_cls & 0x1F) << 1)
                          + (cpr == 3 ? 1 : 0);
        if (item[OFF_CLASSIFICATION] != last_item[OFF_CLASSIFICATION])
            valid_class_ = true;
        enc_class_.encodeSymbol(c.m_classification[ccc], item[OFF_CLASSIFICATION]);
        last_item[OFF_CLASSIFICATION] = item[OFF_CLASSIFICATION];
    }

    // === Flaggor ===
    // Scannerkanalen ingår INTE: den kodades redan i changed_values-lagret, så
    // de sex kvarvarande bitarna packas ihop. Därav 64 symboler.
    {
        const uint32_t flags      = packed_flags(item);
        const uint32_t last_flags = packed_flags(last_item);
        if (flags != last_flags)
            valid_flags_ = true;
        enc_flags_.encodeSymbol(c.m_flags[last_flags], flags);
        set_packed_flags(last_item, flags);
    }

    // === Intensitet ===
    {
        const uint32_t i_idx = static_cast<uint32_t>(cpr << 1)
                             | (gps_time_change ? 1u : 0u);
        const uint16_t value = get_intensity(item);
        if (value != get_intensity(last_item))
            valid_intensity_ = true;
        c.ic_intensity.compress(c.last_intensity[i_idx], value,
                                static_cast<uint32_t>(cpr));
        c.last_intensity[i_idx] = value;
        set_intensity(last_item, value);
    }

    // === Scanvinkel ===
    // Kodas BARA vid ändring — avkodaren vet det via bit 3 i changed_values.
    if (scan_angle_change) {
        valid_scan_angle_ = true;
        c.ic_scan_angle.compress(get_scan_angle(last_item), get_scan_angle(item),
                                 gps_time_change ? 1 : 0);
        set_scan_angle(last_item, get_scan_angle(item));
    }

    // === User data ===
    {
        const uint32_t ud_ctx = last_item[OFF_USER_DATA] / 4u;
        if (item[OFF_USER_DATA] != last_item[OFF_USER_DATA])
            valid_user_data_ = true;
        enc_user_data_.encodeSymbol(c.m_user_data[ud_ctx], item[OFF_USER_DATA]);
        last_item[OFF_USER_DATA] = item[OFF_USER_DATA];
    }

    // === Point source ===
    if (point_source_change) {
        valid_point_source_ = true;
        c.ic_point_source.compress(get_point_source(last_item),
                                   get_point_source(item), 0);
        set_point_source(last_item, get_point_source(item));
    }

    // === GPS-tid ===
    // Kodas BARA vid ändring, precis som scanvinkeln och point source —
    // avkodaren vet det via bit 4 i changed_values.
    if (gps_time_change) {
        valid_gps_ = true;
        encode_gps_time(c, get_gps_time(item));
        set_gps_time(last_item, c.last_gpstime[c.last].f64);
    }

    c.gps_time_change = gps_time_change;

    // Efter en punkt MÅSTE kodarens last_item vara bit för bit lika med den post
    // som kodades. Är den inte det finns ett fält vi inte skrivit, och då kommer
    // avkodaren att bygga en annan post än den vi fick — tyst, och först synligt
    // långt senare. Kontrollen gäller även i release; den kostar en memcmp per
    // punkt mot att en hel fil blir fel.
    if (std::memcmp(last_item, item, kPoint14Size) != 0)
        failed_ = true;

    if (enc_xy_.failed() || enc_z_.failed() || enc_class_.failed() ||
        enc_flags_.failed() || enc_intensity_.failed() || enc_scan_angle_.failed() ||
        enc_user_data_.failed() || enc_point_source_.failed() || enc_gps_.failed())
        failed_ = true;
}

void Point14Encoder::finish_chunk() {
    assert(in_chunk_);
    if (!in_chunk_) { failed_ = true; return; }
    in_chunk_ = false;

    ArithmeticEncoder* encs[9] = {
        &enc_xy_, &enc_z_, &enc_class_, &enc_flags_, &enc_intensity_,
        &enc_scan_angle_, &enc_user_data_, &enc_point_source_, &enc_gps_
    };
    for (auto* e : encs) {
        e->done();
        if (e->failed()) failed_ = true;
    }

    // Lagerlängderna är 32 bitar i formatet. En tyst trunkering här hade skrivit
    // en för kort längd men appenderat hela lagret, och då är resten av filen ur
    // fas — ett fel som syns först långt senare och någon annanstans.
    constexpr size_t kMaxLayer = 0xFFFFFFFFull;
    for (auto* e : encs)
        if (e->size() > kMaxLayer) failed_ = true;

    // XY och Z har ingen giltighetsbit: de skrivs alltid, precis som referensen.
    const bool valid[9] = { true, true, valid_class_, valid_flags_,
                            valid_intensity_, valid_scan_angle_, valid_user_data_,
                            valid_point_source_, valid_gps_ };
    for (int i = 0; i < 9; ++i)
        sizes_[i] = (!failed_ && valid[i]) ? static_cast<uint32_t>(encs[i]->size()) : 0u;
}

void Point14Encoder::append_sizes(std::vector<uint8_t>& out) const {
    // Har något gått fel skrivs ingenting alls. En halv chunk i bufferten vore
    // värre än ingen: anroparen kan inte skilja den från en giltig.
    if (failed_) return;
    for (uint32_t n : sizes_)
        put_u32(out, n);
}

void Point14Encoder::append_data(std::vector<uint8_t>& out) const {
    if (failed_) return;
    const ArithmeticEncoder* encs[9] = {
        &enc_xy_, &enc_z_, &enc_class_, &enc_flags_, &enc_intensity_,
        &enc_scan_angle_, &enc_user_data_, &enc_point_source_, &enc_gps_
    };
    for (int i = 0; i < 9; ++i)
        if (sizes_[i])
            out.insert(out.end(), encs[i]->bytes().begin(), encs[i]->bytes().end());
}

void Point14Encoder::finish_chunk(std::vector<uint8_t>& out) {
    finish_chunk();
    append_sizes(out);
    append_data(out);
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
