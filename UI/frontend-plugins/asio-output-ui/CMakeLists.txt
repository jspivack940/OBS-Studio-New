cmake_minimum_required(VERSION 3.22...3.25)

find_package(Qt6 REQUIRED Widgets)

add_library(asio-output-ui MODULE)
add_library(OBS::asio-output-ui ALIAS asio-output-ui)

target_sources(asio-output-ui PRIVATE # cmake-format: sortable
                                      asio-ui-main.cpp asio-ui-main.h ASIOSettingsDialog.cpp ASIOSettingsDialog.h)
target_sources(asio-output-ui PRIVATE forms/output.ui)

target_link_libraries(asio-output-ui PRIVATE OBS::libobs OBS::frontend-api OBS::ui-support Qt::Widgets)

configure_file(cmake/windows/obs-module.rc.in asio-output-ui.rc)
target_sources(asio-output-ui PRIVATE asio-output-ui.rc)

# cmake-format: off
set_property(TARGET asio-output-ui APPEND PROPERTY AUTORCC_OPTIONS --format-version 1)
# cmake-format: on

# cmake-format: off
set_target_properties_obs(
  asio-output-ui
  PROPERTIES FOLDER frontend
             PREFIX ""
             AUTOMOC ON
             AUTOUIC ON
             AUTORCC ON
             AUTOUIC_SEARCH_PATHS forms)
# cmake-format: on
