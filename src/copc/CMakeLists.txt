# copc — octree-index: copc info-VLR och hierarkisidor.
#
# Beror på core (Vlr, ErrorCode) och på ingenting annat. Ren tolkning av byte
# som redan finns i minnet — den vet inte var de kom ifrån, vilket är hela
# poängen: samma kod ska fungera mot en mmap:ad fil och mot en HTTP-hämtning.
add_library(HLas_copc
    copc_index.cpp
    copc_node.cpp
    copc_query.cpp
)
add_library(HLas::copc ALIAS HLas_copc)

target_link_libraries(HLas_copc PUBLIC HLas_core)

target_include_directories(HLas_copc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
