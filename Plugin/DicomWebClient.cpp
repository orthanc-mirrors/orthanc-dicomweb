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

#include <json/reader.h>
#include <list>
#include <set>
#include <boost/lexical_cast.hpp>

#include <Core/ChunkedBuffer.h>
#include <Core/Toolbox.h>


#include <boost/thread.hpp>


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
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_NetworkProtocol,
      "The STOW-RS JSON response from DICOMweb server " + server + 
      " does not contain the mandatory tag " + upper);
  }
  else
  {
    return false;
  }

  if (value->type() != Json::objectValue ||
      (value->isMember("Value") &&
       (*value) ["Value"].type() != Json::arrayValue))
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_NetworkProtocol,
      "Unable to parse STOW-RS JSON response from DICOMweb server " + server);
  }

  if (value->isMember("Value"))
  {
    result = (*value) ["Value"].size();
  }
  else
  {
    result = 0;
  }

  return true;
}



static void CheckStowAnswer(const Json::Value& response,
                            const std::string& serverName,
                            size_t instancesCount)
{
  if (response.type() != Json::objectValue ||
      !response.isMember("00081199"))
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_NetworkProtocol,
      "Unable to parse STOW-RS JSON response from DICOMweb server " + serverName);
  }

  size_t size;
  if (!GetSequenceSize(size, response, "00081199", true, serverName) ||
      size != instancesCount)
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_NetworkProtocol,
      "The STOW-RS server was only able to receive " + 
      boost::lexical_cast<std::string>(size) + " instances out of " +
      boost::lexical_cast<std::string>(instancesCount));
  }

  if (GetSequenceSize(size, response, "00081198", false, serverName) &&
      size != 0)
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_NetworkProtocol,
      "The response from the STOW-RS server contains " + 
      boost::lexical_cast<std::string>(size) + 
      " items in its Failed SOP Sequence (0008,1198) tag");
  }

  if (GetSequenceSize(size, response, "0008119A", false, serverName) &&
      size != 0)
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_NetworkProtocol,
      "The response from the STOW-RS server contains " + 
      boost::lexical_cast<std::string>(size) + 
      " items in its Other Failures Sequence (0008,119A) tag");
  }
}


static void ParseStowRequest(std::list<std::string>& instances /* out */,
                             std::map<std::string, std::string>& httpHeaders /* out */,
                             const OrthancPluginHttpRequest* request /* in */)
{
  static const char* RESOURCES = "Resources";
  static const char* HTTP_HEADERS = "HttpHeaders";

  Json::Value body;
  OrthancPlugins::ParseJsonBody(body, request);

  if (body.type() != Json::objectValue ||
      !body.isMember(RESOURCES) ||
      body[RESOURCES].type() != Json::arrayValue)
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_BadFileFormat,
      "A request to the DICOMweb STOW-RS client must provide a JSON object "
      "with the field \"" + std::string(RESOURCES) + 
      "\" containing an array of resources to be sent");
  }

  OrthancPlugins::ParseAssociativeArray(httpHeaders, body, HTTP_HEADERS);

  Json::Value& resources = body[RESOURCES];

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

    // Test whether this resource is an instance
    Json::Value tmp;
    if (OrthancPlugins::RestApiGet(tmp, "/instances/" + resource, false))
    {
      AddInstance(instances, tmp);
    }
    // This was not an instance, successively try with series/studies/patients
    else if ((OrthancPlugins::RestApiGet(tmp, "/series/" + resource, false) &&
              OrthancPlugins::RestApiGet(tmp, "/series/" + resource + "/instances", false)) ||
             (OrthancPlugins::RestApiGet(tmp, "/studies/" + resource, false) &&
              OrthancPlugins::RestApiGet(tmp, "/studies/" + resource + "/instances", false)) ||
             (OrthancPlugins::RestApiGet(tmp, "/patients/" + resource, false) &&
              OrthancPlugins::RestApiGet(tmp, "/patients/" + resource + "/instances", false)))
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
                           size_t& instancesCount,
                           bool force)
{
  unsigned int maxInstances = OrthancPlugins::Configuration::GetUnsignedIntegerValue("StowMaxInstances", 10);
  size_t maxSize = static_cast<size_t>(OrthancPlugins::Configuration::GetUnsignedIntegerValue("StowMaxSize", 10)) * 1024 * 1024;

  if ((force && instancesCount > 0) ||
      (maxInstances != 0 && instancesCount >= maxInstances) ||
      (maxSize != 0 && chunks.GetNumBytes() >= maxSize))
  {
    chunks.AddChunk("\r\n--" + boundary + "--\r\n");

    std::string body;
    chunks.Flatten(body);

    OrthancPlugins::MemoryBuffer answerBody;
    std::map<std::string, std::string> answerHeaders;

    OrthancPlugins::CallServer(answerBody, answerHeaders, server, OrthancPluginHttpMethod_Post,
                               httpHeaders, "studies", body);

    Json::Value response;
    Json::Reader reader;
    bool success = reader.parse(reinterpret_cast<const char*>((*answerBody)->data),
                                reinterpret_cast<const char*>((*answerBody)->data) + (*answerBody)->size, response);
    answerBody.Clear();

    if (!success)
    {
      throw Orthanc::OrthancException(
        Orthanc::ErrorCode_NetworkProtocol,
        "Unable to parse STOW-RS JSON response from DICOMweb server " + server.GetUrl());
    }
    else
    {
      CheckStowAnswer(response, server.GetUrl(), instancesCount);
    }

    instancesCount = 0;
  }
}



class StowClientJob : public OrthancPlugins::OrthancJob
{
private:
  enum State
  {
    State_Running,
    State_Paused,
    State_Error,
    State_Done
  };


  boost::mutex                               mutex_;
  std::string                                serverName_;
  std::vector<std::string>                   instances_;
  OrthancPlugins::HttpClient::HttpHeaders    headers_;
  std::string                                boundary_;


  std::auto_ptr<boost::thread>  worker_;


  State   state_;
  size_t  position_;
  Json::Value    content_;


  bool ReadNextInstance(std::string& dicom)
  {
    boost::mutex::scoped_lock lock(mutex_);

    if (state_ != State_Running)
    {
      return false;
    }

    while (position_ < instances_.size())
    {
      size_t i = position_++;

      if (OrthancPlugins::RestApiGetString(dicom, "/instances/" + instances_[i] + "/file", false))
      {
        return true;
      }
    }

    return false;
  }


  class RequestBody : public OrthancPlugins::HttpClient::IRequestBody
  {
  private:
    StowClientJob&  that_;
    std::string     boundary_;
    bool            done_;

  public:
    RequestBody(StowClientJob& that) :
      that_(that),
      boundary_(that.boundary_),
      done_(false)
    {
    }

    virtual bool ReadNextChunk(std::string& chunk)
    {
      if (done_)
      {
        return false;
      }
      else
      {
        std::string dicom;

        if (that_.ReadNextInstance(dicom))
        {
          chunk = ("--" + boundary_ + "\r\n" +
                   "Content-Type: application/dicom\r\n" +
                   "Content-Length: " + boost::lexical_cast<std::string>(dicom.size()) + 
                   "\r\n\r\n" + dicom + "\r\n");
        }
        else
        {
          done_ = true;
          chunk = ("--" + boundary_ + "--");
        }

        //boost::this_thread::sleep(boost::posix_time::seconds(1));

        return true;
      }
    }
  };


  static void Worker(StowClientJob* that)
  {
    try
    {
      std::string serverName;
      size_t startPosition;

      // The lifetime of "body" should be larger than "client"
      std::auto_ptr<RequestBody> body;
      std::auto_ptr<OrthancPlugins::HttpClient> client;

      {
        boost::mutex::scoped_lock lock(that->mutex_);
        serverName = that->serverName_;
        startPosition = that->position_;

        body.reset(new RequestBody(*that));

        client.reset(new OrthancPlugins::HttpClient);
        OrthancPlugins::DicomWebServers::GetInstance().ConfigureHttpClient(*client, that->serverName_, "/studies");
        client->SetMethod(OrthancPluginHttpMethod_Post);
        client->AddHeaders(that->headers_);
      }

      OrthancPlugins::HttpClient::HttpHeaders answerHeaders;
      Json::Value answerBody;

      client->SetBody(*body);
      client->Execute(answerHeaders, answerBody);

      size_t endPosition;

      {
        boost::mutex::scoped_lock lock(that->mutex_);
        endPosition = that->position_;
      }

      CheckStowAnswer(answerBody, serverName, endPosition - startPosition);
    }
    catch (Orthanc::OrthancException& e)
    {
      {
        boost::mutex::scoped_lock lock(that->mutex_);
        LOG(ERROR) << "Error in STOW-RS client job to server " << that->serverName_ << ": " << e.What();
        that->state_ = State_Error;
      }

      that->SetContent("Error", e.What());
    }
  }


  void SetContent(const std::string& key,
                  const std::string& value)
  {
    boost::mutex::scoped_lock lock(mutex_);
    content_[key] = value;
    UpdateContent(content_);
  }


  void StopWorker()
  {
    if (worker_.get() != NULL)
    {
      if (worker_->joinable())
      {
        worker_->join();
      }

      worker_.reset();
    }
  }

  
public:
  StowClientJob(const std::string& serverName,
                const std::list<std::string>& instances,
                const OrthancPlugins::HttpClient::HttpHeaders& headers) :
    OrthancJob("DicomWebStowClient"),
    serverName_(serverName),
    headers_(headers),
    state_(State_Running),
    position_(0),
    content_(Json::objectValue)
  {
    instances_.reserve(instances.size());

    for (std::list<std::string>::const_iterator
           it = instances.begin(); it != instances.end(); ++it)
    {
      instances_.push_back(*it);
    }

    {
      OrthancPlugins::OrthancString tmp;
      tmp.Assign(OrthancPluginGenerateUuid(OrthancPlugins::GetGlobalContext()));
      tmp.ToString(boundary_);
    }

    boundary_ = (boundary_ + "-" + boundary_);  // Make the boundary longer

    headers_["Accept"] = "application/dicom+json";
    headers_["Expect"] = "";
    headers_["Content-Type"] = "multipart/related; type=\"application/dicom\"; boundary=" + boundary_;
  }


  virtual OrthancPluginJobStepStatus Step()
  {
    State state;

    {
      boost::mutex::scoped_lock lock(mutex_);

      if (state_ == State_Paused)
      {
        state_ = State_Running;
      }

      UpdateProgress(instances_.empty() ? 1 :
                     static_cast<float>(position_) / static_cast<float>(instances_.size()));

      if (position_ == instances_.size() &&
          state_ == State_Running)
      {
        state_ = State_Done;
      }

      state = state_;
    }

    switch (state)
    {
      case State_Done:
        StopWorker();
        return (state_ == State_Done ? 
                OrthancPluginJobStepStatus_Success :
                OrthancPluginJobStepStatus_Failure);

      case State_Error:
        StopWorker();
        return OrthancPluginJobStepStatus_Failure;

      case State_Running:
        if (worker_.get() == NULL)
        {
          worker_.reset(new boost::thread(Worker, this));
        }
        
        boost::this_thread::sleep(boost::posix_time::milliseconds(500));
        
        return OrthancPluginJobStepStatus_Continue;

      default:
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
    }
  }


  virtual void Stop(OrthancPluginJobStopReason reason)
  {
    {
      boost::mutex::scoped_lock lock(mutex_);
      state_ = State_Paused;
    }

    StopWorker();
  }

    
  virtual void Reset()
  {
    boost::mutex::scoped_lock lock(mutex_);
    position_ = 0;
    state_ = State_Running;
    content_ = Json::objectValue;
    ClearContent();
  }
};



void StowClient(OrthancPluginRestOutput* output,
                const char* /*url*/,
                const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->groupsCount != 1)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest);
  }

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "POST");
    return;
  }

  std::string serverName(request->groups[0]);

#if 1
  std::list<std::string> instances;
  std::map<std::string, std::string> httpHeaders;
  ParseStowRequest(instances, httpHeaders, request);

  OrthancPlugins::LogInfo("Sending " + boost::lexical_cast<std::string>(instances.size()) +
                          " instances using STOW-RS to DICOMweb server: " + serverName);

  std::string jobId = OrthancPlugins::OrthancJob::Submit(new StowClientJob(serverName, instances, httpHeaders),
                                                         0 /* TODO priority, synchronous */);

  Json::Value answer = Json::objectValue;
  answer["ID"] = jobId;
  answer["Path"] = OrthancPlugins::RemoveMultipleSlashes
    ("../../" + OrthancPlugins::Configuration::GetOrthancApiRoot() + "/jobs/" + jobId);
  
#else
  std::string boundary;
  
  {
    OrthancPlugins::OrthancString tmp;
    tmp.Assign(OrthancPluginGenerateUuid(context));
    tmp.ToString(boundary);
  }

  boundary = (boundary + "-" + boundary);

  std::map<std::string, std::string> httpHeaders;
  httpHeaders["Accept"] = "application/dicom+json";
  httpHeaders["Expect"] = "";
  httpHeaders["Content-Type"] = "multipart/related; type=\"application/dicom\"; boundary=" + boundary;

  std::list<std::string> instances;
  ParseStowRequest(instances, httpHeaders, request);

  OrthancPlugins::LogInfo("Sending " + boost::lexical_cast<std::string>(instances.size()) +
                          " instances using STOW-RS to DICOMweb server: " + serverName);

  Orthanc::WebServiceParameters server(OrthancPlugins::DicomWebServers::GetInstance().GetServer(serverName));

  Orthanc::ChunkedBuffer chunks;
  size_t instancesCount = 0;

  for (std::list<std::string>::const_iterator it = instances.begin(); it != instances.end(); ++it)
  {
    OrthancPlugins::MemoryBuffer dicom;
    if (dicom.RestApiGet("/instances/" + *it + "/file", false))
    {
      chunks.AddChunk("\r\n--" + boundary + "\r\n" +
                      "Content-Type: application/dicom\r\n" +
                      "Content-Length: " + boost::lexical_cast<std::string>(dicom.GetSize()) +
                      "\r\n\r\n");
      chunks.AddChunk(dicom.GetData(), dicom.GetSize());
      instancesCount ++;

      SendStowChunks(server, httpHeaders, boundary, chunks, instancesCount, false);
    }
  }

  SendStowChunks(server, httpHeaders, boundary, chunks, instancesCount, true);

  Json::Value answer = Json::objectValue;
#endif

  std::string tmp = answer.toStyledString();
  OrthancPluginAnswerBuffer(context, output, tmp.c_str(), tmp.size(), "application/json");
}


static bool GetStringValue(std::string& target,
                           const Json::Value& json,
                           const std::string& key)
{
  if (json.type() != Json::objectValue)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
  }
  else if (!json.isMember(key))
  {
    target.clear();
    return false;
  }
  else if (json[key].type() != Json::stringValue)
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_BadFileFormat,
      "The field \"" + key + "\" in a JSON object should be a string");
  }
  else
  {
    target = json[key].asString();
    return true;
  }
}



static void ConfigureGetFromServer(OrthancPlugins::HttpClient& client,
                                   const OrthancPluginHttpRequest* request)
{
  static const char* URI = "Uri";
  static const char* HTTP_HEADERS = "HttpHeaders";
  static const char* GET_ARGUMENTS = "Arguments";

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
  }

  Json::Value body;
  OrthancPlugins::ParseJsonBody(body, request);

  std::string tmp;
  if (body.type() != Json::objectValue ||
      !GetStringValue(tmp, body, URI))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat,
                                    "A request to the DICOMweb client must provide a JSON object "
                                    "with the field \"Uri\" containing the URI of interest");
  }

  std::map<std::string, std::string> getArguments;
  OrthancPlugins::ParseAssociativeArray(getArguments, body, GET_ARGUMENTS);

  std::string uri;
  OrthancPlugins::DicomWebServers::UriEncode(uri, tmp, getArguments);

  OrthancPlugins::DicomWebServers::GetInstance().ConfigureHttpClient(client, request->groups[0], uri);

  std::map<std::string, std::string> additionalHeaders;
  OrthancPlugins::ParseAssociativeArray(additionalHeaders, body, HTTP_HEADERS);
  client.AddHeaders(additionalHeaders);
}



void GetFromServer(OrthancPluginRestOutput* output,
                   const char* /*url*/,
                   const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "POST");
    return;
  }

  OrthancPlugins::HttpClient client;
  ConfigureGetFromServer(client, request);
  
  std::map<std::string, std::string> answerHeaders;
  std::string answer;
  client.Execute(answerHeaders, answer);

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
    else if (key == "transfer-encoding" ||
             key == "content-length" ||
             key == "connection")
    {
      // Do not forward these headers
    }
    else
    {
      OrthancPluginSetHttpHeader(context, output, it->first.c_str(), it->second.c_str());
    }
  }

  OrthancPluginAnswerBuffer(context, output, answer.empty() ? NULL : answer.c_str(), 
                            answer.size(), contentType.c_str());
}


void GetFromServer(Json::Value& result,
                   const OrthancPluginHttpRequest* request)
{
  OrthancPlugins::HttpClient client;
  ConfigureGetFromServer(client, request);

  std::map<std::string, std::string> answerHeaders;
  client.Execute(answerHeaders, result);
}




static void RetrieveFromServerInternal(std::set<std::string>& instances,
                                       const Orthanc::WebServiceParameters& server,
                                       const std::map<std::string, std::string>& httpHeaders,
                                       const std::map<std::string, std::string>& getArguments,
                                       const Json::Value& resource)
{
  static const std::string STUDY = "Study";
  static const std::string SERIES = "Series";
  static const std::string INSTANCE = "Instance";
  static const std::string MULTIPART_RELATED = "multipart/related";
  static const std::string APPLICATION_DICOM = "application/dicom";

  if (resource.type() != Json::objectValue)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat,
                                    "Resources of interest for the DICOMweb WADO-RS Retrieve client "
                                    "must be provided as a JSON object");
  }

  std::string study, series, instance;
  if (!GetStringValue(study, resource, STUDY) ||
      study.empty())
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat,
                                    "A non-empty \"" + STUDY + "\" field is mandatory for the "
                                    "DICOMweb WADO-RS Retrieve client");
  }

  GetStringValue(series, resource, SERIES);
  GetStringValue(instance, resource, INSTANCE);

  if (series.empty() && 
      !instance.empty())
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_BadFileFormat,
      "When specifying a \"" + INSTANCE + "\" field in a call to DICOMweb "
      "WADO-RS Retrieve client, the \"" + SERIES + "\" field is mandatory");
  }

  std::string tmpUri = "studies/" + study;
  if (!series.empty())
  {
    tmpUri += "/series/" + series;
    if (!instance.empty())
    {
      tmpUri += "/instances/" + instance;
    }
  }

  std::string uri;
  OrthancPlugins::DicomWebServers::UriEncode(uri, tmpUri, getArguments);

  OrthancPlugins::MemoryBuffer answerBody;
  std::map<std::string, std::string> answerHeaders;
  OrthancPlugins::CallServer(answerBody, answerHeaders, server, OrthancPluginHttpMethod_Get, httpHeaders, uri, "");

  std::string contentTypeFull;
  std::vector<std::string> contentType;
  for (std::map<std::string, std::string>::const_iterator 
         it = answerHeaders.begin(); it != answerHeaders.end(); ++it)
  {
    std::string s = Orthanc::Toolbox::StripSpaces(it->first);
    Orthanc::Toolbox::ToLowerCase(s);
    if (s == "content-type")
    {
      contentTypeFull = it->second;
      Orthanc::Toolbox::TokenizeString(contentType, it->second, ';');
      break;
    }
  }

  OrthancPlugins::LogInfo("Got " + boost::lexical_cast<std::string>(answerBody.GetSize()) +
                          " bytes from a WADO-RS query with content type: " + contentTypeFull);
  
  if (contentType.empty())
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol,
                                    "No Content-Type provided by the remote WADO-RS server");
  }

  Orthanc::Toolbox::ToLowerCase(contentType[0]);
  if (Orthanc::Toolbox::StripSpaces(contentType[0]) != MULTIPART_RELATED)
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_NetworkProtocol,
      "The remote WADO-RS server answers with a \"" + contentType[0] +
      "\" Content-Type, but \"" + MULTIPART_RELATED + "\" is expected");
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

        // This covers the case where the content-type is quoted,
        // which COULD be the case 
        // cf. https://tools.ietf.org/html/rfc7231#section-3.1.1.1
        size_t len = type.length();
        if (len >= 2 &&
            type[0] == '"' &&
            type[len - 1] == '"')
        {
          type = type.substr(1, len - 2);
        }
        
        Orthanc::Toolbox::ToLowerCase(type);
      }
      else if (s == "boundary")
      {
        boundary = Orthanc::Toolbox::StripSpaces(tokens[1]);
      }
    }
  }

  // Strip the trailing and heading quotes if present
  if (boundary.length() > 2 &&
      boundary[0] == '"' &&
      boundary[boundary.size() - 1] == '"')
  {
    boundary = boundary.substr(1, boundary.size() - 2);
  }

  OrthancPlugins::LogInfo("  Parsing the multipart content type: " + type +
                          " with boundary: " + boundary);

  if (type != APPLICATION_DICOM)
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_NetworkProtocol,
      "The remote WADO-RS server answers with a \"" + type +
      "\" multipart Content-Type, but \"" + APPLICATION_DICOM + "\" is expected");
  }

  if (boundary.empty())
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_NetworkProtocol,
      "The remote WADO-RS server does not provide a boundary for its multipart answer");
  }

  std::vector<OrthancPlugins::MultipartItem> parts;
  OrthancPlugins::ParseMultipartBody(parts, 
                                     reinterpret_cast<const char*>(answerBody.GetData()),
                                     answerBody.GetSize(), boundary);

  OrthancPlugins::LogInfo("The remote WADO-RS server has provided " +
                          boost::lexical_cast<std::string>(parts.size()) + 
                          " DICOM instances");

  for (size_t i = 0; i < parts.size(); i++)
  {
    std::vector<std::string> tokens;
    Orthanc::Toolbox::TokenizeString(tokens, parts[i].contentType_, ';');

    std::string partType;
    if (tokens.size() > 0)
    {
      partType = Orthanc::Toolbox::StripSpaces(tokens[0]);
    }

    if (partType != APPLICATION_DICOM)
    {
      throw Orthanc::OrthancException(
        Orthanc::ErrorCode_NetworkProtocol,
        "The remote WADO-RS server has provided a non-DICOM file in its multipart answer"
        " (content type: " + parts[i].contentType_ + ")");
    }

    OrthancPlugins::MemoryBuffer tmp;
    tmp.RestApiPost("/instances", parts[i].data_, parts[i].size_, false);

    Json::Value result;
    tmp.ToJson(result);

    if (result.type() != Json::objectValue ||
        !result.isMember("ID") ||
        result["ID"].type() != Json::stringValue)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);      
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
  static const std::string GET_ARGUMENTS = "Arguments";

  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "POST");
    return;
  }

  Orthanc::WebServiceParameters server(OrthancPlugins::DicomWebServers::GetInstance().GetServer(request->groups[0]));

  Json::Value body;
  OrthancPlugins::ParseJsonBody(body, request);

  if (body.type() != Json::objectValue ||
      !body.isMember(RESOURCES) ||
      body[RESOURCES].type() != Json::arrayValue)
  {
    throw Orthanc::OrthancException(
      Orthanc::ErrorCode_BadFileFormat,
      "A request to the DICOMweb WADO-RS Retrieve client must provide a JSON object "
      "with the field \"" + RESOURCES + "\" containing an array of resources");
  }

  std::map<std::string, std::string> httpHeaders;
  OrthancPlugins::ParseAssociativeArray(httpHeaders, body, HTTP_HEADERS);

  std::map<std::string, std::string> getArguments;
  OrthancPlugins::ParseAssociativeArray(getArguments, body, GET_ARGUMENTS);


  std::set<std::string> instances;
  for (Json::Value::ArrayIndex i = 0; i < body[RESOURCES].size(); i++)
  {
    RetrieveFromServerInternal(instances, server, httpHeaders, getArguments, body[RESOURCES][i]);
  }

  Json::Value status = Json::objectValue;
  status["Instances"] = Json::arrayValue;
  
  for (std::set<std::string>::const_iterator
         it = instances.begin(); it != instances.end(); ++it)
  {
    status["Instances"].append(*it);
  }

  std::string s = status.toStyledString();
  OrthancPluginAnswerBuffer(context, output, s.c_str(), s.size(), "application/json");
}
