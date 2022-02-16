project(obs-ffmpeg-mux)

if(DEBUG_FFMPEG_MUX)
	add_definitions(-DDEBUG_FFMPEG)
endif()

if(WIN32)
  add_definitions(-DWIN32_LEAN_AND_MEAN)
endif()

find_package(FFmpeg REQUIRED
	COMPONENTS avcodec avutil avformat)
include_directories(${FFMPEG_INCLUDE_DIRS})

find_package(Librist QUIET)
find_package(Libsrt QUIET)

if(WIN32)
	set(LIBSRT_PLATFORM_DEPS
		ws2_32)
endif()

set(obs-ffmpeg-mux_SOURCES
	ffmpeg-mux.c)

set(obs-ffmpeg-mux_HEADERS
	ffmpeg-mux.h
	ffmpeg-mux-url.h
	ffmpeg-mux-srt.h
	ffmpeg-mux-rist.h)

add_executable(obs-ffmpeg-mux
	${obs-ffmpeg-mux_SOURCES}
	${obs-ffmpeg-mux_HEADERS}	)

target_link_libraries(obs-ffmpeg-mux
	libobs
	${LIBRIST_LIBRARIES}
	${LIBSRT_LIBRARIES}
	${LIBSRT_PLATFORM_DEPS}
	${FFMPEG_LIBRARIES})

set_target_properties(obs-ffmpeg-mux PROPERTIES FOLDER "plugins/obs-ffmpeg")

install_obs_core(obs-ffmpeg-mux)
