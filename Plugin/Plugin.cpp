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
#include <Core/SystemToolbox.h>
#include <Core/Toolbox.h>

#include <EmbeddedResources.h>


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
        server.FormatPublic(jsonServer);
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

  switch (request->method)
  {
    case OrthancPluginHttpMethod_Get:
    {
      // Make sure the server does exist
      const Orthanc::WebServiceParameters& server = 
        OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]);

      Json::Value json = Json::arrayValue;
      json.append("get");
      json.append("retrieve");
      json.append("stow");
      json.append("qido");

      std::string value;
      if (server.LookupUserProperty(value, "HasDelete") &&
          value == "1")
      {
        json.append("delete");
      }

      std::string answer = json.toStyledString(); 
      OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), "application/json");
      break;
    }
    
    case OrthancPluginHttpMethod_Delete:
    {
      OrthancPlugins::DicomWebServers::GetInstance().DeleteServer(request->groups[0]);
      std::string answer = "{}";
      OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), "application/json");
      break;
    }
    
    case OrthancPluginHttpMethod_Put:
    {
      Json::Value body;
      OrthancPlugins::ParseJsonBody(body, request);
      
      Orthanc::WebServiceParameters parameters(body);
      
      OrthancPlugins::DicomWebServers::GetInstance().SetServer(request->groups[0], parameters);
      std::string answer = "{}";
      OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), "application/json");
      break;
    }
    
    default:
      OrthancPluginSendMethodNotAllowed(context, output, "GET,PUT,DELETE");
      break;
  }
}



void GetClientInformation(OrthancPluginRestOutput* output,
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
    std::string root = OrthancPlugins::Configuration::GetRoot();
    std::vector<std::string> tokens;
    Orthanc::Toolbox::TokenizeString(tokens, root, '/');
    int depth = 0;
    for (size_t i = 0; i < tokens.size(); i++)
    {
      if (tokens[i].empty() ||
          tokens[i] == ".")
      {
        // Don't change the depth
      }
      else if (tokens[i] == "..")
      {
        depth--;
      }
      else
      {
        depth++;
      }
    }

    std::string orthancRoot = "./";
    for (int i = 0; i < depth; i++)
    {
      orthancRoot += "../";
    }

    Json::Value info = Json::objectValue;
    info["DicomWebRoot"] = root;
    info["OrthancRoot"] = orthancRoot;

    std::string answer = info.toStyledString();
    OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), "application/json");
  }
}



void QidoClient(OrthancPluginRestOutput* output,
                const char* /*url*/,
                const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "POST");
  }
  else
  {
    Json::Value answer;
    GetFromServer(answer, request);
    
    if (answer.type() != Json::arrayValue)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
    }

    Json::Value result = Json::arrayValue;
    for (Json::Value::ArrayIndex i = 0; i < answer.size(); i++)
    {
      if (answer[i].type() != Json::objectValue)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
      }

      Json::Value::Members tags = answer[i].getMemberNames();

      Json::Value item = Json::objectValue;
      
      for (size_t j = 0; j < tags.size(); j++)
      {
        Orthanc::DicomTag tag(0, 0);
        if (Orthanc::DicomTag::ParseHexadecimal(tag, tags[j].c_str()))
        {
          Json::Value value = Json::objectValue;
          value["Group"] = tag.GetGroup();
          value["Element"] = tag.GetElement();
          
#if ORTHANC_PLUGINS_VERSION_IS_ABOVE(1, 5, 7)
          OrthancPlugins::OrthancString name;

          name.Assign(OrthancPluginGetTagName(context, tag.GetGroup(), tag.GetElement(), NULL));
          if (name.GetContent() != NULL)
          {
            value["Name"] = std::string(name.GetContent());
          }
#endif

          const Json::Value& source = answer[i][tags[j]];
          if (source.type() != Json::objectValue ||
              !source.isMember("vr") ||
              source["vr"].type() != Json::stringValue)
          {
            throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
          }

          value["vr"] = source["vr"].asString();

          if (source.isMember("Value") &&
              source["Value"].type() == Json::arrayValue &&
              source["Value"].size() >= 1)
          {
            const Json::Value& content = source["Value"][0];

            switch (content.type())
            {
              case Json::stringValue:
                value["Value"] = content.asString();
                break;

              case Json::objectValue:
                if (content.isMember("Alphabetic") &&
                    content["Alphabetic"].type() == Json::stringValue)
                {
                  value["Value"] = content["Alphabetic"].asString();
                }
                break;

              default:
                break;
            }
          }

          item[tags[j]] = value;
        }
      }

      result.append(item);
    }

    std::string tmp = result.toStyledString();
    OrthancPluginAnswerBuffer(context, output, tmp.c_str(), tmp.size(), "application/json");
  }
}


void DeleteClient(OrthancPluginRestOutput* output,
                const char* /*url*/,
                const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "POST");
  }
  else
  {
    static const char* const LEVEL = "Level";
    static const char* const HAS_DELETE = "HasDelete";
    static const char* const SERIES_INSTANCE_UID = "SeriesInstanceUID";
    static const char* const STUDY_INSTANCE_UID = "StudyInstanceUID";
    static const char* const SOP_INSTANCE_UID = "SOPInstanceUID";

    const std::string serverName = request->groups[0];

    const Orthanc::WebServiceParameters& server = 
      OrthancPlugins::DicomWebServers::GetInstance().GetServer(serverName);

    std::string value;
    if (server.LookupUserProperty(value, HAS_DELETE) &&
        value != "1")
    {
      throw Orthanc::OrthancException(
        Orthanc::ErrorCode_BadFileFormat,
        "Cannot delete on DICOMweb server, check out property \"HasDelete\": " + serverName);
    }

    Json::Value body;
    OrthancPlugins::ParseJsonBody(body, request);

    if (body.type() != Json::objectValue ||
        !body.isMember(LEVEL) ||
        !body.isMember(STUDY_INSTANCE_UID) ||
        body[LEVEL].type() != Json::stringValue ||
        body[STUDY_INSTANCE_UID].type() != Json::stringValue)
    {
      throw Orthanc::OrthancException(
        Orthanc::ErrorCode_BadFileFormat,
        "The request body must contain a JSON object with fields \"Level\" and \"StudyInstanceUID\"");
    }

    Orthanc::ResourceType level = Orthanc::StringToResourceType(body[LEVEL].asCString());

    const std::string study = body[STUDY_INSTANCE_UID].asString();

    std::string series;    
    if (level == Orthanc::ResourceType_Series ||
        level == Orthanc::ResourceType_Instance)
    {
      if (!body.isMember(SERIES_INSTANCE_UID) ||
          body[SERIES_INSTANCE_UID].type() != Json::stringValue)
      {
        throw Orthanc::OrthancException(
          Orthanc::ErrorCode_BadFileFormat,
          "The request body must contain the field \"SeriesInstanceUID\"");
      }
      else
      {
        series = body[SERIES_INSTANCE_UID].asString();
      }
    }

    std::string instance;    
    if (level == Orthanc::ResourceType_Instance)
    {
      if (!body.isMember(SOP_INSTANCE_UID) ||
          body[SOP_INSTANCE_UID].type() != Json::stringValue)
      {
        throw Orthanc::OrthancException(
          Orthanc::ErrorCode_BadFileFormat,
          "The request body must contain the field \"SOPInstanceUID\"");
      }
      else
      {
        instance = body[SOP_INSTANCE_UID].asString();
      }
    }

    std::string uri;
    switch (level)
    {
      case Orthanc::ResourceType_Study:
        uri = "/studies/" + study;
        break;

      case Orthanc::ResourceType_Series:
        uri = "/studies/" + study + "/series/" + series;
        break;

      case Orthanc::ResourceType_Instance:
        uri = "/studies/" + study + "/series/" + series + "/instances/" + instance;
        break;

      default:
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
    }

    OrthancPlugins::HttpClient client;
    OrthancPlugins::DicomWebServers::GetInstance().ConfigureHttpClient(client, serverName, uri);
    client.SetMethod(OrthancPluginHttpMethod_Delete);
    client.Execute();

    std::string tmp = "{}";
    OrthancPluginAnswerBuffer(context, output, tmp.c_str(), tmp.size(), "application/json");
  }
}



void RetrieveInstanceRendered(OrthancPluginRestOutput* output,
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
    Orthanc::MimeType mime = Orthanc::MimeType_Jpeg;
    
    std::string publicId;
    if (LocateInstance(output, publicId, request))
    {
      std::map<std::string, std::string> headers;
      headers["Accept"] = Orthanc::EnumerationToString(mime);
      
      OrthancPlugins::MemoryBuffer buffer;
      if (buffer.RestApiGet("/instances/" + publicId + "/preview", headers, false))
      {
        OrthancPluginAnswerBuffer(context, output, buffer.GetData(),
                                  buffer.GetSize(), Orthanc::EnumerationToString(mime));
      }
    }
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

class StowClientBody : public OrthancPlugins::HttpClient::IRequestBody
{
private:
  std::vector<std::string>  files_;
  size_t                    position_;
  std::string               boundary_;

public:
  StowClientBody() :
    position_(0),
    boundary_(Orthanc::Toolbox::GenerateUuid() + "-" + Orthanc::Toolbox::GenerateUuid())
  {
    //boost::filesystem::path p("/home/jodogne/DICOM/Demo/KNIX/Knee (R)/AX.  FSE PD - 5");
    boost::filesystem::path p("/tmp/dicom");

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
#if 0
      {
        StowClientBody stow;
      
        OrthancPlugins::HttpClient client;
        client.SetUrl("http://localhost:8080/dicom-web/studies");
        client.SetMethod(OrthancPluginHttpMethod_Post);
        client.AddHeader("Accept", "application/dicom+json");
        client.AddHeader("Expect", "");
        client.AddHeader("Content-Type", "multipart/related; type=application/dicom; boundary=" + stow.GetBoundary());
        client.SetTimeout(120);
        client.SetBody(stow);

        OrthancPlugins::HttpClient::HttpHeaders headers;
        std::string answer;
        client.Execute(headers, answer);

        Json::Value v;
        Json::Reader reader;
        if (reader.parse(answer, v))
        {
          std::cout << v["00081190"].toStyledString() << std::endl;
        }
        else
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
        }
      }
#endif

#if 0
      {
        OrthancPlugins::HttpClient client;
        OrthancPlugins::DicomWebServers::GetInstance().ConfigureHttpClient(client, "google", "/studies");

        OrthancPlugins::HttpClient::HttpHeaders headers;
        Json::Value body;
        client.Execute(headers, body);

        std::cout << body.toStyledString() << std::endl;
      }
#endif

#if 0
      {
        OrthancPlugins::HttpClient client;
        OrthancPlugins::DicomWebServers::GetInstance().ConfigureHttpClient(client, "self", "/studies");

        client.SetMethod(OrthancPluginHttpMethod_Post);
        client.AddHeader("Accept", "application/dicom+json");
        client.AddHeader("Expect", "");

        std::string boundary = Orthanc::Toolbox::GenerateUuid() + "-" + Orthanc::Toolbox::GenerateUuid();
        client.AddHeader("Content-Type", "multipart/related; type=application/dicom; boundary=" + boundary);

        std::string f;
        Orthanc::SystemToolbox::ReadFile(f, "/home/jodogne/Subversion/orthanc-tests/Database/Encodings/DavidClunie/SCSI2"); // Korean
        //Orthanc::SystemToolbox::ReadFile(f, "/home/jodogne/Subversion/orthanc-tests/Database/Encodings/DavidClunie/SCSH31"); // Kanji
        //Orthanc::SystemToolbox::ReadFile(f, "/home/jodogne/DICOM/Alain.dcm");
        
        std::string body;
        body += ("--" + boundary + "\r\nContent-Type: application/dicom\r\nContent-Length: " +
                 boost::lexical_cast<std::string>(f.size()) + "\r\n\r\n");
        body += f;
        body += "\r\n--" + boundary + "--";

        Orthanc::SystemToolbox::WriteFile(body, "/tmp/toto");
        
        client.SetBody(body);
        
        OrthancPlugins::HttpClient::HttpHeaders headers;
        Json::Value answer;
        client.Execute(headers, answer);

        std::cout << answer.toStyledString() << std::endl;
      }
#endif
    }
    catch (Orthanc::OrthancException& e)
    {
      LOG(ERROR) << "EXCEPTION: " << e.What();
    }
  }

  return OrthancPluginErrorCode_Success;
}


template <enum Orthanc::EmbeddedResources::DirectoryResourceId folder>
void ServeEmbeddedFolder(OrthancPluginRestOutput* output,
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
    std::string path = "/" + std::string(request->groups[0]);
    const char* mime = Orthanc::EnumerationToString(Orthanc::SystemToolbox::AutodetectMimeType(path));

    std::string s;
    Orthanc::EmbeddedResources::GetDirectoryResource(s, folder, path.c_str());

    const char* resource = s.size() ? s.c_str() : NULL;
    OrthancPluginAnswerBuffer(context, output, resource, s.size(), mime);
  }
}


void ServeDicomWebClient(OrthancPluginRestOutput* output,
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
    const std::string path = std::string(DICOMWEB_CLIENT_PATH) + std::string(request->groups[0]);
    const char* mime = Orthanc::EnumerationToString(Orthanc::SystemToolbox::AutodetectMimeType(path));

    OrthancPlugins::MemoryBuffer f;
    f.ReadFile(path);

    OrthancPluginAnswerBuffer(context, output, f.GetData(), f.GetSize(), mime);
  }
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

      OrthancPluginRegisterOnChangeCallback(context, OnChangeCallback);  // TODO => REMOVE

      // Initialize GDCM
      OrthancPlugins::GdcmParsedDicomFile::Initialize();

      // Configure the DICOMweb callbacks
      if (OrthancPlugins::Configuration::GetBooleanValue("Enable", true))
      {
        std::string root = OrthancPlugins::Configuration::GetRoot();
        assert(!root.empty() && root[root.size() - 1] == '/');

        OrthancPlugins::LogWarning("URI to the DICOMweb REST API: " + root);

        OrthancPlugins::ChunkedRestRegistration<
          SearchForStudies /* TODO => Rename as QIDO-RS */,
          OrthancPlugins::StowServer::PostCallback>::Apply(root + "studies");

        OrthancPlugins::ChunkedRestRegistration<
          RetrieveDicomStudy /* TODO => Rename as WADO-RS */,
          OrthancPlugins::StowServer::PostCallback>::Apply(root + "studies/([^/]*)");

        OrthancPlugins::RegisterRestCallback<SearchForInstances>(root + "instances", true);
        OrthancPlugins::RegisterRestCallback<SearchForSeries>(root + "series", true);    
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
        OrthancPlugins::RegisterRestCallback<QidoClient>(root + "servers/([^/]*)/qido", true);
        OrthancPlugins::RegisterRestCallback<DeleteClient>(root + "servers/([^/]*)/delete", true);

        OrthancPlugins::RegisterRestCallback
          <ServeEmbeddedFolder<Orthanc::EmbeddedResources::JAVASCRIPT_LIBS> >
          (root + "app/libs/(.*)", true);

        OrthancPlugins::RegisterRestCallback<ServeDicomWebClient>(root + "app/client/(.*)", true);
        OrthancPlugins::RegisterRestCallback<GetClientInformation>(root + "app/info", true);

        OrthancPlugins::RegisterRestCallback<RetrieveInstanceRendered>(root + "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/rendered", true);
        
        std::string uri = root + "app/client/index.html";
        OrthancPluginSetRootUri(context, uri.c_str());
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
