# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.16)
project(vsql_stat_otel C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(VillageSQL_USE_DEV_HEADERS ON)
find_package(VillageSQL REQUIRED)

# libcurl for the OTLP/HTTP export (JSON POST to /v1/logs).
find_package(CURL REQUIRED)

message(STATUS "VillageSQL SDK found at: ${VillageSQL_PREFIX}")
message(STATUS "curl: ${CURL_VERSION_STRING}")

add_library(vsql_stat_otel SHARED
    core/event_queue.cc
    core/capture_pipeline.cc
    src/extension.cc
    src/otel_sink.cc
)
target_compile_options(vsql_stat_otel PRIVATE -fPIC)

target_include_directories(vsql_stat_otel PRIVATE
    ${VillageSQL_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}       # so "core/..." resolves
    ${CMAKE_CURRENT_SOURCE_DIR}/src   # so "otel_sink.h" resolves
)
target_link_libraries(vsql_stat_otel PRIVATE CURL::libcurl)

VEF_CREATE_VEB(
    NAME vsql_stat_otel
    LIBRARY_TARGET vsql_stat_otel
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
