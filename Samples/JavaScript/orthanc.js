/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2015 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Affero General Public License for more details.
 * 
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/






// http://www.artandlogic.com/blog/2013/11/jquery-ajax-blobs-and-array-buffers/




var BOUNDARY = 'BOUNDARY_123456789';


function GetTag(answer, tag)
{
  if (!(tag in answer) ||
      !("Value" in answer[tag]) ||
      answer[tag].length == 0) {
    return '';
  } else {
    return answer[tag]["Value"][0];
  }
}


function StringToArrayBuffer(str) 
{
  // http://updates.html5rocks.com/2012/06/How-to-convert-ArrayBuffer-to-and-from-String
  var bufView = new Uint8Array(str.length);

  for (var i=0, strLen=str.length; i < strLen; i++) {
    bufView[i] = str.charCodeAt(i);
  }

  return bufView;
}

function ConstructMultipart(body, contentType) 
{
  var header = '--' + BOUNDARY + '\nContent-Type: ' + contentType + '\n\n';
  var trailer = '\n--' + BOUNDARY + '--\n';
  
  header = StringToArrayBuffer(header);
  trailer = StringToArrayBuffer(trailer);

  // Concatenate the header, the body and the trailer
  // http://stackoverflow.com/a/14071518/881731
  var b = new Uint8Array(header.byteLength + body.byteLength + trailer.byteLength);
  b.set(header);
  b.set(new Uint8Array(body), header.byteLength);
  b.set(trailer, header.byteLength + body.byteLength);

  return b;
}



$(document).ready(function() {
  // STOW-RS to upload one DICOM file
  $('#stow').submit(function() {

    var fileInput = document.getElementById('stow-file');
    var file = fileInput.files[0];
    reader = new FileReader();
    reader.onload = function() {
      $.ajax({
        type: 'POST',
        headers: {
          'Accept' : 'application/json',
          'Content-type' : 'multipart/related; type=application/dicom; boundary=' + BOUNDARY,
        },
        url: '../stow-rs/studies',
        data: ConstructMultipart(reader.result, 'application/dicom'),
        processData: false, // Very important!
        dataType: 'json',
        success: function(resp) {
          alert('Upload was a success!');
        },
        error: function() {
          alert('Cannot process this query');
        }
      });
    };

    reader.readAsArrayBuffer(file);

    // Prevent default action
    return false;
  });


  // QIDO-RS to search for series
  $('#qido-series').submit(function() {
    var data = {};
    $('input[name]', this).each(function() {
      data[$(this).attr('name')] = $(this).val();
    });

    $.ajax({
      headers: {
        'Accept' : 'application/json'
      },
      data: data,
      cache: true,  // If set to false, the "_" GET argument is added, resulting in a bad QIDO-RS request
      dataType: 'json',
      url: '../qido-rs/series',
      success: function(answer) {
        $('#qido-series-results').empty();
        for (var i = 0; i < answer.length; i++) {
          var patientId = GetTag(answer[i], '00100020');
          var patientName = GetTag(answer[i], '00100010');
          var studyDescription = GetTag(answer[i], '00081030');
          var seriesDescription = GetTag(answer[i], '0008103E');
          $('#qido-series-results').append(
            '<li>' + patientId + ' - ' + patientName + ' - ' +
              studyDescription + ' - ' + seriesDescription +
              '</li>');
        }
      },
      error: function() {
        alert('Cannot process this query');
      }
    });

    // Prevent default action
    return false;
  });
});
