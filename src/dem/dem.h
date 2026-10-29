#pragma once
#include "HLas.h"
#include "point_source.h"
#include <cstdint>

namespace HLas::detail {

// Tak för rutnätets storlek.
//
// cell_size kommer från anroparen och polygonen ofta från en användare, så
// kombinationen "stor utbredning, liten cell" är inte ett konstruerat fall — den
// är en felaktig inmatning. Utan tak försöker DEM:en allokera hundratals GB och
// programmet terminerar på ett okastat std::bad_alloc, alltså en krasch i
// anroparens process. Med taket returneras i stället ett tomt DemResult
// (cols == 0), vilket är samma svar som för en tom polygon.
//
// 512 miljoner celler är 2 GB float. Autzen på 1 m är 16 miljoner, så taket
// ligger drygt trettio gånger över det största vi faktiskt kör.
inline constexpr double kDemMaxCells = 512.0 * 1024 * 1024;

// Grid-binnad DEM (min Z per cell) över punkterna i src, begränsad till polygonen.
// Beror bara på PointSource — inte på Reader, inte på vilken backend punkterna kom ur.
//
// accepted[256]: vilka klassificeringar som räknas, nullptr = alla.
// thread_count: parallell punktscan med atomisk min-skrivning.
// Skapar ett tomt rutnät över polygonens omslutande rektangel, allt NaN.
// Bryts ut så att flera punktkällor kan binnas in i samma rutnät — COPC ger
// punkterna nod för nod, inte som en sammanhängande buffert.
DemResult dem_make_grid(const double* poly_x, const double* poly_y,
                        uint32_t num_vertices, double cell_size);

// Binnar en punktkälla in i ett befintligt rutnät (min Z per cell).
// Får anropas upprepade gånger med olika källor.
// `atomic_writes` tvingar atomisk min-skrivning även med thread_count == 1.
// Behövs när flera trådar binnar var sin punktkälla in i SAMMA rutnät — då är
// varje anrop enkeltrådat men rutnätet delat.
void dem_accumulate(DemResult& grid, const PointSource& src,
                    const double* poly_x, const double* poly_y,
                    uint32_t num_vertices,
                    const bool* accepted, int thread_count,
                    bool atomic_writes = false);

// Kör fyllnadspasset. Anropas när alla punkter är inbinnade.
void dem_fill(DemResult& grid, DemFill fill, int thread_count);

DemResult generate_dem(const PointSource& src,
                       const double* poly_x, const double* poly_y,
                       uint32_t num_vertices,
                       double cell_size,
                       DemFill fill,
                       const bool* accepted,
                       int thread_count);

void fill_nearest(float* grid, uint32_t cols, uint32_t rows, int max_radius = 5);
void fill_idw(float* grid, uint32_t cols, uint32_t rows, int max_radius = 5);
void fill_tin(float* grid, uint32_t cols, uint32_t rows, int thread_count = 1);

} // namespace HLas::detail
