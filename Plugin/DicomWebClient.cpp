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

#include <Core/HttpServer/MultipartStreamReader.h>
#include <Core/ChunkedBuffer.h>
#include <Core/Toolbox.h>
#include <Plugins/Samples/Common/OrthancPluginCppWrapper.h>


#include <boost/thread.hpp>
#include <boost/algorithm/string/predicate.hpp>




class SingleFunctionJob : public OrthancPlugins::OrthancJob
{
public:
  class JobContext : public boost::noncopyable
  {
  private:
    SingleFunctionJob&  that_;

  public:
    JobContext(SingleFunctionJob& that) :
      that_(that)
    {
    }

    void SetContent(const std::string& key,
                    const std::string& value)
    {
      that_.SetContent(key, value);
    }

    void SetProgress(unsigned int position,
                     unsigned int maxPosition)
    {
      boost::mutex::scoped_lock lock(that_.mutex_);
      
      if (maxPosition == 0 || 
          position > maxPosition)
      {
        that_.UpdateProgress(1);
      }
      else
      {
        that_.UpdateProgress(static_cast<float>(position) / static_cast<float>(maxPosition));
      }
    }
  };


  class IFunction : public boost::noncopyable
  {
  public:
    virtual ~IFunction()
    {
    }

    // Must return "true" if the job has completed with success, or
    // "false" if the job has been canceled. Pausing the job
    // corresponds to canceling it.
    virtual bool Execute(JobContext& context) = 0;
  };


  class IFunctionFactory : public boost::noncopyable
  {
  public:
    virtual ~IFunctionFactory()
    {
    }

    // WARNING: "CancelFunction()" will be invoked while "Execute()"
    // is running. Mutex is probably necessary.
    virtual void CancelFunction() = 0;

    // Only called when no function is running, to deal with
    // "Resubmit()" after job cancelation/failure.
    virtual void ResetFunction() = 0;

    virtual IFunction* CreateFunction() = 0;
  };


protected:
  void SetFactory(IFunctionFactory& factory)
  {
    boost::mutex::scoped_lock lock(mutex_);

    if (state_ != State_Setup)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadSequenceOfCalls);
    }
    else
    {
      factory_ = &factory;
    }
  }
  

private:
  enum State
  {
    State_Setup,
    State_Running,
    State_Success,
    State_Failure
  };

  boost::mutex                  mutex_;
  State                         state_;  // Can only be modified by the "Worker()" function
  std::auto_ptr<boost::thread>  worker_;
  Json::Value                   content_;
  IFunctionFactory*             factory_;

  void JoinWorker()
  {
    assert(factory_ != NULL);

    if (worker_.get() != NULL)
    {
      if (worker_->joinable())
      {
        worker_->join();
      }

      worker_.reset();
    }
  }

  void StartWorker()
  {
    assert(factory_ != NULL);

    if (worker_.get() == NULL &&
        factory_ != NULL)
    {
      worker_.reset(new boost::thread(Worker, this, factory_));
    }
  }

  void SetContent(const std::string& key,
                  const std::string& value)
  {
    boost::mutex::scoped_lock lock(mutex_);
    content_[key] = value;
    UpdateContent(content_);
  }

  static void Worker(SingleFunctionJob* job,
                     IFunctionFactory* factory)
  {
    assert(job != NULL && factory != NULL);

    JobContext context(*job);

    {
      boost::mutex::scoped_lock lock(job->mutex_);
      job->state_ = State_Running;
    }

    try
    {
      std::auto_ptr<IFunction> function(factory->CreateFunction());
      bool success = function->Execute(context);

      {
        boost::mutex::scoped_lock lock(job->mutex_);
        job->state_ = (success ? State_Success : State_Failure);
        if (success)
        {
          job->UpdateProgress(1);
        }
      }
    }
    catch (Orthanc::OrthancException& e)
    {
      LOG(ERROR) << "Error in a job: " << e.What();

      {
        boost::mutex::scoped_lock lock(job->mutex_);
        job->state_ = State_Failure;
        job->content_["FunctionErrorCode"] = e.GetErrorCode();
        job->content_["FunctionErrorDescription"] = e.What();
        if (e.HasDetails())
        {
          job->content_["FunctionErrorDetails"] = e.GetDetails();
        }
        job->UpdateContent(job->content_);
      }
    }
  }  

public:
  SingleFunctionJob(const std::string& jobName) :
    OrthancJob(jobName),
    state_(State_Setup),
    content_(Json::objectValue),
    factory_(NULL)
  {
  }

  virtual ~SingleFunctionJob()
  {
    if (worker_.get() != NULL)
    {
      LOG(ERROR) << "Classes deriving from SingleFunctionJob must "
                 << "explicitly call Finalize() in their destructor";

      try
      {
        JoinWorker();
      }
      catch (Orthanc::OrthancException&)
      {
      }
    }
  }

  void Finalize()
  {
    try
    {
      if (factory_ != NULL)
      {
        factory_->CancelFunction();
        JoinWorker();
      }
    }
    catch (Orthanc::OrthancException&)
    {
    }
  }

  virtual OrthancPluginJobStepStatus Step()
  {
    if (factory_ == NULL)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadSequenceOfCalls);
    }

    State state;

    {
      boost::mutex::scoped_lock lock(mutex_);
      state = state_;
    }

    switch (state)
    {
      case State_Setup:
        StartWorker();
        break;

      case State_Running:
        break;

      case State_Success:
        JoinWorker();
        return OrthancPluginJobStepStatus_Success;

      case State_Failure:
        JoinWorker();
        return OrthancPluginJobStepStatus_Failure;

      default:
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
    }

    boost::this_thread::sleep(boost::posix_time::milliseconds(500));
    return OrthancPluginJobStepStatus_Continue;
  }

  virtual void Stop(OrthancPluginJobStopReason reason)
  {
    if (factory_ == NULL)
    {
      return;
    }

    if (reason == OrthancPluginJobStopReason_Paused ||
        reason == OrthancPluginJobStopReason_Canceled)
    {
      factory_->CancelFunction();
    }

    JoinWorker();

    if (reason == OrthancPluginJobStopReason_Paused)
    {
      // This type of job cannot be paused: Reset under the hood
      Reset();
    }
  }

  virtual void Reset()
  {
    boost::mutex::scoped_lock lock(mutex_);

    if (factory_ != NULL)
    {
      factory_->ResetFunction();
    }

    state_ = State_Setup;    

    content_ = Json::objectValue;
    ClearContent();
  }
};




static const std::string MULTIPART_RELATED = "multipart/related";



static void SubmitJob(OrthancPluginRestOutput* output,
                      OrthancPlugins::OrthancJob* job,
                      const Json::Value& body,
                      bool defaultSynchronous)
{
  std::auto_ptr<OrthancPlugins::OrthancJob> protection(job);

  bool synchronous;

  bool b;
  if (OrthancPlugins::LookupBooleanValue(b, body, "Synchronous"))
  {
    synchronous = b;
  }
  else if (OrthancPlugins::LookupBooleanValue(b, body, "Asynchronous"))
  {
    synchronous = !b;
  }
  else
  {
    synchronous = defaultSynchronous;
  }

  int priority;
  if (!OrthancPlugins::LookupIntegerValue(priority, body, "Priority"))
  {
    priority = 0;
  }

  Json::Value answer;

  if (synchronous)
  {
    OrthancPlugins::OrthancJob::SubmitAndWait(answer, protection.release(), priority);
  }
  else
  {
    std::string jobId = OrthancPlugins::OrthancJob::Submit(protection.release(), priority);

    answer = Json::objectValue;
    answer["ID"] = jobId;
    answer["Path"] = OrthancPlugins::RemoveMultipleSlashes
      ("../" + OrthancPlugins::Configuration::GetOrthancApiRoot() + "/jobs/" + jobId);
  }

  std::string s = answer.toStyledString();
  OrthancPluginAnswerBuffer(OrthancPlugins::GetGlobalContext(),
                            output, s.c_str(), s.size(), "application/json");    
}


static void AddInstance(std::list<std::string>& target,
                        const Json::Value& instance)
{
  std::string id;
  if (OrthancPlugins::LookupStringValue(id, instance, "ID"))
  {
    target.push_back(id);
  }
  else
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
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
                             const Json::Value& body /* in */)
{
  static const char* RESOURCES = "Resources";
  static const char* HTTP_HEADERS = "HttpHeaders";

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

  const Json::Value& resources = body[RESOURCES];

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


class StowClientJob : public OrthancPlugins::OrthancJob
{
private:
  enum State
  {
    State_Running,
    State_Stopped,
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

      if (state_ == State_Stopped)
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
      state_ = State_Stopped;
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

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "POST");
    return;
  }

  if (request->groupsCount != 1)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest);
  }

  std::string serverName(request->groups[0]);

  Json::Value body;
  OrthancPlugins::ParseJsonBody(body, request);

  std::list<std::string> instances;
  std::map<std::string, std::string> httpHeaders;
  ParseStowRequest(instances, httpHeaders, body);

  OrthancPlugins::LogInfo("Sending " + boost::lexical_cast<std::string>(instances.size()) +
                          " instances using STOW-RS to DICOMweb server: " + serverName);

  Json::Value answer;
  SubmitJob(output, new StowClientJob(serverName, instances, httpHeaders), body, 
            true /* synchronous by default, for compatibility with <= 0.6 */);
}



static void ParseGetFromServer(std::string& uri,
                               std::map<std::string, std::string>& additionalHeaders,
                               const Json::Value& resource)
{
  static const char* URI = "Uri";
  static const char* HTTP_HEADERS = "HttpHeaders";
  static const char* GET_ARGUMENTS = "Arguments";

  std::string tmp;
  if (resource.type() != Json::objectValue ||
      !OrthancPlugins::LookupStringValue(tmp, resource, URI))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat,
                                    "A request to the DICOMweb client must provide a JSON object "
                                    "with the field \"Uri\" containing the URI of interest");
  }

  std::map<std::string, std::string> getArguments;
  OrthancPlugins::ParseAssociativeArray(getArguments, resource, GET_ARGUMENTS); 
  OrthancPlugins::DicomWebServers::UriEncode(uri, tmp, getArguments);

  OrthancPlugins::ParseAssociativeArray(additionalHeaders, resource, HTTP_HEADERS);
}



static void ConfigureGetFromServer(OrthancPlugins::HttpClient& client,
                                   const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Post)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
  }

  Json::Value body;
  OrthancPlugins::ParseJsonBody(body, request);

  std::string uri;
  std::map<std::string, std::string> additionalHeaders;
  ParseGetFromServer(uri, additionalHeaders, body);

  OrthancPlugins::DicomWebServers::GetInstance().ConfigureHttpClient(client, request->groups[0], uri);
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





class WadoRetrieveAnswer : 
  public OrthancPlugins::HttpClient::IAnswer,
  private Orthanc::MultipartStreamReader::IHandler
{
private:
  enum State
  {
    State_Headers,
    State_Body,
    State_Canceled
  };
  
  boost::mutex                                   mutex_;
  State                                          state_;
  std::list<std::string>                         instances_;
  std::auto_ptr<Orthanc::MultipartStreamReader>  reader_;
  uint64_t                                       networkSize_;

  virtual void HandlePart(const Orthanc::MultipartStreamReader::HttpHeaders& headers,
                          const void* part,
                          size_t size)
  {
    std::string contentType;
    if (!Orthanc::MultipartStreamReader::GetMainContentType(contentType, headers))
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol,
                                      "Missing Content-Type for a part of WADO-RS answer");
    }

    size_t pos = contentType.find(';');
    if (pos != std::string::npos)
    {
      contentType = contentType.substr(0, pos);
    }

    contentType = Orthanc::Toolbox::StripSpaces(contentType);
    if (!boost::iequals(contentType, "application/dicom"))
    {
      throw Orthanc::OrthancException(
        Orthanc::ErrorCode_NetworkProtocol,
        "Parts of a WADO-RS retrieve should have \"application/dicom\" type, but received: " + contentType);
    }

    OrthancPlugins::MemoryBuffer tmp;
    tmp.RestApiPost("/instances", part, size, false);

    Json::Value result;
    tmp.ToJson(result);

    std::string id;
    if (OrthancPlugins::LookupStringValue(id, result, "ID"))
    {
      instances_.push_back(id);
    }
    else
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);      
    }
  }

public:
  WadoRetrieveAnswer() :
    state_(State_Headers),
    networkSize_(0)
  {
  }

  virtual ~WadoRetrieveAnswer()
  {
  }

  void Close()
  {
    boost::mutex::scoped_lock lock(mutex_);

    if (state_ != State_Canceled &&
        reader_.get() != NULL)
    {
      reader_->CloseStream();
    }
  }

  virtual void AddHeader(const std::string& key,
                         const std::string& value)
  {
    boost::mutex::scoped_lock lock(mutex_);

    if (state_ == State_Canceled)
    {
      return;
    }
    else if (state_ != State_Headers)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
    }

    if (boost::iequals(key, "Content-Type"))
    {
      if (reader_.get() != NULL)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol,
                                        "Received twice a Content-Type header in WADO-RS");
      }

      std::string contentType, subType, boundary;

      if (!Orthanc::MultipartStreamReader::ParseMultipartContentType
          (contentType, subType, boundary, value))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol,
                                        "Cannot parse the Content-Type for WADO-RS: " + value);
      }

      if (!boost::iequals(contentType, MULTIPART_RELATED))
      {
        throw Orthanc::OrthancException(
          Orthanc::ErrorCode_NetworkProtocol,
          "The remote WADO-RS server answers with a \"" + contentType +
          "\" Content-Type, but \"" + MULTIPART_RELATED + "\" is expected");
      }

      reader_.reset(new Orthanc::MultipartStreamReader(boundary));
      reader_->SetHandler(*this);
    }
  }

  virtual void AddChunk(const void* data,
                        size_t size)
  {
    boost::mutex::scoped_lock lock(mutex_);

    if (state_ == State_Canceled)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_CanceledJob);
    }
    else if (reader_.get() == NULL)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol,
                                      "No Content-Type provided by the remote WADO-RS server");
    }
    else
    {
      state_ = State_Body;
      networkSize_ += size;
      reader_->AddChunk(data, size);
    }
  }

  void GetReceivedInstances(std::list<std::string>& target)
  {
    boost::mutex::scoped_lock lock(mutex_);
    target = instances_;
  }

  void Cancel()
  {
    boost::mutex::scoped_lock lock(mutex_);
    LOG(ERROR) << "A WADO-RS retrieve job has been canceled, expect \"Error in the network protocol\" errors";
    state_ = State_Canceled;
  }

  uint64_t GetNetworkSize()
  {
    boost::mutex::scoped_lock lock(mutex_);
    return networkSize_;
  }
};





class WadoRetrieveJob : 
  public SingleFunctionJob,
  private SingleFunctionJob::IFunctionFactory
{
private:
  class Resource : public boost::noncopyable
  {
  private:
    std::string                        uri_;
    std::map<std::string, std::string> additionalHeaders_;

  public:
    Resource(const std::string& uri) :
      uri_(uri)
    {
    }

    Resource(const std::string& uri,
             const std::map<std::string, std::string>& additionalHeaders) :
      uri_(uri),
      additionalHeaders_(additionalHeaders)
    {
    }

    const std::string& GetUri() const
    {
      return uri_;
    }

    const std::map<std::string, std::string>& GetAdditionalHeaders() const
    {
      return additionalHeaders_;
    }
  };


  enum Status
  {
    Status_Done,
    Status_Canceled,
    Status_Continue
  };


  class F : public IFunction
  {
  private:
    WadoRetrieveJob&   that_;

  public:
    F(WadoRetrieveJob& that) :
    that_(that)
    {
    }

    virtual bool Execute(JobContext& context)
    {
      for (;;)
      {
        OrthancPlugins::HttpClient client;

        switch (that_.SetupNextResource(client))
        {
          case Status_Continue:
            client.Execute(*that_.answer_);
            that_.CloseResource(context);
            break;

          case Status_Canceled:
            return false;

          case Status_Done:
            return true;
        }
      }
    }
  };


  boost::mutex            mutex_;
  std::string             serverName_;
  size_t                  position_;
  std::vector<Resource*>  resources_;
  bool                    canceled_;
  std::list<std::string>  retrievedInstances_;
  std::auto_ptr<WadoRetrieveAnswer>  answer_;
  uint64_t                networkSize_;


  Status SetupNextResource(OrthancPlugins::HttpClient& client)
  {
    boost::mutex::scoped_lock lock(mutex_);

    if (canceled_)
    {
      return Status_Canceled;
    }
    else if (position_ == resources_.size())
    {
      return Status_Done;
    }
    else
    {
      answer_.reset(new WadoRetrieveAnswer);

      const Resource* resource = resources_[position_++];
      if (resource == NULL)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
      }

      OrthancPlugins::DicomWebServers::GetInstance().ConfigureHttpClient
        (client, serverName_, resource->GetUri());
      client.AddHeaders(resource->GetAdditionalHeaders());

      return Status_Continue;
    }
  }


  void CloseResource(JobContext& context)
  {
    boost::mutex::scoped_lock lock(mutex_);
    answer_->Close();

    std::list<std::string> instances;
    answer_->GetReceivedInstances(instances);
    networkSize_ += answer_->GetNetworkSize();

    answer_.reset();

    retrievedInstances_.splice(retrievedInstances_.end(), instances);

    context.SetProgress(position_, resources_.size());
    context.SetContent("NetworkUsageMB", boost::lexical_cast<std::string>
                       (networkSize_ / static_cast<uint64_t>(1024 * 1024)));
    context.SetContent("ReceivedInstancesCount", boost::lexical_cast<std::string>(retrievedInstances_.size()));
  }


  virtual void CancelFunction()
  {
    boost::mutex::scoped_lock lock(mutex_);
    canceled_ = true;
    if (answer_.get() != NULL)
    {
      answer_->Cancel();
    }
  }

  virtual void ResetFunction()
  {
    boost::mutex::scoped_lock lock(mutex_);
    canceled_ = false;
    position_ = 0;
    retrievedInstances_.clear();
  }

  virtual IFunction* CreateFunction()
  {
    return new F(*this);
  }

public:
  WadoRetrieveJob(const std::string& serverName) :
    SingleFunctionJob("DicomWebWadoRetrieveClient"),
    serverName_(serverName),
    position_(0),
    canceled_(false),
    networkSize_(0)
  {
    SetFactory(*this);
  }

  virtual ~WadoRetrieveJob()
  {
    SingleFunctionJob::Finalize();

    for (size_t i = 0; i < resources_.size(); i++)
    {
      assert(resources_[i] != NULL);
      delete resources_[i];
    }
  }

  void AddResource(const std::string uri)
  {
    resources_.push_back(new Resource(uri));
  }

  void AddResource(const std::string uri,
                   const std::map<std::string, std::string>& additionalHeaders)
  {
    resources_.push_back(new Resource(uri, additionalHeaders));
  }

  void AddResourceFromRequest(const Json::Value& resource)
  {
    std::string uri;
    std::map<std::string, std::string> additionalHeaders;
    ParseGetFromServer(uri, additionalHeaders, resource);

    resources_.push_back(new Resource(uri, additionalHeaders));
  }
};


void WadoRetrieveClient(OrthancPluginRestOutput* output,
                        const char* url,
                        const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Post)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
  }

  if (request->groupsCount != 1)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest);
  }

  std::string serverName(request->groups[0]);

  Json::Value body;
  OrthancPlugins::ParseJsonBody(body, request);

  std::auto_ptr<WadoRetrieveJob>  job(new WadoRetrieveJob(serverName));
  job->AddResourceFromRequest(body);

  SubmitJob(output, job.release(), body, false /* asynchronous by default */);
}



void RetrieveFromServer(OrthancPluginRestOutput* output,
                        const char* url,
                        const OrthancPluginHttpRequest* request)
{
  static const char* const GET_ARGUMENTS = "GetArguments";
  static const char* const HTTP_HEADERS = "HttpHeaders";
  static const char* const RESOURCES = "Resources";
  static const char* const STUDY = "Study";
  static const char* const SERIES = "Series";
  static const char* const INSTANCE = "Instance";

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
  }

  if (request->groupsCount != 1)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest);
  }

  std::string serverName(request->groups[0]);

  Json::Value body;
  OrthancPlugins::ParseJsonBody(body, request);

  std::map<std::string, std::string> getArguments;
  OrthancPlugins::ParseAssociativeArray(getArguments, body, GET_ARGUMENTS); 

  std::map<std::string, std::string> additionalHeaders;
  OrthancPlugins::ParseAssociativeArray(additionalHeaders, body, HTTP_HEADERS);

  std::auto_ptr<WadoRetrieveJob> job(new WadoRetrieveJob(serverName));

  if (body.type() != Json::objectValue ||
      !body.isMember(RESOURCES) ||
      body[RESOURCES].type() != Json::arrayValue)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat,
                                    "The body must be a JSON object containing an array \"" + 
                                    std::string(RESOURCES) + "\"");
  }

  const Json::Value& resources = body[RESOURCES];

  for (Json::Value::ArrayIndex i = 0; i < resources.size(); i++)
  {
    std::string study;
    if (!OrthancPlugins::LookupStringValue(study, resources[i], STUDY))
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat,
                                      "Missing \"Study\" field in the body");
    }

    std::string series;
    if (!OrthancPlugins::LookupStringValue(series, resources[i], SERIES))
    {
      series.clear();
    }

    std::string instance;
    if (!OrthancPlugins::LookupStringValue(instance, resources[i], INSTANCE))
    {
      instance.clear();
    }

    if (series.empty() &&
        !instance.empty())
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat,
                                      "Missing \"Series\" field in the body, as \"Instance\" is present");
    }

    std::string tmp = "/studies/" + study;

    if (!series.empty())
    {
      tmp += "/series/" + series;
    }

    if (!instance.empty())
    {
      tmp += "/instances/" + instance;
    }

    std::string uri;
    OrthancPlugins::DicomWebServers::UriEncode(uri, tmp, getArguments);

    job->AddResource(uri, additionalHeaders);
  }

  SubmitJob(output, job.release(), body, 
            true /* synchronous by default, for compatibility with <= 0.6 */);
}

