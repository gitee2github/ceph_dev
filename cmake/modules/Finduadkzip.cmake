# - Find uadkzip
# Find the uadkzip compression library and includes
#
# UADK_INCLUDE_DIR - where to find uadkzip.h, etc.
# UADK_LIBRARIES - List of libraries when using uadkzip.
# UADK_FOUND - True if uadkzip found.

find_path(UADKZIP_INCLUDE_DIR NAMES uadk/wd_comp.h)
message("UADKZIP_INCLUDE_DIR: " ${UADKZIP_INCLUDE_DIR})

find_library(UADKZIP_LIBRARIES NAMES wd_comp)
message("UADKZIP_LIBRARIES: " ${UADKZIP_LIBRARIES})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(uadkzip DEFAULT_MSG UADKZIP_LIBRARIES UADKZIP_INCLUDE_DIR)

if(uadkzip_FOUND AND NOT TARGET uadkzip::uadkzip)
  add_library(uadkzip::uadkzip UNKNOWN IMPORTED)
  set_target_properties(uadkzip::uadkzip PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${UADKZIP_INCLUDE_DIR}"
    IMPORTED_LINK_INTERFACE_LANGUAGES "C"
    IMPORTED_LOCATION "${UADKZIP_LIBRARIES}")
endif()

mark_as_advanced(UADKZIP_LIBRARIES UADKZIP_INCLUDE_DIR)
