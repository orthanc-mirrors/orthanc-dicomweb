/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
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

#include "Configuration.h"
#include "Dicom.h"
#include "DicomResults.h"
#include "../Orthanc/Core/Toolbox.h"

#include <memory>

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
    OrthancPlugins::Configuration::LogError("This WADO-RS plugin cannot generate the following content type: " + accept);
    return false;
  }

  if (attributes.find("type") != attributes.end())
  {
    std::string s = attributes["type"];
    Orthanc::Toolbox::ToLowerCase(s);
    if (s != "application/dicom")
    {
      OrthancPlugins::Configuration::LogError("This WADO-RS plugin only supports application/dicom "
                                              "return type for DICOM retrieval (" + accept + ")");
      return false;
    }
  }

  if (attributes.find("transfer-syntax") != attributes.end())
  {
    OrthancPlugins::Configuration::LogError("This WADO-RS plugin cannot change the transfer syntax to " + 
                                            attributes["transfer-syntax"]);
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
    OrthancPlugins::Configuration::LogError("This WADO-RS plugin cannot generate the following content type: " + accept);
    return false;
  }

  if (attributes.find("type") != attributes.end())
  {
    std::string s = attributes["type"];
    Orthanc::Toolbox::ToLowerCase(s);
    if (s != "application/dicom+xml")
    {
      OrthancPlugins::Configuration::LogError("This WADO-RS plugin only supports application/json or "
                                              "application/dicom+xml return types for metadata (" + accept + ")");
      return false;
    }
  }

  if (attributes.find("transfer-syntax") != attributes.end())
  {
    OrthancPlugins::Configuration::LogError("This WADO-RS plugin cannot change the transfer syntax to " + 
                                            attributes["transfer-syntax"]);
    return false;
  }

  return true;
}



static bool AcceptBulkData(const OrthancPluginHttpRequest* request)
{
  std::string accept;

  if (!OrthancPlugins::LookupHttpHeader(accept, request, "accept"))
  {
    return true;   // By default, return "multipart/related; type=application/octet-stream;"
  }

  std::string application;
  std::map<std::string, std::string> attributes;
  OrthancPlugins::ParseContentType(application, attributes, accept);

  if (application != "multipart/related" &&
      application != "*/*")
  {
    OrthancPlugins::Configuration::LogError("This WADO-RS plugin cannot generate the following bulk data type: " + accept);
    return false;
  }

  if (attributes.find("type") != attributes.end())
  {
    std::string s = attributes["type"];
    Orthanc::Toolbox::ToLowerCase(s);
    if (s != "application/octet-stream")
    {
      OrthancPlugins::Configuration::LogError("This WADO-RS plugin only supports application/octet-stream "
                                              "return type for bulk data retrieval (" + accept + ")");
      return false;
    }
  }

  if (attributes.find("ra,ge") != attributes.end())
  {
    OrthancPlugins::Configuration::LogError("This WADO-RS plugin does not support Range retrieval, "
                                            "it can only return entire bulk data object");
    return false;
  }

  return true;
}


static void AnswerListOfDicomInstances(OrthancPluginRestOutput* output,
                                       const std::string& resource)
{
  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  Json::Value instances;
  if (!OrthancPlugins::RestApiGet(instances, context, resource + "/instances", false))
  {
    // Internal error
    OrthancPluginSendHttpStatusCode(context, output, 400);
    return;
  }

  if (OrthancPluginStartMultipartAnswer(context, output, "related", "application/dicom"))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }
  
  for (Json::Value::ArrayIndex i = 0; i < instances.size(); i++)
  {
    std::string uri = "/instances/" + instances[i]["ID"].asString() + "/file";

    OrthancPlugins::MemoryBuffer dicom(context);
    if (dicom.RestApiGet(uri, false) &&
        OrthancPluginSendMultipartItem(context, output, dicom.GetData(), dicom.GetSize()) != 0)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
    }
  }
}



static void AnswerMetadata(OrthancPluginRestOutput* output,
                           const OrthancPluginHttpRequest* request,
                           const std::string& resource,
                           bool isInstance,
                           bool isXml)
{
  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  std::list<std::string> files;
  if (isInstance)
  {
    files.push_back(resource + "/file");
  }
  else
  {
    Json::Value instances;
    if (!OrthancPlugins::RestApiGet(instances, context, resource + "/instances", false))
    {
      // Internal error
      OrthancPluginSendHttpStatusCode(context, output, 400);
      return;
    }

    for (Json::Value::ArrayIndex i = 0; i < instances.size(); i++)
    {
      files.push_back("/instances/" + instances[i]["ID"].asString() + "/file");
    }
  }

  const std::string wadoBase = OrthancPlugins::Configuration::GetBaseUrl(request);
  OrthancPlugins::DicomResults results(context, output, wadoBase, *dictionary_, isXml, true);
  
  for (std::list<std::string>::const_iterator
         it = files.begin(); it != files.end(); ++it)
  {
    OrthancPlugins::MemoryBuffer content(context);
    if (content.RestApiGet(*it, false))
    {
      OrthancPlugins::ParsedDicomFile dicom(content);
      results.Add(dicom.GetFile());
    }
  }

  results.Answer();
}




static bool LocateStudy(OrthancPluginRestOutput* output,
                        std::string& uri,
                        const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
    return false;
  }

  std::string id;

  {
    char* tmp = OrthancPluginLookupStudy(context, request->groups[0]);
    if (tmp == NULL)
    {
      OrthancPlugins::Configuration::LogError("Accessing an inexistent study with WADO-RS: " + std::string(request->groups[0]));
      OrthancPluginSendHttpStatusCode(context, output, 404);
      return false;
    }

    id.assign(tmp);
    OrthancPluginFreeString(context, tmp);
  }
  
  uri = "/studies/" + id;
  return true;
}


static bool LocateSeries(OrthancPluginRestOutput* output,
                         std::string& uri,
                         const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
    return false;
  }

  std::string id;

  {
    char* tmp = OrthancPluginLookupSeries(context, request->groups[1]);
    if (tmp == NULL)
    {
      OrthancPlugins::Configuration::LogError("Accessing an inexistent series with WADO-RS: " + std::string(request->groups[1]));
      OrthancPluginSendHttpStatusCode(context, output, 404);
      return false;
    }

    id.assign(tmp);
    OrthancPluginFreeString(context, tmp);
  }
  
  Json::Value study;
  if (!OrthancPlugins::RestApiGet(study, context, "/series/" + id + "/study", false))
  {
    OrthancPluginSendHttpStatusCode(context, output, 404);
    return false;
  }

  if (study["MainDicomTags"]["StudyInstanceUID"].asString() != std::string(request->groups[0]))
  {
    OrthancPlugins::Configuration::LogError("No series " + std::string(request->groups[1]) + 
                                            " in study " + std::string(request->groups[0]));
    OrthancPluginSendHttpStatusCode(context, output, 404);
    return false;
  }
  
  uri = "/series/" + id;
  return true;
}


bool LocateInstance(OrthancPluginRestOutput* output,
                    std::string& uri,
                    const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
    return false;
  }

  std::string id;

  {
    char* tmp = OrthancPluginLookupInstance(context, request->groups[2]);
    if (tmp == NULL)
    {
      OrthancPlugins::Configuration::LogError("Accessing an inexistent instance with WADO-RS: " + 
                                              std::string(request->groups[2]));
      OrthancPluginSendHttpStatusCode(context, output, 404);
      return false;
    }

    id.assign(tmp);
    OrthancPluginFreeString(context, tmp);
  }
  
  Json::Value study, series;
  if (!OrthancPlugins::RestApiGet(series, context, "/instances/" + id + "/series", false) ||
      !OrthancPlugins::RestApiGet(study, context, "/instances/" + id + "/study", false))
  {
    OrthancPluginSendHttpStatusCode(context, output, 404);
    return false;
  }

  if (study["MainDicomTags"]["StudyInstanceUID"].asString() != std::string(request->groups[0]) ||
      series["MainDicomTags"]["SeriesInstanceUID"].asString() != std::string(request->groups[1]))
  {
    OrthancPlugins::Configuration::LogError("No instance " + std::string(request->groups[2]) + 
                                            " in study " + std::string(request->groups[0]) + " or " +
                                            " in series " + std::string(request->groups[1]));
    OrthancPluginSendHttpStatusCode(context, output, 404);
    return false;
  }

  uri = "/instances/" + id;
  return true;
}


void RetrieveDicomStudy(OrthancPluginRestOutput* output,
                        const char* url,
                        const OrthancPluginHttpRequest* request)
{
  if (!AcceptMultipartDicom(request))
  {
    OrthancPluginSendHttpStatusCode(OrthancPlugins::Configuration::GetContext(), output, 400 /* Bad request */);
  }
  else
  {
    std::string uri;
    if (LocateStudy(output, uri, request))
    {
      AnswerListOfDicomInstances(output, uri);
    }
  }
}


void RetrieveDicomSeries(OrthancPluginRestOutput* output,
                         const char* url,
                         const OrthancPluginHttpRequest* request)
{
  if (!AcceptMultipartDicom(request))
  {
    OrthancPluginSendHttpStatusCode(OrthancPlugins::Configuration::GetContext(), output, 400 /* Bad request */);
  }
  else
  {
    std::string uri;
    if (LocateSeries(output, uri, request))
    {
      AnswerListOfDicomInstances(output, uri);
    }
  }
}



void RetrieveDicomInstance(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  if (!AcceptMultipartDicom(request))
  {
    OrthancPluginSendHttpStatusCode(context, output, 400 /* Bad request */);
  }
  else
  {
    std::string uri;
    if (LocateInstance(output, uri, request))
    {
      if (OrthancPluginStartMultipartAnswer(context, output, "related", "application/dicom"))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
      }

      OrthancPlugins::MemoryBuffer dicom(context);
      if (dicom.RestApiGet(uri + "/file", false) &&
          OrthancPluginSendMultipartItem(context, output, dicom.GetData(), dicom.GetSize()) != 0)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
      }
    }
  }
}



void RetrieveStudyMetadata(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request)
{
  bool isXml;
  if (!AcceptMetadata(request, isXml))
  {
    OrthancPluginSendHttpStatusCode(OrthancPlugins::Configuration::GetContext(), output, 400 /* Bad request */);
  }
  else
  {
    std::string uri;
    if (LocateStudy(output, uri, request))
    {
      AnswerMetadata(output, request, uri, false, isXml);
    }
  }
}


void RetrieveSeriesMetadata(OrthancPluginRestOutput* output,
                            const char* url,
                            const OrthancPluginHttpRequest* request)
{
  bool isXml;
  if (!AcceptMetadata(request, isXml))
  {
    OrthancPluginSendHttpStatusCode(OrthancPlugins::Configuration::GetContext(), output, 400 /* Bad request */);
  }
  else
  {
    std::string uri;
    if (LocateSeries(output, uri, request))
    {
      AnswerMetadata(output, request, uri, false, isXml);
    }
  }
}


void RetrieveInstanceMetadata(OrthancPluginRestOutput* output,
                              const char* url,
                              const OrthancPluginHttpRequest* request)
{
  bool isXml;
  if (!AcceptMetadata(request, isXml))
  {
    OrthancPluginSendHttpStatusCode(OrthancPlugins::Configuration::GetContext(), output, 400 /* Bad request */);
  }
  else
  {
    std::string uri;
    if (LocateInstance(output, uri, request))
    {
      AnswerMetadata(output, request, uri, true, isXml);
    }
  }
}



static uint32_t Hex2Dec(char c)
{
  return (c >= '0' && c <= '9') ? c - '0' : c - 'a' + 10;
}


static bool ParseBulkTag(gdcm::Tag& tag,
                         const std::string& s)
{
  if (s.size() != 8)
  {
    return false;
  }

  for (size_t i = 0; i < 8; i++)
  {
    if ((s[i] < '0' || s[i] > '9') &&
        (s[i] < 'a' || s[i] > 'f'))
    {
      return false;
    }
  }

  uint32_t g = ((Hex2Dec(s[0]) << 12) +
                (Hex2Dec(s[1]) << 8) +
                (Hex2Dec(s[2]) << 4) +
                Hex2Dec(s[3]));

  uint32_t e = ((Hex2Dec(s[4]) << 12) +
                (Hex2Dec(s[5]) << 8) +
                (Hex2Dec(s[6]) << 4) +
                Hex2Dec(s[7]));

  tag = gdcm::Tag(g, e);
  return true;
}


static bool ExploreBulkData(std::string& content,
                            const std::vector<std::string>& path,
                            size_t position,
                            const gdcm::DataSet& dataset)
{
  gdcm::Tag tag;
  if (!ParseBulkTag(tag, path[position]) ||
      !dataset.FindDataElement(tag))
  {
    return false;
  }

  const gdcm::DataElement& element = dataset.GetDataElement(tag);

  if (position + 1 == path.size())
  {
    const gdcm::ByteValue* data = element.GetByteValue();
    if (!data)
    {
      content.clear();
    }
    else
    {
      content.assign(data->GetPointer(), data->GetLength());
    }

    return true;
  }

  return false;
}


void RetrieveBulkData(OrthancPluginRestOutput* output,
                      const char* url,
                      const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

  if (!AcceptBulkData(request))
  {
    OrthancPluginSendHttpStatusCode(context, output, 400 /* Bad request */);
    return;
  }

  std::string uri;
  OrthancPlugins::MemoryBuffer content(context);
  if (LocateInstance(output, uri, request) &&
      content.RestApiGet(uri + "/file", false))
  {
    OrthancPlugins::ParsedDicomFile dicom(content);

    std::vector<std::string> path;
    Orthanc::Toolbox::TokenizeString(path, request->groups[3], '/');
      
    std::string result;
    if (path.size() % 2 == 1 &&
        ExploreBulkData(result, path, 0, dicom.GetDataSet()))
    {
      if (OrthancPluginStartMultipartAnswer(context, output, "related", "application/octet-stream") != 0 ||
          OrthancPluginSendMultipartItem(context, output, result.c_str(), result.size()) != 0)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_Plugin);
      }
    }
    else
    {
      OrthancPluginSendHttpStatusCode(context, output, 400 /* Bad request */);
    }      
  }
}
