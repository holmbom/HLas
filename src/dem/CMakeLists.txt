# dem — grid-binning och fyllnadsmetoder (nearest, IDW, TIN).
#
# Beror på core (PointSource) och på den publika headern (DemResult, DemFill).
# Vet inget om Reader, sidecars eller var punkterna kom ifrån.
add_library(HLas_dem dem.cpp)
add_library(HLas::dem ALIAS HLas_dem)

target_link_libraries(HLas_dem PUBLIC HLas_core Threads::Threads)

target_include_directories(HLas_dem PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
