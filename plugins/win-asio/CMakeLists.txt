cmake_minimum_required(VERSION 3.24...3.25)

legacy_check()

add_library(win-asio MODULE)
add_library(OBS::asio ALIAS win-asio)

target_sources(win-asio PRIVATE win-asio.cpp asio-loader.hpp)

set(MODULE_DESCRIPTION "OBS ASIO module")

configure_file(cmake/windows/obs-module.rc.in win-asio.rc)

target_sources(win-asio PRIVATE win-asio.rc)

target_link_libraries(win-asio PRIVATE OBS::libobs OBS::frontend-api)

# cmake-format: off
set_target_properties_obs(win-asio PROPERTIES FOLDER "plugins" PREFIX "")
# cmake-format: on
