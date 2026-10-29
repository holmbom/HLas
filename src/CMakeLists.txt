# Moduler. Beroenden pekar åt ett håll:
#
#   core      LAS/LAZ-parsning + dekompression          noll beroenden
#   copc      octree-index, nod-uppslagning             beror på core
#   dem       grid-binning, nearest/IDW/TIN             beror på core
#   write     LAS- och LAZ-utskrift                     beror på core
#   manager   Reader, sidecar-cache                    beror på core + dem
#   net       ByteSource över HTTP (libcurl)            beror på core, opt-in
#
# net/ är den enda modulen med ett externt beroende och byggs bara när
# HLAS_ENABLE_NET är på. Övriga moduler är beroendefria — core och copc länkar
# ingenting alls, dem och manager bara pthread.
add_subdirectory(core)
add_subdirectory(copc)
add_subdirectory(dem)
add_subdirectory(write)
add_subdirectory(manager)

if(HLAS_ENABLE_NET)
    add_subdirectory(net)
endif()
