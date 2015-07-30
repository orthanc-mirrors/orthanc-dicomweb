#!/usr/bin/python

#
# This maintenance script updates the content of the "Orthanc" folder
# to match the latest version of the Orthanc source code.
#

import os
import shutil

SOURCE = '/home/jodogne/Subversion/Orthanc'
TARGET = os.path.join(os.path.dirname(__file__), '..', 'Orthanc')

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
    'Resources/WindowsResources.py',
    'Resources/WindowsResources.rc',
]

for f in FILES:
    source = os.path.join(SOURCE, f)
    target = os.path.join(TARGET, f)
    try:
        os.makedirs(os.path.dirname(target))
    except:
        pass

    shutil.copy(source, target)
