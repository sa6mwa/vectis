include("${CMAKE_CURRENT_LIST_DIR}/vectis_owned_paths.cmake")
if(DEFINED VECTIS_ROOT)
  vectis_require_project_root("${VECTIS_ROOT}")
endif()
if(DEFINED VECTIS_DIST_DIR AND NOT "${VECTIS_DIST_DIR}" STREQUAL "")
  set(dist_dir "${VECTIS_DIST_DIR}")
else()
  set(dist_dir "${VECTIS_ROOT}/dist")
endif()

vectis_require_owned_path("${dist_dir}" "Package output directory"
  "${CMAKE_CURRENT_LIST_DIR}/../dist")
file(REMOVE_RECURSE "${dist_dir}")
file(MAKE_DIRECTORY "${dist_dir}")
