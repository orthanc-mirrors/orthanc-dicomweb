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


#include "DicomWebClient.h"
#include "DicomWebServers.h"
#include "GdcmParsedDicomFile.h"
#include "QidoRs.h"
#include "StowRs.h"
#include "WadoRs.h"
#include "WadoUri.h"

#include <Plugins/Samples/Common/OrthancPluginCppWrapper.h>
#include <Core/Toolbox.h>


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
      OrthancPluginSendMethodNotAllowed(OrthancPlugins::GetGlobalContext(), output, "GET,POST");
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
      OrthancPluginSendMethodNotAllowed(OrthancPlugins::GetGlobalContext(), output, "GET,POST");
      break;
  }
}

bool RequestHasKey(const OrthancPluginHttpRequest* request, const char* key)
{
  for (uint32_t i = 0; i < request->getCount; i++)
  {
    if (strcmp(key, request->getKeys[i]) == 0)
      return true;
  }
  return false;
}


void ListServers(OrthancPluginRestOutput* output,
                 const char* url,
                 const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
  }
  else
  {
    std::list<std::string> servers;
    OrthancPlugins::DicomWebServers::GetInstance().ListServers(servers);

    if (RequestHasKey(request, "expand"))
    {
      Json::Value result = Json::objectValue;
      for (std::list<std::string>::const_iterator it = servers.begin(); it != servers.end(); ++it)
      {
        Orthanc::WebServiceParameters server = OrthancPlugins::DicomWebServers::GetInstance().GetServer(*it);
        Json::Value jsonServer;
        // only return the minimum information to identify the destination, do not include "security" information like passwords
        jsonServer["Url"] = server.GetUrl();
        if (!server.GetUsername().empty())
        {
          jsonServer["Username"] = server.GetUsername();
        }
        result[*it] = jsonServer;
      }

      std::string answer = result.toStyledString();
      OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), "application/json");
    }
    else // if expand is not present, keep backward compatibility and return an array of server names
    {
      Json::Value json = Json::arrayValue;
      for (std::list<std::string>::const_iterator it = servers.begin(); it != servers.end(); ++it)
      {
        json.append(*it);
      }

      std::string answer = json.toStyledString();
      OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), "application/json");
    }
  }
}

void ListServerOperations(OrthancPluginRestOutput* output,
                          const char* /*url*/,
                          const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
  }
  else
  {
    // Make sure the server does exist
    OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]);

    Json::Value json = Json::arrayValue;
    json.append("get");
    json.append("retrieve");
    json.append("stow");

    std::string answer = json.toStyledString(); 
    OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), "application/json");
  }
}


static bool DisplayPerformanceWarning(OrthancPluginContext* context)
{
  (void) DisplayPerformanceWarning;   // Disable warning about unused function
  OrthancPluginLogWarning(context, "Performance warning in DICOMweb: "
                          "Non-release build, runtime debug assertions are turned on");
  return true;
}



#include <boost/filesystem.hpp>
#include <Core/SystemToolbox.h>

class StowSender : public OrthancPlugins::HttpClient::IChunkedBody
{
private:
  std::vector<std::string>  files_;
  size_t                    position_;
  std::string               boundary_;

public:
  StowSender(unsigned int size) :
    position_(0),
    boundary_(Orthanc::Toolbox::GenerateUuid())
  {
    boost::filesystem::path p("/home/jodogne/DICOM/Demo/KNIX/Knee (R)/AX.  FSE PD - 5");

    boost::filesystem::directory_iterator end;

    // cycle through the directory
    for (boost::filesystem::directory_iterator it(p); it != end; ++it)
    {
      if (is_regular_file(it->path()))
      {
        files_.push_back(it->path().string());
      }
    }
  }

  const std::string& GetBoundary() const
  {
    return boundary_;
  }

  virtual bool ReadNextChunk(std::string& chunk)
  {
    if (position_ == files_.size() + 1)
    {
      return false;
    }
    else
    {
      if (position_ == files_.size())
      {
        chunk = ("--" + boundary_ + "--");
      }
      else
      {
        std::string f;
        Orthanc::SystemToolbox::ReadFile(f, files_[position_]);
        chunk = ("--" + boundary_ + "\r\n" +
                 "Content-Type: application/dicom\r\n" +
                 "Content-Length: " + boost::lexical_cast<std::string>(f.size()) + "\r\n" +
                 "\r\n" + f + "\r\n");
      }

      position_++;
      return true;
    }
  }
};


ORTHANC_PLUGINS_API OrthancPluginErrorCode OnChangeCallback(OrthancPluginChangeType changeType,
                                                            OrthancPluginResourceType resourceType,
                                                            const char* resourceId)
{
  if (changeType == OrthancPluginChangeType_OrthancStarted)
  {
    try
    {
      StowSender stow(1024*128);
      
      OrthancPlugins::HttpClient client;
      client.SetUrl("http://localhost:8080/dicom-web/studies");
      client.SetMethod(OrthancPluginHttpMethod_Post);
      client.AddHeader("Accept", "application/dicom+json");
      client.AddHeader("Transfer-Encoding", "chunked");
      client.AddHeader("Expect", "");
      client.AddHeader("Content-Type", "multipart/related; type=application/dicom; boundary=" + stow.GetBoundary());
      client.SetTimeout(120);
      client.SetBody(stow);
      client.Execute();

      Json::Value v;
      client.GetAnswerBody().ToJson(v);
      std::cout << v.toStyledString() << std::endl;
    }
    catch (Orthanc::OrthancException& e)
    {
      LOG(ERROR) << "EXCEPTION: " << e.What();
    }
  }

  return OrthancPluginErrorCode_Success;
}


extern "C"
{
  ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* context)
  {
    assert(DisplayPerformanceWarning(context));
    OrthancPlugins::SetGlobalContext(context);
    Orthanc::Logging::Initialize(context);

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
      OrthancPlugins::Configuration::Initialize();

      //OrthancPluginRegisterOnChangeCallback(context, OnChangeCallback);  // TODO => REMOVE
      
      // Initialize GDCM
      OrthancPlugins::GdcmParsedDicomFile::Initialize();

      // Configure the DICOMweb callbacks
      if (OrthancPlugins::Configuration::GetBooleanValue("Enable", true))
      {
        std::string root = OrthancPlugins::Configuration::GetRoot();
        assert(!root.empty() && root[root.size() - 1] == '/');

        OrthancPlugins::LogWarning("URI to the DICOMweb REST API: " + root);

        OrthancPlugins::RegisterRestCallback<SearchForInstances>(root + "instances", true);
        OrthancPlugins::RegisterRestCallback<SearchForSeries>(root + "series", true);    
        OrthancPlugins::RegisterRestCallback<SwitchStudies>(root + "studies", true);
        OrthancPlugins::RegisterRestCallback<SwitchStudy>(root + "studies/([^/]*)", true);
        OrthancPlugins::RegisterRestCallback<SearchForInstances>(root + "studies/([^/]*)/instances", true);    
        OrthancPlugins::RegisterRestCallback<RetrieveStudyMetadata>(root + "studies/([^/]*)/metadata", true);
        OrthancPlugins::RegisterRestCallback<SearchForSeries>(root + "studies/([^/]*)/series", true);    
        OrthancPlugins::RegisterRestCallback<RetrieveDicomSeries>(root + "studies/([^/]*)/series/([^/]*)", true);
        OrthancPlugins::RegisterRestCallback<SearchForInstances>(root + "studies/([^/]*)/series/([^/]*)/instances", true);    
        OrthancPlugins::RegisterRestCallback<RetrieveDicomInstance>(root + "studies/([^/]*)/series/([^/]*)/instances/([^/]*)", true);
        OrthancPlugins::RegisterRestCallback<RetrieveBulkData>(root + "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/bulk/(.*)", true);
        OrthancPlugins::RegisterRestCallback<RetrieveInstanceMetadata>(root + "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/metadata", true);
        OrthancPlugins::RegisterRestCallback<RetrieveSeriesMetadata>(root + "studies/([^/]*)/series/([^/]*)/metadata", true);
        OrthancPlugins::RegisterRestCallback<RetrieveFrames>(root + "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/frames", true);
        OrthancPlugins::RegisterRestCallback<RetrieveFrames>(root + "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/frames/([^/]*)", true);

        OrthancPlugins::RegisterRestCallback<ListServers>(root + "servers", true);
        OrthancPlugins::RegisterRestCallback<ListServerOperations>(root + "servers/([^/]*)", true);
        OrthancPlugins::RegisterRestCallback<StowClient>(root + "servers/([^/]*)/stow", true);
        OrthancPlugins::RegisterRestCallback<GetFromServer>(root + "servers/([^/]*)/get", true);
        OrthancPlugins::RegisterRestCallback<RetrieveFromServer>(root + "servers/([^/]*)/retrieve", true);
      }
      else
      {
        OrthancPlugins::LogWarning("DICOMweb support is disabled");
      }

      // Configure the WADO callback
      if (OrthancPlugins::Configuration::GetBooleanValue("EnableWado", true))
      {
        std::string wado = OrthancPlugins::Configuration::GetWadoRoot();
        OrthancPlugins::LogWarning("URI to the WADO-URI API: " + wado);

        OrthancPlugins::RegisterRestCallback<WadoUriCallback>(wado, true);
      }
      else
      {
        OrthancPlugins::LogWarning("WADO-URI support is disabled");
      }
    }
    catch (Orthanc::OrthancException& e)
    {
      OrthancPlugins::LogError("Exception while initializing the DICOMweb plugin: " + 
                               std::string(e.What()));
      return -1;
    }
    catch (...)
    {
      OrthancPlugins::LogError("Exception while initializing the DICOMweb plugin");
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
