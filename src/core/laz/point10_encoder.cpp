#ifdef HLAS_HAVE_LAZ
#include "point10_encoder.h"
#include "point10_item.h"
#include <cassert>

namespace HLas::detail::laz {

namespace {

// Masken som säger vilka fält som ändrats sedan föregående punkt.
//
// Sex bitar, och ordningen är formatets: bit 5 flaggbyten, 4 intensitet,
// 3 klassificering, 2 scanvinkel, 1 user data, 0 point source.
//
// INTENSITETEN JÄMFÖRS INTE MOT FÖREGÅENDE PUNKT utan mot den senaste
// intensiteten för samma retur-klass m, som skickas in separat. Det är därför
// den här funktionen inte kan läsa allt ur `prev`.
uint32_t changed_mask(const uint8_t* p, const uint8_t* prev,
                      uint16_t last_intensity_m) {
    uint16_t intensity, prev_ps, ps;
    std::memcpy(&intensity, p + OFF10_INTENSITY, 2);
    std::memcpy(&ps,        p + OFF10_POINT_SOURCE, 2);
    std::memcpy(&prev_ps,   prev + OFF10_POINT_SOURCE, 2);

    // Referensen jämför de fyra bitfälten var för sig och or:ar ihop resultatet.
    // Bitfälten ÄR hela byten — 3+3+1+1 — så en byte-jämförelse ger exakt samma
    // svar och slipper packa upp och ihop igen.
    const uint32_t bits  = (p[OFF10_BIT_BYTE]       != prev[OFF10_BIT_BYTE]);
    const uint32_t inten = (intensity               != last_intensity_m);
    const uint32_t cls   = (p[OFF10_CLASSIFICATION] != prev[OFF10_CLASSIFICATION]);
    const uint32_t ang   = (p[OFF10_SCAN_ANGLE]     != prev[OFF10_SCAN_ANGLE]);
    const uint32_t ud    = (p[OFF10_USER_DATA]      != prev[OFF10_USER_DATA]);
    const uint32_t psrc  = (ps                      != prev_ps);

    return (bits << 5) | (inten << 4) | (cls << 3) | (ang << 2) | (ud << 1) | psrc;
}

} // namespace

void Point10Encoder::init(ArithmeticEncoder& enc) {
    enc_ = &enc;
    first_ = true;
    failed_ = false;

    changed_values_model_.init(64);

    for (auto& m : bit_byte_models_) m.init(256);
    for (auto& m : classification_models_) m.init(256);
    for (auto& m : scan_angle_models_) m.init(256);
    for (auto& m : user_data_models_) m.init(256);

    ic_intensity_.init(enc, 16, 4);
    ic_point_source_.init(enc, 16, 1);
    ic_dx_.init(enc, 32, 2);
    ic_dy_.init(enc, 32, 22);
    ic_dz_.init(enc, 32, 20);

    for (auto& sm : median_x_) sm.reset();
    for (auto& sm : median_y_) sm.reset();
    for (auto& z : prev_z_) z = 0;
    for (auto& v : prev_intensity_) v = 0;
    std::memset(prev_, 0, sizeof(prev_));
}

void Point10Encoder::start_chunk(const uint8_t* seed) {
    // init() måste ha körts. Utan den är varken kodaren eller de fem
    // heltalskompressorerna uppsatta, och encode() hade avrefererat en
    // nollpekare i stället för att rapportera felet.
    assert(enc_ != nullptr);
    if (!enc_) { failed_ = true; return; }

    // En andra start_chunk() utan mellanliggande init() skulle bara byta ut
    // prev_ och lämna modeller, medianer, Z- och intensitetshistorik samt den
    // påbörjade strömmen orörda. Chunken avkodas då UTAN fel men till fel
    // punkter, alltså den värsta felklassen. Kontrollen gäller även i release:
    // skrivaren roterar chunkar och har felvägar, och där är det lätt gjort.
    //
    // Kontraktet är därmed: EN init() per chunk, eftersom chunken äger sin
    // aritmetiska ström i compressor 2 — till skillnad från Point14, där varje
    // lager har sin egen och start_chunk_item() nollställer dem själv.
    assert(first_);
    if (!first_) { failed_ = true; return; }

    std::memcpy(prev_, seed, kPoint10Size);
    first_ = false;
    // prev_intensity_ och prev_z_ står kvar på noll. Se kommentaren i headern:
    // avkodaren gör likadant, och att "rätta" det här ger en fil som avkodas
    // fel av både vår avkodare och referensens.
}

void Point10Encoder::encode(const uint8_t* item) {
    // start_chunk() måste ha körts. Utan den är prev_ nollställt och kodaren
    // skulle producera en ström som beskriver skillnader mot en punkt som inte
    // står i filen — alltså en chunk som avkodas utan fel men till fel punkter.
    if (first_) { failed_ = true; return; }
    if (failed()) return;

    const uint8_t r = get10_return_number(item);
    const uint8_t n = get10_number_of_returns(item);
    const uint8_t m = kReturnMap[n][r];
    const uint8_t l = kReturnLevel[n][r];

    uint16_t intensity;
    std::memcpy(&intensity, item + OFF10_INTENSITY, 2);

    const uint32_t changed = changed_mask(item, prev_, prev_intensity_[m]);
    enc_->encodeSymbol(changed_values_model_, changed);

    if (changed & 0x20)
        enc_->encodeSymbol(bit_byte_models_[prev_[OFF10_BIT_BYTE]], item[OFF10_BIT_BYTE]);

    if (changed & 0x10) {
        const uint32_t ic_instance = (m < 3) ? m : 3u;
        ic_intensity_.compress(prev_intensity_[m], intensity, ic_instance);
        prev_intensity_[m] = intensity;
    }

    if (changed & 0x08)
        enc_->encodeSymbol(classification_models_[prev_[OFF10_CLASSIFICATION]],
                           item[OFF10_CLASSIFICATION]);

    if (changed & 0x04) {
        // Modellen väljs på DEN NYA punktens scanriktning, inte föregåendes.
        const uint32_t scan_dir = get10_scan_direction(item);
        const uint8_t d = static_cast<uint8_t>(item[OFF10_SCAN_ANGLE] - prev_[OFF10_SCAN_ANGLE]);
        enc_->encodeSymbol(scan_angle_models_[scan_dir], d);
    }

    if (changed & 0x02)
        enc_->encodeSymbol(user_data_models_[prev_[OFF10_USER_DATA]], item[OFF10_USER_DATA]);

    if (changed & 0x01) {
        uint16_t ps, prev_ps;
        std::memcpy(&ps, item + OFF10_POINT_SOURCE, 2);
        std::memcpy(&prev_ps, prev_ + OFF10_POINT_SOURCE, 2);
        ic_point_source_.compress(prev_ps, ps, 0);
    }

    int32_t x, y, z, prev_x, prev_y;
    std::memcpy(&x, item + OFF10_X, 4);
    std::memcpy(&y, item + OFF10_Y, 4);
    std::memcpy(&z, item + OFF10_Z, 4);
    std::memcpy(&prev_x, prev_ + OFF10_X, 4);
    std::memcpy(&prev_y, prev_ + OFF10_Y, 4);

    // dX: skillnaden mot föregående punkt, prediktarad av medianen av de fem
    // senaste skillnaderna för samma m. Subtraktionen ska wrappa — avkodaren
    // gör ISum32 åt andra hållet.
    const int32_t dx = IDiff32(x, prev_x);
    ic_dx_.compress(median_x_[m].median(), dx, (n == 1) ? 1u : 0u);
    median_x_[m].insert(dx);
    const uint32_t kx = ic_dx_.last_k();

    // dY: kontexten kommer ur hur många bitar dX kostade. Är X svårt att gissa
    // är Y det troligen också, och då används en modell som räknar med det.
    uint32_t dy_instance = (kx < 20) ? (kx / 2) * 2 : 20u;
    if (n == 1) dy_instance += 1;

    const int32_t dy = IDiff32(y, prev_y);
    ic_dy_.compress(median_y_[m].median(), dy, dy_instance);
    median_y_[m].insert(dy);
    const uint32_t ky = ic_dy_.last_k();

    // dZ: ingen median. Z predikteras ur senaste Z på SAMMA returnivå l, vilket
    // håller isär marken från trädtopparna.
    const uint32_t kxy = (kx + ky) / 2;
    uint32_t dz_instance = (kxy < 18) ? (kxy / 2) * 2 : 18u;
    if (n == 1) dz_instance += 1;

    ic_dz_.compress(prev_z_[l], z, dz_instance);
    prev_z_[l] = z;

    std::memcpy(prev_, item, kPoint10Size);
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
