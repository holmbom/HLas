add_executable(HLas_tests
    main.cpp
    test_types.cpp
    test_file_io.cpp
    test_header.cpp
    test_vlr.cpp
    test_point_reader.cpp
    test_reader.cpp
    test_copc.cpp
    test_copc_reader.cpp
    test_net_settings.cpp
    test_point_view.cpp
    test_las_writer.cpp
    test_laz_encoder.cpp
    test_golden.cpp
    test_golden10.cpp
    test_reencode_legacy.cpp
    test_integration.cpp
    test_sidecar.cpp
    test_sidecar_lite.cpp
)
target_link_libraries(HLas_tests PRIVATE HLas)

add_executable(HLas_bench bench_dem.cpp)
target_link_libraries(HLas_bench PRIVATE HLas)

# Laboratorium för sidecar-formen, inte ett test — inga påståenden, bara siffror.
# Se doc/plans/sidecar.md för frågan det ska svara på.
add_executable(HLas_bench_sidecar bench_sidecar.cpp)
target_link_libraries(HLas_bench_sidecar PRIVATE HLas)

# Lönar sig en sidecar för en .las? Mätning inför 1.1.4.
add_executable(HLas_bench_las_vs_cache bench_las_vs_cache.cpp)
target_link_libraries(HLas_bench_las_vs_cache PRIVATE HLas)

# Histogram i headern och taket för LOD. Mätning inför 1.1.4.
add_executable(HLas_bench_hist_lod bench_hist_lod.cpp)
target_link_libraries(HLas_bench_hist_lod PRIVATE HLas)

# Håller budget-uppskattningen för en sned korridor? Mätning inför 1.1.4.
add_executable(HLas_bench_corridor bench_corridor.cpp)
target_link_libraries(HLas_bench_corridor PRIVATE HLas)

# Rutnät mot Hilbert-block, båda med korrekt polygonsållning. Inför 1.1.4.
add_executable(HLas_bench_grid bench_grid.cpp)
target_link_libraries(HLas_bench_grid PRIVATE HLas)

# Standalone runner for the sidecar test (no dependency on legacy las/ data)
add_executable(HLas_test_sidecar
    test_sidecar.cpp
    test_sidecar_lite.cpp
    test_sidecar_main.cpp
)
target_link_libraries(HLas_test_sidecar PRIVATE HLas)

# A/B mot föregående version. Bara API som finns i båda.
add_executable(HLas_bench_ab bench_ab.cpp)
target_link_libraries(HLas_bench_ab PRIVATE HLas)

# DLL:erna bredvid de körbara — se hlas_stage_runtime_dlls i rot-CMakeLists.
hlas_stage_runtime_dlls(${CMAKE_CURRENT_SOURCE_DIR})
