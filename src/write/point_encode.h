#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace HLas::detail {

// Delad kvantisering för LAS- och LAZ-utskrift.
//
// Ligger här och inte i respektive skrivare för att de två annars glider isär:
// samma punkter måste ge samma heltal oavsett format, annars är en LAZ-fil inte
// samma data som motsvarande LAS-fil.

inline constexpr uint16_t kWriteRecordLen  = 30;   // punktformat 6
inline constexpr uint8_t  kWritePointFormat = 6;

// ---- LAS-postens layout per punktformat ------------------------------------
//
// Ligger här och inte i laz_header.h av ett skäl: LasWriter byggs ÄVEN när
// HLAS_HAVE_LAZ är av, och laz_header.h finns inte då. Formatkunskapen är
// dessutom LAS-ens, inte LAZ-ens.

// Punktformat 0-5 är de "gamla": LAS 1.2, klassbyten delad med tre flaggbitar,
// scanvinkel i hela grader. 6 och uppåt är LAS 1.4.
inline constexpr bool legacy_point_format(uint8_t format) { return format <= 5; }

// Headerstorleken som hör till formatet.
//
//   0-3    LAS 1.2, 227 byte
//   4-5    LAS 1.3, 235 byte — de infördes med vågformerna och 1.3 lade till
//          fältet för deras position. Skrivaren stöder dem inte, men helparen
//          får inte svara fel om någon frågar
//   6-10   LAS 1.4, 375 byte
//
// Punktformat 6 och uppåt FINNS inte i 1.2, och de äldre har ingen anledning att
// skrivas i 1.4 — 1.2 beskriver dem fullständigt och läses av allt.
inline constexpr uint16_t las_header_size(uint8_t format) {
    if (format <= 3) return 227;
    if (format <= 5) return 235;
    return 375;
}

// Postens längd utan extra bytes.
inline constexpr uint16_t las_record_len(uint8_t format) {
    switch (format) {
        case 0:  return 20;
        case 1:  return 28;
        case 2:  return 26;
        case 3:  return 34;
        case 4:  return 57;
        case 5:  return 63;
        case 6:  return 30;
        case 7:  return 36;
        case 8:  return 38;
        case 9:  return 59;
        case 10: return 67;
        default: return 0;
    }
}

// Vilka format skrivarna kan producera i FULL.
//
// Format 4 och 5 kräver Wavepacket13, format 8 RGBNIR14, format 9 Wavepacket14
// och format 10 båda. Av dem finns bara RGBNIR14 som AVKODARE; ingen finns som
// kodare, och det finns ingen testfil att verifiera mot. Gränsen är alltså densamma för LAS- och LAZ-skrivningen även om
// den okomprimerade vägen inte behöver någon codec: en post vi inte kan LÄSA
// tillbaka och jämföra är en post vi inte kan påstå att vi bevarat.
inline constexpr bool writer_supported_format(uint8_t format) {
    return format <= 3 || format == 6 || format == 7;
}

struct QuantOptions {
    double x_scale = 0.01, y_scale = 0.01, z_scale = 0.01;
    double x_offset = 0.0, y_offset = 0.0, z_offset = 0.0;
};

// Avrundar nedåt till jämnt tusental. Att lägga origo där i stället för på
// första punkten gör filen reproducerbar: samma urval ger samma offset oavsett
// vilken punkt som råkade komma först.
inline double floor_1000(double v) { return std::floor(v / 1000.0) * 1000.0; }

// Bygger en punktpost i format 6. Returnerar false om punkten inte ryms i
// int32 med vald skala och offset — då skulle den wrappa och hamna någon helt
// annanstans i molnet, vilket är värre än ett fel.
//
// `stored_x/y/z` får de koordinater som faktiskt hamnar i filen, alltså efter
// kvantisering. Utbredningen i headern ska beskriva dem, inte anroparens
// doubles — annars kan min/max ligga en halv skala fel.
inline bool encode_point6(const QuantOptions& q,
                          double x, double y, double z, uint8_t classification,
                          uint8_t* out,
                          double& stored_x, double& stored_y, double& stored_z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;

    const double dx = (x - q.x_offset) / q.x_scale;
    const double dy = (y - q.y_offset) / q.y_scale;
    const double dz = (z - q.z_offset) / q.z_scale;

    // Kontrollen görs på det AVRUNDADE värdet och mot det asymmetriska
    // intervallet: int32 går till -2147483648, och fabs() <= INT32_MAX
    // avvisade den giltiga ändpunkten.
    const long long rx = std::llround(dx);
    const long long ry = std::llround(dy);
    const long long rz = std::llround(dz);
    constexpr long long lo = -2147483648LL, hi = 2147483647LL;
    if (rx < lo || rx > hi || ry < lo || ry > hi || rz < lo || rz > hi)
        return false;

    const int32_t ix = static_cast<int32_t>(rx);
    const int32_t iy = static_cast<int32_t>(ry);
    const int32_t iz = static_cast<int32_t>(rz);

    std::memset(out, 0, kWriteRecordLen);
    std::memcpy(out,     &ix, 4);
    std::memcpy(out + 4, &iy, 4);
    std::memcpy(out + 8, &iz, 4);
    out[14] = 0x11;              // retur 1 av 1
    out[16] = classification;    // format 6: hel byte, inga flaggbitar

    stored_x = ix * q.x_scale + q.x_offset;
    stored_y = iy * q.y_scale + q.y_offset;
    stored_z = iz * q.z_scale + q.z_offset;
    return true;
}

} // namespace HLas::detail
