#ifdef HLAS_HAVE_LAZ
#include "point10.h"
namespace HLas::detail::laz {

void Point10Decoder::init(ArithmeticDecoder& dec) {
    dec_ = &dec;
    first_ = true;

    changed_values_model_.init(64);

    for (auto& m : bit_byte_models_) m.init(256);
    for (auto& m : classification_models_) m.init(256);
    for (auto& m : scan_angle_models_) m.init(256);
    for (auto& m : user_data_models_) m.init(256);

    ic_intensity_.init(dec, 16, 4);
    ic_point_source_.init(dec, 16, 1);
    ic_dx_.init(dec, 32, 2);
    ic_dy_.init(dec, 32, 22);
    ic_dz_.init(dec, 32, 20);

    for (auto& sm : median_x_) sm.reset();
    for (auto& sm : median_y_) sm.reset();
    for (auto& z : prev_z_) z = 0;
    for (auto& v : prev_intensity_) v = 0;
    std::memset(prev_, 0, sizeof(prev_));
}

void Point10Decoder::decompress(const uint8_t* seed, uint8_t* dest) {
    // First point per chunk is uncompressed (the seed)
    if (first_) {
        std::memcpy(prev_, seed, 20);
        std::memcpy(dest, seed, 20);
        first_ = false;

        // prev_z and prev_intensity stay at 0 from init()
        // spec 13.1.3: "the first, uncompressed item of the chunk is ignored"
        // i.e. treated as having had Z=0 and intensity=0
        return;
    }

    // Read changed values bitmask (6 bits → 64 symbols)
    uint32_t changed = dec_->decodeSymbol(changed_values_model_);

    // Bit-Byte (return_number | num_returns | scan_dir | edge_of_flight)
    uint8_t bit_byte = prev_[14];  // flags byte
    if (changed & 0x20) {  // bit 5
        bit_byte = static_cast<uint8_t>(
            dec_->decodeSymbol(bit_byte_models_[prev_[14]]));
    }

    // Extract r, n for return map
    uint8_t r = bit_byte & 0x07;         // return number (bits 0-2)
    uint8_t n = (bit_byte >> 3) & 0x07;  // number of returns (bits 3-5)
    uint8_t m = kReturnMap[n][r];
    uint8_t l = kReturnLevel[n][r];

    // dIntensity (spec: only stored if bit 4 set)
    uint16_t intensity;
    if (changed & 0x10) {  // bit 4
        uint32_t ic_instance = (m < 3) ? m : 3u;
        int32_t di = ic_intensity_.decompress(0, ic_instance);
        intensity = ISum16(prev_intensity_[m], di);
    } else {
        intensity = prev_intensity_[m];
    }
    prev_intensity_[m] = intensity;

    // Classification (spec: only stored if bit 3 set)
    uint8_t classification = prev_[15];
    if (changed & 0x08) {  // bit 3
        classification = static_cast<uint8_t>(
            dec_->decodeSymbol(classification_models_[prev_[15]]));
    }

    // dScan Angle Rank (spec: only stored if bit 2 set)
    uint8_t scan_angle = prev_[16];
    if (changed & 0x04) {  // bit 2
        uint32_t scan_dir = (bit_byte >> 6) & 1;
        uint8_t d = static_cast<uint8_t>(
            dec_->decodeSymbol(scan_angle_models_[scan_dir]));
        scan_angle = static_cast<uint8_t>((prev_[16] + d) & 0xFF);
    }

    // User Data (spec: only stored if bit 1 set)
    uint8_t user_data = prev_[17];
    if (changed & 0x02) {  // bit 1
        user_data = static_cast<uint8_t>(
            dec_->decodeSymbol(user_data_models_[prev_[17]]));
    }

    // dPoint Source ID (spec: only stored if bit 0 set)
    uint16_t point_source;
    std::memcpy(&point_source, prev_ + 18, 2);
    if (changed & 0x01) {  // bit 0
        int32_t d = ic_point_source_.decompress(0, 0);
        point_source = ISum16(point_source, d);
    }

    // dX: predicted by streaming median[m]
    int32_t prev_x;
    std::memcpy(&prev_x, prev_, 4);
    uint32_t dx_instance = (n == 1) ? 1u : 0u;
    int32_t median_dx = median_x_[m].median();
    int32_t ddx = ic_dx_.decompress(0, dx_instance);
    int32_t actual_dx = ISum32(median_dx, ddx);
    int32_t x = ISum32(prev_x, actual_dx);
    median_x_[m].insert(actual_dx);
    uint32_t kx = ic_dx_.last_k();

    // dY: instance depends on k from dX
    int32_t prev_y;
    std::memcpy(&prev_y, prev_ + 4, 4);
    uint32_t dy_instance;
    if (kx < 20)
        dy_instance = (kx / 2) * 2;  // unset bit 0
    else
        dy_instance = 20;
    if (n == 1)
        dy_instance += 1;

    int32_t median_dy = median_y_[m].median();
    int32_t ddy = ic_dy_.decompress(0, dy_instance);
    int32_t actual_dy = ISum32(median_dy, ddy);
    int32_t y = ISum32(prev_y, actual_dy);
    median_y_[m].insert(actual_dy);
    uint32_t ky = ic_dy_.last_k();

    // dZ: instance depends on kX + kY; relative to prev Z with same l
    uint32_t kxy = (kx + ky) / 2;
    uint32_t dz_instance;
    if (kxy < 18)
        dz_instance = (kxy / 2) * 2;
    else
        dz_instance = 18;
    if (n == 1)
        dz_instance += 1;

    int32_t dz = ic_dz_.decompress(0, dz_instance);
    int32_t z = ISum32(prev_z_[l], dz);
    prev_z_[l] = z;

    // Write output (20 bytes)
    std::memcpy(dest,      &x,            4);
    std::memcpy(dest + 4,  &y,            4);
    std::memcpy(dest + 8,  &z,            4);
    std::memcpy(dest + 12, &intensity,    2);
    dest[14] = bit_byte;
    dest[15] = classification;
    dest[16] = scan_angle;
    dest[17] = user_data;
    std::memcpy(dest + 18, &point_source, 2);

    // Update previous
    std::memcpy(prev_, dest, 20);
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
