cmake_minimum_required(VERSION 3.25)
project(BZ)

find_package(SQLite3 REQUIRED)
find_package(tinyspline CONFIG REQUIRED)

add_library(bzpath SHARED bspline.c)

target_link_libraries(bzpath
  PRIVATE SQLite::SQLite3
  tinyspline::tinyspline
  )
set_target_properties(bzpath PROPERTIES
  OUTPUT_NAME bspline
  PREFIX ""
  )

file(WRITE ${CMAKE_BINARY_DIR}/.gitignore "*")
