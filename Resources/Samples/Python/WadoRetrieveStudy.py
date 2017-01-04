#!/usr/bin/python

# Orthanc - A Lightweight, RESTful DICOM Store
# Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
# Department, University Hospital of Liege, Belgium
# Copyright (C) 2017 Osimis, Belgium
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


import email
import urllib2
import sys

if len(sys.argv) != 2:
    print('Usage: %s <Uri>' % sys.argv[0])
    print('')
    print('Example: %s http://localhost:8042/dicom-web/studies/1.3.51.0.1.1.192.168.29.133.1681753.1681732' % sys.argv[0])
    sys.exit(-1)

answer = urllib2.urlopen(sys.argv[1])
s = str(answer.info()) + "\n" + answer.read()

msg = email.message_from_string(s)

for i, part in enumerate(msg.walk(), 1):
    filename = 'wado-%06d.dcm' % i
    dicom = part.get_payload(decode = True)
    if dicom != None:
        print('Storing DICOM file: %s' % filename)
        with open(filename, 'wb') as f:
            f.write(str(dicom))
