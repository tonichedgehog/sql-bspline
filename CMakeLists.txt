# Copyright 2026 Tonic Hedgehog
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its contributors
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

cmake_minimum_required(VERSION 3.25)
project(SQLSPLINE VERSION 1.1 LANGUAGES C Fortran)

set(CMAKE_Fortran_COMPILER fort77)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(SQLite3 REQUIRED)
add_subdirectory(dep/dierckx)
add_library(bspline OBJECT bspline.c)
set_target_properties(bspline PROPERTIES
  C_VISIBILITY_PRESET hidden
  )
target_link_libraries(bspline
  PUBLIC dierckx
  )
add_library(fpk_spline SHARED extension.c)
set_source_files_properties(extension.c bspline.c PROPERTIES
  COMPILE_OPTIONS "$<$<C_COMPILER_ID:GNU>:-Wall;-pedantic>"
  )
target_link_libraries(fpk_spline
  PRIVATE SQLite::SQLite3
  bspline
  )
set_target_properties(fpk_spline PROPERTIES
  C_VISIBILITY_PRESET hidden
  PREFIX ""
  )
add_executable(testprog tests/testprog.c)
target_compile_definitions(testprog
  PRIVATE -DFPK_EXTENSION="$<TARGET_FILE:fpk_spline>"
  )
target_link_libraries(testprog
  PRIVATE SQLite::SQLite3
  bspline
  )
file(WRITE ${CMAKE_BINARY_DIR}/.gitignore "*")
