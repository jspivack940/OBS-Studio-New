project(opencv-out)

set(opencv-out_HEADERS

)

set(opencv-out_SOURCES
	opencv-out.c
)

add_library(opencv-out MODULE
	${opencv-out_HEADERS}
	${opencv-out_SOURCES}
)
	
target_link_libraries(opencv-out
	libobs
)

install_obs_plugin_with_data(opencv-out data)