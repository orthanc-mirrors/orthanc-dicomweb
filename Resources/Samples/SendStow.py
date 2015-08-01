#!/usr/bin/python

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


import email
import requests
import sys
import json
from email.mime.multipart import MIMEMultipart
from email.mime.application import MIMEApplication

if len(sys.argv) < 2:
    print('Usage: %s <StowUri> <file>...' % sys.argv[0])
    print('')
    print('Example: %s http://localhost:8042/dicom-web/studies hello.dcm world.dcm' % sys.argv[0])
    sys.exit(-1)

URL = sys.argv[1]

related = MIMEMultipart('related')
related.set_boundary('hello')

for i in range(2, len(sys.argv)):
    try:
        with open(sys.argv[i], 'rb') as f:
            dicom = MIMEApplication(f.read(), 'dicom', email.encoders.encode_noop)
            related.attach(dicom)
    except:
        print('Ignoring directory %s' % sys.argv[i])

headers = dict(related.items())
body = related.as_string()

# Discard the header
body = body.split('\n\n', 1)[1]

headers['Content-Type'] = 'multipart/related; type=application/dicom; boundary=%s' % related.get_boundary()
headers['Accept'] = 'application/json'

r = requests.post(URL, data=body, headers=headers)
j = json.loads(r.text)

# Loop over the successful instances
print('\nWADO-RS URL of the uploaded instances:')
for instance in j['00081199']['Value']:
    if '00081190' in instance:  # This instance has not been discarded
        url = instance['00081190']['Value'][0]
        print(url)

print('\nWADO-RS URL of the study:')
print(j['00081190']['Value'][0])
