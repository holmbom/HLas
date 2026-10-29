#include "copc_query.h"
#include "geometry.h"
#include <cmath>
#include <limits>

namespace HLas::detail {

namespace {

bool finite_bounds(const CopcBounds& b) {
    return std::isfinite(b.min_x) && std::isfinite(b.max_x) &&
           std::isfinite(b.min_y) && std::isfinite(b.max_y);
}

} // namespace

CopcBounds copc_node_bounds(const CopcInfo& info, const CopcKey& key) {
    CopcBounds b{};
    if (key.level < 0 || key.level > 62)
        return b;

    // Sidan halveras per nivå. 2^level som double klarar godtycklig nivå utan
    // att spilla över, till skillnad från ett heltalsskift.
    const double divisor = static_cast<double>(uint64_t(1) << key.level);

    // Kuben enligt specen, eller filens utbredning när read_copc_index visat
    // att octreet är byggt över den — då delas varje axel för sig.
    double x0, y0, z0, sx, sy, sz;
    if (info.root_from_extent) {
        x0 = info.root_min_x;  sx = info.root_size_x / divisor;
        y0 = info.root_min_y;  sy = info.root_size_y / divisor;
        z0 = info.root_min_z;  sz = info.root_size_z / divisor;
    } else {
        const double side = (info.halfsize * 2.0) / divisor;
        x0 = info.center_x - info.halfsize;  sx = side;
        y0 = info.center_y - info.halfsize;  sy = side;
        z0 = info.center_z - info.halfsize;  sz = side;
    }

    b.min_x = x0 + key.x * sx;  b.max_x = b.min_x + sx;
    b.min_y = y0 + key.y * sy;  b.max_y = b.min_y + sy;
    b.min_z = z0 + key.z * sz;  b.max_z = b.min_z + sz;

    // Vidga en ULP utåt. Minimum och maximum avrundas oberoende av varandra, så
    // två intilliggande noder kan lämna ett hårfint glapp — och en punkt i det
    // glappet skulle falla utanför båda kuberna trots att COPC placerat den i
    // en av dem. Att vidga kan bara ge fler kandidatnoder, aldrig färre.
    const double inf = std::numeric_limits<double>::infinity();
    b.min_x = std::nextafter(b.min_x, -inf);  b.max_x = std::nextafter(b.max_x, inf);
    b.min_y = std::nextafter(b.min_y, -inf);  b.max_y = std::nextafter(b.max_y, inf);
    b.min_z = std::nextafter(b.min_z, -inf);  b.max_z = std::nextafter(b.max_z, inf);
    return b;
}

CopcSelection select_copc_nodes(const CopcInfo& info,
                                const std::vector<CopcEntry>& nodes,
                                double min_x, double min_y,
                                double max_x, double max_y,
                                int max_level) {
    CopcSelection sel;

    // NaN i rektangeln: byte nedan normaliserar inte, och varje jämförelse blir
    // false, vilket skulle ge ett urval som ser rimligt ut men inte betyder
    // något. En rektangel utan definierad insida ger tomt urval.
    if (!std::isfinite(min_x) || !std::isfinite(max_x) ||
        !std::isfinite(min_y) || !std::isfinite(max_y))
        return sel;

    // Tål att rektangeln kommer omvänd
    if (min_x > max_x) { double t = min_x; min_x = max_x; max_x = t; }
    if (min_y > max_y) { double t = min_y; min_y = max_y; max_y = t; }

    for (const auto& e : nodes) {
        if (e.is_subpage())
            continue;
        // Poster som inte kommit via parsern kan bära negativa värden, och
        // omvandlade till uint64 skulle de spränga summorna nedan.
        if (e.point_count < 0 || e.byte_size < 0)
            continue;
        if (e.key.level < 0 || e.key.level > kCopcMaxLevel)
            continue;
        if (max_level >= 0 && e.key.level > max_level)
            continue;

        const CopcBounds b = copc_node_bounds(info, e.key);

        // Icke-ändliga gränser kan uppstå ur en fientlig CopcInfo (halfsize nära
        // DBL_MAX, eller side som underflödar). Ta med noden i stället för att
        // tyst tappa den — konservativt urval bevarar garantin.
        if (!finite_bounds(b)) {
            sel.nodes.push_back(e);
            sel.point_count += static_cast<uint64_t>(e.point_count);
            sel.byte_size   += static_cast<uint64_t>(e.byte_size);
            continue;
        }
        // Överlapp, inte inneslutning. Gränsfall räknas som överlapp så att
        // punkter exakt på kanten inte tappas.
        if (b.max_x < min_x || b.min_x > max_x ||
            b.max_y < min_y || b.min_y > max_y)
            continue;

        sel.nodes.push_back(e);
        sel.point_count += static_cast<uint64_t>(e.point_count);
        sel.byte_size   += static_cast<uint64_t>(e.byte_size);
    }

    return sel;
}

CopcSelection select_copc_nodes_polygon(const CopcInfo& info,
                                        const std::vector<CopcEntry>& nodes,
                                        const double* poly_x, const double* poly_y,
                                        uint32_t num_vertices,
                                        int max_level) {
    CopcSelection sel;
    if (!poly_x || !poly_y || num_vertices < 3)
        return sel;

    // Polygonens AABB som första, billiga gallring — de flesta noder faller
    // bort där, och det dyra kant-mot-kant-testet körs bara på resten.
    double min_x = poly_x[0], max_x = poly_x[0];
    double min_y = poly_y[0], max_y = poly_y[0];
    for (uint32_t v = 0; v < num_vertices; ++v) {
        if (!std::isfinite(poly_x[v]) || !std::isfinite(poly_y[v]))
            return sel;                       // odefinierad insida
        if (poly_x[v] < min_x) min_x = poly_x[v];
        if (poly_x[v] > max_x) max_x = poly_x[v];
        if (poly_y[v] < min_y) min_y = poly_y[v];
        if (poly_y[v] > max_y) max_y = poly_y[v];
    }

    for (const auto& e : nodes) {
        if (e.is_subpage()) continue;
        if (e.point_count < 0 || e.byte_size < 0) continue;
        if (e.key.level < 0 || e.key.level > kCopcMaxLevel) continue;
        if (max_level >= 0 && e.key.level > max_level) continue;

        const CopcBounds b = copc_node_bounds(info, e.key);
        if (!finite_bounds(b)) {
            sel.nodes.push_back(e);
            sel.point_count += static_cast<uint64_t>(e.point_count);
            sel.byte_size   += static_cast<uint64_t>(e.byte_size);
            continue;
        }
        if (b.max_x < min_x || b.min_x > max_x ||
            b.max_y < min_y || b.min_y > max_y)
            continue;
        if (!rect_intersects_polygon(b.min_x, b.min_y, b.max_x, b.max_y,
                                     poly_x, poly_y, num_vertices))
            continue;

        sel.nodes.push_back(e);
        sel.point_count += static_cast<uint64_t>(e.point_count);
        sel.byte_size   += static_cast<uint64_t>(e.byte_size);
    }
    return sel;
}

} // namespace HLas::detail
