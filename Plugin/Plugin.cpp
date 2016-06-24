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

#include "QidoRs.h"
#include "StowRs.h"
#include "DicomWebClient.h"
#include "WadoRs.h"
#include "WadoUri.h"
#include "Configuration.h"
#include "DicomWebServers.h"

#include "../Orthanc/Plugins/Samples/Common/OrthancPluginCppWrapper.h"
#include "../Orthanc/Core/Toolbox.h"

#include <gdcmDictEntry.h>
#include <gdcmDict.h>
#include <gdcmDicts.h>
#include <gdcmGlobal.h>


// Global state
const gdcm::Dict* dictionary_ = NULL;


#include <boost/lexical_cast.hpp>


void SwitchStudies(OrthancPluginRestOutput* output,
                   const char* url,
                   const OrthancPluginHttpRequest* request)
{
  switch (request->method)
  {
    case OrthancPluginHttpMethod_Get:
      // This is QIDO-RS
      SearchForStudies(output, url, request);
      break;

    case OrthancPluginHttpMethod_Post:
      // This is STOW-RS
      StowCallback(output, url, request);
      break;

    default:
      OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "GET,POST");
      break;
  }
}


void SwitchStudy(OrthancPluginRestOutput* output,
                 const char* url,
                 const OrthancPluginHttpRequest* request)
{
  switch (request->method)
  {
    case OrthancPluginHttpMethod_Get:
      // This is WADO-RS
      RetrieveDicomStudy(output, url, request);
      break;

    case OrthancPluginHttpMethod_Post:
      // This is STOW-RS
      StowCallback(output, url, request);
      break;

    default:
      OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "GET,POST");
      break;
  }
}


void ListServers(OrthancPluginRestOutput* output,
                 const char* url,
                 const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "GET");
  }
  else
  {
    std::list<std::string> servers;
    OrthancPlugins::DicomWebServers::GetInstance().ListServers(servers);

    Json::Value json = Json::arrayValue;
    for (std::list<std::string>::const_iterator it = servers.begin(); it != servers.end(); ++it)
    {
      json.append(*it);
    }

    std::string answer = json.toStyledString(); 
    OrthancPluginAnswerBuffer(OrthancPlugins::Configuration::GetContext(), output, answer.c_str(), answer.size(), "application/json");
  }
}


void ListServerOperations(OrthancPluginRestOutput* output,
                          const char* /*url*/,
                          const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "GET");
  }
  else
  {
    // Make sure the server does exist
    OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]);

    Json::Value json = Json::arrayValue;
    json.append("get");
    json.append("stow");

    std::string answer = json.toStyledString(); 
    OrthancPluginAnswerBuffer(OrthancPlugins::Configuration::GetContext(), output, answer.c_str(), answer.size(), "application/json");
  }
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
    uri += "/" + series;
    if (!instance.empty())
    {
      uri += "/" + instance;
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



extern "C"
{
  ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* context)
  {
    /* Check the version of the Orthanc core */
    if (OrthancPluginCheckVersion(context) == 0)
    {
      char info[1024];
      sprintf(info, "Your version of Orthanc (%s) must be above %d.%d.%d to run this plugin",
              context->orthancVersion,
              ORTHANC_PLUGINS_MINIMAL_MAJOR_NUMBER,
              ORTHANC_PLUGINS_MINIMAL_MINOR_NUMBER,
              ORTHANC_PLUGINS_MINIMAL_REVISION_NUMBER);
      OrthancPluginLogError(context, info);
      return -1;
    }

    OrthancPluginSetDescription(context, "Implementation of DICOMweb (QIDO-RS, STOW-RS and WADO-RS) and WADO-URI.");

    try
    {
      // Read the configuration
      OrthancPlugins::Configuration::Initialize(context);

      // Initialize GDCM
      dictionary_ = &gdcm::Global::GetInstance().GetDicts().GetPublicDict();

      // Configure the DICOMweb callbacks
      if (OrthancPlugins::Configuration::GetBooleanValue("Enable", true))
      {
        std::string root = OrthancPlugins::Configuration::GetRoot();
        assert(!root.empty() && root[root.size() - 1] == '/');

        OrthancPlugins::Configuration::LogWarning("URI to the DICOMweb REST API: " + root);

        OrthancPlugins::RegisterRestCallback<SearchForInstances>(context, root + "instances", true);
        OrthancPlugins::RegisterRestCallback<SearchForSeries>(context, root + "series", true);    
        OrthancPlugins::RegisterRestCallback<SwitchStudies>(context, root + "studies", true);
        OrthancPlugins::RegisterRestCallback<SwitchStudy>(context, root + "studies/([^/]*)", true);
        OrthancPlugins::RegisterRestCallback<SearchForInstances>(context, root + "studies/([^/]*)/instances", true);    
        OrthancPlugins::RegisterRestCallback<RetrieveStudyMetadata>(context, root + "studies/([^/]*)/metadata", true);
        OrthancPlugins::RegisterRestCallback<SearchForSeries>(context, root + "studies/([^/]*)/series", true);    
        OrthancPlugins::RegisterRestCallback<RetrieveDicomSeries>(context, root + "studies/([^/]*)/series/([^/]*)", true);
        OrthancPlugins::RegisterRestCallback<SearchForInstances>(context, root + "studies/([^/]*)/series/([^/]*)/instances", true);    
        OrthancPlugins::RegisterRestCallback<RetrieveDicomInstance>(context, root + "studies/([^/]*)/series/([^/]*)/instances/([^/]*)", true);
        OrthancPlugins::RegisterRestCallback<RetrieveBulkData>(context, root + "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/bulk/(.*)", true);
        OrthancPlugins::RegisterRestCallback<RetrieveInstanceMetadata>(context, root + "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/metadata", true);
        OrthancPlugins::RegisterRestCallback<RetrieveSeriesMetadata>(context, root + "studies/([^/]*)/series/([^/]*)/metadata", true);
        OrthancPlugins::RegisterRestCallback<RetrieveFrames>(context, root + "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/frames", true);
        OrthancPlugins::RegisterRestCallback<RetrieveFrames>(context, root + "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/frames/([^/]*)", true);

        OrthancPlugins::RegisterRestCallback<ListServers>(context, root + "servers", true);
        OrthancPlugins::RegisterRestCallback<ListServerOperations>(context, root + "servers/([^/]*)", true);
        OrthancPlugins::RegisterRestCallback<StowClient>(context, root + "servers/([^/]*)/stow", true);
        OrthancPlugins::RegisterRestCallback<GetFromServer>(context, root + "servers/([^/]*)/get", true);
        OrthancPlugins::RegisterRestCallback<RetrieveFromServer>(context, root + "servers/([^/]*)/retrieve", true);
      }
      else
      {
        OrthancPlugins::Configuration::LogWarning("DICOMweb support is disabled");
      }

      // Configure the WADO callback
      if (OrthancPlugins::Configuration::GetBooleanValue("EnableWado", true))
      {
        std::string wado = OrthancPlugins::Configuration::GetWadoRoot();
        OrthancPlugins::Configuration::LogWarning("URI to the WADO-URI API: " + wado);

        OrthancPlugins::RegisterRestCallback<WadoUriCallback>(context, wado, true);
      }
      else
      {
        OrthancPlugins::Configuration::LogWarning("WADO-URI support is disabled");
      }
    }
    catch (OrthancPlugins::PluginException& e)
    {
      OrthancPlugins::Configuration::LogError("Exception while initializing the DICOMweb plugin: " + 
                                              std::string(e.GetErrorDescription(context)));
      return -1;
    }
    catch (Orthanc::OrthancException& e)
    {
      OrthancPlugins::Configuration::LogError("Exception while initializing the DICOMweb plugin: " + 
                                              std::string(e.What()));
      return -1;
    }
    catch (...)
    {
      OrthancPlugins::Configuration::LogError("Exception while initializing the DICOMweb plugin");
      return -1;
    }

    return 0;
  }


  ORTHANC_PLUGINS_API void OrthancPluginFinalize()
  {
  }


  ORTHANC_PLUGINS_API const char* OrthancPluginGetName()
  {
    return "dicom-web";
  }


  ORTHANC_PLUGINS_API const char* OrthancPluginGetVersion()
  {
    return ORTHANC_DICOM_WEB_VERSION;
  }
}
