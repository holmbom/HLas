// Engångsverktyg: skriver en okomprimerad LAS 1.2 ur en LAZ. Inte del av biblioteket.
#include "HLas.h"
#include <cstdio>
#include <cstring>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "bruk: make_las <in.laz> <ut.las>\n"); return 2; }
    HLas::Reader r;
    if (r.open(argv[1]) != HLas::Status::Ok) { fprintf(stderr,"open: %s\n", r.last_error().c_str()); return 1; }
    const auto& h = r.header();
    if (h.version_major != 1 || h.version_minor != 2) { fprintf(stderr,"kräver LAS 1.2-källa\n"); return 1; }

    uint8_t hdr[227]; std::memset(hdr, 0, sizeof hdr);
    std::memcpy(hdr, "LASF", 4);
    auto w16=[&](size_t o,uint16_t v){std::memcpy(hdr+o,&v,2);};
    auto w32=[&](size_t o,uint32_t v){std::memcpy(hdr+o,&v,4);};
    auto wd =[&](size_t o,double  v){std::memcpy(hdr+o,&v,8);};
    w16(4, h.file_source_id); w16(6, h.global_encoding);
    hdr[24]=1; hdr[25]=2;
    std::memcpy(hdr+26, h.system_id, 32);
    std::snprintf((char*)hdr+58, 32, "HLas make_las");
    w16(90, h.creation_day); w16(92, h.creation_year);
    w16(94, 227); w32(96, 227); w32(100, 0);
    hdr[104] = h.point_format; w16(105, h.point_record_length);
    w32(107, (uint32_t)h.point_count);
    for (int i=0;i<5;i++) w32(111+4*i, (uint32_t)h.points_by_return[i]);
    wd(131,h.x_scale); wd(139,h.y_scale); wd(147,h.z_scale);
    wd(155,h.x_offset); wd(163,h.y_offset); wd(171,h.z_offset);
    wd(179,h.max_x); wd(187,h.min_x);
    wd(195,h.max_y); wd(203,h.min_y);
    wd(211,h.max_z); wd(219,h.min_z);

    FILE* f = std::fopen(argv[2], "wb");
    if (!f) { fprintf(stderr,"kan inte skriva %s\n", argv[2]); return 1; }
    std::fwrite(hdr, 1, sizeof hdr, f);

    const uint64_t BATCH = 1u<<20;
    std::vector<uint8_t> buf(BATCH * h.point_record_length);
    uint64_t done = 0;
    while (done < h.point_count) {
        uint64_t n = r.read_points(done, BATCH, buf.data());
        if (n == 0) break;
        std::fwrite(buf.data(), h.point_record_length, (size_t)n, f);
        done += n;
    }
    std::fclose(f);
    printf("skrev %llu punkter (%d B/pt, fmt %d) till %s\n",
           (unsigned long long)done, h.point_record_length, h.point_format, argv[2]);
    return done == h.point_count ? 0 : 1;
}
