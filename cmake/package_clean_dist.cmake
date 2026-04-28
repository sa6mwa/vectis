if(DEFINED VECTIS_DIST_DIR AND NOT "${VECTIS_DIST_DIR}" STREQUAL "")
  set(dist_dir "${VECTIS_DIST_DIR}")
else()
  set(dist_dir "${VECTIS_ROOT}/dist")
endif()

file(REMOVE_RECURSE "${dist_dir}")
file(MAKE_DIRECTORY "${dist_dir}")
