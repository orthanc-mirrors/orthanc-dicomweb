#!/usr/bin/python

#
# This maintenance script updates the content of the "Orthanc" folder
# to match the latest version of the Orthanc source code.
#

import os
import shutil
import urllib2

PLUGIN_SDK_VERSION = '0.9.1'

SOURCE = '/home/jodogne/Subversion/Orthanc'
TARGET = os.path.join(os.path.dirname(__file__), '..', 'Orthanc')
REPOSITORY = 'https://bitbucket.org/sjodogne/orthanc/raw/Orthanc-%s/Plugins/Include' % PLUGIN_SDK_VERSION

FILES = [
    'Core/ChunkedBuffer.cpp',
    'Core/ChunkedBuffer.h',
    'Core/Enumerations.cpp',
    'Core/Enumerations.h',
    'Core/ImageFormats/ImageAccessor.cpp',
    'Core/ImageFormats/ImageAccessor.h',
    'Core/ImageFormats/ImageBuffer.cpp',
    'Core/ImageFormats/ImageBuffer.h',
    'Core/ImageFormats/PngReader.cpp',
    'Core/ImageFormats/PngReader.h',
    'Core/OrthancException.cpp',
    'Core/OrthancException.h',
    'Core/PrecompiledHeaders.h',
    'Core/Toolbox.cpp',
    'Core/Toolbox.h',
    'Plugins/Samples/Common/VersionScript.map',
    'Resources/CMake/BoostConfiguration.cmake',
    'Resources/CMake/Compiler.cmake',
    'Resources/CMake/DownloadPackage.cmake',
    'Resources/CMake/GoogleTestConfiguration.cmake',
    'Resources/CMake/JsonCppConfiguration.cmake',
    'Resources/CMake/LibPngConfiguration.cmake',
    'Resources/CMake/ZlibConfiguration.cmake',
    'Resources/CMake/PugixmlConfiguration.cmake',
    'Resources/MinGW-W64-Toolchain32.cmake',
    'Resources/MinGW-W64-Toolchain64.cmake',
    'Resources/MinGWToolchain.cmake',
    'Resources/ThirdParty/VisualStudio/stdint.h',
    'Resources/ThirdParty/base64/base64.cpp',
    'Resources/ThirdParty/base64/base64.h',
    'Resources/ThirdParty/md5/md5.c',
    'Resources/ThirdParty/md5/md5.h',
    'Resources/WindowsResources.py',
    'Resources/WindowsResources.rc',
]

SDK = [
    'orthanc/OrthancCPlugin.h',
]   


for f in FILES:
    source = os.path.join(SOURCE, f)
    target = os.path.join(TARGET, f)
    try:
        os.makedirs(os.path.dirname(target))
    except:
        pass

    shutil.copy(source, target)

for f in SDK:
    source = '%s/%s' % (REPOSITORY, f)
    target = os.path.join(TARGET, 'Sdk-%s' % PLUGIN_SDK_VERSION, f)
    try:
        os.makedirs(os.path.dirname(target))
    except:
        pass

    with open(target, 'w') as g:
        g.write(urllib2.urlopen(source).read())
