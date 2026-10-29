# write — LAS- och LAZ-utskrift.
#
# Biblioteket är i övrigt läsare-bara. Modulen finns för att kunna spara ut ett
# urval som en fil andra verktyg kan läsa.
#
# LAZ-kodningen är HLas egen och ligger i core (laz/arithmetic_encoder,
# laz/point14_encoder) tillsammans med avkodaren de speglar. Den här modulen
# äger filen: header, LAZ-VLR, chunkning och chunk-tabell. Inga externa
# beroenden — referensimplementationen användes under 1.1.0 och är borta sedan 1.1.1.
#
# Beroendet på core är för path_utf8.h: sökvägar är UTF-8 i hela API:t, och på
# Windows kräver det W-varianterna av fil-API:et. Konverteringen ligger på ett
# ställe i core så att varje filöppning i biblioteket går samma väg.
add_library(HLas_write
    las_writer.cpp
    laz_writer.cpp
    laz_chunk_table.cpp
)
add_library(HLas::write ALIAS HLas_write)

target_link_libraries(HLas_write PUBLIC HLas_core)

target_include_directories(HLas_write PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
