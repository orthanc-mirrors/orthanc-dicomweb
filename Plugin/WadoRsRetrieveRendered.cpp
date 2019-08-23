/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2019 Osimis S.A., Belgium
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

#include "WadoRs.h"

#include <Plugins/Samples/Common/OrthancPluginCppWrapper.h>

#include <boost/algorithm/string/predicate.hpp>

static void AnswerFrameRendered(OrthancPluginRestOutput* output,
                                int frame,
                                const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
  }
  else
  {
    std::string instanceId;
    if (LocateInstance(output, instanceId, request))
    {
      Orthanc::MimeType mime = Orthanc::MimeType_Jpeg;  // This is the default in DICOMweb
      
      for (uint32_t i = 0; i < request->headersCount; i++)
      {
        if (boost::iequals(request->headersKeys[i], "Accept") &&
            !boost::iequals(request->headersValues[i], "*/*"))
        {
          try
          {
            // TODO - Support conversion to GIF
        
            mime = Orthanc::StringToMimeType(request->headersValues[i]);
            if (mime != Orthanc::MimeType_Png &&
                mime != Orthanc::MimeType_Jpeg)
            {
              throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
            }
          }
          catch (Orthanc::OrthancException&)
          {
            LOG(ERROR) << "Unsupported MIME type in WADO-RS rendered frame: " << request->headersValues[i];
            throw;
          }
        }
      }

      std::map<std::string, std::string> headers;
      headers["Accept"] = Orthanc::EnumerationToString(mime);

      // NB: In DICOMweb, the "frame" parameter is in the range [1..N], whereas
      // Orthanc uses range [0..N-1], hence the "-1" below
      OrthancPlugins::MemoryBuffer buffer;
      if (buffer.RestApiGet("/instances/" + instanceId + "/frames/" +
                            boost::lexical_cast<std::string>(frame - 1) + "/preview", headers, false))
      {
        OrthancPluginAnswerBuffer(context, output, buffer.GetData(),
                                  buffer.GetSize(), Orthanc::EnumerationToString(mime));
      }
      else
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                        "Inexistent frame index in this image: " + boost::lexical_cast<std::string>(frame));
      }
    }
    else
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem, "Inexistent instance");
    }
  }
}


void RetrieveInstanceRendered(OrthancPluginRestOutput* output,
                              const char* url,
                              const OrthancPluginHttpRequest* request)
{
  AnswerFrameRendered(output, 1 /* first frame */, request);
}


void RetrieveFrameRendered(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request)
{
  assert(request->groupsCount == 4);
  const char* frame = request->groups[3];

  AnswerFrameRendered(output, boost::lexical_cast<int>(frame), request);
}
