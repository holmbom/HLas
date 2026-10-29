#pragma once
#include <cstdint>
#include <cstring>

namespace HLas::detail {

// Icke-ägande vy över en sammanhängande punktbuffert.
//
// Alla tre backends — PointReader (mmap:ad LAS), LazReader (dekomprimerad heap)
// och SidecarReader (mmap:ad .hlz) — lagrar punkter som `base + index * stride`.
// PointSource är den gemensamma nämnaren, så kod som bara läser punkter (DEM,
// polygonfilter, histogram) kan bero på den i stället för på alla tre.
//
// Medvetet icke-virtuell: de heta looparna hämtar stride och baspekare en gång
// och går sedan direkt i minnet. En vtable hade lagt ett indirekt anrop per punkt
// utan att köpa något — backends har inget beteende att specialisera, bara data.
//
// Vyn äger ingenting. Den gäller så länge backenden som fyllde i den lever och
// inte har stängts om.
struct PointSource {
    const uint8_t* base         = nullptr;
    uint64_t       count        = 0;
    uint16_t       stride       = 0;
    uint16_t       class_offset = 0;

    // Klassbyten delas med tre flaggbitar i punktformat 0–5: bit 0–4 är klassen,
    // bit 5–7 är synthetic, key-point och withheld. Masken är 0x1F för de
    // formaten och 0xFF för 6–10, där klassen har en egen byte.
    //
    // Utan den räknas en markpunkt med synthetic satt som klass 34, alltså
    // osynlig för accepted[2] och felplacerad i histogrammet. Masken är
    // headerdata precis som skalan nedan — backendarna känner bara till byten.
    uint8_t        class_mask   = 0xFF;

    // Skala och offset ur LAS-headern. Utan dem är vyn bara halva sanningen —
    // råa int32 säger ingenting förrän de skalats. Att ha med dem gör att
    // konsumenter (DEM, polygonfilter) klarar sig med ett enda argument.
    double x_scale = 1.0, y_scale = 1.0, z_scale = 1.0;
    double x_offset = 0.0, y_offset = 0.0, z_offset = 0.0;

    bool valid() const { return base != nullptr && stride != 0; }

    const uint8_t* raw_point(uint64_t index) const {
        if (index >= count) return nullptr;
        return base + index * stride;
    }

    uint8_t classification(uint64_t index) const {
        const uint8_t* p = raw_point(index);
        return p ? static_cast<uint8_t>(p[class_offset] & class_mask) : 0;
    }

    // Masken för ett punktformat. 0–5 delar klassbyten med flaggbitar, 6–10 inte.
    static uint8_t class_mask_for_format(uint8_t point_format) {
        return point_format <= 5 ? 0x1Fu : 0xFFu;
    }

    // Kopierar upp till n punkter från start. Returnerar antalet som faktiskt
    // kopierades — kortare än n vid filslut, 0 om start ligger utanför.
    uint64_t read_raw(uint64_t start, uint64_t n, uint8_t* dest) const {
        if (start >= count) return 0;
        const uint64_t available = count - start;
        const uint64_t to_read = n < available ? n : available;
        std::memcpy(dest, base + start * stride, to_read * stride);
        return to_read;
    }
};

} // namespace HLas::detail
