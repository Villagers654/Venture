# Set before dependency targets so every Ninja link shares the same budget.
# AUTO follows ThinLTO even when an existing build switches configurations.
set(VENTURE_HOST_LINK_JOBS "AUTO" CACHE STRING
  "Concurrent Ninja links: AUTO uses two normally and one with ThinLTO")
if(VENTURE_HOST_LINK_JOBS STREQUAL "AUTO")
  if(VENTURE_HOST_THINLTO)
    set(_venture_link_jobs 1)
  else()
    set(_venture_link_jobs 2)
  endif()
elseif(VENTURE_HOST_LINK_JOBS MATCHES "^[1-9][0-9]*$")
  set(_venture_link_jobs "${VENTURE_HOST_LINK_JOBS}")
else()
  message(FATAL_ERROR "VENTURE_HOST_LINK_JOBS must be AUTO or a positive integer")
endif()
if(CMAKE_GENERATOR MATCHES "Ninja")
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS "link_pool=${_venture_link_jobs}")
  set(CMAKE_JOB_POOL_LINK link_pool)
endif()
