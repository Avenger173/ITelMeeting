# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Debug")
  file(REMOVE_RECURSE
  "CMakeFiles\\SmartMeet_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\SmartMeet_autogen.dir\\ParseCache.txt"
  "SmartMeet_autogen"
  )
endif()
