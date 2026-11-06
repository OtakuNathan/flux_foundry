
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was flux_foundryConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

include(CMakeFindDependencyMacro)

set(flux_foundry_uv_executor_FOUND OFF)
if(flux_foundry_uv_executor_FOUND)
    find_dependency(PkgConfig)
    find_dependency(Threads)
    pkg_check_modules(FLUX_FOUNDRY_LIBUV REQUIRED IMPORTED_TARGET libuv)
endif()

set(flux_foundry_gsource_executor_FOUND OFF)
if(flux_foundry_gsource_executor_FOUND)
    find_dependency(PkgConfig)
    pkg_check_modules(FLUX_FOUNDRY_GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/flux_foundryTargets.cmake")
check_required_components(flux_foundry)
