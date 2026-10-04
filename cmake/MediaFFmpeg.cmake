# FFmpeg for movie (PSS/MPEG-2) playback: libavformat (PS demux), libavcodec (mpeg2video),
# libswscale (YUV->RGBA). System dev packages, found via pkg-config (REQUIRED).
# Provides FFmpeg::avformat FFmpeg::avcodec FFmpeg::swscale FFmpeg::avutil with SYSTEM includes,
# so their headers never warn under our -Wall.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libswscale libavutil)
add_library(FFmpeg INTERFACE IMPORTED GLOBAL)
target_link_libraries(FFmpeg INTERFACE PkgConfig::FFMPEG)
if(WIN32 AND FFMPEG_ROOT)
  file(GLOB FFMPEG_RUNTIME_DLLS CONFIGURE_DEPENDS "${FFMPEG_ROOT}/bin/*.dll")
  if(NOT FFMPEG_RUNTIME_DLLS)
    message(FATAL_ERROR "No FFmpeg runtime DLLs found under ${FFMPEG_ROOT}/bin")
  endif()
  install(FILES ${FFMPEG_RUNTIME_DLLS} DESTINATION bin)
  install(FILES "${FFMPEG_ROOT}/LICENSE.txt" "${FFMPEG_ROOT}/COPYING.GPLv3"
    DESTINATION share/doc/nightfire/licenses)
endif()
