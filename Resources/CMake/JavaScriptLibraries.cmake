# Orthanc - A Lightweight, RESTful DICOM Store
# Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
# Department, University Hospital of Liege, Belgium
# Copyright (C) 2017-2023 Osimis S.A., Belgium
# Copyright (C) 2024-2024 Orthanc Team SRL, Belgium
# Copyright (C) 2021-2024 Sebastien Jodogne, ICTEAM UCLouvain, Belgium
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


set(BASE_URL "https://orthanc.uclouvain.be/downloads/third-party-downloads")

DownloadFile(
  "2c872dbe60f4ba70fb85356113d8b35e"
  "${BASE_URL}/jquery-3.7.1.min.js")

DownloadPackage(
  "da0189f7c33bf9f652ea65401e0a3dc9"
  "${BASE_URL}/dicom-web/bootstrap-4.3.1.zip"
  "${CMAKE_CURRENT_BINARY_DIR}/bootstrap-4.3.1")

DownloadPackage(
  "8242afdc5bd44105d9dc9e6535315484"
  "${BASE_URL}/dicom-web/vuejs-2.6.10.tar.gz"
  "${CMAKE_CURRENT_BINARY_DIR}/vue-2.6.10")

DownloadPackage(
  "3e2b4e1522661f7fcf8ad49cb933296c"
  "${BASE_URL}/dicom-web/axios-0.19.0.tar.gz"
  "${CMAKE_CURRENT_BINARY_DIR}/axios-0.19.0")

DownloadPackage(
  "a6145901f233f7d54165d8ade779082e"
  "${BASE_URL}/dicom-web/Font-Awesome-4.7.0.tar.gz"
  "${CMAKE_CURRENT_BINARY_DIR}/Font-Awesome-4.7.0")


set(BOOTSTRAP_VUE_SOURCES_DIR ${CMAKE_CURRENT_BINARY_DIR}/bootstrap-vue-2.0.0-rc.24)

if (BUILD_BOOTSTRAP_VUE OR
    BUILD_BABEL_POLYFILL)
  find_program(NPM_EXECUTABLE npm)
  if (${NPM_EXECUTABLE} MATCHES "NPM_EXECUTABLE-NOTFOUND")
    message(FATAL_ERROR "Please install the 'npm' standard command-line tool")
  endif()
endif()

if (BUILD_BOOTSTRAP_VUE)
  DownloadPackage(
    "36ab31495ab94162e159619532e8def5"
    "${BASE_URL}/dicom-web/bootstrap-vue-2.0.0-rc.24.tar.gz"
    "${BOOTSTRAP_VUE_SOURCES_DIR}")

  if (NOT IS_DIRECTORY "${BOOTSTRAP_VUE_SOURCES_DIR}/node_modules")
    execute_process(
      COMMAND ${NPM_EXECUTABLE} install
      WORKING_DIRECTORY ${BOOTSTRAP_VUE_SOURCES_DIR}
      RESULT_VARIABLE Failure
      OUTPUT_QUIET
      )
    
    if (Failure)
      message(FATAL_ERROR "Error while running 'npm install' on Bootstrap-Vue")
    endif()
  endif()

  if (NOT IS_DIRECTORY "${BOOTSTRAP_VUE_SOURCES_DIR}/dist")
    execute_process(
      COMMAND ${NPM_EXECUTABLE} run build
      WORKING_DIRECTORY ${BOOTSTRAP_VUE_SOURCES_DIR}
      RESULT_VARIABLE Failure
      OUTPUT_QUIET
      )
    
    if (Failure)
      message(FATAL_ERROR "Error while running 'npm build' on Bootstrap-Vue")
    endif()
  endif()

else()

  ##
  ## Generation of the precompiled Bootstrap-Vue package:
  ##
  ## Possibility 1 (build from sources):
  ##  $ cmake -DBUILD_BOOTSTRAP_VUE=ON .
  ##  $ tar cvfz bootstrap-vue-2.0.0-rc.24-dist.tar.gz bootstrap-vue-2.0.0-rc.24/dist/
  ##
  ## Possibility 2 (download from CDN):
  ##  $ mkdir /tmp/i && cd /tmp/i
  ##  $ wget -r --no-parent https://unpkg.com/bootstrap-vue@2.0.0-rc.24/dist/
  ##  $ mv unpkg.com/bootstrap-vue@2.0.0-rc.24/ bootstrap-vue-2.0.0-rc.24
  ##  $ rm bootstrap-vue-2.0.0-rc.24/dist/index.html
  ##  $ tar cvfz bootstrap-vue-2.0.0-rc.24-dist.tar.gz bootstrap-vue-2.0.0-rc.24/dist/

  DownloadPackage(
    "ba0e67b1f0b4ce64e072b42b17f6c578"
    "${BASE_URL}/dicom-web/bootstrap-vue-2.0.0-rc.24-dist.tar.gz"
    "${BOOTSTRAP_VUE_SOURCES_DIR}")

endif()


if (BUILD_BABEL_POLYFILL)
  set(BABEL_POLYFILL_SOURCES_DIR ${CMAKE_CURRENT_BINARY_DIR}/node_modules/babel-polyfill/dist)

  if (NOT IS_DIRECTORY "${BABEL_POLYFILL_SOURCES_DIR}")
    execute_process(
      COMMAND ${NPM_EXECUTABLE} install babel-polyfill
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      RESULT_VARIABLE Failure
      OUTPUT_QUIET
      )
    
    if (Failure)
      message(FATAL_ERROR "Error while running 'npm install' on Bootstrap-Vue")
    endif()
  endif()
else()

  ## curl -L https://unpkg.com/babel-polyfill@6.26.0/dist/polyfill.min.js | gzip > babel-polyfill-6.26.0.min.js.gz

  set(BABEL_POLYFILL_SOURCES_DIR ${CMAKE_CURRENT_BINARY_DIR})
  DownloadCompressedFile(
    "49f7bad4176d715ce145e75c903988ef"
    "${BASE_URL}/dicom-web/babel-polyfill-6.26.0.min.js.gz"
    "${CMAKE_CURRENT_BINARY_DIR}/polyfill.min.js")

endif()


set(JAVASCRIPT_LIBS_DIR  ${CMAKE_CURRENT_BINARY_DIR}/javascript-libs)
file(MAKE_DIRECTORY ${JAVASCRIPT_LIBS_DIR})

file(COPY
  ${BABEL_POLYFILL_SOURCES_DIR}/polyfill.min.js
  ${BOOTSTRAP_VUE_SOURCES_DIR}/dist/bootstrap-vue.min.js
  ${BOOTSTRAP_VUE_SOURCES_DIR}/dist/bootstrap-vue.min.js.map
  ${CMAKE_CURRENT_BINARY_DIR}/axios-0.19.0/dist/axios.min.js
  ${CMAKE_CURRENT_BINARY_DIR}/axios-0.19.0/dist/axios.min.map
  ${CMAKE_CURRENT_BINARY_DIR}/bootstrap-4.3.1/dist/js/bootstrap.min.js
  ${CMAKE_CURRENT_BINARY_DIR}/vue-2.6.10/dist/vue.min.js
  ${CMAKE_SOURCE_DIR}/ThirdPartyDownloads/jquery-3.7.1.min.js
  DESTINATION
  ${JAVASCRIPT_LIBS_DIR}/js
  )

file(COPY
  ${BOOTSTRAP_VUE_SOURCES_DIR}/dist/bootstrap-vue.min.css
  ${BOOTSTRAP_VUE_SOURCES_DIR}/dist/bootstrap-vue.min.css.map
  ${CMAKE_CURRENT_BINARY_DIR}/Font-Awesome-4.7.0/css/font-awesome.min.css
  ${CMAKE_CURRENT_BINARY_DIR}/bootstrap-4.3.1/dist/css/bootstrap.min.css
  ${CMAKE_CURRENT_BINARY_DIR}/bootstrap-4.3.1/dist/css/bootstrap.min.css.map
  DESTINATION
  ${JAVASCRIPT_LIBS_DIR}/css
  )

file(COPY
  ${CMAKE_CURRENT_BINARY_DIR}/Font-Awesome-4.7.0/fonts/FontAwesome.otf
  ${CMAKE_CURRENT_BINARY_DIR}/Font-Awesome-4.7.0/fonts/fontawesome-webfont.eot
  ${CMAKE_CURRENT_BINARY_DIR}/Font-Awesome-4.7.0/fonts/fontawesome-webfont.svg
  ${CMAKE_CURRENT_BINARY_DIR}/Font-Awesome-4.7.0/fonts/fontawesome-webfont.ttf
  ${CMAKE_CURRENT_BINARY_DIR}/Font-Awesome-4.7.0/fonts/fontawesome-webfont.woff
  ${CMAKE_CURRENT_BINARY_DIR}/Font-Awesome-4.7.0/fonts/fontawesome-webfont.woff2
  DESTINATION
  ${JAVASCRIPT_LIBS_DIR}/fonts
  )

file(COPY
  ${CMAKE_CURRENT_LIST_DIR}/../Orthanc/OrthancLogo.png
  DESTINATION
  ${JAVASCRIPT_LIBS_DIR}/img
  )
