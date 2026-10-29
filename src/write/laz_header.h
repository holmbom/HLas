#pragma once
#ifdef HLAS_HAVE_LAZ

#include "point_encode.h"
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace HLas::detail {

// Filens fasta layout. Allt måste stämma överens, så det räknas fram här och
// inte på anropsstället.
//
// TVÅ filversioner, och valet följer punktformatet:
//
//   punktformat 0-3   LAS 1.2, header 227 byte, compressor 2 (Chunked)
//   punktformat 6-7   LAS 1.4, header 375 byte, compressor 3 (LayerChunked)
//
// Det är inte en godtycklig uppdelning. Punktformat 6 och uppåt FINNS inte i
// LAS 1.2, och de äldre formaten har ingen anledning att skrivas i 1.4 — 1.2
// beskriver dem fullständigt och läses av allt. FULL bevarar punktPOSTERNA;
// filversionen är inte punktdata.
//
//   0     LAS-header, 227 eller 375 byte
//   ...   VLR-header 54 byte + LAZ-payload 34 + 6 per item
//   ...   int64 chunk-tabellens position
//   ...   första chunken
//   ...   chunk-tabellen sist
inline constexpr uint16_t kLazHeaderMaxSize = 375;
inline constexpr uint16_t kLazVlrHeaderSize = 54;

// Formatkunskapen — vilka format som är "gamla", hur stor headern är, hur lång
// posten är och vad skrivaren stöder — ligger i point_encode.h, eftersom
// LasWriter behöver samma svar och byggs även utan LAZ.
using detail::legacy_point_format;

// Compressor 2 = Chunked, 3 = LayerChunked. Se laz_vlr.h för hela uppräkningen.
inline constexpr uint16_t laz_compressor(uint8_t format) {
    return legacy_point_format(format) ? 2 : 3;
}

// Antal items i LAZ-VLR:n. Varje item är en egen codec, och de kodas efter
// varandra per punkt i den ordning de står här.
inline constexpr uint16_t laz_num_items(uint8_t format) {
    switch (format) {
        case 0:  return 1;   // Point10
        case 1:  return 2;   // Point10 + GPSTime11
        case 2:  return 2;   // Point10 + RGB12
        case 3:  return 3;   // Point10 + GPSTime11 + RGB12
        case 7:  return 2;   // Point14 + RGB14
        default: return 1;   // format 6: Point14
    }
}

// LAZ-payloaden är 34 fasta byte plus 6 per item, så layouten flyttar sig med
// formatet. En header som pekar fel förbi VLR:n ger en fil ingen läser.
inline constexpr uint16_t laz_vlr_payload(uint8_t format) {
    return static_cast<uint16_t>(34 + 6 * laz_num_items(format));
}
inline constexpr uint32_t laz_point_data_offset(uint8_t format) {
    return las_header_size(format) + kLazVlrHeaderSize + laz_vlr_payload(format);
}
inline constexpr uint32_t laz_first_chunk_offset(uint8_t format) {
    return laz_point_data_offset(format) + 8;
}

// Punkter per chunk. Samma storlek som LAZ-filer normalt har. Fast storlek betyder att
// chunk-tabellen är icke-adaptiv — se laz_chunk_table.h.
inline constexpr uint32_t kLazChunkSize = 50000;

template <typename T>
inline void laz_put(uint8_t* p, T v) { std::memcpy(p, &v, sizeof(T)); }

// LAZ-VLR:n: 54 byte huvud + payload.
inline void build_laz_vlr(uint8_t* out, uint8_t format) {
    const uint16_t payload = laz_vlr_payload(format);
    std::memset(out, 0, kLazVlrHeaderSize + payload);

    // VLR-huvud
    laz_put<uint16_t>(out + 0, 0);                       // reserved
    std::memcpy(out + 2, "laszip encoded", 14);          // user_id[16]
    laz_put<uint16_t>(out + 18, 22204);                  // record_id
    laz_put<uint16_t>(out + 20, payload);                // record_length_after_header
    std::snprintf(reinterpret_cast<char*>(out + 22), 32, "HLas LazWriter");

    // Payload
    const bool legacy = legacy_point_format(format);
    uint8_t* p = out + kLazVlrHeaderSize;
    laz_put<uint16_t>(p + 0, laz_compressor(format));
    laz_put<uint16_t>(p + 2, 0);         // coder: aritmetisk
    // Versionsfälten anger vilken version av LAZ-kodningen som skrev filen. De är rent
    // informativa — ingen läsare validerar dem — men de ska vara rimliga för
    // codec-generationen: 2.1 skrev v2-items, 3.4 skriver v3.
    p[4] = legacy ? 2 : 3;               // version major
    p[5] = legacy ? 1 : 4;               // version minor
    laz_put<uint16_t>(p + 6, legacy ? 0 : 3);
    laz_put<uint32_t>(p + 8, 0);         // options
    laz_put<uint32_t>(p + 12, kLazChunkSize);
    laz_put<int64_t>( p + 16, -1);       // number of special EVLRs
    laz_put<int64_t>( p + 24, -1);       // offset to special EVLRs
    laz_put<uint16_t>(p + 32, laz_num_items(format));

    // Item-listan, i kodningsordning.
    uint8_t* it = p + 34;
    auto add_item = [&it](uint16_t type, uint16_t size, uint16_t version) {
        laz_put<uint16_t>(it + 0, type);
        laz_put<uint16_t>(it + 2, size);
        laz_put<uint16_t>(it + 4, version);
        it += 6;
    };

    if (legacy) {
        add_item(6, 20, 2);                                  // Point10
        if (format == 1 || format == 3) add_item(7, 8, 2);   // GPSTime11
        if (format == 2 || format == 3) add_item(8, 6, 2);   // RGB12
    } else {
        add_item(10, 30, 3);                                 // Point14
        if (format == 7) add_item(11, 6, 3);                 // RGB14
    }
}

// LAS-headern för en komprimerad fil.
//
// Skillnaderna mot LasWriter är tre: offset_to_point_data pekar förbi VLR:n,
// num_vlrs är 1, och punktformatet har den höga biten satt — det är den biten
// som gör filen till LAZ.
struct LazHeaderInfo {
    QuantOptions q{};
    uint8_t  format = kWritePointFormat;
    uint64_t count = 0;
    double min_x = 0, min_y = 0, min_z = 0;
    double max_x = 0, max_y = 0, max_z = 0;
    bool   have_bounds = false;
    uint64_t by_return[15] = {};
    bool   by_return_valid = false;
    // Ärvs från källan i FULL. global_encoding bär bland annat GPS-tidens typ
    // (bit 0), och skrivs den som noll tolkas tiderna fel av den som läser —
    // alltså är den punktdata i allt utom namnet.
    uint16_t global_encoding = 0;
    uint16_t file_source_id = 0;
};

inline void build_laz_header(const LazHeaderInfo& h, uint8_t* out) {
    const bool legacy = legacy_point_format(h.format);
    const uint16_t hdr_size = las_header_size(h.format);
    std::memset(out, 0, hdr_size);
    std::memcpy(out, "LASF", 4);

    laz_put<uint16_t>(out + 4, h.file_source_id);

    // global_encoding bit 4 = koordinatsystemet anges som WKT. LAS 1.4 kräver
    // den satt för punktformat 6 och uppåt. Vi skriver ingen WKT-VLR — källans
    // CRS är inte något vi känner till — men biten ska ändå stämma med formatet,
    // annars är filen inte 1.4-konform.
    //
    // I LAS 1.2 är BARA BIT 0 definierad (GPS-tidens typ); 1-15 är reserverade
    // och ska vara noll. En 1.4-källa i punktformat 0-3 kan mycket väl ha bit 4
    // satt, och att bära med den ner i en 1.2-fil vore att skriva en flagga som
    // inte betyder något där.
    laz_put<uint16_t>(out + 6, legacy ? static_cast<uint16_t>(h.global_encoding & 0x0001u)
                                      : static_cast<uint16_t>(h.global_encoding | (1u << 4)));

    out[24] = 1;
    out[25] = legacy ? 2 : 4;
    std::snprintf(reinterpret_cast<char*>(out + 58), 32, "HLas LazWriter");

    laz_put<uint16_t>(out + 94, hdr_size);
    laz_put<uint32_t>(out + 96, laz_point_data_offset(h.format));
    laz_put<uint32_t>(out + 100, 1);                           // en VLR
    out[104] = static_cast<uint8_t>(h.format | 0x80);          // LAZ-biten
    laz_put<uint16_t>(out + 105, las_record_len(h.format));

    laz_put<double>(out + 131, h.q.x_scale);
    laz_put<double>(out + 139, h.q.y_scale);
    laz_put<double>(out + 147, h.q.z_scale);
    laz_put<double>(out + 155, h.q.x_offset);
    laz_put<double>(out + 163, h.q.y_offset);
    laz_put<double>(out + 171, h.q.z_offset);

    if (h.have_bounds) {
        laz_put<double>(out + 179, h.max_x); laz_put<double>(out + 187, h.min_x);
        laz_put<double>(out + 195, h.max_y); laz_put<double>(out + 203, h.min_y);
        laz_put<double>(out + 211, h.max_z); laz_put<double>(out + 219, h.min_z);
    }

    if (legacy) {
        // LAS 1.2 räknar punkter i uint32 och har bara FEM returfack.
        // Anroparen har kontrollerat att antalet ryms; se laz_count_fits().
        laz_put<uint32_t>(out + 107, static_cast<uint32_t>(h.count));
        if (h.by_return_valid) {
            // Returnummer 6 och 7 finns i punktformat 0-5 men har inget fack i
            // 1.2-headern. De räknas alltså inte, vilket är formatets
            // begränsning och inte vår.
            for (int i = 0; i < 5; ++i)
                laz_put<uint32_t>(out + 111 + i * 4,
                                  static_cast<uint32_t>(h.by_return[i]));
        } else if (h.count) {
            laz_put<uint32_t>(out + 111, static_cast<uint32_t>(h.count));
        }
        return;
    }

    // Legacy-fälten ska vara noll för format 6 och uppåt.
    laz_put<uint32_t>(out + 107, 0);

    // 227 waveform, 235 första EVLR, 243 antal EVLR:er — alla noll via memset.
    laz_put<uint64_t>(out + 247, h.count);
    // points_by_return summerar till antalet. I LITE är varje punkt retur 1 av
    // 1; i FULL vet vi inte fördelningen utan att räkna, och att räkna den
    // kräver att alla punkter setts — vilket de har när headern skrivs om i
    // finalize(). Tabellen fylls därför av anroparen via by_return.
    if (h.by_return_valid) {
        for (int i = 0; i < 15; ++i)
            laz_put<uint64_t>(out + 255 + i * 8, h.by_return[i]);
    } else if (h.count) {
        laz_put<uint64_t>(out + 255, h.count);   // alla punkter är retur 1 av 1
    }
}

// LAS 1.2 räknar punkter i uint32. En fil med fler punkter än så GÅR inte att
// beskriva i den headern, och att tyst skriva ett trunkerat antal vore värre än
// att fela: filen hade sett komplett ut och läst som en bråkdel.
inline bool laz_count_fits(uint8_t format, uint64_t count) {
    return !legacy_point_format(format) || count <= 0xFFFFFFFFull;
}

} // namespace HLas::detail

#endif // HLAS_HAVE_LAZ
