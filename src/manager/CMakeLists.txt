# manager — komponerar core och dem till det publika Reader-API:t.
#
# Här bor det som är policy snarare än format: sidecar-cachen.
# Beror på allt ovan; inget ovan beror på den.
add_library(HLas_manager
    reader.cpp
    sidecar_lite.cpp
    cached_laz_source.cpp
    copc_reader.cpp
)
add_library(HLas::manager ALIAS HLas_manager)

target_link_libraries(HLas_manager PUBLIC HLas_core HLas_copc HLas_dem Threads::Threads)

# net är opt-in; manager använder den bara när den byggts.
if(HLAS_ENABLE_NET)
    target_link_libraries(HLas_manager PUBLIC HLas_net)
endif()

target_include_directories(HLas_manager PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
