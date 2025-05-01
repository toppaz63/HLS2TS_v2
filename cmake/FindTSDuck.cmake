# FindTSDuck.cmake - Find TSDuck library
# This module defines
#  TSDUCK_INCLUDE_DIRS, where to find the headers
#  TSDUCK_LIBRARIES, the libraries needed to use TSDuck
#  TSDUCK_FOUND, If false, do not try to use TSDuck

# Try to find tslibrary
find_library(TSDUCK_LIBRARY
    NAMES tsduck
    PATHS /usr/local/lib /usr/lib /opt/tsduck/lib
)

# Try to find include path
find_path(TSDUCK_INCLUDE_DIR
    NAMES tsduck.h
    PATHS /usr/local/include /usr/include /opt/tsduck/include
    PATH_SUFFIXES tsduck
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TSDuck DEFAULT_MSG
    TSDUCK_LIBRARY
    TSDUCK_INCLUDE_DIR
)

if(TSDUCK_FOUND)
    set(TSDUCK_LIBRARIES ${TSDUCK_LIBRARY})
    set(TSDUCK_INCLUDE_DIRS ${TSDUCK_INCLUDE_DIR})
else()
    set(TSDUCK_LIBRARIES)
    set(TSDUCK_INCLUDE_DIRS)
endif()

mark_as_advanced(TSDUCK_INCLUDE_DIR TSDUCK_LIBRARY)
