#ifdef HLAS_HAVE_LAZ
#include "gpstime11_encoder.h"
#include <cassert>
#include <cmath>
#include <cstring>

namespace HLas::detail::laz {

namespace {

// Formatets symbolkonstanter. MULTI är 500 och MULTI_MINUS -10, vilket ger
// 511 för "oförändrad", 512 för "fullt värde" och 513-515 för sekvensbyten.
constexpr int32_t  kMulti          = 500;
constexpr int32_t  kMultiMinus     = -10;
constexpr uint32_t kMultiUnchanged = static_cast<uint32_t>(kMulti - kMultiMinus + 1);
constexpr uint32_t kMultiCodeFull  = static_cast<uint32_t>(kMulti - kMultiMinus + 2);

int64_t load_i64(const uint8_t* p) {
    int64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

uint64_t as_u64(int64_t v) { return static_cast<uint64_t>(v); }

} // namespace

void GPSTime11Encoder::init(ArithmeticEncoder& enc) {
    enc_ = &enc;
    first_ = true;
    failed_ = false;
    last_ = 0;
    next_ = 0;

    case_model_.init(516);
    case0_model_.init(6);
    ic_gps_.init(enc, 32, 9);

    for (auto& f : frames_) {
        f.prev_gps_time = 0;
        f.delta = 0;
        f.counter = 0;
    }
}

void GPSTime11Encoder::start_chunk(const uint8_t* seed) {
    assert(enc_ != nullptr);
    if (!enc_) { failed_ = true; return; }
    // En andra start_chunk() utan mellanliggande init() skulle lämna modeller
    // och fack orörda och ge en chunk som avkodas utan fel men till fel tider.
    assert(first_);
    if (!first_) { failed_ = true; return; }

    frames_[0].prev_gps_time = load_i64(seed);
    first_ = false;
}

void GPSTime11Encoder::encode(const uint8_t* item) {
    if (first_) { failed_ = true; return; }
    if (failed()) return;

    const int64_t v = load_i64(item);

    // Söker det första facket från `start` vars differens ryms i en int32.
    // Returnerar avståndet framåt från last_, eller -1 om inget gör det.
    auto find_seq = [this, v](int start, int32_t& diff) -> int {
        for (int i = start; i < 4; ++i) {
            const uint32_t f = (last_ + static_cast<uint32_t>(i)) & 3u;
            const int64_t d64 = IDiff64(v, frames_[f].prev_gps_time);
            diff = static_cast<int32_t>(d64);
            if (d64 == diff)
                return i;
        }
        return -1;
    };

    // Skriver hela 64-bitarsvärdet och tar ett NYTT fack i bruk. Den höga halvan
    // kodas mot föregående facks höga halva, den låga skrivs rått — den är i
    // praktiken slumpmässig.
    auto write_full = [this, v]() {
        ic_gps_.compress(
            static_cast<int32_t>(as_u64(frames_[last_].prev_gps_time) >> 32),
            static_cast<int32_t>(as_u64(v) >> 32), 8);
        enc_->writeInt(static_cast<uint32_t>(as_u64(v)));
        next_ = (next_ + 1) & 3u;
        last_ = next_;
        frames_[last_].delta = 0;
        frames_[last_].counter = 0;
        frames_[last_].prev_gps_time = v;
    };

    for (;;) {
        RefFrame& fr = frames_[last_];

        if (fr.delta == 0) {
            // Andra punkten i en sekvens, eller ett fack som nyss nollställts.
            if (v == fr.prev_gps_time) {
                enc_->encodeSymbol(case0_model_, 0);
                return;
            }

            int32_t diff = 0;
            const int idx = find_seq(0, diff);

            if (idx == 0) {
                enc_->encodeSymbol(case0_model_, 1);
                ic_gps_.compress(0, diff, 0);
                fr.delta = diff;
                fr.counter = 0;
                fr.prev_gps_time = v;
            } else if (idx > 0) {
                // Ett annat fack passar. Byt dit och räkna om — därav loopen.
                // Avkodaren gör samma sak rekursivt.
                enc_->encodeSymbol(case0_model_, static_cast<uint32_t>(idx + 2));
                last_ = (last_ + static_cast<uint32_t>(idx)) & 3u;
                continue;
            } else {
                enc_->encodeSymbol(case0_model_, 2);
                write_full();
            }
            return;
        }

        if (v == fr.prev_gps_time) {
            enc_->encodeSymbol(case_model_, kMultiUnchanged);
            return;
        }

        const int64_t d64  = IDiff64(v, fr.prev_gps_time);
        const int32_t diff = static_cast<int32_t>(d64);

        if (d64 != diff) {
            // För stort steg för en int32. Pröva de andra facken innan vi ger
            // upp och skriver hela värdet.
            int32_t other = 0;
            const int idx = find_seq(1, other);
            if (idx > 0) {
                enc_->encodeSymbol(case_model_,
                                   kMultiCodeFull + static_cast<uint32_t>(idx));
                last_ = (last_ + static_cast<uint32_t>(idx)) & 3u;
                continue;
            }
            enc_->encodeSymbol(case_model_, kMultiCodeFull);
            write_full();
            return;
        }

        // Steget ryms. Hur många gånger föregående steg är det?
        //
        // CASTET ÄR (float), ENKEL PRECISION, och det är inte en slarvighet att
        // rätta. Räknas kvoten i double väljs ibland ett annat multi, och då
        // avkodas filen fortfarande RÄTT men byten skiljer sig från referensens.
        // Byte-identitet hänger på den här raden. Samma fälla som i Point14.
        const float rounded = std::round(static_cast<float>(diff)
                                       / static_cast<float>(fr.delta));

        // AVSIKTLIG AVVIKELSE, samma som i Point14: referensen castar `rounded`
        // rakt till int. Kvoten kan bli större än int32 rymmer — det räcker att
        // föregående steg var 1 och det här är stort — och då är castet
        // odefinierat beteende. Vi klämmer i stället.
        //
        // Klämningen ändrar aldrig vilken GREN som väljs, och det är grenen som
        // bestämmer bytena: ett värde som kläms till INT32_MAX hamnar i samma
        // gren som det otrimmade hade gjort, nämligen multi >= 500. Själva
        // multi-VÄRDET kan däremot skilja sig — 2147483520.0f ryms i en int32
        // men kläms ändå, eftersom villkoret är >= och inte >. Grenen är
        // densamma, och multi används inte i den.
        int32_t multi;
        if (rounded >= 2147483520.0f)       multi = INT32_MAX;
        else if (rounded <= -2147483648.0f) multi = INT32_MIN;
        else                                multi = static_cast<int32_t>(rounded);

        const int32_t last_diff = fr.delta;

        if (multi == 1) {
            // Det vanligaste fallet vid jämnt pulsad skanner.
            enc_->encodeSymbol(case_model_, 1);
            ic_gps_.compress(last_diff, diff, 1);
            fr.counter = 0;
        } else if (multi > 0) {
            if (multi < kMulti) {
                enc_->encodeSymbol(case_model_, static_cast<uint32_t>(multi));
                ic_gps_.compress(IMul32(multi, last_diff), diff, multi < 10 ? 2u : 3u);
            } else {
                enc_->encodeSymbol(case_model_, static_cast<uint32_t>(kMulti));
                ic_gps_.compress(IMul32(kMulti, last_diff), diff, 4);
                if (++fr.counter > 3) { fr.delta = diff; fr.counter = 0; }
            }
        } else if (multi < 0) {
            if (multi > kMultiMinus) {
                enc_->encodeSymbol(case_model_, static_cast<uint32_t>(kMulti - multi));
                ic_gps_.compress(IMul32(multi, last_diff), diff, 5);
            } else {
                enc_->encodeSymbol(case_model_,
                                   static_cast<uint32_t>(kMulti - kMultiMinus));
                ic_gps_.compress(IMul32(kMultiMinus, last_diff), diff, 6);
                if (++fr.counter > 3) { fr.delta = diff; fr.counter = 0; }
            }
        } else {
            // multi == 0: steget är litet mot föregående, men inte noll.
            enc_->encodeSymbol(case_model_, 0);
            ic_gps_.compress(0, diff, 7);
            if (++fr.counter > 3) { fr.delta = diff; fr.counter = 0; }
        }

        fr.prev_gps_time = v;
        return;
    }
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
