project(obs-layer-view)

set(CMAKE_PREFIX_PATH "${QTDIR}")
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

find_package(LibObs REQUIRED)
find_package(Qt5Core REQUIRED)
find_package(Qt5Widgets REQUIRED)

include_directories(
	${CMAKE_SOURCE_DIR}/UI/obs-frontend-api
	${Qt5Core_INCLUDES}
	${Qt5Widgets_INCLUDES}
)

set(obs-layer-view_HEADERS
	obs-layer-view.hpp)
	
set(obs-layer-view_SOURCES
	obs-layer-view.cpp)

set(obs-layer-view_QRC
	obs-layer-view.qrc
	)

#qt5_add_resources(obs-layer-view_QRC_SOURCES ${obs-layer-view_QRC})

add_library(obs-layer-view MODULE
	${obs-layer-view_SOURCES}
	${obs-layer-view_HEADERS}
#	${obs-layer-view_QRC_SOURCES}
)

target_link_libraries(obs-layer-view
	libobs
	obs-frontend-api
	Qt5::Core
	Qt5::Widgets
)

install_obs_plugin_with_data(obs-layer-view data)