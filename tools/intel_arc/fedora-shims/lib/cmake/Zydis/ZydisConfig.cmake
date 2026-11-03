# Fedora ships Zydis with pkg-config only; this gives the build its Zydis::Zydis target.
find_package(PkgConfig REQUIRED)
pkg_check_modules(_ZYDIS REQUIRED IMPORTED_TARGET zydis)
if(NOT TARGET Zydis::Zydis)
  add_library(Zydis::Zydis INTERFACE IMPORTED)
  target_link_libraries(Zydis::Zydis INTERFACE PkgConfig::_ZYDIS)
endif()
set(Zydis_FOUND TRUE)
