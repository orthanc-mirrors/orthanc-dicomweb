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
#include "../Core/DicomResults.h"
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
      std::string s = "This WADO-RS plugin only supports application/dicom return type for DICOM retrieval (" + accept + ")";
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



static bool AcceptMetadata(const OrthancPluginHttpRequest* request,
                           bool& isXml)
{
  isXml = true;

  std::string accept;

  if (!OrthancPlugins::LookupHttpHeader(accept, request, "accept"))
  {
    // By default, return "multipart/related; type=application/dicom+xml;"
    return true;
  }

  std::string application;
  std::map<std::string, std::string> attributes;
  OrthancPlugins::ParseContentType(application, attributes, accept);

  if (application == "application/json")
  {
    isXml = false;
    return true;
  }

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
    if (s != "application/dicom+xml")
    {
      std::string s = "This WADO-RS plugin only supports application/json or application/dicom+xml return types for metadata (" + accept + ")";
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



static void AnswerMetadata(OrthancPluginRestOutput* output,
                           const std::string& resource,
                           bool isInstance,
                           bool isXml)
{
  std::list<std::string> files;
  if (isInstance)
  {
    files.push_back(resource + "/file");
  }
  else
  {
    Json::Value instances;
    if (!OrthancPlugins::RestApiGetJson(instances, context_, resource + "/instances"))
    {
      // Internal error
      OrthancPluginSendHttpStatusCode(context_, output, 400);
      return;
    }

    for (Json::Value::ArrayIndex i = 0; i < instances.size(); i++)
    {
      files.push_back("/instances/" + instances[i]["ID"].asString() + "/file");
    }
  }

  OrthancPlugins::DicomResults results(*dictionary_, isXml, true);
  
  for (std::list<std::string>::const_iterator
         it = files.begin(); it != files.end(); ++it)
  {
    std::string content; 
    if (OrthancPlugins::RestApiGetString(content, context_, *it))
    {
      OrthancPlugins::ParsedDicomFile dicom(content);
      results.Add(dicom.GetDataSet());
    }
  }

  results.Answer(context_, output);
}




static bool LocateStudy(OrthancPluginRestOutput* output,
                        std::string& uri,
                        const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context_, output, "GET");
    return false;
  }

  std::string id;

  {
    char* tmp = OrthancPluginLookupStudy(context_, request->groups[0]);
    if (tmp == NULL)
    {
      std::string s = "Accessing an inexistent study with WADO-RS: " + std::string(request->groups[0]);
      OrthancPluginLogError(context_, s.c_str());
      OrthancPluginSendHttpStatusCode(context_, output, 404);
      return false;
    }

    id.assign(tmp);
    OrthancPluginFreeString(context_, tmp);
  }
  
  uri = "/studies/" + id;
  return true;
}


static bool LocateSeries(OrthancPluginRestOutput* output,
                         std::string& uri,
                         const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context_, output, "GET");
    return false;
  }

  std::string id;

  {
    char* tmp = OrthancPluginLookupSeries(context_, request->groups[1]);
    if (tmp == NULL)
    {
      std::string s = "Accessing an inexistent series with WADO-RS: " + std::string(request->groups[1]);
      OrthancPluginLogError(context_, s.c_str());
      OrthancPluginSendHttpStatusCode(context_, output, 404);
      return false;
    }

    id.assign(tmp);
    OrthancPluginFreeString(context_, tmp);
  }
  
  Json::Value study;
  if (!OrthancPlugins::RestApiGetJson(study, context_, "/series/" + id + "/study"))
  {
    OrthancPluginSendHttpStatusCode(context_, output, 404);
    return false;
  }

  if (study["MainDicomTags"]["StudyInstanceUID"].asString() != std::string(request->groups[0]))
  {
    std::string s = "No series " + std::string(request->groups[1]) + " in study " + std::string(request->groups[0]);
    OrthancPluginLogError(context_, s.c_str());
    OrthancPluginSendHttpStatusCode(context_, output, 404);
    return false;
  }
  
  uri = "/series/" + id;
  return true;
}


static bool LocateInstance(OrthancPluginRestOutput* output,
                           std::string& uri,
                           const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context_, output, "GET");
    return false;
  }

  std::string id;

  {
    char* tmp = OrthancPluginLookupInstance(context_, request->groups[2]);
    if (tmp == NULL)
    {
      std::string s = "Accessing an inexistent instance with WADO-RS: " + std::string(request->groups[2]);
      OrthancPluginLogError(context_, s.c_str());
      OrthancPluginSendHttpStatusCode(context_, output, 404);
      return false;
    }

    id.assign(tmp);
    OrthancPluginFreeString(context_, tmp);
  }
  
  Json::Value study, series;
  if (!OrthancPlugins::RestApiGetJson(series, context_, "/instances/" + id + "/series") ||
      !OrthancPlugins::RestApiGetJson(study, context_, "/instances/" + id + "/study"))
  {
    OrthancPluginSendHttpStatusCode(context_, output, 404);
    return false;
  }

  if (study["MainDicomTags"]["StudyInstanceUID"].asString() != std::string(request->groups[0]) ||
      series["MainDicomTags"]["SeriesInstanceUID"].asString() != std::string(request->groups[1]))
  {
    std::string s = ("No instance " + std::string(request->groups[2]) + 
                     " in study " + std::string(request->groups[0]) + " or " +
                     " in series " + std::string(request->groups[1]));
    OrthancPluginLogError(context_, s.c_str());
    OrthancPluginSendHttpStatusCode(context_, output, 404);
    return false;
  }

  uri = "/instances/" + id;
  return true;
}


int32_t RetrieveDicomStudy(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request)
{
  try
  {
    if (!AcceptMultipartDicom(request))
    {
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
      return false;
    }

    std::string uri;
    if (LocateStudy(output, uri, request))
    {
      AnswerListOfDicomInstances(output, uri);
    }

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
    if (!AcceptMultipartDicom(request))
    {
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
      return false;
    }

    std::string uri;
    if (LocateSeries(output, uri, request))
    {
      AnswerListOfDicomInstances(output, uri);
    }

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
    if (!AcceptMultipartDicom(request))
    {
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
      return false;
    }

    std::string uri;
    if (LocateInstance(output, uri, request))
    {
      OrthancPlugins::MultipartWriter writer("application/dicom");
      std::string dicom;
      if (OrthancPlugins::RestApiGetString(dicom, context_, uri + "/file"))
      {
        writer.AddPart(dicom);
      }

      writer.Answer(context_, output);
    }

    return 0;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}



int32_t RetrieveStudyMetadata(OrthancPluginRestOutput* output,
                              const char* url,
                              const OrthancPluginHttpRequest* request)
{
  try
  {
    bool isXml;
    if (!AcceptMetadata(request, isXml))
    {
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
      return false;
    }

    std::string uri;
    if (LocateStudy(output, uri, request))
    {
      AnswerMetadata(output, uri, false, isXml);
    }

    return 0;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}


int32_t RetrieveSeriesMetadata(OrthancPluginRestOutput* output,
                               const char* url,
                               const OrthancPluginHttpRequest* request)
{
  try
  {
    bool isXml;
    if (!AcceptMetadata(request, isXml))
    {
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
      return false;
    }

    std::string uri;
    if (LocateSeries(output, uri, request))
    {
      AnswerMetadata(output, uri, false, isXml);
    }

    return 0;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}


int32_t RetrieveInstanceMetadata(OrthancPluginRestOutput* output,
                                 const char* url,
                                 const OrthancPluginHttpRequest* request)
{
  try
  {
    bool isXml;
    if (!AcceptMetadata(request, isXml))
    {
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
      return false;
    }

    std::string uri;
    if (LocateInstance(output, uri, request))
    {
      AnswerMetadata(output, uri, true, isXml);
    }

    return 0;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}
