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


if (STATIC_BUILD OR NOT USE_SYSTEM_JSONCPP)
  set(JSONCPP_SOURCES_DIR ${CMAKE_BINARY_DIR}/jsoncpp-src-0.6.0-rc2)
  DownloadPackage(
    "363e2f4cbd3aeb63bf4e571f377400fb"
    "http://www.montefiore.ulg.ac.be/~jodogne/Orthanc/ThirdPartyDownloads/jsoncpp-src-0.6.0-rc2.tar.gz"
    "${JSONCPP_SOURCES_DIR}")

  list(APPEND JSONCPP_SOURCES
    ${JSONCPP_SOURCES_DIR}/src/lib_json/json_reader.cpp
    ${JSONCPP_SOURCES_DIR}/src/lib_json/json_value.cpp
    ${JSONCPP_SOURCES_DIR}/src/lib_json/json_writer.cpp
    )

  include_directories(
    ${JSONCPP_SOURCES_DIR}/include
    )

  source_group(ThirdParty\\JsonCpp REGULAR_EXPRESSION ${JSONCPP_SOURCES_DIR}/.*)

else()
  CHECK_INCLUDE_FILE_CXX(jsoncpp/json/reader.h HAVE_JSONCPP_H)
  if (NOT HAVE_JSONCPP_H)
    message(FATAL_ERROR "Please install the libjsoncpp-dev package")
  endif()

  include_directories(/usr/include/jsoncpp)
  link_libraries(jsoncpp)

endif()
