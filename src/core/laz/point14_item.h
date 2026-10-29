#pragma once
#ifdef HLAS_HAVE_LAZ

#include <cstdint>
#include <cstring>

namespace HLas::detail::laz {

// Point14-postens 30 byte, fält för fält.
//
// Delas av avkodaren och kodaren. Två kopior av de här offseten kan glida isär
// — en kan inte — och det är samma skäl som gör att IntegerCompressor är EN
// klass för både läsning och skrivning i stället för två.
//
// Layouten är LAS 1.4 punktformat 6. Format 7, 8 och 10 lägger RGB och NIR
// EFTER de här 30 byten och rör dem inte.

constexpr int OFF_X              = 0;
constexpr int OFF_Y              = 4;
constexpr int OFF_Z              = 8;
constexpr int OFF_INTENSITY      = 12;
constexpr int OFF_RETURN_INFO    = 14;  // return_number:4 | number_of_returns:4
constexpr int OFF_FLAGS          = 15;  // class_flags:4 | scanner_channel:2 | scan_dir:1 | edge:1
constexpr int OFF_CLASSIFICATION = 16;
constexpr int OFF_USER_DATA      = 17;
constexpr int OFF_SCAN_ANGLE     = 18;  // int16, steg om 0,006 grader
constexpr int OFF_POINT_SOURCE   = 20;  // uint16
constexpr int OFF_GPS_TIME       = 22;  // float64

constexpr int kPoint14Size = 30;

inline uint8_t get_return_number(const uint8_t* p) { return p[OFF_RETURN_INFO] & 0x0F; }
inline uint8_t get_number_of_returns(const uint8_t* p) { return (p[OFF_RETURN_INFO] >> 4) & 0x0F; }
inline void set_return_number(uint8_t* p, uint8_t r) {
    p[OFF_RETURN_INFO] = static_cast<uint8_t>((p[OFF_RETURN_INFO] & 0xF0) | (r & 0x0F));
}
inline void set_number_of_returns(uint8_t* p, uint8_t n) {
    p[OFF_RETURN_INFO] = static_cast<uint8_t>((p[OFF_RETURN_INFO] & 0x0F) | ((n & 0x0F) << 4));
}

inline uint8_t get_scanner_channel(const uint8_t* p) { return (p[OFF_FLAGS] >> 4) & 0x03; }
inline void set_scanner_channel(uint8_t* p, uint8_t ch) {
    p[OFF_FLAGS] = static_cast<uint8_t>((p[OFF_FLAGS] & ~0x30) | ((ch & 0x03) << 4));
}
inline uint8_t get_classification_flags(const uint8_t* p) { return p[OFF_FLAGS] & 0x0F; }
inline uint8_t get_scan_direction(const uint8_t* p) { return (p[OFF_FLAGS] >> 6) & 0x01; }
inline uint8_t get_edge_of_flight(const uint8_t* p) { return (p[OFF_FLAGS] >> 7) & 0x01; }

// Flaggorna som de KODAS, vilket inte är samma sak som flaggbyten.
//
// Scannerkanalen ligger i byten men kodas i channel_returns_XY-lagret, så den
// måste bort innan flaggorna går till sitt eget lager. Bitarna packas om till
// class_flags:4 | scan_dir:1 | edge:1 — sex bitar, vilket är varför modellen
// har 64 symboler.
inline uint32_t packed_flags(const uint8_t* p) {
    return static_cast<uint32_t>(get_classification_flags(p))
         | (static_cast<uint32_t>(get_scan_direction(p)) << 4)
         | (static_cast<uint32_t>(get_edge_of_flight(p)) << 5);
}

// Motsatsen: skriver tillbaka de sex bitarna och LÅTER scannerkanalen stå kvar.
inline void set_packed_flags(uint8_t* p, uint32_t flags) {
    const uint8_t sc = get_scanner_channel(p);
    p[OFF_FLAGS] = static_cast<uint8_t>((flags & 0x0F)
                 | (sc << 4)
                 | (((flags >> 4) & 1) << 6)
                 | (((flags >> 5) & 1) << 7));
}

inline int32_t  get_X(const uint8_t* p) { int32_t v; std::memcpy(&v, p + OFF_X, 4); return v; }
inline int32_t  get_Y(const uint8_t* p) { int32_t v; std::memcpy(&v, p + OFF_Y, 4); return v; }
inline int32_t  get_Z(const uint8_t* p) { int32_t v; std::memcpy(&v, p + OFF_Z, 4); return v; }
inline uint16_t get_intensity(const uint8_t* p) { uint16_t v; std::memcpy(&v, p + OFF_INTENSITY, 2); return v; }
inline int16_t  get_scan_angle(const uint8_t* p) { int16_t v; std::memcpy(&v, p + OFF_SCAN_ANGLE, 2); return v; }
inline uint16_t get_point_source(const uint8_t* p) { uint16_t v; std::memcpy(&v, p + OFF_POINT_SOURCE, 2); return v; }
inline double   get_gps_time(const uint8_t* p) { double v; std::memcpy(&v, p + OFF_GPS_TIME, 8); return v; }

inline void set_X(uint8_t* p, int32_t v) { std::memcpy(p + OFF_X, &v, 4); }
inline void set_Y(uint8_t* p, int32_t v) { std::memcpy(p + OFF_Y, &v, 4); }
inline void set_Z(uint8_t* p, int32_t v) { std::memcpy(p + OFF_Z, &v, 4); }
inline void set_intensity(uint8_t* p, uint16_t v) { std::memcpy(p + OFF_INTENSITY, &v, 2); }
inline void set_scan_angle(uint8_t* p, int16_t v) { std::memcpy(p + OFF_SCAN_ANGLE, &v, 2); }
inline void set_point_source(uint8_t* p, uint16_t v) { std::memcpy(p + OFF_POINT_SOURCE, &v, 2); }
inline void set_gps_time(uint8_t* p, double v) { std::memcpy(p + OFF_GPS_TIME, &v, 8); }

// Rensar bit 0 — används för k-värdena som blir kontext för dY och Z. Den lägsta
// biten frigörs för att bära "antal returer är 1", så kontexterna inte krockar.
inline uint32_t u32_zero_bit_0(uint32_t n) { return n & ~1u; }

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
