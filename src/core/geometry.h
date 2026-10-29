#pragma once
#include <cstdint>

namespace HLas::detail {

// Ray casting. Ligger här och inte i reader.cpp för att både kärnans
// polygonfilter och DEM-modulen behöver den, och beroendet ska peka åt ett håll.
inline bool point_in_polygon(double px, double py,
                             const double* vx, const double* vy, uint32_t n) {
    bool inside = false;
    for (uint32_t i = 0, j = n - 1; i < n; j = i++) {
        if ((vy[i] > py) != (vy[j] > py) &&
            px < (vx[j] - vx[i]) * (py - vy[i]) / (vy[j] - vy[i]) + vx[i])
            inside = !inside;
    }
    return inside;
}

// Skär två sträckor varandra? Används för att avgöra om en nods rektangel
// berör en polygon utan att någon av dem innehåller den andra — tänk en lång
// korridor som passerar tvärs över en nod.
inline bool segments_intersect(double ax, double ay, double bx, double by,
                               double cx, double cy, double dx, double dy) {
    auto orient = [](double px, double py, double qx, double qy,
                     double rx, double ry) {
        const double v = (qx - px) * (ry - py) - (qy - py) * (rx - px);
        if (v > 0) return 1;
        if (v < 0) return -1;
        return 0;
    };
    auto on_seg = [](double px, double py, double qx, double qy,
                     double rx, double ry) {
        return qx <= (px > rx ? px : rx) && qx >= (px < rx ? px : rx) &&
               qy <= (py > ry ? py : ry) && qy >= (py < ry ? py : ry);
    };

    const int o1 = orient(ax, ay, bx, by, cx, cy);
    const int o2 = orient(ax, ay, bx, by, dx, dy);
    const int o3 = orient(cx, cy, dx, dy, ax, ay);
    const int o4 = orient(cx, cy, dx, dy, bx, by);

    if (o1 != o2 && o3 != o4) return true;

    // Kollineära gränsfall — en punkt som ligger på den andra sträckan räknas.
    if (o1 == 0 && on_seg(ax, ay, cx, cy, bx, by)) return true;
    if (o2 == 0 && on_seg(ax, ay, dx, dy, bx, by)) return true;
    if (o3 == 0 && on_seg(cx, cy, ax, ay, dx, dy)) return true;
    if (o4 == 0 && on_seg(cx, cy, bx, by, dx, dy)) return true;
    return false;
}

// Överlappar rektangeln polygonen?
//
// Tre fall måste täckas, och alla tre behövs:
//   1. ett polygonhörn ligger i rektangeln  (polygonen sticker in)
//   2. ett rektangelhörn ligger i polygonen  (rektangeln ligger inuti)
//   3. en polygonkant korsar en rektangelkant (de korsar varandra utan att
//      något hörn ligger i den andra — en korridor tvärs över en nod)
//
// Utan fall 3 skulle en lång smal korridor som passerar rakt genom en nod
// rapporteras som "berör inte", och punkterna där skulle tappas.
inline bool rect_intersects_polygon(double min_x, double min_y,
                                    double max_x, double max_y,
                                    const double* vx, const double* vy,
                                    uint32_t n) {
    if (n < 3) return false;

    // 1
    for (uint32_t i = 0; i < n; ++i)
        if (vx[i] >= min_x && vx[i] <= max_x && vy[i] >= min_y && vy[i] <= max_y)
            return true;

    // 2 — ett hörn räcker; ligger ett i polygonen och inget skär, ligger hela inuti
    if (point_in_polygon(min_x, min_y, vx, vy, n)) return true;

    // 3
    const double rx[4] = { min_x, max_x, max_x, min_x };
    const double ry[4] = { min_y, min_y, max_y, max_y };
    for (uint32_t i = 0, j = n - 1; i < n; j = i++)
        for (int k = 0, m = 3; k < 4; m = k++)
            if (segments_intersect(vx[j], vy[j], vx[i], vy[i],
                                   rx[m], ry[m], rx[k], ry[k]))
                return true;

    return false;
}

// Polygonens x-utbredning inom ett vågrätt band [ybot, ytop].
//
// Rutnätscachen rastrerar: den går rad för rad och behöver veta hur långt åt
// sidorna polygonen når i JUST den raden. Utan det måste hela bbox-intervallet
// svepas, och för en diagonal korridor är det hela rutnätet — vilket är precis
// det fel som gjorde 1.1.3:s sållning verkningslös.
//
// Returnerar false när polygonen inte når bandet alls.
inline bool polygon_row_span(const double* vx, const double* vy, uint32_t n,
                             double ybot, double ytop,
                             double& xmin, double& xmax) {
    if (n < 3) return false;
    bool any = false;
    xmin = 0; xmax = 0;
    auto take = [&](double x) {
        if (!any) { xmin = xmax = x; any = true; }
        else { if (x < xmin) xmin = x; if (x > xmax) xmax = x; }
    };
    for (uint32_t i = 0, j = n - 1; i < n; j = i++) {
        const double ax = vx[j], ay = vy[j], bx = vx[i], by = vy[i];
        // Hörn inne i bandet.
        if (ay >= ybot && ay <= ytop) take(ax);
        if (by >= ybot && by <= ytop) take(bx);
        // Där kanten skär bandets kanter.
        if (ay == by) continue;
        const double lo = ay < by ? ay : by, hi = ay < by ? by : ay;
        for (int k = 0; k < 2; ++k) {
            const double yy = k ? ytop : ybot;
            if (yy < lo || yy > hi) continue;
            const double t = (yy - ay) / (by - ay);
            take(ax + t * (bx - ax));
        }
    }
    return any;
}

} // namespace HLas::detail
