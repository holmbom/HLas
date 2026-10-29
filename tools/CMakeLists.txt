# Hjälpverktyg — inte del av biblioteket.
add_executable(HLas_make_las make_las.cpp)
target_link_libraries(HLas_make_las PRIVATE HLas)

# DLL:erna bredvid de körbara — se hlas_stage_runtime_dlls i rot-CMakeLists.
hlas_stage_runtime_dlls(${CMAKE_CURRENT_SOURCE_DIR})
