cmake_minimum_required(VERSION 3.24...3.25)

legacy_check()

add_library(win-asio MODULE)
add_library(OBS::asio ALIAS win-asio)
set(MODULE_DESCRIPTION "OBS ASIO module")

find_package(Qt6 REQUIRED Widgets)

target_sources(win-asio PRIVATE plugin-main.cpp win-asio.cpp ASIODevice.cpp ASIODevice.hpp ASIOSettingsDialog.cpp ASIOSettingsDialog.hpp)
target_sources(win-asio PRIVATE win-asio.rc)
target_sources(win-asio PRIVATE forms/output.ui)
configure_file(cmake/windows/obs-module.rc.in win-asio.rc)

target_link_libraries(
  win-asio
  PRIVATE OBS::libobs
          OBS::frontend-api
          OBS::ui-support
          Qt::Widgets)

# cmake-format: off
set_property(TARGET win-asio APPEND PROPERTY AUTORCC_OPTIONS --format-version 1)
set_target_properties_obs(
  win-asio
  PROPERTIES FOLDER plugins
             PREFIX ""
             AUTOMOC ON
             AUTOUIC ON
             AUTORCC ON
             AUTOUIC_SEARCH_PATHS forms)
# cmake-format: on
