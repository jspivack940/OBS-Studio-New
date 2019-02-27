project(obs-shader-filter)

set(CMAKE_PREFIX_PATH "${QTDIR}")
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

#find_package(LibObs REQUIRED)
find_package(Qt5Core REQUIRED)
find_package(Qt5Widgets REQUIRED)

include_directories(
#	SYSTEM "${CMAKE_SOURCE_DIR}/libobs"
#	"${CMAKE_SOURCE_DIR}/UI/obs-frontend-api"
	${Qt5Core_INCLUDES}
	${Qt5Widgets_INCLUDES}
)

find_package(FFmpeg REQUIRED
	COMPONENTS avcodec)
include_directories(${FFMPEG_INCLUDE_DIRS})

if(MSVC)
	set(obs-shader-filter_PLATFORM_DEPS
		w32-pthreads)
endif()

#add_subdirectory(muparser)

set(muparser_HEADERS
	muParserBase.h
	muParserBytecode.h
	muParserCallback.h
	muParserDef.h
	muParserDLL.h
	muParserError.h
	muParserFixes.h
	muParser.h
	muParserInt.h
	muParserStack.h
	muParserTemplateMagic.h
	#muParserTest.h
	muParserToken.h
	muParserTokenReader.h
)

set(muparser_SOURCES
	muParserBase.cpp
	muParserBytecode.cpp
	muParserCallback.cpp
	muParser.cpp
	muParserDLL.cpp
	muParserError.cpp
	muParserInt.cpp
	#muParserTest.cpp
	muParserTokenReader.cpp
)

set(obs-shader-filter_HEADERS
	fft.h
	tinyexpr.h
	mtrandom.h
	obs-shader-filter.hpp
)

set(obs-shader-filter_SOURCES
	fft.c
	obs-shader-filter.cpp
	tinyexpr.c
	mtrandom.cpp
)

add_library(obs-shader-filter MODULE
	${muparser_HEADERS}
	${muparser_SOURCES}
	${obs-shader-filter_SOURCES}
	${obs-shader-filter_HEADERS}
)
	
target_link_libraries(obs-shader-filter
	libobs
	Qt5::Core
	Qt5::Widgets
	${obs-shader-filter_PLATFORM_DEPS}
	${FFMPEG_LIBRARIES}
#	obs-frontend-api
#	Qt5::Core
#	Qt5::Widgets
)

install_obs_plugin_with_data(obs-shader-filter data)
