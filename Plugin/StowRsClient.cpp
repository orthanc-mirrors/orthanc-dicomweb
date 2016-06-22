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


#include "StowRsClient.h"

#include "Plugin.h"
#include "DicomWebServers.h"

#include <json/reader.h>
#include <list>
#include <boost/lexical_cast.hpp>

#include "../Orthanc/Core/ChunkedBuffer.h"
#include "../Orthanc/Core/OrthancException.h"
#include "../Orthanc/Core/Toolbox.h"


static void AddInstance(std::list<std::string>& target,
                        const Json::Value& instance)
{
  if (instance.type() != Json::objectValue ||
      !instance.isMember("ID") ||
      instance["ID"].type() != Json::stringValue)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
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
    std::string s = ("The STOW-RS JSON response from DICOMweb server " + server + 
                     " does not contain the mandatory tag " + upper);
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }
  else
  {
    return false;
  }

  if (value->type() != Json::objectValue ||
      !value->isMember("Value") ||
      (*value) ["Value"].type() != Json::arrayValue)
  {
    std::string s = "Unable to parse STOW-RS JSON response from DICOMweb server " + server;
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  result = (*value) ["Value"].size();
  return true;
}



static const char* ConvertToCString(const std::string& s)
{
  if (s.empty())
  {
    return NULL;
  }
  else
  {
    return s.c_str();
  }
}



static void SendStowRequest(const Orthanc::WebServiceParameters& server,
                            const std::map<std::string, std::string>& httpHeaders,
                            const std::string& body,
                            size_t countInstances)
{
  std::vector<const char*> httpHeadersKeys(httpHeaders.size());
  std::vector<const char*> httpHeadersValues(httpHeaders.size());

  {
    size_t pos = 0;
    for (std::map<std::string, std::string>::const_iterator
           it = httpHeaders.begin(); it != httpHeaders.end(); ++it)
    {
      httpHeadersKeys[pos] = it->first.c_str();
      httpHeadersValues[pos] = it->second.c_str();
      pos += 1;
    }
  }

  std::string url = server.GetUrl() + "studies";

  uint16_t status = 0;
  OrthancPluginMemoryBuffer answerBody;
  OrthancPluginErrorCode code = OrthancPluginHttpClient(
    context_, &answerBody, 
    NULL,                                   /* No interest in the HTTP headers of the answer */
    &status, 
    OrthancPluginHttpMethod_Post,
    url.c_str(), 
    /* HTTP headers*/
    httpHeaders.size(),
    httpHeadersKeys.empty() ? NULL : &httpHeadersKeys[0],
    httpHeadersValues.empty() ? NULL : &httpHeadersValues[0],
    body.c_str(), body.size(),              /* POST body */
    ConvertToCString(server.GetUsername()), /* Authentication */
    ConvertToCString(server.GetPassword()), 
    0,                                      /* Timeout */
    ConvertToCString(server.GetCertificateFile()),
    ConvertToCString(server.GetCertificateKeyFile()),
    ConvertToCString(server.GetCertificateKeyPassword()),
    server.IsPkcs11Enabled() ? 1 : 0);

  if (code != OrthancPluginErrorCode_Success ||
      (status != 200 && status != 202))
  {
    std::string s = ("Cannot send DICOM images through STOW-RS to DICOMweb server " + server.GetUrl() + 
                     " (HTTP status: " + boost::lexical_cast<std::string>(status) + ")");
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(static_cast<Orthanc::ErrorCode>(code));
  }

  Json::Value response;
  Json::Reader reader;
  bool success = reader.parse(reinterpret_cast<const char*>(answerBody.data),
                              reinterpret_cast<const char*>(answerBody.data) + answerBody.size, response);
  OrthancPluginFreeMemoryBuffer(context_, &answerBody);

  if (!success ||
      response.type() != Json::objectValue ||
      !response.isMember("00081199"))
  {
    std::string s = "Unable to parse STOW-RS JSON response from DICOMweb server " + server.GetUrl();
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  size_t size;
  if (!GetSequenceSize(size, response, "00081199", true, server.GetUrl()) ||
      size != countInstances)
  {
    std::string s = ("The STOW-RS server was only able to receive " + 
                     boost::lexical_cast<std::string>(size) + " instances out of " +
                     boost::lexical_cast<std::string>(countInstances));
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  if (GetSequenceSize(size, response, "00081198", false, server.GetUrl()) &&
      size != 0)
  {
    std::string s = ("The response from the STOW-RS server contains " + 
                     boost::lexical_cast<std::string>(size) + 
                     " items in its Failed SOP Sequence (0008,1198) tag");
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);    
  }

  if (GetSequenceSize(size, response, "0008119A", false, server.GetUrl()) &&
      size != 0)
  {
    std::string s = ("The response from the STOW-RS server contains " + 
                     boost::lexical_cast<std::string>(size) + 
                     " items in its Other Failures Sequence (0008,119A) tag");
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);    
  }
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
    std::string s = ("A request to the DICOMweb STOW-RS client must provide a JSON object "
                     "with the field \"Resources\" containing an array of resources to be sent");
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }

  Json::Value& resources = body[RESOURCES];

  if (body.isMember(HTTP_HEADERS))
  {
    const Json::Value& tmp = body[HTTP_HEADERS];

    if (tmp.type() != Json::objectValue)
    {
      std::string s = "The HTTP headers of a DICOMweb STOW-RS client request must be given as a JSON associative array";
      OrthancPluginLogError(context_, s.c_str());
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
    }
    else
    {
      Json::Value::Members names = tmp.getMemberNames();
      for (size_t i = 0; i < names.size(); i++)
      {
        if (tmp[names[i]].type() != Json::stringValue)
        {
          std::string s = "The HTTP header \"" + names[i] + "\" is not a string in some DICOMweb STOW-RS client request";
          OrthancPluginLogError(context_, s.c_str());
          throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
        }
        else
        {
          httpHeaders[names[i]] = tmp[names[i]].asString();
        }
      }
    }
  }

  // Extract information about all the child instances
  for (Json::Value::ArrayIndex i = 0; i < resources.size(); i++)
  {
    if (resources[i].type() != Json::stringValue)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
    }

    std::string resource = resources[i].asString();
    if (resource.empty())
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource);
    }

    Json::Value tmp;
    if (OrthancPlugins::RestApiGetJson(tmp, context_, "/instances/" + resource, false))
    {
      AddInstance(instances, tmp);
    }
    else if ((OrthancPlugins::RestApiGetJson(tmp, context_, "/series/" + resource, false) &&
              OrthancPlugins::RestApiGetJson(tmp, context_, "/series/" + resource + "/instances", false)) ||
             (OrthancPlugins::RestApiGetJson(tmp, context_, "/studies/" + resource, false) &&
              OrthancPlugins::RestApiGetJson(tmp, context_, "/studies/" + resource + "/instances", false)) ||
             (OrthancPlugins::RestApiGetJson(tmp, context_, "/patients/" + resource, false) &&
              OrthancPlugins::RestApiGetJson(tmp, context_, "/patients/" + resource + "/instances", false)))
    {
      if (tmp.type() != Json::arrayValue)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
      }

      for (Json::Value::ArrayIndex j = 0; j < tmp.size(); j++)
      {
        AddInstance(instances, tmp[j]);
      }
    }
    else
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource);
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
  if ((force && countInstances > 0) ||
      countInstances > 10 /* TODO Parameter */ ||
      chunks.GetNumBytes() > 10 * 1024 * 1024 /* TODO Parameter */)
  {
    chunks.AddChunk("\r\n--" + boundary + "--\r\n");

    std::string body;
    chunks.Flatten(body);

    SendStowRequest(server, httpHeaders, body, countInstances);
    countInstances = 0;
  }
}


void StowClient(OrthancPluginRestOutput* output,
                const char* /*url*/,
                const OrthancPluginHttpRequest* request)
{
  if (request->groupsCount != 1)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest);
  }

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(context_, output, "POST");
    return;
  }

  Orthanc::WebServiceParameters server(OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]));

  std::string boundary;

  {
    char* uuid = OrthancPluginGenerateUuid(context_);
    try
    {
      boundary.assign(uuid);
    }
    catch (...)
    {
      OrthancPluginFreeString(context_, uuid);
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NotEnoughMemory);
    }

    OrthancPluginFreeString(context_, uuid);
  }

  std::string mime = "multipart/related; type=application/dicom; boundary=" + boundary;

  std::map<std::string, std::string> httpHeaders;
  httpHeaders["Accept"] = "application/json";
  httpHeaders["Expect"] = "";
  httpHeaders["Content-Type"] = mime;

  std::list<std::string> instances;
  ParseRestRequest(instances, httpHeaders, request);

  {
    std::string s = ("Sending " + boost::lexical_cast<std::string>(instances.size()) + 
                     " instances using STOW-RS to DICOMweb server: " + server.GetUrl());
    OrthancPluginLogInfo(context_, s.c_str());
  }

  Orthanc::ChunkedBuffer chunks;
  size_t countInstances = 0;

  for (std::list<std::string>::const_iterator it = instances.begin(); it != instances.end(); it++)
  {
    std::string dicom;
    if (OrthancPlugins::RestApiGetString(dicom, context_, "/instances/" + *it + "/file"))
    {
      chunks.AddChunk("\r\n--" + boundary + "\r\n" +
                      "Content-Type: application/dicom\r\n" +
                      "Content-Length: " + boost::lexical_cast<std::string>(dicom.size()) +
                      "\r\n\r\n");
      chunks.AddChunk(dicom);
      countInstances ++;

      SendStowChunks(server, httpHeaders, boundary, chunks, countInstances, false);
    }
  }

  SendStowChunks(server, httpHeaders, boundary, chunks, countInstances, true);

  std::string answer = "{}\n";
  OrthancPluginAnswerBuffer(context_, output, answer.c_str(), answer.size(), "application/json");
}
