# net — ByteSource över HTTP range requests.
#
# ENDA modulen som får ha ett externt beroende, och den byggs bara med
# -DHLAS_ENABLE_NET=ON. Paraply-targetet HLas refererar den inte när flaggan är
# av, så den som bara vill läsa lokala filer ser aldrig libcurl.
#
# Behövs inte vår implementation räcker det att ärva ByteSource och koppla in en
# befintlig HTTP-klient — interfacet är hela integrationspunkten.
find_package(CURL REQUIRED)

add_library(HLas_net http_byte_source.cpp)
add_library(HLas::net ALIAS HLas_net)

target_link_libraries(HLas_net PUBLIC HLas_core PRIVATE CURL::libcurl)
target_compile_definitions(HLas_net PUBLIC HLAS_HAVE_NET=1)

target_include_directories(HLas_net PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# net behöver se den publika headern, till skillnad från core och copc.
#
# Skälet är NetworkSettings: den är en publik värdetyp som måste finnas i
# HLas.h, och att spegla den i en intern kopia här vore precis den sortens
# dubblering som resten av kodbasen undviker — två kopior kan glida isär, en kan
# inte. Pilen skapar ingen cykel: net når manager lika lite som förut, och net
# används bara GENOM manager, som redan har samma sökväg.
#
# PRIVATE och inte PUBLIC: sökvägen behövs när HLas_net kompileras, inte av den
# som länkar den. Den som länkar net gör det genom manager och har den redan.
target_include_directories(HLas_net PRIVATE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
