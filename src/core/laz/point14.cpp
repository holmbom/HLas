#ifdef HLAS_HAVE_LAZ
#include "point14.h"
#include "point14_item.h"
#include <cstring>

namespace HLas::detail::laz {

// Helper: clear bit 0 of uint32

// Return map: 16×16 for up to 15 returns (the LAZ format's six-context map)
const uint8_t number_return_map_6ctx[16][16] = {
    {  0,  1,  2,  3,  4,  5,  3,  4,  4,  5,  5,  5,  5,  5,  5,  5 },
    {  1,  0,  1,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3 },
    {  2,  1,  2,  4,  4,  4,  4,  4,  4,  4,  4,  3,  3,  3,  3,  3 },
    {  3,  3,  4,  5,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4 },
    {  4,  3,  4,  4,  5,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4 },
    {  5,  3,  4,  4,  4,  5,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4 },
    {  3,  3,  4,  4,  4,  4,  5,  4,  4,  4,  4,  4,  4,  4,  4,  4 },
    {  4,  3,  4,  4,  4,  4,  4,  5,  4,  4,  4,  4,  4,  4,  4,  4 },
    {  4,  3,  4,  4,  4,  4,  4,  4,  5,  4,  4,  4,  4,  4,  4,  4 },
    {  5,  3,  4,  4,  4,  4,  4,  4,  4,  5,  4,  4,  4,  4,  4,  4 },
    {  5,  3,  4,  4,  4,  4,  4,  4,  4,  4,  5,  4,  4,  4,  4,  4 },
    {  5,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  4,  4,  4 },
    {  5,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  4,  4 },
    {  5,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  4 },
    {  5,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5 },
    {  5,  3,  3,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5 },
};

const uint8_t number_return_level_8ctx[16][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  7,  7,  7,  7,  7,  7,  7,  7 },
    {  1,  0,  1,  2,  3,  4,  5,  6,  7,  7,  7,  7,  7,  7,  7,  7 },
    {  2,  1,  0,  1,  2,  3,  4,  5,  6,  7,  7,  7,  7,  7,  7,  7 },
    {  3,  2,  1,  0,  1,  2,  3,  4,  5,  6,  7,  7,  7,  7,  7,  7 },
    {  4,  3,  2,  1,  0,  1,  2,  3,  4,  5,  6,  7,  7,  7,  7,  7 },
    {  5,  4,  3,  2,  1,  0,  1,  2,  3,  4,  5,  6,  7,  7,  7,  7 },
    {  6,  5,  4,  3,  2,  1,  0,  1,  2,  3,  4,  5,  6,  7,  7,  7 },
    {  7,  6,  5,  4,  3,  2,  1,  0,  1,  2,  3,  4,  5,  6,  7,  7 },
    {  7,  7,  6,  5,  4,  3,  2,  1,  0,  1,  2,  3,  4,  5,  6,  7 },
    {  7,  7,  7,  6,  5,  4,  3,  2,  1,  0,  1,  2,  3,  4,  5,  6 },
    {  7,  7,  7,  7,  6,  5,  4,  3,  2,  1,  0,  1,  2,  3,  4,  5 },
    {  7,  7,  7,  7,  7,  6,  5,  4,  3,  2,  1,  0,  1,  2,  3,  4 },
    {  7,  7,  7,  7,  7,  7,  6,  5,  4,  3,  2,  1,  0,  1,  2,  3 },
    {  7,  7,  7,  7,  7,  7,  7,  6,  5,  4,  3,  2,  1,  0,  1,  2 },
    {  7,  7,  7,  7,  7,  7,  7,  7,  6,  5,  4,  3,  2,  1,  0,  1 },
    {  7,  7,  7,  7,  7,  7,  7,  7,  7,  6,  5,  4,  3,  2,  1,  0 },
};

void Point14Decoder::read_chunk_sizes(const uint8_t*& ptr) {
    std::memcpy(&num_bytes_channel_returns_XY_, ptr, 4); ptr += 4;
    std::memcpy(&num_bytes_Z_, ptr, 4); ptr += 4;
    std::memcpy(&num_bytes_classification_, ptr, 4); ptr += 4;
    std::memcpy(&num_bytes_flags_, ptr, 4); ptr += 4;
    std::memcpy(&num_bytes_intensity_, ptr, 4); ptr += 4;
    std::memcpy(&num_bytes_scan_angle_, ptr, 4); ptr += 4;
    std::memcpy(&num_bytes_user_data_, ptr, 4); ptr += 4;
    std::memcpy(&num_bytes_point_source_, ptr, 4); ptr += 4;
    std::memcpy(&num_bytes_gps_time_, ptr, 4); ptr += 4;
}

void Point14Decoder::init_context(uint32_t ctx, const uint8_t* seed_item) {
    auto& c = contexts_[ctx];
    c.unused = false;

    std::memcpy(c.last_item, seed_item, 30);

    for (auto& v : c.last_intensity) v = get_intensity(seed_item);
    for (auto& sm : c.last_X_diff_median5) sm.reset();
    for (auto& sm : c.last_Y_diff_median5) sm.reset();
    for (auto& z : c.last_Z) z = get_Z(seed_item);

    // Channel returns XY models
    for (auto& m : c.m_changed_values) m.init(128);
    c.m_scanner_channel.init(3);
    for (auto& m : c.m_number_of_returns) m.init(16);
    c.m_return_number_gps_same.init(13);
    for (auto& m : c.m_return_number) m.init(16);

    c.ic_dX.init(dec_channel_returns_XY_, 32, 2);
    c.ic_dY.init(dec_channel_returns_XY_, 32, 22);

    // Z layer
    c.ic_Z.init(dec_Z_, 32, 20);

    // Classification
    for (auto& m : c.m_classification) m.init(256);

    // Flags
    for (auto& m : c.m_flags) m.init(64);

    // Intensity
    c.ic_intensity.init(dec_intensity_, 16, 4);

    // Scan angle
    c.ic_scan_angle.init(dec_scan_angle_, 16, 2);

    // User data
    for (auto& m : c.m_user_data) m.init(256);

    // Point source
    c.ic_point_source.init(dec_point_source_, 16, 1);

    // GPS time
    c.last = 0;
    c.next = 0;
    // BARA fack 0 får seed-tiden. De tre andra lämnas nollställda, och det är
    // inte en förenkling utan en förutsättning: kodarens findSeq letar efter det
    // första facket vars differens ryms i en int32, och ett fack som redan
    // innehåller seed-tiden skulle då väljas före det referensen väljer. Fyller
    // man alla fyra blir strömmen giltig men INTE byte-identisk.
    c.last_gpstime[0].f64 = 0.0;
    std::memcpy(&c.last_gpstime[0].f64, seed_item + OFF_GPS_TIME, 8);
    for (int i = 1; i < 4; ++i) c.last_gpstime[i].u64 = 0;
    for (auto& d : c.last_gpstime_diff) d = 0;
    for (auto& e : c.multi_extreme_counter) e = 0;
    c.m_gpstime_multi.init(GPSTIME_MULTI_TOTAL);
    c.m_gpstime_0diff.init(5);
    c.ic_gpstime.init(dec_gps_time_, 32, 9);

    c.gps_time_change = false;
}

void Point14Decoder::init_chunk(const uint8_t*& ptr, const uint8_t* seed) {
    // Allocate buffer for all layer bytes
    uint32_t total = num_bytes_channel_returns_XY_ + num_bytes_Z_ +
                     num_bytes_classification_ + num_bytes_flags_ +
                     num_bytes_intensity_ + num_bytes_scan_angle_ +
                     num_bytes_user_data_ + num_bytes_point_source_ +
                     num_bytes_gps_time_;
    layer_bytes_.resize(total);

    // Read all layer bytes from stream
    std::memcpy(layer_bytes_.data(), ptr, total);
    ptr += total;

    // Initialize per-layer ArithmeticDecoders
    uint32_t off = 0;
    const uint8_t* lb = layer_bytes_.data();

    dec_channel_returns_XY_.init(lb + off, num_bytes_channel_returns_XY_);
    off += num_bytes_channel_returns_XY_;

    changed_Z_ = (num_bytes_Z_ > 0);
    if (changed_Z_) {
        dec_Z_.init(lb + off, num_bytes_Z_);
        off += num_bytes_Z_;
    }

    changed_classification_ = (num_bytes_classification_ > 0);
    if (changed_classification_) {
        dec_classification_.init(lb + off, num_bytes_classification_);
        off += num_bytes_classification_;
    }

    changed_flags_ = (num_bytes_flags_ > 0);
    if (changed_flags_) {
        dec_flags_.init(lb + off, num_bytes_flags_);
        off += num_bytes_flags_;
    }

    changed_intensity_ = (num_bytes_intensity_ > 0);
    if (changed_intensity_) {
        dec_intensity_.init(lb + off, num_bytes_intensity_);
        off += num_bytes_intensity_;
    }

    changed_scan_angle_ = (num_bytes_scan_angle_ > 0);
    if (changed_scan_angle_) {
        dec_scan_angle_.init(lb + off, num_bytes_scan_angle_);
        off += num_bytes_scan_angle_;
    }

    changed_user_data_ = (num_bytes_user_data_ > 0);
    if (changed_user_data_) {
        dec_user_data_.init(lb + off, num_bytes_user_data_);
        off += num_bytes_user_data_;
    }

    changed_point_source_ = (num_bytes_point_source_ > 0);
    if (changed_point_source_) {
        dec_point_source_.init(lb + off, num_bytes_point_source_);
        off += num_bytes_point_source_;
    }

    changed_gps_time_ = (num_bytes_gps_time_ > 0);
    if (changed_gps_time_) {
        dec_gps_time_.init(lb + off, num_bytes_gps_time_);
        off += num_bytes_gps_time_;
    }

    // Reset contexts
    for (auto& c : contexts_) c.unused = true;
    current_context_ = get_scanner_channel(seed);
    init_context(current_context_, seed);
}

void Point14Decoder::decompress(uint32_t& context, uint8_t* dest) {
    uint8_t* last_item = contexts_[current_context_].last_item;
    auto& ctx = contexts_[current_context_];

    // Compute lpr: single(3) / first(1) / last(2) / intermediate(0) + gps_time_change(4)
    uint8_t last_r = get_return_number(last_item);
    uint8_t last_n = get_number_of_returns(last_item);
    int32_t lpr = (last_r == 1 ? 1 : 0);
    lpr += (last_r >= last_n ? 2 : 0);
    lpr += (ctx.gps_time_change ? 4 : 0);

    // Decode changed_values
    int32_t changed_values = static_cast<int32_t>(
        dec_channel_returns_XY_.decodeSymbol(ctx.m_changed_values[lpr]));

    // Scanner channel switch (bit 6)
    if (changed_values & (1 << 6)) {
        uint32_t diff = dec_channel_returns_XY_.decodeSymbol(ctx.m_scanner_channel);
        uint32_t scanner_channel = (current_context_ + diff + 1) % 4;
        if (contexts_[scanner_channel].unused) {
            init_context(scanner_channel, contexts_[current_context_].last_item);
        }
        current_context_ = scanner_channel;
        last_item = contexts_[current_context_].last_item;
        set_scanner_channel(last_item, static_cast<uint8_t>(scanner_channel));
    }
    context = current_context_;
    auto& cur = contexts_[current_context_];

    // Extract changed attribute flags
    bool point_source_change = (changed_values & (1 << 5)) != 0;
    bool gps_time_change = (changed_values & (1 << 4)) != 0;
    const uint32_t gtc = gps_time_change ? 1u : 0u;   // indexbit, inte bool i en bitoperation
    bool scan_angle_change = (changed_values & (1 << 3)) != 0;

    // Decode number_of_returns (bit 2)
    uint32_t n;
    if (changed_values & (1 << 2)) {
        n = dec_channel_returns_XY_.decodeSymbol(cur.m_number_of_returns[last_n]);
        set_number_of_returns(last_item, static_cast<uint8_t>(n));
    } else {
        n = last_n;
    }

    // Decode return_number (bits 0-1)
    uint32_t r;
    int rv = changed_values & 3;
    if (rv == 0) {
        r = last_r;
    } else if (rv == 1) {
        r = (last_r + 1) % 16;
        set_return_number(last_item, static_cast<uint8_t>(r));
    } else if (rv == 2) {
        r = (last_r + 15) % 16;
        set_return_number(last_item, static_cast<uint8_t>(r));
    } else {
        if (gps_time_change) {
            r = dec_channel_returns_XY_.decodeSymbol(cur.m_return_number[last_r]);
        } else {
            uint32_t sym = dec_channel_returns_XY_.decodeSymbol(cur.m_return_number_gps_same);
            r = (last_r + sym + 2) % 16;
        }
        set_return_number(last_item, static_cast<uint8_t>(r));
    }

    // Get return map m and return level l
    uint32_t m = number_return_map_6ctx[n][r];
    uint32_t l = number_return_level_8ctx[n][r];

    // Current point return context: single(3) / first(2) / last(1) / intermediate(0)
    int32_t cpr = (r == 1 ? 2 : 0);
    cpr += (r >= n ? 1 : 0);

    // === Decode X ===
    int32_t median = cur.last_X_diff_median5[(m << 1) | gtc].median();
    int32_t diff = cur.ic_dX.decompress(median, n == 1 ? 1 : 0);
    int32_t x = ISum32(get_X(last_item), diff);
    set_X(last_item, x);
    cur.last_X_diff_median5[(m << 1) | gtc].insert(diff);

    // === Decode Y ===
    median = cur.last_Y_diff_median5[(m << 1) | gtc].median();
    uint32_t kx = cur.ic_dX.last_k();
    uint32_t dy_ctx = (n == 1 ? 1u : 0u) + (kx < 20 ? u32_zero_bit_0(kx) : 20u);
    diff = cur.ic_dY.decompress(median, dy_ctx);
    int32_t y = ISum32(get_Y(last_item), diff);
    set_Y(last_item, y);
    cur.last_Y_diff_median5[(m << 1) | gtc].insert(diff);

    // === Decode Z (if changed) ===
    if (changed_Z_) {
        uint32_t ky = cur.ic_dY.last_k();
        uint32_t kxy = (kx + ky) / 2;
        uint32_t z_ctx = (n == 1 ? 1u : 0u) + (kxy < 18 ? u32_zero_bit_0(kxy) : 18u);
        int32_t z = cur.ic_Z.decompress(cur.last_Z[l], z_ctx);
        set_Z(last_item, z);
        cur.last_Z[l] = z;
    }

    // === Decode classification (if changed) ===
    if (changed_classification_) {
        uint32_t last_cls = last_item[OFF_CLASSIFICATION];
        int32_t ccc = ((last_cls & 0x1F) << 1) + (cpr == 3 ? 1 : 0);
        last_item[OFF_CLASSIFICATION] = static_cast<uint8_t>(
            dec_classification_.decodeSymbol(cur.m_classification[ccc]));
    }

    // === Decode flags (if changed) ===
    if (changed_flags_) {
        uint32_t last_flags = (get_edge_of_flight(last_item) << 5) |
                              (get_scan_direction(last_item) << 4) |
                              get_classification_flags(last_item);
        uint32_t flags = dec_flags_.decodeSymbol(cur.m_flags[last_flags]);
        // Reconstruct flags byte: classification_flags:4 | scanner_channel:2 | scan_dir:1 | edge:1
        uint8_t edge = (flags >> 5) & 1;
        uint8_t scan_dir = (flags >> 4) & 1;
        uint8_t cls_flags = flags & 0x0F;
        uint8_t sc = get_scanner_channel(last_item);
        last_item[OFF_FLAGS] = cls_flags | (sc << 4) | (scan_dir << 6) | (edge << 7);
    }

    // === Decode intensity (if changed) ===
    if (changed_intensity_) {
        uint32_t i_ctx = static_cast<uint32_t>(cpr);
        uint16_t intensity = static_cast<uint16_t>(
            cur.ic_intensity.decompress(cur.last_intensity[(cpr << 1) | gtc], i_ctx));
        cur.last_intensity[(cpr << 1) | gtc] = intensity;
        set_intensity(last_item, intensity);
    }

    // === Decode scan angle (if changed) ===
    if (changed_scan_angle_ && scan_angle_change) {
        int16_t angle = static_cast<int16_t>(
            cur.ic_scan_angle.decompress(get_scan_angle(last_item), gps_time_change ? 1 : 0));
        set_scan_angle(last_item, angle);
    }

    // === Decode user data (if changed) ===
    if (changed_user_data_) {
        uint32_t ud_ctx = last_item[OFF_USER_DATA] / 4;
        last_item[OFF_USER_DATA] = static_cast<uint8_t>(
            dec_user_data_.decodeSymbol(cur.m_user_data[ud_ctx]));
    }

    // === Decode point source (if changed) ===
    if (changed_point_source_ && point_source_change) {
        uint16_t ps = static_cast<uint16_t>(
            cur.ic_point_source.decompress(get_point_source(last_item), 0));
        set_point_source(last_item, ps);
    }

    // === Decode GPS time (if changed) ===
    if (changed_gps_time_ && gps_time_change) {
        read_gps_time();
        set_gps_time(last_item, cur.last_gpstime[cur.last].f64);
    }

    // Copy to output
    std::memcpy(dest, last_item, 30);

    // Remember gps_time_change for next point's lpr
    cur.gps_time_change = gps_time_change;
}

void Point14Decoder::read_gps_time() {
    auto& ctx = contexts_[current_context_];
    int32_t multi;

    if (ctx.last_gpstime_diff[ctx.last] == 0) {
        // Last GPS time diff was zero
        multi = static_cast<int32_t>(dec_gps_time_.decodeSymbol(ctx.m_gpstime_0diff));
        if (multi == 0) {
            // Decode new diff
            ctx.last_gpstime_diff[ctx.last] =
                ctx.ic_gpstime.decompress(0, 0);
            ctx.last_gpstime[ctx.last].i64 = ISum64(ctx.last_gpstime[ctx.last].i64,
                                                    ctx.last_gpstime_diff[ctx.last]);
            ctx.multi_extreme_counter[ctx.last] = 0;
        } else if (multi == 1) {
            // Switch to new sequence with full value
            ctx.next = (ctx.next + 1) & 3;
            ctx.last_gpstime[ctx.next].u64 = static_cast<uint64_t>(static_cast<uint32_t>(
                ctx.ic_gpstime.decompress(
                    static_cast<int32_t>(ctx.last_gpstime[ctx.last].u64 >> 32), 8)));
            ctx.last_gpstime[ctx.next].u64 <<= 32;
            ctx.last_gpstime[ctx.next].u64 |= dec_gps_time_.readInt();
            ctx.last = ctx.next;
            ctx.last_gpstime_diff[ctx.last] = 0;
            ctx.multi_extreme_counter[ctx.last] = 0;
        } else {
            // Switch to existing sequence
            ctx.last = (ctx.last + multi - 1) & 3;
            read_gps_time();
        }
    } else {
        // Last GPS time diff was non-zero
        multi = static_cast<int32_t>(dec_gps_time_.decodeSymbol(ctx.m_gpstime_multi));
        if (multi == 1) {
            ctx.last_gpstime[ctx.last].i64 = ISum64(
                ctx.last_gpstime[ctx.last].i64,
                ctx.ic_gpstime.decompress(ctx.last_gpstime_diff[ctx.last], 1));
            ctx.multi_extreme_counter[ctx.last] = 0;
        } else if (multi < GPSTIME_MULTI_CODE_FULL) {
            int32_t gpstime_diff;
            if (multi == 0) {
                gpstime_diff = ctx.ic_gpstime.decompress(0, 7);
                ctx.multi_extreme_counter[ctx.last]++;
                if (ctx.multi_extreme_counter[ctx.last] > 3) {
                    ctx.last_gpstime_diff[ctx.last] = gpstime_diff;
                    ctx.multi_extreme_counter[ctx.last] = 0;
                }
            } else if (multi < GPSTIME_MULTI) {
                if (multi < 10)
                    gpstime_diff = ctx.ic_gpstime.decompress(
                        IMul32(multi, ctx.last_gpstime_diff[ctx.last]), 2);
                else
                    gpstime_diff = ctx.ic_gpstime.decompress(
                        IMul32(multi, ctx.last_gpstime_diff[ctx.last]), 3);
            } else if (multi == GPSTIME_MULTI) {
                gpstime_diff = ctx.ic_gpstime.decompress(
                    IMul32(GPSTIME_MULTI, ctx.last_gpstime_diff[ctx.last]), 4);
                ctx.multi_extreme_counter[ctx.last]++;
                if (ctx.multi_extreme_counter[ctx.last] > 3) {
                    ctx.last_gpstime_diff[ctx.last] = gpstime_diff;
                    ctx.multi_extreme_counter[ctx.last] = 0;
                }
            } else {
                // Negative multiplier
                multi = GPSTIME_MULTI - multi;
                if (multi > GPSTIME_MULTI_MINUS) {
                    gpstime_diff = ctx.ic_gpstime.decompress(
                        IMul32(multi, ctx.last_gpstime_diff[ctx.last]), 5);
                } else {
                    gpstime_diff = ctx.ic_gpstime.decompress(
                        IMul32(GPSTIME_MULTI_MINUS, ctx.last_gpstime_diff[ctx.last]), 6);
                    ctx.multi_extreme_counter[ctx.last]++;
                    if (ctx.multi_extreme_counter[ctx.last] > 3) {
                        ctx.last_gpstime_diff[ctx.last] = gpstime_diff;
                        ctx.multi_extreme_counter[ctx.last] = 0;
                    }
                }
            }
            ctx.last_gpstime[ctx.last].i64 = ISum64(ctx.last_gpstime[ctx.last].i64,
                                                    gpstime_diff);
        } else if (multi == GPSTIME_MULTI_CODE_FULL) {
            // Full new value
            ctx.next = (ctx.next + 1) & 3;
            ctx.last_gpstime[ctx.next].u64 = static_cast<uint64_t>(static_cast<uint32_t>(
                ctx.ic_gpstime.decompress(
                    static_cast<int32_t>(ctx.last_gpstime[ctx.last].u64 >> 32), 8)));
            ctx.last_gpstime[ctx.next].u64 <<= 32;
            ctx.last_gpstime[ctx.next].u64 |= dec_gps_time_.readInt();
            ctx.last = ctx.next;
            ctx.last_gpstime_diff[ctx.last] = 0;
            ctx.multi_extreme_counter[ctx.last] = 0;
        } else if (multi >= GPSTIME_MULTI_CODE_FULL) {
            // Switch to existing sequence
            ctx.last = (ctx.last + multi - GPSTIME_MULTI_CODE_FULL) & 3;
            read_gps_time();
        }
    }
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
