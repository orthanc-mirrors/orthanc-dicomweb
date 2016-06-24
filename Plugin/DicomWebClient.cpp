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


#include "DicomWebClient.h"

#include "Plugin.h"
#include "DicomWebServers.h"

#include <json/reader.h>
#include <list>
#include <set>
#include <boost/lexical_cast.hpp>

#include "../Orthanc/Core/ChunkedBuffer.h"
#include "../Orthanc/Core/Toolbox.h"


static void AddInstance(std::list<std::string>& target,
                        const Json::Value& instance)
{
  if (instance.type() != Json::objectValue ||
      !instance.isMember("ID") ||
      instance["ID"].type() != Json::stringValue)
  {
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_InternalError);
  }
  else
  {
    target.push_back(instance["ID"].asString());
  }
}


static bool GetSequenceSize(size_t& result,
                            const Json::Value& answer,
                            const std::string& tag,
                            bool isMandatory,
                            const std::string& server)
{
  const Json::Value* value = NULL;

  std::string upper, lower;
  Orthanc::Toolbox::ToUpperCase(upper, tag);
  Orthanc::Toolbox::ToLowerCase(lower, tag);
  
  if (answer.isMember(upper))
  {
    value = &answer[upper];
  }
  else if (answer.isMember(lower))
  {
    value = &answer[lower];
  }
  else if (isMandatory)
  {
    OrthancPlugins::Configuration::LogError("The STOW-RS JSON response from DICOMweb server " + server + 
                                            " does not contain the mandatory tag " + upper);
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
  }
  else
  {
    return false;
  }

  if (value->type() != Json::objectValue ||
      !value->isMember("Value") ||
      (*value) ["Value"].type() != Json::arrayValue)
  {
    OrthancPlugins::Configuration::LogError("Unable to parse STOW-RS JSON response from DICOMweb server " + server);
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
  }

  result = (*value) ["Value"].size();
  return true;
}



static void ParseRestRequest(std::list<std::string>& instances /* out */,
                             std::map<std::string, std::string>& httpHeaders /* out */,
                             const OrthancPluginHttpRequest* request /* in */)
{
  static const char* RESOURCES = "Resources";
  static const char* HTTP_HEADERS = "HttpHeaders";

  Json::Value body;
  Json::Reader reader;
  if (!reader.parse(request->body, request->body + request->bodySize, body) ||
      body.type() != Json::objectValue ||
      !body.isMember(RESOURCES) ||
      body[RESOURCES].type() != Json::arrayValue)
  {
    OrthancPlugins::Configuration::LogError("A request to the DICOMweb STOW-RS client must provide a JSON object "
                                            "with the field \"" + std::string(RESOURCES) + 
                                            "\" containing an array of resources to be sent");
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
  }

  OrthancPlugins::ParseAssociativeArray(httpHeaders, body, HTTP_HEADERS);

  Json::Value& resources = body[RESOURCES];

  // Extract information about all the child instances
  for (Json::Value::ArrayIndex i = 0; i < resources.size(); i++)
  {
    if (resources[i].type() != Json::stringValue)
    {
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
    }

    std::string resource = resources[i].asString();
    if (resource.empty())
    {
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_UnknownResource);
    }

    Json::Value tmp;
    if (OrthancPlugins::RestApiGetJson(tmp, OrthancPlugins::Configuration::GetContext(), "/instances/" + resource, false))
    {
      AddInstance(instances, tmp);
    }
    else if ((OrthancPlugins::RestApiGetJson(tmp, OrthancPlugins::Configuration::GetContext(), "/series/" + resource, false) &&
              OrthancPlugins::RestApiGetJson(tmp, OrthancPlugins::Configuration::GetContext(), "/series/" + resource + "/instances", false)) ||
             (OrthancPlugins::RestApiGetJson(tmp, OrthancPlugins::Configuration::GetContext(), "/studies/" + resource, false) &&
              OrthancPlugins::RestApiGetJson(tmp, OrthancPlugins::Configuration::GetContext(), "/studies/" + resource + "/instances", false)) ||
             (OrthancPlugins::RestApiGetJson(tmp, OrthancPlugins::Configuration::GetContext(), "/patients/" + resource, false) &&
              OrthancPlugins::RestApiGetJson(tmp, OrthancPlugins::Configuration::GetContext(), "/patients/" + resource + "/instances", false)))
    {
      if (tmp.type() != Json::arrayValue)
      {
        throw OrthancPlugins::PluginException(OrthancPluginErrorCode_InternalError);
      }

      for (Json::Value::ArrayIndex j = 0; j < tmp.size(); j++)
      {
        AddInstance(instances, tmp[j]);
      }
    }
    else
    {
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_UnknownResource);
    }   
  }
}


static void SendStowChunks(const Orthanc::WebServiceParameters& server,
                           const std::map<std::string, std::string>& httpHeaders,
                           const std::string& boundary,
                           Orthanc::ChunkedBuffer& chunks,
                           size_t& countInstances,
                           bool force)
{
  unsigned int maxInstances = OrthancPlugins::Configuration::GetUnsignedIntegerValue("StowMaxInstances", 10);
  size_t maxSize = static_cast<size_t>(OrthancPlugins::Configuration::GetUnsignedIntegerValue("StowMaxSize", 10)) * 1024 * 1024;

  if ((force && countInstances > 0) ||
      (maxInstances != 0 && countInstances >= maxInstances) ||
      (maxSize != 0 && chunks.GetNumBytes() >= maxSize))
  {
    chunks.AddChunk("\r\n--" + boundary + "--\r\n");

    std::string body;
    chunks.Flatten(body);

    OrthancPlugins::MemoryBuffer answerBody(OrthancPlugins::Configuration::GetContext());
    std::map<std::string, std::string> answerHeaders;
    OrthancPlugins::CallServer(answerBody, answerHeaders, server, OrthancPluginHttpMethod_Post,
                               httpHeaders, "studies", body);

    Json::Value response;
    Json::Reader reader;
    bool success = reader.parse(reinterpret_cast<const char*>((*answerBody)->data),
                                reinterpret_cast<const char*>((*answerBody)->data) + (*answerBody)->size, response);
    answerBody.Clear();

    if (!success ||
        response.type() != Json::objectValue ||
        !response.isMember("00081199"))
    {
      OrthancPlugins::Configuration::LogError("Unable to parse STOW-RS JSON response from DICOMweb server " + server.GetUrl());
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
    }

    size_t size;
    if (!GetSequenceSize(size, response, "00081199", true, server.GetUrl()) ||
        size != countInstances)
    {
      OrthancPlugins::Configuration::LogError("The STOW-RS server was only able to receive " + 
                                              boost::lexical_cast<std::string>(size) + " instances out of " +
                                              boost::lexical_cast<std::string>(countInstances));
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
    }

    if (GetSequenceSize(size, response, "00081198", false, server.GetUrl()) &&
        size != 0)
    {
      OrthancPlugins::Configuration::LogError("The response from the STOW-RS server contains " + 
                                              boost::lexical_cast<std::string>(size) + 
                                              " items in its Failed SOP Sequence (0008,1198) tag");
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);    
    }

    if (GetSequenceSize(size, response, "0008119A", false, server.GetUrl()) &&
        size != 0)
    {
      OrthancPlugins::Configuration::LogError("The response from the STOW-RS server contains " + 
                                              boost::lexical_cast<std::string>(size) + 
                                              " items in its Other Failures Sequence (0008,119A) tag");
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);    
    }

    countInstances = 0;
  }
}


void StowClient(OrthancPluginRestOutput* output,
                const char* /*url*/,
                const OrthancPluginHttpRequest* request)
{
  if (request->groupsCount != 1)
  {
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadRequest);
  }

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "POST");
    return;
  }

  Orthanc::WebServiceParameters server(OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]));

  std::string boundary;

  {
    char* uuid = OrthancPluginGenerateUuid(OrthancPlugins::Configuration::GetContext());
    try
    {
      boundary.assign(uuid);
    }
    catch (...)
    {
      OrthancPluginFreeString(OrthancPlugins::Configuration::GetContext(), uuid);
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NotEnoughMemory);
    }

    OrthancPluginFreeString(OrthancPlugins::Configuration::GetContext(), uuid);
  }

  std::string mime = "multipart/related; type=application/dicom; boundary=" + boundary;

  std::map<std::string, std::string> httpHeaders;
  httpHeaders["Accept"] = "application/json";
  httpHeaders["Expect"] = "";
  httpHeaders["Content-Type"] = mime;

  std::list<std::string> instances;
  ParseRestRequest(instances, httpHeaders, request);

  {
    OrthancPlugins::Configuration::LogInfo("Sending " + boost::lexical_cast<std::string>(instances.size()) + 
                                           " instances using STOW-RS to DICOMweb server: " + server.GetUrl());
  }

  Orthanc::ChunkedBuffer chunks;
  size_t countInstances = 0;

  for (std::list<std::string>::const_iterator it = instances.begin(); it != instances.end(); it++)
  {
    OrthancPlugins::MemoryBuffer dicom(OrthancPlugins::Configuration::GetContext());
    if (dicom.RestApiGet("/instances/" + *it + "/file", false))
    {
      chunks.AddChunk("\r\n--" + boundary + "\r\n" +
                      "Content-Type: application/dicom\r\n" +
                      "Content-Length: " + boost::lexical_cast<std::string>(dicom.GetSize()) +
                      "\r\n\r\n");
      chunks.AddChunk(dicom.GetData(), dicom.GetSize());
      countInstances ++;

      SendStowChunks(server, httpHeaders, boundary, chunks, countInstances, false);
    }
  }

  SendStowChunks(server, httpHeaders, boundary, chunks, countInstances, true);

  std::string answer = "{}\n";
  OrthancPluginAnswerBuffer(OrthancPlugins::Configuration::GetContext(), output, answer.c_str(), answer.size(), "application/json");
}


static bool GetStringValue(std::string& target,
                           const Json::Value& json,
                           const std::string& key)
{
  if (json.type() != Json::objectValue)
  {
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
  }
  else if (!json.isMember(key))
  {
    target.clear();
    return false;
  }
  else if (json[key].type() != Json::stringValue)
  {
    OrthancPlugins::Configuration::LogError("The field \"" + key + "\" in a JSON object should be a string");
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
  }
  else
  {
    target = json[key].asString();
    return true;
  }
}


void GetFromServer(OrthancPluginRestOutput* output,
                   const char* /*url*/,
                   const OrthancPluginHttpRequest* request)
{
  static const char* URI = "Uri";
  static const char* HTTP_HEADERS = "HttpHeaders";
  static const char* GET_ARGUMENTS = "Arguments";

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "POST");
    return;
  }

  Orthanc::WebServiceParameters server(OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]));

  std::string tmp;
  Json::Value body;
  Json::Reader reader;
  if (!reader.parse(request->body, request->body + request->bodySize, body) ||
      body.type() != Json::objectValue ||
      !GetStringValue(tmp, body, URI))
  {
    OrthancPlugins::Configuration::LogError("A request to the DICOMweb STOW-RS client must provide a JSON object "
                                            "with the field \"Uri\" containing the URI of interest");
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
  }

  std::map<std::string, std::string> getArguments;
  OrthancPlugins::ParseAssociativeArray(getArguments, body, GET_ARGUMENTS);

  std::string uri;
  OrthancPlugins::UriEncode(uri, tmp, getArguments);

  std::map<std::string, std::string> httpHeaders;
  OrthancPlugins::ParseAssociativeArray(httpHeaders, body, HTTP_HEADERS);

  OrthancPlugins::MemoryBuffer answerBody(OrthancPlugins::Configuration::GetContext());
  std::map<std::string, std::string> answerHeaders;
  OrthancPlugins::CallServer(answerBody, answerHeaders, server, OrthancPluginHttpMethod_Get, httpHeaders, uri, "");

  std::string contentType = "application/octet-stream";

  for (std::map<std::string, std::string>::const_iterator
         it = answerHeaders.begin(); it != answerHeaders.end(); ++it)
  {
    std::string key = it->first;
    Orthanc::Toolbox::ToLowerCase(key);

    if (key == "content-type")
    {
      contentType = it->second;
    }
    else if (key == "transfer-encoding")
    {
      // Do not forward this header
    }
    else
    {
      OrthancPluginSetHttpHeader(OrthancPlugins::Configuration::GetContext(), output, it->first.c_str(), it->second.c_str());
    }
  }

  OrthancPluginAnswerBuffer(OrthancPlugins::Configuration::GetContext(), output, 
                            reinterpret_cast<const char*>(answerBody.GetData()),
                            answerBody.GetSize(), contentType.c_str());
}



static void RetrieveFromServerInternal(std::set<std::string>& instances,
                                       const Orthanc::WebServiceParameters& server,
                                       const std::map<std::string, std::string>& httpHeaders,
                                       const Json::Value& resource)
{
  static const std::string STUDY = "Study";
  static const std::string SERIES = "Series";
  static const std::string INSTANCE = "Instance";
  static const std::string MULTIPART_RELATED = "multipart/related";
  static const std::string APPLICATION_DICOM = "application/dicom";

  if (resource.type() != Json::objectValue)
  {
    OrthancPlugins::Configuration::LogError("Resources of interest for the DICOMweb WADO-RS Retrieve client "
                                            "must be provided as a JSON object");
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
  }

  std::string study, series, instance;
  if (!GetStringValue(study, resource, STUDY) ||
      study.empty())
  {
    OrthancPlugins::Configuration::LogError("A non-empty \"" + STUDY + "\" field is mandatory for the "
                                            "DICOMweb WADO-RS Retrieve client");
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
  }

  GetStringValue(series, resource, SERIES);
  GetStringValue(instance, resource, INSTANCE);

  if (series.empty() && 
      !instance.empty())
  {
    OrthancPlugins::Configuration::LogError("When specifying a \"" + INSTANCE + "\" field in a call to DICOMweb "
                                            "WADO-RS Retrieve client, the \"" + SERIES + "\" field is mandatory");
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
  }

  std::string uri = "studies/" + study;
  if (!series.empty())
  {
    uri += "/series/" + series;
    if (!instance.empty())
    {
      uri += "/instances/" + instance;
    }
  }

  OrthancPlugins::MemoryBuffer answerBody(OrthancPlugins::Configuration::GetContext());
  std::map<std::string, std::string> answerHeaders;
  OrthancPlugins::CallServer(answerBody, answerHeaders, server, OrthancPluginHttpMethod_Get, httpHeaders, uri, "");

  std::vector<std::string> contentType;
  for (std::map<std::string, std::string>::const_iterator 
         it = answerHeaders.begin(); it != answerHeaders.end(); ++it)
  {
    std::string s = Orthanc::Toolbox::StripSpaces(it->first);
    Orthanc::Toolbox::ToLowerCase(s);
    if (s == "content-type")
    {
      Orthanc::Toolbox::TokenizeString(contentType, it->second, ';');
      break;
    }
  }

  if (contentType.empty())
  {
    OrthancPlugins::Configuration::LogError("No Content-Type provided by the remote WADO-RS server");
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
  }

  Orthanc::Toolbox::ToLowerCase(contentType[0]);
  if (Orthanc::Toolbox::StripSpaces(contentType[0]) != MULTIPART_RELATED)
  {
    OrthancPlugins::Configuration::LogError("The remote WADO-RS server answers with a \"" + contentType[0] +
                                            "\" Content-Type, but \"" + MULTIPART_RELATED + "\" is expected");
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
  }

  std::string type, boundary;
  for (size_t i = 1; i < contentType.size(); i++)
  {
    std::vector<std::string> tokens;
    Orthanc::Toolbox::TokenizeString(tokens, contentType[i], '=');

    if (tokens.size() == 2)
    {
      std::string s = Orthanc::Toolbox::StripSpaces(tokens[0]);
      Orthanc::Toolbox::ToLowerCase(s);

      if (s == "type")
      {
        type = Orthanc::Toolbox::StripSpaces(tokens[1]);
        Orthanc::Toolbox::ToLowerCase(type);
      }
      else if (s == "boundary")
      {
        boundary = Orthanc::Toolbox::StripSpaces(tokens[1]);
      }
    }
  }

  if (type != APPLICATION_DICOM)
  {
    OrthancPlugins::Configuration::LogError("The remote WADO-RS server answers with a \"" + type +
                                            "\" multipart Content-Type, but \"" + APPLICATION_DICOM + "\" is expected");
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
  }

  if (boundary.empty())
  {
    OrthancPlugins::Configuration::LogError("The remote WADO-RS server does not provide a boundary for its multipart answer");
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
  }

  std::vector<OrthancPlugins::MultipartItem> parts;
  OrthancPlugins::ParseMultipartBody(parts, OrthancPlugins::Configuration::GetContext(), 
                                     reinterpret_cast<const char*>(answerBody.GetData()),
                                     answerBody.GetSize(), boundary);

  OrthancPlugins::Configuration::LogInfo("The remote WADO-RS server has provided " +
                                         boost::lexical_cast<std::string>(parts.size()) + 
                                         " DICOM instances");

  for (size_t i = 0; i < parts.size(); i++)
  {
    if (parts[i].contentType_ != APPLICATION_DICOM)
    {
      OrthancPlugins::Configuration::LogError("The remote WADO-RS server has provided a non-DICOM file in its multipart answer");
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);      
    }

    OrthancPlugins::MemoryBuffer tmp(OrthancPlugins::Configuration::GetContext());
    tmp.RestApiPost("/instances", parts[i].data_, parts[i].size_, false);

    Json::Value result;
    tmp.ToJson(result);

    if (result.type() != Json::objectValue ||
        !result.isMember("ID") ||
        result["ID"].type() != Json::stringValue)
    {
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_InternalError);      
    }
    else
    {
      instances.insert(result["ID"].asString());
    }
  }
}



void RetrieveFromServer(OrthancPluginRestOutput* output,
                        const char* /*url*/,
                        const OrthancPluginHttpRequest* request)
{
  static const std::string RESOURCES("Resources");
  static const char* HTTP_HEADERS = "HttpHeaders";

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "POST");
    return;
  }

  Orthanc::WebServiceParameters server(OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]));

  Json::Value body;
  Json::Reader reader;
  if (!reader.parse(request->body, request->body + request->bodySize, body) ||
      body.type() != Json::objectValue ||
      !body.isMember(RESOURCES) ||
      body[RESOURCES].type() != Json::arrayValue)
  {
    OrthancPlugins::Configuration::LogError("A request to the DICOMweb WADO-RS Retrieve client must provide a JSON object "
                                            "with the field \"" + RESOURCES + "\" containing an array of resources");
    throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
  }

  std::map<std::string, std::string> httpHeaders;
  OrthancPlugins::ParseAssociativeArray(httpHeaders, body, HTTP_HEADERS);

  std::set<std::string> instances;
  for (Json::Value::ArrayIndex i = 0; i < body[RESOURCES].size(); i++)
  {
    RetrieveFromServerInternal(instances, server, httpHeaders, body[RESOURCES][i]);
  }

  Json::Value status = Json::objectValue;
  status["Instances"] = Json::arrayValue;
  
  for (std::set<std::string>::const_iterator
         it = instances.begin(); it != instances.end(); ++it)
  {
    status["Instances"].append(*it);
  }

  std::string s = status.toStyledString();
  OrthancPluginAnswerBuffer(OrthancPlugins::Configuration::GetContext(), output, s.c_str(), s.size(), "application/json");
}
