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
    'Plugins/Samples/Common/VersionScript.map',
    'Resources/CMake/BoostConfiguration.cmake',
    'Resources/CMake/Compiler.cmake',
    'Resources/CMake/DownloadPackage.cmake',
    'Resources/CMake/GoogleTestConfiguration.cmake',
    'Resources/CMake/JsonCppConfiguration.cmake',
    'Resources/CMake/PugixmlConfiguration.cmake',
    'Resources/MinGW-W64-Toolchain32.cmake',
    'Resources/MinGW-W64-Toolchain64.cmake',
    'Resources/MinGWToolchain.cmake',
    'Resources/Patches/boost-1.55.0-clang-atomic.patch',
    'Resources/ThirdParty/VisualStudio/stdint.h',
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
