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


#include "Plugin.h"

#include "../Core/Configuration.h"
#include "../Core/Dicom.h"
#include "../Core/MultipartWriter.h"


static bool AcceptMultipartDicom(const OrthancPluginHttpRequest* request)
{
  std::string accept;

  if (!OrthancPlugins::LookupHttpHeader(accept, request, "accept"))
  {
    return true;   // By default, return "multipart/related; type=application/dicom;"
  }

  std::string application;
  std::map<std::string, std::string> attributes;
  OrthancPlugins::ParseContentType(application, attributes, accept);

  if (application != "multipart/related" &&
      application != "*/*")
  {
    std::string s = "This WADO-RS plugin cannot generate the following content type: " + accept;
    OrthancPluginLogError(context_, s.c_str());
    return false;
  }

  if (attributes.find("type") != attributes.end())
  {
    std::string s = attributes["type"];
    OrthancPlugins::ToLowerCase(s);
    if (s != "application/dicom")
    {
      std::string s = "This WADO-RS plugin only supports application/dicom return type (" + accept + ")";
      OrthancPluginLogError(context_, s.c_str());
      return false;
    }
  }

  if (attributes.find("transfer-syntax") != attributes.end())
  {
    std::string s = "This WADO-RS plugin cannot change the transfer syntax to " + attributes["transfer-syntax"];
    OrthancPluginLogError(context_, s.c_str());
    return false;
  }

  return true;
}



static int32_t AnswerListOfDicomInstances(OrthancPluginRestOutput* output,
                                          const std::string& resource)
{
  Json::Value instances;
  if (!OrthancPlugins::RestApiGetJson(instances, context_, resource + "/instances"))
  {
    // Internal error
    OrthancPluginSendHttpStatusCode(context_, output, 400);
    return 0;
  }
  
  
  OrthancPlugins::MultipartWriter writer("application/dicom");
  for (Json::Value::ArrayIndex i = 0; i < instances.size(); i++)
  {
    std::string uri = "/instances/" + instances[i]["ID"].asString() + "/file";
    std::string dicom;
    if (OrthancPlugins::RestApiGetString(dicom, context_, uri))
    {
      writer.AddPart(dicom);
    }
  }

  writer.Answer(context_, output);
  return 0;
}



int32_t RetrieveDicomStudy(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request)
{
  try
  {
    if (request->method != OrthancPluginHttpMethod_Get)
    {
      OrthancPluginSendMethodNotAllowed(context_, output, "GET");
      return 0;
    }

    if (!AcceptMultipartDicom(request))
    {
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
      return 0;
    }

    std::string id;

    {
      char* tmp = OrthancPluginLookupStudy(context_, request->groups[0]);
      if (tmp == NULL)
      {
        std::string s = "Accessing an inexistent study with WADO-RS: " + std::string(request->groups[0]);
        OrthancPluginLogError(context_, s.c_str());
        OrthancPluginSendHttpStatusCode(context_, output, 404);
        return 0;
      }

      id.assign(tmp);
      OrthancPluginFreeString(context_, tmp);
    }
  
    AnswerListOfDicomInstances(output, "/studies/" + id);

    return 0;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}


int32_t RetrieveDicomSeries(OrthancPluginRestOutput* output,
                            const char* url,
                            const OrthancPluginHttpRequest* request)
{
  try
  {
    if (request->method != OrthancPluginHttpMethod_Get)
    {
      OrthancPluginSendMethodNotAllowed(context_, output, "GET");
      return 0;
    }

    if (!AcceptMultipartDicom(request))
    {
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
      return 0;
    }

    std::string id;

    {
      char* tmp = OrthancPluginLookupSeries(context_, request->groups[1]);
      if (tmp == NULL)
      {
        std::string s = "Accessing an inexistent series with WADO-RS: " + std::string(request->groups[1]);
        OrthancPluginLogError(context_, s.c_str());
        OrthancPluginSendHttpStatusCode(context_, output, 404);
        return 0;
      }

      id.assign(tmp);
      OrthancPluginFreeString(context_, tmp);
    }
  
    Json::Value study;
    if (!OrthancPlugins::RestApiGetJson(study, context_, "/series/" + id + "/study"))
    {
      OrthancPluginSendHttpStatusCode(context_, output, 404);
      return 0;
    }

    if (study["MainDicomTags"]["StudyInstanceUID"].asString() != std::string(request->groups[0]))
    {
      std::string s = "No series " + std::string(request->groups[1]) + " in study " + std::string(request->groups[0]);
      OrthancPluginLogError(context_, s.c_str());
      OrthancPluginSendHttpStatusCode(context_, output, 404);
      return 0;
    }

    AnswerListOfDicomInstances(output, "/series/" + id);

    return 0;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}



int32_t RetrieveDicomInstance(OrthancPluginRestOutput* output,
                              const char* url,
                              const OrthancPluginHttpRequest* request)
{
  try
  {
    if (request->method != OrthancPluginHttpMethod_Get)
    {
      OrthancPluginSendMethodNotAllowed(context_, output, "GET");
      return 0;
    }

    if (!AcceptMultipartDicom(request))
    {
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
      return 0;
    }

    std::string id;

    {
      char* tmp = OrthancPluginLookupInstance(context_, request->groups[2]);
      if (tmp == NULL)
      {
        std::string s = "Accessing an inexistent instance with WADO-RS: " + std::string(request->groups[2]);
        OrthancPluginLogError(context_, s.c_str());
        OrthancPluginSendHttpStatusCode(context_, output, 404);
        return 0;
      }

      id.assign(tmp);
      OrthancPluginFreeString(context_, tmp);
    }
  
    Json::Value study, series;
    if (!OrthancPlugins::RestApiGetJson(series, context_, "/instances/" + id + "/series") ||
        !OrthancPlugins::RestApiGetJson(study, context_, "/instances/" + id + "/study"))
    {
      OrthancPluginSendHttpStatusCode(context_, output, 404);
      return 0;
    }

    if (study["MainDicomTags"]["StudyInstanceUID"].asString() != std::string(request->groups[0]) ||
        series["MainDicomTags"]["SeriesInstanceUID"].asString() != std::string(request->groups[1]))
    {
      std::string s = ("No instance " + std::string(request->groups[2]) + 
                       " in study " + std::string(request->groups[0]) + " or " +
                       " in series " + std::string(request->groups[1]));
      OrthancPluginLogError(context_, s.c_str());
      OrthancPluginSendHttpStatusCode(context_, output, 404);
      return 0;
    }

    OrthancPlugins::MultipartWriter writer("application/dicom");
    std::string dicom;
    if (OrthancPlugins::RestApiGetString(dicom, context_, "/instances/" + id + "/file"))
    {
      writer.AddPart(dicom);
    }

    writer.Answer(context_, output);

    return 0;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}
