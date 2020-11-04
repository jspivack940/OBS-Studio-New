project(win-asio)

set(CMAKE_PREFIX_PATH "${QTDIR}")
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LibObs REQUIRED)
find_package(Qt5 REQUIRED Core Widgets)

if(DISABLE_ASIO)
	message(STATUS "Asio support disabled")
	return()
endif()

include_directories(
	/
	SYSTEM "${CMAKE_SOURCE_DIR}/libobs"
	SYSTEM "${CMAKE_SOURCE_DIR}/UI/obs-frontend-api"
	${Qt5Core_INCLUDES}
	${Qt5Widgets_INCLUDES}
)
set(win-asio-sdk_HEADERS
	bassasio-loader.h
	)

set(win-asio_HEADERS
	)

set(win-asio_SOURCES
	asio-input.cpp
)

set(OBS_FRONTEND_LIB "OBS_FRONTEND_LIB-NOTFOUND" CACHE FILEPATH "OBS frontend library")

add_library(win-asio MODULE
	${win-asio_SOURCES}
	${win-asio_HEADERS}
	${win-asio-sdk_HEADERS}
)

target_link_libraries(win-asio
	libobs
	"${OBS_FRONTEND_LIB}"
	Qt5::Core
	Qt5::Widgets
)

install_obs_plugin_with_data(win-asio data)
