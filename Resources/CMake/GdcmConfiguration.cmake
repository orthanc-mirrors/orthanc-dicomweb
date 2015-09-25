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


if (STATIC_BUILD OR NOT USE_SYSTEM_GDCM)
  # If using gcc, build GDCM with the "-fPIC" argument to allow its
  # embedding into the shared library containing the Orthanc plugin
  if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR
      ${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD" OR
      ${CMAKE_SYSTEM_NAME} STREQUAL "kFreeBSD")
    set(AdditionalFlags "-fPIC")
  endif()
  
  set(Flags
    "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS} ${AdditionalFlags}"
    "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS} ${AdditionalFlags}"
    -DCMAKE_C_FLAGS_DEBUG=${CMAKE_C_FLAGS_DEBUG}
    -DCMAKE_CXX_FLAGS_DEBUG=${CMAKE_CXX_FLAGS_DEBUG}
    -DCMAKE_C_FLAGS_RELEASE=${CMAKE_C_FLAGS_RELEASE}
    -DCMAKE_CXX_FLAGS_RELEASE=${CMAKE_CXX_FLAGS_RELEASE}
    -DCMAKE_C_FLAGS_MINSIZEREL=${CMAKE_C_FLAGS_MINSIZEREL}
    -DCMAKE_CXX_FLAGS_MINSIZEREL=${CMAKE_CXX_FLAGS_MINSIZEREL} 
    -DCMAKE_C_FLAGS_RELWITHDEBINFO=${CMAKE_C_FLAGS_RELWITHDEBINFO} 
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO=${CMAKE_CXX_FLAGS_RELWITHDEBINFO}
    )

  if (CMAKE_TOOLCHAIN_FILE)
    list(APPEND Flags -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
  endif()

  include(ExternalProject)
  externalproject_add(GDCM
    URL "http://www.montefiore.ulg.ac.be/~jodogne/Orthanc/ThirdPartyDownloads/gdcm-2.4.4.tar.gz"
    URL_MD5 "5dca87a061c536b6fa377263b7839dcb"
    CMAKE_ARGS -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE} ${Flags}
    #-DLIBRARY_OUTPUT_PATH=${CMAKE_CURRENT_BINARY_DIR}
    INSTALL_COMMAND ""  # Skip the install step
    )

  if(MSVC)
    set(Suffix ".lib")
    set(Prefix "")
  else()
    set(Suffix ".a")
    list(GET CMAKE_FIND_LIBRARY_PREFIXES 0 Prefix)
  endif()

  set(GDCM_LIBRARIES 
    ${Prefix}gdcmMSFF${Suffix}
    ${Prefix}gdcmcharls${Suffix}
    ${Prefix}gdcmDICT${Suffix}
    ${Prefix}gdcmDSED${Suffix}
    ${Prefix}gdcmIOD${Suffix}
    ${Prefix}gdcmjpeg8${Suffix}
    ${Prefix}gdcmjpeg12${Suffix}
    ${Prefix}gdcmjpeg16${Suffix}
    ${Prefix}gdcmMEXD${Suffix}
    ${Prefix}gdcmopenjpeg${Suffix}
    ${Prefix}gdcmzlib${Suffix}
    ${Prefix}socketxx${Suffix}
    ${Prefix}gdcmCommon${Suffix}
    ${Prefix}gdcmexpat${Suffix}

    #${Prefix}gdcmgetopt${Suffix}
    #${Prefix}gdcmuuid${Suffix}
    )

  ExternalProject_Get_Property(GDCM binary_dir)
  include_directories(${binary_dir}/Source/Common)
  link_directories(${binary_dir}/bin)

  ExternalProject_Get_Property(GDCM source_dir)
  include_directories(
    ${source_dir}/Source/Common
    ${source_dir}/Source/MediaStorageAndFileFormat
    ${source_dir}/Source/DataStructureAndEncodingDefinition
    )

else()
  find_package(GDCM REQUIRED)
  if (GDCM_FOUND)
    include(${GDCM_USE_FILE})
    set(GDCM_LIBRARIES gdcmCommon gdcmMSFF)
  else(GDCM_FOUND)
    message(FATAL_ERROR "Cannot find GDCM, did you set GDCM_DIR?")
  endif(GDCM_FOUND)
endif()
