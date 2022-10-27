cmake_minimum_required(VERSION 3.24...3.25)

add_library(win-asio MODULE)
add_library(OBS::asio ALIAS win-asio)
set(MODULE_DESCRIPTION "OBS ASIO module")

target_sources(win-asio PRIVATE # cmake-format: sortable
                                ASIODevice.cpp ASIODevice.hpp plugin-main.cpp win-asio.cpp)

target_link_libraries(win-asio PRIVATE OBS::libobs OBS::frontend-api)

configure_file(cmake/windows/obs-module.rc.in win-asio.rc)
target_sources(win-asio PRIVATE win-asio.rc)

# cmake-format: off
set_property(TARGET win-asio APPEND PROPERTY AUTORCC_OPTIONS --format-version 1)
set_target_properties_obs(win-asio PROPERTIES FOLDER plugins PREFIX "")
# cmake-format: on
