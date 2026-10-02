# FFmpeg for movie (PSS/MPEG-2) playback: libavformat (PS demux), libavcodec (mpeg2video),
# libswscale (YUV->RGBA). System dev packages, found via pkg-config (REQUIRED).
# Provides FFmpeg::avformat FFmpeg::avcodec FFmpeg::swscale FFmpeg::avutil with SYSTEM includes,
# so their headers never warn under our -Wall.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libswscale libavutil)
add_library(FFmpeg INTERFACE IMPORTED GLOBAL)
target_link_libraries(FFmpeg INTERFACE PkgConfig::FFMPEG)
target_include_directories(FFmpeg SYSTEM INTERFACE ${FFMPEG_INCLUDE_DIRS})
