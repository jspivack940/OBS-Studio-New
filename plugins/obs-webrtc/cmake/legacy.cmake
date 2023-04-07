project(obs-webrtc)

option(ENABLE_WEBRTC "Enable WebRTC Output support" OFF)

if(NOT ENABLE_WEBRTC)
  obs_status(DISABLED, "obs-webrtc")
  return()
endif()

find_package(LibDataChannel REQUIRED)
find_package(CURL REQUIRED)

add_library(obs-webrtc MODULE)
add_library(OBS::webrtc ALIAS obs-webrtc)

target_sources(obs-webrtc PRIVATE obs-webrtc.c whip-output.c whip-output.h whip-service.c)

target_link_libraries(obs-webrtc PRIVATE OBS::libobs LibDataChannel::LibDataChannel CURL::libcurl)

set_target_properties(obs-webrtc PROPERTIES FOLDER "plugins")

setup_plugin_target(obs-webrtc)
