#pragma once
#include "las_error.h"
#include "point_source.h"
#include "sidecar_lite.h"
#ifdef HLAS_HAVE_LAZ
#include "laz_reader.h"
#endif
#include <cstddef>
#include <vector>

namespace HLas::detail {

struct ParsedHeader;
struct Vlr;

// Vad cachen måste stämma överens med för att höra till en viss källfil.
//
// Punktantalet ensamt räckte inte: byttes x.laz mot en annan fil med samma antal
// användes den gamla cachen ändå, och läsningen gav punkter ur fel fil utan att
// något felade. Källans storlek, punktformat och kvantisering skiljer dem åt.
//
// De räcker ändå inte hela vägen: en fil som klassas om och kodas om till samma
// längd har samma alla fem. Därför också ett stickprov ur källans byte.
SidecarHeader sidecar_expectation(const char* path, const ParsedHeader& parsed,
                                  const uint8_t* file_data, size_t file_size);

// LAZ-dekompression. Inget mer.
//
// ⛔ Cachen låg förut HÄR, som en policy ovanpå LAZ-vägen. Den är utlyft till
// Reader::Impl i 1.1.4, av ett skäl som inte fanns förut: cachen gäller numera
// även en .las, och en .las går aldrig genom den här klassen. Låg den kvar hade
// LAS-vägen behövt en egen kopia av samma policy.
class LazSource {
public:
    ErrorCode open(const ParsedHeader& parsed,
                   const uint8_t* file_data, size_t file_size,
                   const std::vector<Vlr>& vlrs);
    void close();
    PointSource point_source() const;

private:
#ifdef HLAS_HAVE_LAZ
    LazReader laz_;
#endif
};

} // namespace HLas::detail
