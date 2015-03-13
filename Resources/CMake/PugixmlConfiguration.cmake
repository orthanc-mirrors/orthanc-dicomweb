if (STATIC_BUILD OR NOT USE_SYSTEM_PUGIXML)
  set(PUGIXML_SOURCES_DIR ${CMAKE_BINARY_DIR}/pugixml-1.4)

  DownloadPackage(
    "7c56c91cfe3ecdee248a8e4892ef5781"
    "http://www.montefiore.ulg.ac.be/~jodogne/Orthanc/ThirdPartyDownloads/pugixml-1.4.tar.gz"
    "${PUGIXML_SOURCES_DIR}")

  include_directories(
    ${PUGIXML_SOURCES_DIR}/src
    )

  set(PUGIXML_SOURCES
    ${PUGIXML_SOURCES_DIR}/src/pugixml.cpp
    )

else()
  CHECK_INCLUDE_FILE_CXX(pugixml.hpp HAVE_PUGIXML_H)
  if (NOT HAVE_PUGIXML_H)
    message(FATAL_ERROR "Please install the libpugixml-dev package")
  endif()

  link_libraries(pugixml)
endif()
