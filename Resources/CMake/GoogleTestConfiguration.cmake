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


if (USE_GTEST_DEBIAN_SOURCE_PACKAGE)
  set(GTEST_SOURCES /usr/src/gtest/src/gtest-all.cc)
  include_directories(/usr/src/gtest)

  if (NOT EXISTS /usr/include/gtest/gtest.h OR
      NOT EXISTS ${GTEST_SOURCES})
    message(FATAL_ERROR "Please install the libgtest-dev package")
  endif()

elseif (STATIC_BUILD OR NOT USE_SYSTEM_GOOGLE_TEST)
  set(GTEST_SOURCES_DIR ${CMAKE_BINARY_DIR}/gtest-1.7.0)
  DownloadPackage(
    "2d6ec8ccdf5c46b05ba54a9fd1d130d7"
    "http://www.montefiore.ulg.ac.be/~jodogne/Orthanc/ThirdPartyDownloads/gtest-1.7.0.zip"
    "${GTEST_SOURCES_DIR}")

  include_directories(
    ${GTEST_SOURCES_DIR}/include
    ${GTEST_SOURCES_DIR}
    )

  set(GTEST_SOURCES
    ${GTEST_SOURCES_DIR}/src/gtest-all.cc
    )

  # https://code.google.com/p/googletest/issues/detail?id=412
  if (MSVC) # VS2012 does not support tuples correctly yet
    add_definitions(/D _VARIADIC_MAX=10)
  endif()

else()
  include(FindGTest)
  if (NOT GTEST_FOUND)
    message(FATAL_ERROR "Unable to find GoogleTest")
  endif()

  include_directories(${GTEST_INCLUDE_DIRS})
  link_libraries(${GTEST_LIBRARIES})
endif()
