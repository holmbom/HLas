# core — LAS/LAZ-parsning och dekompression.
#
# Noll beroenden: varken på de andra modulerna eller på något externt. Den vet
# inte att dem/, manager/ eller den publika HLas.h finns. Include-stigarna nedan
# är det som håller den gränsen — core får ingen väg till de andras headers,
# så ett felaktigt beroende blir ett kompileringsfel, inte en kodgranskningsfråga.
add_library(HLas_core
    las_error.cpp
    file_io.cpp
    header_parser.cpp
    vlr_parser.cpp
    point_reader.cpp
)
add_library(HLas::core ALIAS HLas_core)

if(HLAS_ENABLE_LAZ)
    target_sources(HLas_core PRIVATE
        laz_reader.cpp
        laz/arithmetic.cpp
        laz/arithmetic_encoder.cpp
        laz/point14_encoder.cpp
        laz/integer_compressor.cpp
        laz/point10.cpp
        laz/point10_encoder.cpp
        laz/gpstime11.cpp
        laz/gpstime11_encoder.cpp
        laz/rgb12.cpp
        laz/rgb12_encoder.cpp
        laz/byte_item.cpp
        laz/point14.cpp
        laz/rgb14.cpp
        laz/rgb14_encoder.cpp
        laz/byte14.cpp
    )
    # PUBLIC: dem/ och manager/ måste se samma flagga, annars skiljer sig
    # PointSource-layouten mellan översättningsenheter.
    target_compile_definitions(HLas_core PUBLIC HLAS_HAVE_LAZ=1)
endif()

target_include_directories(HLas_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
