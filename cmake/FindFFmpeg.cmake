# FindFFmpeg.cmake
# Find the FFmpeg includes and libraries

# This module defines
#  FFMPEG_INCLUDE_DIRS, where to find the FFmpeg headers
#  FFMPEG_LIBRARIES, the libraries needed to use FFmpeg
#  FFMPEG_FOUND, If false, do not try to use FFmpeg

find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h
  PATHS /usr/include /usr/local/include
  PATH_SUFFIXES ffmpeg)
find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h
  PATHS /usr/include /usr/local/include
  PATH_SUFFIXES ffmpeg)
find_path(AVUTIL_INCLUDE_DIR libavutil/avutil.h
  PATHS /usr/include /usr/local/include
  PATH_SUFFIXES ffmpeg)

find_library(AVCODEC_LIBRARY avcodec
  PATHS /usr/lib /usr/local/lib)
find_library(AVFORMAT_LIBRARY avformat
  PATHS /usr/lib /usr/local/lib)
find_library(AVUTIL_LIBRARY avutil
  PATHS /usr/lib /usr/local/lib)

set(FFMPEG_INCLUDE_DIRS
  ${AVCODEC_INCLUDE_DIR}
  ${AVFORMAT_INCLUDE_DIR}
  ${AVUTIL_INCLUDE_DIR})

set(FFMPEG_LIBRARIES
  ${AVCODEC_LIBRARY}
  ${AVFORMAT_LIBRARY}
  ${AVUTIL_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg DEFAULT_MSG
  AVCODEC_INCLUDE_DIR AVFORMAT_INCLUDE_DIR AVUTIL_INCLUDE_DIR
  AVCODEC_LIBRARY AVFORMAT_LIBRARY AVUTIL_LIBRARY)

mark_as_advanced(
  AVCODEC_INCLUDE_DIR AVFORMAT_INCLUDE_DIR AVUTIL_INCLUDE_DIR
  AVCODEC_LIBRARY AVFORMAT_LIBRARY AVUTIL_LIBRARY)
