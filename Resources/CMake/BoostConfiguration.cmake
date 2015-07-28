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


if (STATIC_BUILD OR NOT USE_SYSTEM_BOOST)
  set(BOOST_STATIC 1)
else()
  include(FindBoost)
  set(BOOST_STATIC 0)
  find_package(Boost COMPONENTS system thread filesystem regex locale)

  if (NOT Boost_FOUND)
    message(FATAL_ERROR "Unable to locate Boost on this system")
  endif()

  include_directories(${Boost_INCLUDE_DIRS})
  link_libraries(${Boost_LIBRARIES})
endif()


if (BOOST_STATIC)
  # Parameters for Boost 1.55.0
  set(BOOST_NAME boost_1_55_0)
  set(BOOST_BCP_SUFFIX bcpdigest-0.7.4)
  set(BOOST_MD5 "409f7a0e4fb1f5659d07114f3133b67b")
  set(BOOST_FILESYSTEM_SOURCES_DIR "${BOOST_NAME}/libs/filesystem/src")
  
  set(BOOST_SOURCES_DIR ${CMAKE_BINARY_DIR}/${BOOST_NAME})
  DownloadPackage(
    "${BOOST_MD5}"
    "http://www.montefiore.ulg.ac.be/~jodogne/Orthanc/ThirdPartyDownloads/${BOOST_NAME}_${BOOST_BCP_SUFFIX}.tar.gz"
    "${BOOST_SOURCES_DIR}"
    )

  add_definitions(
    # Static build of Boost
    -DBOOST_ALL_NO_LIB 
    -DBOOST_ALL_NOLIB 
    -DBOOST_DATE_TIME_NO_LIB 
    -DBOOST_THREAD_BUILD_LIB
    -DBOOST_PROGRAM_OPTIONS_NO_LIB
    -DBOOST_REGEX_NO_LIB
    -DBOOST_SYSTEM_NO_LIB
    -DBOOST_LOCALE_NO_LIB
    )

  if (${CMAKE_COMPILER_IS_GNUCXX})
    add_definitions(-isystem ${BOOST_SOURCES_DIR})
  endif()

  include_directories(
    ${BOOST_SOURCES_DIR}
    )

  list(APPEND BOOST_SOURCES
    ${BOOST_SOURCES_DIR}/libs/system/src/error_code.cpp
    )


  ## Boost::thread

  if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR
      ${CMAKE_SYSTEM_NAME} STREQUAL "Darwin" OR
      ${CMAKE_SYSTEM_NAME} STREQUAL "kFreeBSD")
    list(APPEND BOOST_SOURCES
      ${BOOST_SOURCES_DIR}/libs/thread/src/pthread/once.cpp
      ${BOOST_SOURCES_DIR}/libs/thread/src/pthread/thread.cpp
      )

    if ("${CMAKE_SYSTEM_VERSION}" STREQUAL "LinuxStandardBase")
      add_definitions(-DBOOST_HAS_SCHED_YIELD=1)
    endif()

  elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    list(APPEND BOOST_SOURCES
      ${BOOST_SOURCES_DIR}/libs/thread/src/win32/tss_dll.cpp
      ${BOOST_SOURCES_DIR}/libs/thread/src/win32/thread.cpp
      ${BOOST_SOURCES_DIR}/libs/thread/src/win32/tss_pe.cpp
      )
  endif()

  if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    # This is a patch to compile Boost 1.55.0 with Clang 3.4 and later
    # (including XCode 5.1). Fixes issue 14 of Orthanc.
    # https://trac.macports.org/ticket/42282#comment:10
    execute_process(
      COMMAND patch -p0 -i ${CMAKE_SOURCE_DIR}/Resources/Patches/boost-1.55.0-clang-atomic.patch
      WORKING_DIRECTORY ${BOOST_SOURCES_DIR}
      )
  endif()


  ## Boost::filesystem

  list(APPEND BOOST_SOURCES
    ${BOOST_FILESYSTEM_SOURCES_DIR}/codecvt_error_category.cpp
    ${BOOST_FILESYSTEM_SOURCES_DIR}/operations.cpp
    ${BOOST_FILESYSTEM_SOURCES_DIR}/path.cpp
    ${BOOST_FILESYSTEM_SOURCES_DIR}/path_traits.cpp
    )

  if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    list(APPEND BOOST_SOURCES
      ${BOOST_SOURCES_DIR}/libs/filesystem/src/utf8_codecvt_facet.cpp
      )
  elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    list(APPEND BOOST_SOURCES
      ${BOOST_FILESYSTEM_SOURCES_DIR}/windows_file_codecvt.cpp
      )
  endif()


  ## Boost::regex

  aux_source_directory(${BOOST_SOURCES_DIR}/libs/regex/src BOOST_REGEX_SOURCES)
  list(APPEND BOOST_SOURCES ${BOOST_REGEX_SOURCES})


  ## Boost::locale

  list(APPEND BOOST_SOURCES 
    ${BOOST_SOURCES_DIR}/libs/locale/src/encoding/codepage.cpp
    ${BOOST_SOURCES_DIR}/libs/system/src/error_code.cpp
    )

  if (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    SET(GCC_ICONV_LINK_FLAGS  "-liconv -framework CoreFoundation")
    SET(CMAKE_SHARED_LINKER_FLAGS  "${CMAKE_SHARED_LINKER_FLAGS} ${GCC_ICONV_LINK_FLAGS}" )
    SET(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} ${GCC_ICONV_LINK_FLAGS}" )
  endif()

  if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR
      ${CMAKE_SYSTEM_NAME} STREQUAL "Darwin" OR
      ${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD" OR
      ${CMAKE_SYSTEM_NAME} STREQUAL "kFreeBSD")
    add_definitions(-DBOOST_LOCALE_WITH_ICONV=1)
  endif()

  source_group(ThirdParty\\Boost REGULAR_EXPRESSION ${BOOST_SOURCES_DIR}/.*)
endif()


add_definitions(
  -DBOOST_HAS_FILESYSTEM_V3=1
  -DBOOST_HAS_LOCALE=1
  )
