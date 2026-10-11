# CMake commun à toutes les applications (géré par build.sh, ne pas modifier).
# Compile automatiquement tous les .c de <appli>/src/ et rend visibles
# les en-têtes de <appli>/include/ et de la racine du dépôt (protocol_icd.h).
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
if(NOT APP_SOURCES)
  message(FATAL_ERROR "Aucun fichier .c dans ${CMAKE_CURRENT_SOURCE_DIR}/src/")
endif()
target_sources(app PRIVATE ${APP_SOURCES})
target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/..)
