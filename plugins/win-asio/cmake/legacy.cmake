project(win-asio)

add_library(win-asio MODULE)
add_library(OBS::asio ALIAS win-asio)

target_sources(win-asio PRIVATE win-asio.cpp asio-loader.hpp)

set(MODULE_DESCRIPTION "OBS ASIO module")

configure_file(${CMAKE_SOURCE_DIR}/cmake/bundle/windows/obs-module.rc.in win-asio.rc)

target_sources(win-asio PRIVATE win-asio.rc)

target_link_libraries(win-asio PRIVATE OBS::libobs)

set_target_properties(win-asio PROPERTIES FOLDER "plugins")

setup_plugin_target(win-asio)
