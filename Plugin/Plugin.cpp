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
#include "WadoRs.h"
#include "WadoUri.h"
#include "Configuration.h"


#include <gdcmDictEntry.h>
#include <gdcmDict.h>
#include <gdcmDicts.h>
#include <gdcmGlobal.h>


#include <json/reader.h>
#include <list>
#include "../Orthanc/Core/ChunkedBuffer.h"
#include "../Orthanc/Core/Toolbox.h"


// Global state
OrthancPluginContext* context_ = NULL;
Json::Value configuration_;
const gdcm::Dict* dictionary_ = NULL;

#include "../Orthanc/Core/OrthancException.h"
#include <boost/lexical_cast.hpp>


typedef void (*RestCallback) (OrthancPluginRestOutput* output,
                              const char* url,
                              const OrthancPluginHttpRequest* request);


template <RestCallback Callback>
OrthancPluginErrorCode Protect(OrthancPluginRestOutput* output,
                               const char* url,
                               const OrthancPluginHttpRequest* request)
{
  try
  {
    Callback(output, url, request);
    return OrthancPluginErrorCode_Success;
  }
  catch (Orthanc::OrthancException& e)
  {
    OrthancPluginLogError(context_, e.What());
    return OrthancPluginErrorCode_Plugin;
  }
  catch (boost::bad_lexical_cast& e)
  {
    OrthancPluginLogError(context_, e.what());
    return OrthancPluginErrorCode_Plugin;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return OrthancPluginErrorCode_Plugin;
  }
}



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
      OrthancPluginSendMethodNotAllowed(context_, output, "GET,POST");
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
      OrthancPluginSendMethodNotAllowed(context_, output, "GET,POST");
      break;
  }
}


static void Register(const std::string& root,
                     const std::string& uri,
                     OrthancPluginRestCallback callback)
{
  assert(!uri.empty() && uri[0] != '/');
  std::string s = root + uri;
  OrthancPluginRegisterRestCallback(context_, s.c_str(), callback);
}






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
                            const std::string& peer)
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
    std::string s = ("The STOW-RS JSON response from DICOMweb peer " + peer + 
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
    std::string s = "Unable to parse STOW-RS JSON response from DICOMweb peer " + peer;
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  result = (*value) ["Value"].size();
  return true;
}


static void SendStowRequest(const std::string& url,
                            const char* username,
                            const char* password,
                            const std::string& body,
                            const std::string& mime,
                            size_t countInstances)
{
  const char* headersKeys[] = {
    "Accept",
    "Expect",
    "Content-Type"
  };

  const char* headersValues[] = {
    "application/json",
    "",
    mime.c_str()
  };

  uint16_t status = 0;
  OrthancPluginMemoryBuffer answer;
  OrthancPluginErrorCode code = OrthancPluginHttpClient(context_, &answer, &status, OrthancPluginHttpMethod_Post,
                                                        url.c_str(), 3, headersKeys, headersValues,
                                                        body.c_str(), body.size(), username, password, 0);
  if (code != OrthancPluginErrorCode_Success ||
      (status != 200 && status != 202))
  {
    std::string s = ("Cannot send DICOM images through STOW-RS to DICOMweb peer " + url + 
                     " (HTTP status: " + boost::lexical_cast<std::string>(status) + ")");
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(static_cast<Orthanc::ErrorCode>(code));
  }

  Json::Value response;
  Json::Reader reader;
  bool success = reader.parse(reinterpret_cast<const char*>(answer.data),
                              reinterpret_cast<const char*>(answer.data) + answer.size, response);
  OrthancPluginFreeMemoryBuffer(context_, &answer);

  if (!success ||
      response.type() != Json::objectValue ||
      !response.isMember("00081199"))
  {
    std::string s = "Unable to parse STOW-RS JSON response from DICOMweb peer " + url;
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  size_t size;
  if (!GetSequenceSize(size, response, "00081199", true, url) ||
      size != countInstances)
  {
    std::string s = ("The STOW-RS server was only able to receive " + 
                     boost::lexical_cast<std::string>(size) + " instances out of " +
                     boost::lexical_cast<std::string>(countInstances));
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  if (GetSequenceSize(size, response, "00081198", false, url) &&
      size != 0)
  {
    std::string s = ("The response from the STOW-RS server contains " + 
                     boost::lexical_cast<std::string>(size) + 
                     " items in its Failed SOP Sequence (0008,1198) tag");
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);    
  }

  if (GetSequenceSize(size, response, "0008119A", false, url) &&
      size != 0)
  {
    std::string s = ("The response from the STOW-RS server contains " + 
                     boost::lexical_cast<std::string>(size) + 
                     " items in its Other Failures Sequence (0008,119A) tag");
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);    
  }
}



void StowClient(OrthancPluginRestOutput* output,
                const char* url,
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

  std::string peer(request->groups[0]);

  Json::Value resources;
  Json::Reader reader;
  if (!reader.parse(request->body, request->body + request->bodySize, resources) ||
      resources.type() != Json::arrayValue)
  {
    std::string s = "The list of resources to be sent through DICOMweb STOW-RS must be given as a JSON array";
    OrthancPluginLogError(context_, s.c_str());
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }

  // Extract information about all the child instances
  std::list<std::string> instances;
  for (Json::Value::ArrayIndex i = 0; i < resources.size(); i++)
  {
    if (resources[i].type() != Json::stringValue)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
    }

    std::string resource = resources[i].asString();

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
      // Unkown resource
      throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource);
    }   
  }


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

  Orthanc::ChunkedBuffer chunks;
  chunks.AddChunk("\r\n"); // Empty preamble

  for (std::list<std::string>::const_iterator it = instances.begin(); it != instances.end(); it++)
  {
    std::string dicom;
    if (OrthancPlugins::RestApiGetString(dicom, context_, "/instances/" + *it + "/file"))
    {
      chunks.AddChunk("--" + boundary + "\r\n" +
                      "Content-Type: application/dicom\r\n" +
                      "Content-Length: " + boost::lexical_cast<std::string>(dicom.size()) +
                      "\r\n\r\n");
      chunks.AddChunk(dicom);
      chunks.AddChunk("\r\n");
    }
  }

  chunks.AddChunk("--" + boundary + "--\r\n");

  std::string body;
  chunks.Flatten(body);

  // TODO Split the message

  SendStowRequest("http://localhost:8043/dicom-web/studies", NULL, NULL, body, mime, instances.size());
}


extern "C"
{
  ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* context)
  {
    context_ = context;

    /* Check the version of the Orthanc core */
    if (OrthancPluginCheckVersion(context_) == 0)
    {
      char info[1024];
      sprintf(info, "Your version of Orthanc (%s) must be above %d.%d.%d to run this plugin",
              context_->orthancVersion,
              ORTHANC_PLUGINS_MINIMAL_MAJOR_NUMBER,
              ORTHANC_PLUGINS_MINIMAL_MINOR_NUMBER,
              ORTHANC_PLUGINS_MINIMAL_REVISION_NUMBER);
      OrthancPluginLogError(context_, info);
      return -1;
    }

    {
      std::string version(context_->orthancVersion);
      if (version == "0.9.1")
      {
        OrthancPluginLogWarning(context_, "If using STOW-RS, the DICOMweb plugin can lead to "
                                "deadlocks in Orthanc version 0.9.1. Please upgrade Orthanc!");
      }
    }


    OrthancPluginSetDescription(context_, "Implementation of DICOM Web (QIDO-RS, STOW-RS and WADO-RS) and WADO.");

    // Read the configuration
    dictionary_ = &gdcm::Global::GetInstance().GetDicts().GetPublicDict();

    configuration_ = Json::objectValue;

    {
      Json::Value tmp;
      if (!OrthancPlugins::Configuration::Read(tmp, context) ||
          tmp.type() != Json::objectValue)
      {
        OrthancPluginLogError(context_, "Unable to read the configuration file");
        return -1;
      }

      if (tmp.isMember("DicomWeb") &&
          tmp["DicomWeb"].type() == Json::objectValue)
      {
        configuration_ = tmp["DicomWeb"];
      }
    }

    // Configure the DICOMweb callbacks
    if (OrthancPlugins::Configuration::GetBoolValue(configuration_, "Enable", true))
    {
      std::string root = OrthancPlugins::Configuration::GetRoot(configuration_);

      std::string message = "URI to the DICOMweb REST API: " + root;
      OrthancPluginLogWarning(context_, message.c_str());

      Register(root, "instances", Protect<SearchForInstances>);
      Register(root, "series", Protect<SearchForSeries>);    
      Register(root, "studies", Protect<SwitchStudies>);
      Register(root, "studies/([^/]*)", Protect<SwitchStudy>);
      Register(root, "studies/([^/]*)/instances", Protect<SearchForInstances>);    
      Register(root, "studies/([^/]*)/metadata", Protect<RetrieveStudyMetadata>);
      Register(root, "studies/([^/]*)/series", Protect<SearchForSeries>);    
      Register(root, "studies/([^/]*)/series/([^/]*)", Protect<RetrieveDicomSeries>);
      Register(root, "studies/([^/]*)/series/([^/]*)/instances", Protect<SearchForInstances>);    
      Register(root, "studies/([^/]*)/series/([^/]*)/instances/([^/]*)", Protect<RetrieveDicomInstance>);
      Register(root, "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/bulk/(.*)", Protect<RetrieveBulkData>);
      Register(root, "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/metadata", Protect<RetrieveInstanceMetadata>);
      Register(root, "studies/([^/]*)/series/([^/]*)/metadata", Protect<RetrieveSeriesMetadata>);
      Register(root, "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/frames", Protect<RetrieveFrames>);
      Register(root, "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/frames/([^/]*)", Protect<RetrieveFrames>);

      Register(root, "peers/([^/]*)/stow", Protect<StowClient>);
    }
    else
    {
      OrthancPluginLogWarning(context_, "DICOMweb support is disabled");
    }

    // Configure the WADO callback
    if (OrthancPlugins::Configuration::GetBoolValue(configuration_, "EnableWado", true))
    {
      std::string wado = OrthancPlugins::Configuration::GetWadoRoot(configuration_);

      std::string message = "URI to the WADO API: " + wado;
      OrthancPluginLogWarning(context_, message.c_str());

      OrthancPluginRegisterRestCallback(context_, wado.c_str(), Protect<WadoUriCallback>);
    }
    else
    {
      OrthancPluginLogWarning(context_, "WADO support is disabled");
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
