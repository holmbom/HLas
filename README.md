**English** · [Svenska](README.sv.md)

# HLas

A fast C++20 library for reading LAS and LAZ point clouds, with no
dependencies. It reads every point format through one memory-mapped view, has
its own LAZ decoder and encoder, and reads COPC files partially, locally or over
HTTP, fetching only the octree nodes a query touches.

The focus is the corridor query: a buffered polyline over a large tile. With the
optional `.hlz` cache, such a query on a 140 M-point file takes 0.2–9 ms.

## Features

- **LAS 1.1–1.4**, point formats 0–10, through one format-aware `PointView`:
  every field the file carries, nothing converted or thrown away.
- **LAZ** decoding and encoding written from scratch for point formats 0–3, 6
  and 7. Re-encoding files written by third-party tools gives back exactly the
  bytes on disk.
- **COPC:** partial reads by region or polygon, streamed node by node, with
  separate knobs for threads and for connections to the server.
- **Queries:** XYZ in batches, classification filters, polygon clipping,
  classification histograms and DEMs (Nearest, IDW, TIN), multi-threaded.
- **Cache:** a bit-packed grid of about 5 bytes per point that opens in ~0 ms and
  doubles as a spatial index. Level of detail per class that gives the same
  points zoomed in and out.
- **Writing a selection** as LAS or LAZ: `Lite` (XYZ + class) or `Full` (the
  source records unchanged).
- **Errors are never silent:** a `Status` from `open()`, and `last_error()` after
  every call.

## Reading a file

```cpp
#include "HLas.h"

HLas::Reader reader;
if (reader.open("tile.laz") != HLas::Status::Ok)
    return 1;                                   // reader.last_error() says why

const HLas::PointView pts = reader.points();    // every field, every format
for (uint64_t i = 0; i < pts.count(); ++i) {
    double x = pts.x(i), y = pts.y(i), z = pts.z(i);
    uint8_t cls = pts.classification(i);
}

// Ground and water inside a polygon, as a 1 m DEM on four threads.
bool accepted[256] = {};
accepted[2] = accepted[9] = true;
HLas::DemResult dem = reader.generate_dem(poly_x, poly_y, n, 1.0,
                                          HLas::DemFill::Tin, accepted, 4);
```

## Using HLas in your project

```cmake
include(FetchContent)
FetchContent_Declare(HLas
    GIT_REPOSITORY https://github.com/holmbom/HLas.git
    GIT_TAG        main)   # or a release tag, e.g. vX.Y.Z
FetchContent_MakeAvailable(HLas)

target_link_libraries(myapp PRIVATE HLas)
```

Only the library is built in your project; the tests, benchmarks and tools are
not. `HLas` links everything. A program that only reads points can link
`HLas::core` alone, which pulls in neither the DEM nor the cache.

## Building

Requirements: CMake 3.20+, Ninja and a C++20 compiler. Linux, Windows (MSVC or
MSYS2) and macOS; macOS is not yet tested.

```sh
cmake -B build -G Ninja
cmake --build build
./build/tests/HLas_tests
```

- `-DHLAS_ENABLE_LAZ=OFF` builds without LAZ.
- `-DHLAS_ENABLE_NET=ON` enables HTTP range requests and needs libcurl, the only
  external dependency, and an optional one.
- `-DBUILD_SHARED_LIBS=ON` builds shared libraries.
- `-DHLAS_BUILD_TESTS=OFF` skips the tests, benchmarks and tools. It is on when
  HLas is built on its own and off when another project pulls it in.

The test files are not in the repository. Tests whose file is missing print
`SKIP`; [tests/test_data.h](tests/test_data.h) says where to download them.

## Documentation

- [README.sv.md](README.sv.md): the full description, with every API call,
  the cache format and the measurements. In Swedish.
- [CLAUDE.md](CLAUDE.md): the developer guide: modules, design decisions and
  the traps on each platform. Written for both people and AI coding tools. In
  Swedish.
- [doc/CHANGELOG.md](doc/CHANGELOG.md) and [TODO.md](TODO.md): what is done
  and what is left.

## License

[MIT](LICENSE) © 2026 Erik Holmbom

Third-party material (the GPS time encoding in the LAZ encoder) is listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
