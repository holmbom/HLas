#ifdef HLAS_HAVE_LAZ
#include "gpstime11.h"
#include <cstring>

namespace HLas::detail::laz {

void GPSTime11Decoder::init(ArithmeticDecoder& dec) {
    dec_ = &dec;
    first_ = true;
    current_frame_ = 0;
    next_frame_ = 0;

    case_model_.init(516);
    case0_model_.init(6);
    ic_gps_.init(dec, 32, 9);

    for (auto& f : frames_) {
        f.prev_gps_time = 0;
        f.delta = 0;
        f.counter = 0;
    }
}

void GPSTime11Decoder::decompress(const uint8_t* seed, uint8_t* dest) {
    if (first_) {
        std::memcpy(dest, seed, 8);
        int64_t gps;
        std::memcpy(&gps, seed, 8);
        frames_[0].prev_gps_time = gps;
        first_ = false;
        return;
    }

    // Ett sekvensbyte konsumerar en symbol utan att producera en tid, så
    // avkodningen kan behöva flera varv. Referensen ANROPAR SIG SJÄLV och har
    // därmed inget tak alls — en elak fil med en oändlig kedja byten spränger
    // dess stack. Vi loopar i stället med ett tak.
    //
    // Referensens kodare skriver aldrig två byten i rad, så fyra varv räcker med
    // marginal för varje fil den producerat. Tar taket ändå slut skrivs det
    // aktuella fackets tid ut: fel data, men DETERMINISTISKT fel, i stället för
    // att lämna posten orörd med vad anroparens buffert nu råkade innehålla.
    constexpr int kMaxFrameSwitches = 4;
    for (int attempt = 0; attempt <= kMaxFrameSwitches; ++attempt) {
        auto& fr = frames_[current_frame_];

        if (fr.delta == 0) {
            // case_0delta (spec 13.2: 6 symbols: 0-5)
            uint32_t c = dec_->decodeSymbol(case0_model_);

            if (c >= 3 && c <= 5) {
                // Switch reference frame, then process next entry
                current_frame_ = (current_frame_ + (c - 2)) & 3;
                continue;
            }

            if (c == 0) {
                // Identical to previous GPS time
                int64_t gps = fr.prev_gps_time;
                std::memcpy(dest, &gps, 8);
                return;
            }

            if (c == 1) {
                // dGPS Time (low) with instance 0
                int32_t d = ic_gps_.decompress(0, 0);
                int64_t gps = ISum64(fr.prev_gps_time, d);
                fr.delta = d;
                fr.counter = 0;
                fr.prev_gps_time = gps;
                std::memcpy(dest, &gps, 8);
                return;
            }

            // c == 2: Full store — new reference frame
            {
                int32_t hi32 = static_cast<int32_t>(
                    static_cast<uint64_t>(fr.prev_gps_time) >> 32);
                int32_t d_low = ic_gps_.decompress(0, 8);
                uint32_t extra = dec_->readInt();

                // Bitmönstret byggs i uint64 och konverteras sist. Skiftet
                // gjordes tidigare på int64: har det översta av de 32 bitarna
                // satt hamnar en etta i teckenbiten, och signed vänsterskift
                // som spiller över är odefinierat beteende. Samma sätt som
                // Point14 redan gör det.
                const uint64_t bits =
                    (static_cast<uint64_t>(static_cast<uint32_t>(ISum32(hi32, d_low))) << 32)
                    | extra;
                const int64_t tmp = static_cast<int64_t>(bits);

                // Det nya facket räknas ur next_frame_, inte ur current_frame_.
                // Se kommentaren i headern.
                next_frame_ = (next_frame_ + 1) & 3;
                current_frame_ = next_frame_;
                frames_[current_frame_].prev_gps_time = tmp;
                frames_[current_frame_].delta = 0;
                frames_[current_frame_].counter = 0;
                std::memcpy(dest, &tmp, 8);
                return;
            }
        } else {
            // case (spec 13.2: 516 symbols: 0-515)
            uint32_t c = dec_->decodeSymbol(case_model_);

            if (c >= 513 && c <= 515) {
                // Switch reference frame
                current_frame_ = (current_frame_ + (c - 512)) & 3;
                continue;
            }

            if (c == 511) {
                // Identical to previous GPS time
                int64_t gps = fr.prev_gps_time;
                std::memcpy(dest, &gps, 8);
                return;
            }

            if (c == 512) {
                // Full store — new reference frame
                int32_t hi32 = static_cast<int32_t>(
                    static_cast<uint64_t>(fr.prev_gps_time) >> 32);
                int32_t d_low = ic_gps_.decompress(0, 8);
                uint32_t extra = dec_->readInt();

                // Bitmönstret byggs i uint64 och konverteras sist. Skiftet
                // gjordes tidigare på int64: har det översta av de 32 bitarna
                // satt hamnar en etta i teckenbiten, och signed vänsterskift
                // som spiller över är odefinierat beteende. Samma sätt som
                // Point14 redan gör det.
                const uint64_t bits =
                    (static_cast<uint64_t>(static_cast<uint32_t>(ISum32(hi32, d_low))) << 32)
                    | extra;
                const int64_t tmp = static_cast<int64_t>(bits);

                // Det nya facket räknas ur next_frame_, inte ur current_frame_.
                // Se kommentaren i headern.
                next_frame_ = (next_frame_ + 1) & 3;
                current_frame_ = next_frame_;
                frames_[current_frame_].prev_gps_time = tmp;
                frames_[current_frame_].delta = 0;
                frames_[current_frame_].counter = 0;
                std::memcpy(dest, &tmp, 8);
                return;
            }

            // Symbol 0-510: steget beskrivs som en multipel av föregående steg.
            //
            // PREDIKTIONEN SKICKAS TILL IntegerCompressor OCH RÄKNAS I INT32
            // med wrappning — inte i int64. Skillnaden märks först när
            // multi * delta spiller över int32, och då hamnar tiden en eller
            // flera 2^32 fel. Kodaren bildar korrektorn mot det WRAPPADE värdet
            // (IMul32), så allt annat är en oenighet mellan kodare och avkodare
            // och inte bara en avvikelse från referensen. Point14 har alltid
            // gjort så här; den här avkodaren räknade i int64 fram till 1.1.2.
            int32_t gpstime_diff;
            if (c == 1) {
                gpstime_diff = ic_gps_.decompress(fr.delta, 1);
                fr.counter = 0;
            } else if (c == 0) {
                gpstime_diff = ic_gps_.decompress(0, 7);
                if (++fr.counter > 3) { fr.delta = gpstime_diff; fr.counter = 0; }
            } else if (c < 500) {
                gpstime_diff = ic_gps_.decompress(
                    IMul32(static_cast<int32_t>(c), fr.delta), (c < 10) ? 2u : 3u);
            } else if (c == 500) {
                gpstime_diff = ic_gps_.decompress(IMul32(500, fr.delta), 4);
                if (++fr.counter > 3) { fr.delta = gpstime_diff; fr.counter = 0; }
            } else {
                // 501-510 är negativa multiplar: symbolen är 500 - multi.
                const int32_t multi = 500 - static_cast<int32_t>(c);
                if (multi > -10) {
                    gpstime_diff = ic_gps_.decompress(IMul32(multi, fr.delta), 5);
                } else {
                    gpstime_diff = ic_gps_.decompress(IMul32(-10, fr.delta), 6);
                    if (++fr.counter > 3) { fr.delta = gpstime_diff; fr.counter = 0; }
                }
            }

            const int64_t gps = ISum64(fr.prev_gps_time,
                                       static_cast<int64_t>(gpstime_diff));
            fr.prev_gps_time = gps;
            std::memcpy(dest, &gps, 8);
            return;
        }
    }

    // Taket slog i. Se kommentaren ovan.
    const int64_t fallback = frames_[current_frame_].prev_gps_time;
    std::memcpy(dest, &fallback, 8);
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
