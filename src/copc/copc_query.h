#pragma once
#include "copc_index.h"
#include <cstdint>
#include <vector>

namespace HLas::detail {

struct CopcBounds {
    double min_x = 0, min_y = 0, min_z = 0;
    double max_x = 0, max_y = 0, max_z = 0;
};

// Nodens kub, härledd ur rotens center/halfsize och nyckeln.
//
// Octreet delar en KUB, inte filens bounding box: rotkuben har samma sida på
// alla tre axlar. För Autzen betyder det att Z sträcker sig 406..5062 fast
// punkterna bara ligger mellan 406 och 615. Det är korrekt och gör ingen skada
// — urvalet blir bara konservativt i Z.
//
// Undantaget är `info.root_from_extent`: då är octreet bevisligen byggt över
// filens utbredning, och varje axel delas för sig ur den lådan.
CopcBounds copc_node_bounds(const CopcInfo& info, const CopcKey& key);

// Noder som valts ut för hämtning, med summorna man behöver för att bedöma
// kostnaden innan man hämtar något.
struct CopcSelection {
    std::vector<CopcEntry> nodes;
    uint64_t point_count = 0;   // hur många punkter urvalet innehåller
    uint64_t byte_size   = 0;   // hur mycket som måste hämtas
};

// Väljer de noder vars kub överlappar rektangeln i XY och som ligger på nivå
// <= max_level. `max_level < 0` betyder alla nivåer.
//
// Urvalet är avsiktligt grovt: en nod tas med om dess kub *överlappar*
// rektangeln, inte om alla dess punkter ligger inuti. Finfiltreringen sker
// punkt för punkt efter uppackning — det är där polygonfiltret hör hemma.
//
// **Garantin gäller bara med `max_level < 0`:** då tas ingen punkt bort som
// ligger innanför rektangeln. COPC-noder är INTE redundanta — en djupare nod
// innehåller ytterligare punkter, inte kopior av förälderns. Att sätta ett
// nivåtak tappar alltså verkliga punkter. Det är precis vad LOD ska göra, men
// resultatet är ett glesare moln, inte ett komplett moln av färre noder.
//
// Gränserna vidgas en ULP utåt innan överlappstestet. Nodkuberna räknas fram i
// flyttal, och två intilliggande noder kan annars lämna ett hårfint glapp där en
// punkt som COPC placerat i noden hamnar utanför den beräknade kuben.
//
// En rektangel med NaN har ingen meningsfull insida och ger tomt urval.
// Undersidor (`is_subpage()`) tas aldrig med; de är index, inte punkter.
// Poster med negativ storlek eller negativt punktantal hoppas över.
CopcSelection select_copc_nodes(const CopcInfo& info,
                                const std::vector<CopcEntry>& nodes,
                                double min_x, double min_y,
                                double max_x, double max_y,
                                int max_level = -1);

// Samma sak, men mot en godtycklig polygon i stället för en rektangel.
//
// Skillnaden spelar roll för allt som inte är kompakt. En korridor — en linje
// buffrad några meter åt vardera hållet — har en omslutande rektangel som kan
// vara hundra gånger större än korridoren själv, och urval mot den rektangeln
// hämtar då nästan hela filen för att sedan kasta 99 % av punkterna.
//
// Testet är nodens kub mot polygonen: hörn i rektangeln, rektangelhörn i
// polygonen, eller korsande kanter. Fortfarande konservativt — en nod tas med
// så fort den *berör* polygonen — men utan AABB-uppblåsningen.
CopcSelection select_copc_nodes_polygon(const CopcInfo& info,
                                        const std::vector<CopcEntry>& nodes,
                                        const double* poly_x, const double* poly_y,
                                        uint32_t num_vertices,
                                        int max_level = -1);

} // namespace HLas::detail
