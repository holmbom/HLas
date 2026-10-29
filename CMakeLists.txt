cmake_minimum_required(VERSION 3.20)
project(HLas VERSION 1.1.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_SHARED_LIBS "Build as shared library" OFF)

# Windows exporterar INGENTING ur en DLL utan __declspec(dllexport), och utan
# exporter skapas heller inget importbibliotek — bygget föll då på
# "LNK1181: cannot open input file HLas_core.lib", alltså ett fel som pekar på
# en saknad fil i stället för på en saknad annotering.
#
# Alternativet vore att märka upp hela det publika API:t med ett makro som
# växlar mellan dllexport och dllimport. Det är fyra moduler och en header som
# medvetet inte känner till någon plattform, och priset vore att varje ny
# publik funktion måste komma ihåg makrot. CMake kan i stället lista symbolerna
# ur objektfilerna och generera en .def, och då beter sig bygget som på Linux.
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
option(HLAS_ENABLE_LAZ "Enable native LAZ decompression" ON)

# AV som standard, och det är avsiktligt: det här är den enda flaggan som drar in
# ett externt beroende (libcurl). Den som inte slår på den får ett bibliotek utan
# tredjepartsberoenden överhuvudtaget.
option(HLAS_ENABLE_NET "Enable HTTP range-request support (requires libcurl)" OFF)

# PÅ när HLas byggs för sig själv, AV när ett annat projekt drar in det med
# FetchContent eller add_subdirectory — där ska bara biblioteket byggas, inte
# sviten, mätprogrammen och verktygen.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(hlas_top_level ON)
else()
    set(hlas_top_level OFF)
endif()
option(HLAS_BUILD_TESTS "Build tests, benchmarks and tools" ${hlas_top_level})

# Installationsregler och find_package-konfiguration. Av som förval när ett annat
# projekt drar in HLas, så att dess `cmake --install` inte får med våra filer.
option(HLAS_INSTALL "Generate install rules and the HLas CMake package" ${hlas_top_level})

find_package(Threads REQUIRED)

add_subdirectory(src)

# Paraply för den som vill ha allt — tester, verktyg och program länkar det här.
# Den som bara vill läsa punkter kan länka HLas::core i stället.
#
# HLas::HLas är samma mål under det namn det installerade paketet ger, så att
# FetchContent och find_package(HLas) länkas likadant.
add_library(HLas INTERFACE)
add_library(HLas::HLas ALIAS HLas)
target_link_libraries(HLas INTERFACE HLas_core HLas_copc HLas_dem HLas_write HLas_manager)

# Bara när flaggan är på. Annars skulle alla som länkar paraplyet dra in libcurl,
# och isoleringen vore kosmetisk.
if(HLAS_ENABLE_NET)
    target_link_libraries(HLas INTERFACE HLas_net)
endif()

# Windows har ingen RPATH. En .exe letar sina DLL:er i sin EGEN katalog, och
# modulerna byggs en per katalog under src/ — så ett shared-bygge länkade klart
# och vägrade sedan starta med 0xC0000135, "DLL not found", utan att säga vilken.
# På Linux och macOS löser RPATH:en samma sak av sig själv, och därför gör
# funktionen ingenting där.
function(hlas_stage_runtime_dlls dir)
    if(NOT WIN32 OR NOT BUILD_SHARED_LIBS)
        return()
    endif()
    set(libs HLas_core HLas_copc HLas_dem HLas_write HLas_manager)
    if(HLAS_ENABLE_NET)
        list(APPEND libs HLas_net)
    endif()
    # Målen räknas upp ur katalogen i stället för att listas här: tio
    # körbara finns redan, och en lista vid sidan om hade tystnat vid den elfte.
    get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(t IN LISTS targets)
        get_target_property(type ${t} TYPE)
        if(type STREQUAL "EXECUTABLE")
            foreach(lib IN LISTS libs)
                add_custom_command(TARGET ${t} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            $<TARGET_FILE:${lib}> $<TARGET_FILE_DIR:${t}>)
            endforeach()
        endif()
    endforeach()
endfunction()

# Anropas SIST i tests/ respektive tools/ egna CMakeLists: add_custom_command(TARGET)
# kräver att målet skapats i samma katalog, så den kan inte köras härifrån.
if(HLAS_BUILD_TESTS)
    add_subdirectory(tests)
    add_subdirectory(tools)
endif()

# ---- Installation -----------------------------------------------------------
#
# Det installerade paketet bär biblioteken och den enda publika headern. De
# interna headers under src/ installeras inte: allt API:t når går genom HLas.h.
# Exportnamnen är modulernas, så målen heter HLas::core, HLas::manager och så
# vidare även efter find_package — samma namn som aliasen i källbygget.
if(HLAS_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(hlas_install_targets HLas_core HLas_copc HLas_dem HLas_write HLas_manager)
    if(HLAS_ENABLE_NET)
        list(APPEND hlas_install_targets HLas_net)
    endif()
    foreach(t IN LISTS hlas_install_targets)
        string(REPLACE "HLas_" "" export_name ${t})
        set_target_properties(${t} PROPERTIES EXPORT_NAME ${export_name})
    endforeach()

    install(TARGETS ${hlas_install_targets} HLas
        EXPORT HLasTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES include/HLas.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT HLasTargets
        NAMESPACE HLas::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HLas)
    configure_package_config_file(cmake/HLasConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/HLasConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HLas)
    # Inom samma major är API:t kompatibelt; en ny major får inte väljas tyst.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/HLasConfigVersion.cmake
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/HLasConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/HLasConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HLas)
endif()
