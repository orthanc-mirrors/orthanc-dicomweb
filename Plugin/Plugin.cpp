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
#include "StowRsClient.h"
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


#include "../Orthanc/Core/OrthancException.h"
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



static const char* GET_ARGUMENTS = "Arguments";


static void UrlEncode(std::string& url,
                      const Orthanc::WebServiceParameters& server,
                      const std::string& uri,
                      const std::map<std::string, std::string>& getArguments)
{
  url = server.GetUrl();
  assert(!url.empty() && url[url.size() - 1] == '/');

  if (uri.find('?') != std::string::npos)
  {
    OrthancPlugins::Configuration::LogError("The GET arguments must be provided in the \"" + 
                                            std::string(GET_ARGUMENTS) + "\" field (\"?\" is disallowed): " + uri);
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }

  // Remove the leading "/" in the URI if need be
  std::string tmp;
  if (!uri.empty() &&
      uri[0] == '/')
  {
    url += uri.substr(1);
  }
  else
  {
    url += uri;
  }

  bool isFirst = true;
  for (std::map<std::string, std::string>::const_iterator
         it = getArguments.begin(); it != getArguments.end(); ++it)
  {
    if (isFirst)
    {
      url += '?';
      isFirst = false;
    }
    else
    {
      url += '&';
    }

    std::string key, value;
    Orthanc::Toolbox::UriEncode(key, it->first);
    Orthanc::Toolbox::UriEncode(value, it->second);

    if (value.empty())
    {
      url += key;
    }
    else
    {
      url += key + "=" + value;
    }
  }
}
                      


void GetFromServer(OrthancPluginRestOutput* output,
                   const char* /*url*/,
                   const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "POST");
    return;
  }

  Orthanc::WebServiceParameters server(OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]));

  static const char* URI = "Uri";
  static const char* HTTP_HEADERS = "HttpHeaders";

  Json::Value body;
  Json::Reader reader;
  if (!reader.parse(request->body, request->body + request->bodySize, body) ||
      body.type() != Json::objectValue ||
      !body.isMember(URI) ||
      body[URI].type() != Json::stringValue)
  {
    OrthancPlugins::Configuration::LogError("A request to the DICOMweb STOW-RS client must provide a JSON object "
                                            "with the field \"Uri\" containing the URI of interest");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }

  std::map<std::string, std::string> httpHeaders, getArguments;
  OrthancPlugins::ParseAssociativeArray(httpHeaders, body, HTTP_HEADERS);
  OrthancPlugins::ParseAssociativeArray(getArguments, body, GET_ARGUMENTS);

  std::string url; 
  UrlEncode(url, server, body[URI].asString(), getArguments);

  printf("URL: [%s]\n", url.c_str());
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
