# Once done these will be defined:
#
#  LIBNVAFX_FOUND
#  LIBNVAFX_INCLUDE_DIRS
#  LIBNVAFX_LIBRARIES
#
# For use in OBS:
#
#  NVAFX_INCLUDE_DIR

find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
	pkg_check_modules(_NVAFX QUIET nvafx libnvafx)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(_lib_suffix 64)
else()
	set(_lib_suffix 32)
endif()

find_path(NVAFX_INCLUDE_DIR
	NAMES nvAudioEffects.h
	HINTS
		ENV DepsPath
		${nvafxPath}
		${DepsPath${_lib_suffix}}
		${DepsPath}
		${_NVAFX_INCLUDE_DIRS}
	PATHS
		/nvafx/include
	PATH_SUFFIXES
		include)

find_library(NVAFX_LIB
	NAMES ${_NVAFX_LIBRARIES} NVAudioEffects.lib
	HINTS
		ENV DepsPath${_lib_suffix}
		ENV DepsPath
		${nvafxPath${_lib_suffix}}
		${DepsPath${_lib_suffix}}
		${DepsPath}
		${_NVAFX_LIBRARY_DIRS}
	PATHS
		/bin
	PATH_SUFFIXES
		lib${_lib_suffix} lib
		libs${_lib_suffix} libs
		bin${_lib_suffix} bin
		../lib${_lib_suffix} ../lib
		../libs${_lib_suffix} ../libs
		../bin${_lib_suffix} ../bin)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libspeexdsp DEFAULT_MSG NVAFX_LIB NVAFX_INCLUDE_DIR)
mark_as_advanced(NVAFX_INCLUDE_DIR NVAFX_LIB)

if (NVAFX_INCLUDE_DIR AND NVAFX_LIB)
	message(STATUS "Found nvafx library")
	set(LIBNVAFX_FOUND TRUE)
endif()

if(LIBNVAFX_FOUND)
	set(LIBNVAFX_INCLUDE_DIRS ${NVAFX_INCLUDE_DIR})
	set(LIBNVAFX_LIBRARIES ${NVAFX_LIB})
else()
	message(STATUS "Nvafx not found")
endif()
