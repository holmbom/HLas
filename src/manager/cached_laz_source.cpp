#include "cached_laz_source.h"
#include "header_parser.h"
#include "vlr_parser.h"
#include <cstdio>
#include <string>

namespace HLas::detail {

SidecarHeader sidecar_expectation(const char* path, const ParsedHeader& parsed,
                                  const uint8_t* file_data, size_t file_size) {
    SidecarHeader m;
    m.point_count = parsed.point_count;
    m.x_scale = parsed.x_scale; m.y_scale = parsed.y_scale; m.z_scale = parsed.z_scale;
    m.x_offset = parsed.x_offset; m.y_offset = parsed.y_offset; m.z_offset = parsed.z_offset;
    m.source_format = parsed.point_format_id;
    m.source_size = file_size_64(path);
    m.source_fingerprint = source_fingerprint_of(file_data, file_size);
    return m;
}

ErrorCode LazSource::open(const ParsedHeader& parsed,
                          const uint8_t* file_data, size_t file_size,
                          const std::vector<Vlr>& vlrs) {
    close();
#ifdef HLAS_HAVE_LAZ
    return laz_.init(parsed, file_data, file_size, vlrs);
#else
    (void)parsed; (void)file_data; (void)file_size; (void)vlrs;
    return ErrorCode::LazNotSupported;
#endif
}

void LazSource::close() {}

PointSource LazSource::point_source() const {
#ifdef HLAS_HAVE_LAZ
    return laz_.point_source();
#else
    return PointSource{};
#endif
}

} // namespace HLas::detail
