# Orthanc - A Lightweight, RESTful DICOM Store
# Copyright (C) 2012-2015 Sebastien Jodogne, Medical Physics
# Department, University Hospital of Liege, Belgium
#
# This program is free software: you can redistribute it and/or
# modify it under the terms of the GNU Affero General Public License
# as published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Affero General Public License for more details.
# 
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.


# Force static build when cross-compiling
if (CMAKE_CROSSCOMPILING)
  SET(STATIC_BUILD ON)
  SET(STANDALONE_BUILD ON)
endif()

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  SET(OS_LIBRARIES uuid rt dl)
  SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pthread")
  SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -pthread")
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
  SET(OS_LIBRARIES rpcrt4 ws2_32 secur32)
  if (CMAKE_COMPILER_IS_GNUCXX)
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
    SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static-libgcc -static-libstdc++")
  endif()
endif ()

if (CMAKE_COMPILER_IS_GNUCXX)
  SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--version-script=${CMAKE_SOURCE_DIR}/Resources/VersionScript.map -Wl,--no-undefined")
endif()

if (MSVC)
  # Use static runtime under Visual Studio
  # http://www.cmake.org/Wiki/CMake_FAQ#Dynamic_Replace
  # http://stackoverflow.com/a/6510446
  foreach(flag_var
    CMAKE_C_FLAGS_DEBUG
    CMAKE_CXX_FLAGS_DEBUG
    CMAKE_C_FLAGS_RELEASE 
    CMAKE_CXX_FLAGS_RELEASE
    CMAKE_C_FLAGS_MINSIZEREL 
    CMAKE_CXX_FLAGS_MINSIZEREL 
    CMAKE_C_FLAGS_RELWITHDEBINFO 
    CMAKE_CXX_FLAGS_RELWITHDEBINFO) 
    string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
    string(REGEX REPLACE "/MDd" "/MTd" ${flag_var} "${${flag_var}}")
  endforeach(flag_var)

  add_definitions(
    -D_CRT_SECURE_NO_WARNINGS=1
    -D_CRT_NONSTDC_NO_DEPRECATE=1
    )
endif()
