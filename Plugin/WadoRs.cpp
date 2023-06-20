/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2021 Osimis S.A., Belgium
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


#include "../Resources/Orthanc/Plugins/OrthancPluginCppWrapper.h"
#include "Configuration.h"
#include "DicomWebFormatter.h"

#include <Compatibility.h>
#include <ChunkedBuffer.h>
#include <Logging.h>
#include <Toolbox.h>
#include <MultiThreading/SharedMessageQueue.h>

#include <memory>
#include <boost/thread/mutex.hpp>
#include <boost/thread.hpp>

static const char* const MAIN_DICOM_TAGS = "MainDicomTags";
static const char* const REQUESTED_TAGS = "RequestedTags";
static const char* const INSTANCES = "Instances";
static const char* const PATIENT_MAIN_DICOM_TAGS = "PatientMainDicomTags";

static std::string instancesMainDicomTagsList;
static boost::mutex mainDicomTagsListMutex;

static const std::string& GetInstancesMainDicomTagsList()
{
  boost::mutex::scoped_lock lock(mainDicomTagsListMutex);
  
  if (instancesMainDicomTagsList.empty())
  {
    Json::Value system;
    OrthancPlugins::RestApiGet(system, "/system", false);

    instancesMainDicomTagsList = system[MAIN_DICOM_TAGS]["Instance"].asString();
  }

  return instancesMainDicomTagsList;
}

static std::string GetResourceUri(Orthanc::ResourceType level,
                                  const std::string& publicId)
{
  switch (level)
  {
    case Orthanc::ResourceType_Study:
      return "/studies/" + publicId;
      
    case Orthanc::ResourceType_Series:
      return "/series/" + publicId;
      
    case Orthanc::ResourceType_Instance:
      return "/instances/" + publicId;
      
    default:
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }
}



static void AcceptMultipartDicom(bool& transcode,
                                 Orthanc::DicomTransferSyntax& targetSyntax /* only if transcoding */,
                                 const OrthancPluginHttpRequest* request)
{
  /**
   * Up to release 1.4 of the DICOMweb plugin, WADO-RS
   * RetrieveInstance, RetrieveSeries and RetrieveStudy did *NOT*
   * transcode if no transer syntax was explicitly provided. This was
   * because the DICOM standard didn't specify a behavior in this case
   * up to DICOM 2016b:
   * http://dicom.nema.org/medical/dicom/2016b/output/chtml/part18/sect_6.5.3.html
   *
   * However, starting with DICOM 2016c, it is explicitly stated that
   * "If transfer-syntax is not specified in the dcm-parameters the
   * origin server shall use the Explicit VR Little Endian Transfer
   * Syntax "1.2.840.10008.1.2.1" for each Instance":
   * http://dicom.nema.org/medical/dicom/2016c/output/chtml/part18/sect_6.5.3.html
   * 
   * As a consequence, starting with release 1.5 of the DICOMweb
   * plugin, transcoding to "Little Endian Explicit" takes place by
   * default. If this transcoding is not desirable, the "Accept" HTTP
   * header can be set to
   * "multipart/related;type=application/dicom;transfer-syntax=*" (not
   * the asterisk "*") in order to prevent transcoding. The same
   * convention is used by the Google Cloud Platform:
   * https://cloud.google.com/healthcare/docs/dicom
   **/
  transcode = true;
  targetSyntax = Orthanc::DicomTransferSyntax_LittleEndianExplicit;
  
  std::string accept;

  if (!OrthancPlugins::LookupHttpHeader(accept, request, "accept"))
  {
    return;   // By default, return "multipart/related; type=application/dicom;"
  }

  std::string application;
  std::map<std::string, std::string> attributes;
  OrthancPlugins::ParseContentType(application, attributes, accept);

  if (application != "multipart/related" &&
      application != "*/*")
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                    "This WADO-RS plugin cannot generate the following content type: " + accept);
  }

  if (attributes.find("type") != attributes.end())
  {
    std::string s = attributes["type"];
    Orthanc::Toolbox::ToLowerCase(s);
    if (s != "application/dicom")
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                      "This WADO-RS plugin only supports application/dicom "
                                      "return type for DICOM retrieval (" + accept + ")");
    }
  }

  static const char* const TRANSFER_SYNTAX = "transfer-syntax";

  std::map<std::string, std::string>::const_iterator found = attributes.find(TRANSFER_SYNTAX);
  if (found != attributes.end())
  {
    /**
     * The "*" case below is related to Google Healthcare API:
     * https://groups.google.com/d/msg/orthanc-users/w1Ekrsc6-U8/T2a_DoQ5CwAJ
     **/
    if (found->second == "*")
    {
      transcode = false;
    }
    else
    {
      transcode = true;

      if (!Orthanc::LookupTransferSyntax(targetSyntax, found->second))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                        "Unsupported transfer syntax in WADO-RS: " + found->second);
      }
    }
  }
}



static bool AcceptMetadata(const OrthancPluginHttpRequest* request,
                           bool& isXml)
{
  isXml = false;    // By default, return application/dicom+json

  std::string accept;
  if (!OrthancPlugins::LookupHttpHeader(accept, request, "accept"))
  {
    return true;
  }

  std::string application;
  std::map<std::string, std::string> attributes;
  OrthancPlugins::ParseContentType(application, attributes, accept);

  std::vector<std::string> applicationTokens;
  Orthanc::Toolbox::TokenizeString(applicationTokens, application, ',');

  for (size_t i = 0; i < applicationTokens.size(); i++)
  {
    std::string token = Orthanc::Toolbox::StripSpaces(applicationTokens[i]);
    
    if (token == "application/json" ||
        token == "application/dicom+json" ||
        token == "*/*")
    {
      return true;
    }
  }

  if (application != "multipart/related")
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                    "This WADO-RS plugin cannot generate the following content type: " + accept);
  }

  if (attributes.find("type") != attributes.end())
  {
    std::string s = attributes["type"];
    Orthanc::Toolbox::ToLowerCase(s);
    if (s == "application/dicom+xml")
    {
      isXml = true;
    }
    else
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                      "This WADO-RS plugin only supports application/dicom+xml "
                                      "type for multipart/related accept (" + accept + ")");
    }
  }
  else
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                    "Missing \"type\" in multipart/related accept type (" + accept + ")");
  }

  if (attributes.find("transfer-syntax") != attributes.end())
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                    "This WADO-RS plugin cannot change the transfer syntax to " + 
                                    attributes["transfer-syntax"]);
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
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                    "This WADO-RS plugin cannot generate the following "
                                    "bulk data type: " + accept);
  }

  if (attributes.find("type") != attributes.end())
  {
    std::string s = attributes["type"];
    Orthanc::Toolbox::ToLowerCase(s);
    if (s != "application/octet-stream")
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                      "This WADO-RS plugin only supports application/octet-stream "
                                      "return type for bulk data retrieval (" + accept + ")");
    }
  }

  if (attributes.find("ra,ge") != attributes.end())
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                    "This WADO-RS plugin does not support Range retrieval, "
                                    "it can only return entire bulk data object");
  }

  return true;
}


static void AnswerListOfDicomInstances(OrthancPluginRestOutput* output,
                                       Orthanc::ResourceType level,
                                       const std::string& publicId,
                                       bool transcode,
                                       Orthanc::DicomTransferSyntax targetSyntax /* only if transcoding */)
{
  if (level != Orthanc::ResourceType_Study &&
      level != Orthanc::ResourceType_Series &&
      level != Orthanc::ResourceType_Instance)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
  }

  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  Json::Value instances;

  if (level == Orthanc::ResourceType_Instance)
  {
    Json::Value tmp = Json::objectValue;
    tmp["ID"] = publicId;
    
    instances = Json::arrayValue;
    instances.append(tmp);
  }
  else
  {
    if (!OrthancPlugins::RestApiGet(instances, GetResourceUri(level, publicId) + "/instances", false))
    {
      // Internal error
      OrthancPluginSendHttpStatusCode(context, output, 400);
      return;
    }
  }

  if (OrthancPluginStartMultipartAnswer(context, output, "related", "application/dicom"))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }

  for (Json::Value::ArrayIndex i = 0; i < instances.size(); i++)
  {
    const std::string uri = "/instances/" + instances[i]["ID"].asString();

    bool transcodeThisInstance;
    
    std::string sourceTransferSyntax;
    if (!transcode)
    {
      transcodeThisInstance = false;      
    }
    else if (OrthancPlugins::RestApiGetString(sourceTransferSyntax, uri + "/metadata/TransferSyntax", false))
    {
      // Avoid transcoding if the source file already uses the expected transfer syntax
      Orthanc::DicomTransferSyntax syntax;
      if (Orthanc::LookupTransferSyntax(syntax, sourceTransferSyntax))
      {
        transcodeThisInstance = (syntax != targetSyntax);
      }
      else
      {
        transcodeThisInstance = true;
      }
    }
    else
    {
      // The transfer syntax of the source file is unknown, transcode it to be sure
      transcodeThisInstance = true;
    }
    
    OrthancPlugins::MemoryBuffer dicom;
    if (dicom.RestApiGet(uri + "/file", false))
    {
      if (transcodeThisInstance)
      {
        std::unique_ptr<OrthancPlugins::DicomInstance> transcoded(
          OrthancPlugins::DicomInstance::Transcode(
            dicom.GetData(), dicom.GetSize(), Orthanc::GetTransferSyntaxUid(targetSyntax)));

        if (OrthancPluginSendMultipartItem(
              context, output, reinterpret_cast<const char*>(transcoded->GetBuffer()),
              transcoded->GetSize()) != 0)
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
        }
      }
      else
      {
        if (OrthancPluginSendMultipartItem(context, output, dicom.GetData(), dicom.GetSize()) != 0)
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
        }
      }
    }
  }
}



namespace
{
  class SetOfDicomInstances : public boost::noncopyable
  {
  private:
    std::vector<Orthanc::DicomMap*>  instances_;

  public:
    ~SetOfDicomInstances()
    {
      for (size_t i = 0; i < instances_.size(); i++)
      {
        assert(instances_[i] != NULL);
        delete instances_[i];
      }
    }

    size_t GetSize() const
    {
      return instances_.size();
    }

    bool ReadInstance(const std::string& publicId)
    {
      Json::Value dicomAsJson;
      
      if (OrthancPlugins::RestApiGet(dicomAsJson, "/instances/" + publicId + "/tags", false))
      {
        std::unique_ptr<Orthanc::DicomMap> instance(new Orthanc::DicomMap);
        instance->FromDicomAsJson(dicomAsJson);
        instances_.push_back(instance.release());
        
        return true;
      }
      else
      {
        return false;
      }
    }

    
    void MinorityReport(Orthanc::DicomMap& target,
                        const Orthanc::DicomTag& tag) const
    {
      typedef std::map<std::string, unsigned int>  Counters;

      Counters counters;

      for (size_t i = 0; i < instances_.size(); i++)
      {
        assert(instances_[i] != NULL);

        std::string value;
        if (instances_[i]->LookupStringValue(value, tag, false))
        {
          Counters::iterator found = counters.find(value);
          if (found == counters.end())
          {
            counters[value] = 1;
          }
          else
          {
            found->second ++;
          }
        }
      }

      if (!counters.empty())
      {
        Counters::const_iterator current = counters.begin();
          
        std::string maxValue = current->first;
        size_t maxCount = current->second;

        ++current;

        while (current != counters.end())
        {
          if (maxCount < current->second)
          {
            maxValue = current->first;
            maxCount = current->second;
          }
            
          ++current;
        }

        target.SetValue(tag, maxValue, false);

        // Take the ceiling of the number of available instances
        const size_t threshold = instances_.size() / 2 + 1;
        if (maxCount < threshold)
        {
          LOG(WARNING) << "No consensus on the value of a tag during WADO-RS Retrieve "
                       << "Metadata in Extrapolate mode: " << tag.Format();
        }
      }
    }
  };

  
  class MainDicomTagsCache : public boost::noncopyable
  {
  private:
    struct Info : public boost::noncopyable
    {
      Orthanc::DicomMap  dicom_;
      std::string        parent_;
    };
    
    typedef std::pair<std::string, Orthanc::ResourceType>  Index;
    typedef std::map<Index, Info*>                         Content;

    Content  content_;

    static bool ReadResource(Orthanc::DicomMap& dicom,
                             std::string& parent,
                             OrthancPlugins::MetadataMode mode,
                             const std::string& orthancId,
                             Orthanc::ResourceType level)
    {
      std::string uri;
      std::string parentField;

      switch (level)
      {
        case Orthanc::ResourceType_Study:
          uri = "/studies/" + orthancId + "?full";
          break;
            
        case Orthanc::ResourceType_Series:
          uri = "/series/" + orthancId + "?full";
          parentField = "ParentStudy";
          break;
            
        case Orthanc::ResourceType_Instance:
          uri = "/instances/" + orthancId + "?full";
          parentField = "ParentSeries";
          break;

        default:
          throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
      }

      Json::Value value;
      if (!OrthancPlugins::RestApiGet(value, uri, false))
      {
        return false;
      }
         

      if (value.type() != Json::objectValue ||
          !value.isMember(MAIN_DICOM_TAGS))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
      }

      dicom.FromDicomAsJson(value[MAIN_DICOM_TAGS], false /* append */, true /* parseSequences */);

      if (level == Orthanc::ResourceType_Study)
      {
        if (value.isMember(PATIENT_MAIN_DICOM_TAGS))
        {
          dicom.FromDicomAsJson(value[PATIENT_MAIN_DICOM_TAGS], true /* append */, true /* parseSequences */);
        }
        else
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
        }
      }
        
      if (!parentField.empty())
      {
        if (value.isMember(parentField) &&
            value[parentField].type() == Json::stringValue)
        {
          parent = value[parentField].asString();
        }
        else
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
        }
      }

      
      if (mode == OrthancPlugins::MetadataMode_Extrapolate &&
          (level == Orthanc::ResourceType_Series ||
           level == Orthanc::ResourceType_Study))
      {
        std::set<Orthanc::DicomTag> tags;
        OrthancPlugins::Configuration::GetExtrapolatedMetadataTags(tags, level);

        if (!tags.empty())
        {
          /**
           * Complete the series/study-level tags, with instance-level
           * tags that are not considered as "main DICOM tags" in
           * Orthanc, but that are necessary for Web viewers, and that
           * are expected to be constant throughout all the instances of
           * the study/series. To this end, we read up to "N" DICOM
           * instances of this study/series from disk, and for the tags
           * of interest, we look at whether there is a consensus in the
           * value among these instances. Obviously, this is an
           * approximation to improve performance.
           **/

          std::set<std::string> allInstances;

          switch (level)
          {
            case Orthanc::ResourceType_Series:
              if (!value.isMember(INSTANCES) ||
                  value[INSTANCES].type() != Json::arrayValue)
              {
                throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
              }
              else
              {
                for (Json::Value::ArrayIndex i = 0; i < value[INSTANCES].size(); i++)
                {
                  if (value[INSTANCES][i].type() != Json::stringValue)
                  {
                    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
                  }
                  else
                  {
                    allInstances.insert(value[INSTANCES][i].asString());
                  }            
                }
              }
              
              break;

            case Orthanc::ResourceType_Study:
            {
              Json::Value tmp;
              if (OrthancPlugins::RestApiGet(tmp, "/studies/" + orthancId + "/instances", false))
              {
                if (tmp.type() != Json::arrayValue)
                {
                  throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
                }

                for (Json::Value::ArrayIndex i = 0; i < tmp.size(); i++)
                {
                  if (tmp[i].type() != Json::objectValue ||
                      !tmp[i].isMember("ID") ||
                      tmp[i]["ID"].type() != Json::stringValue)
                  {
                    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
                  }
                  else
                  {
                    allInstances.insert(tmp[i]["ID"].asString());
                  }
                }
              }
              
              break;
            }

            default:
              throw Orthanc::OrthancException(Orthanc::ErrorCode_NotImplemented);
          }

          
          // Select up to N random instances. The instances are
          // implicitly selected randomly, as the public ID of an
          // instance is a SHA-1 hash (whose domain is uniformly distributed)

          static const size_t N = 3;
          SetOfDicomInstances selectedInstances;

          for (std::set<std::string>::const_iterator it = allInstances.begin();
               selectedInstances.GetSize() < N && it != allInstances.end(); ++it)
          {
            selectedInstances.ReadInstance(*it);
          }

          for (std::set<Orthanc::DicomTag>::const_iterator
                 it = tags.begin(); it != tags.end(); ++it)
          {
            selectedInstances.MinorityReport(dicom, *it);
          }
        }
      }

      return true;
    }
    

    bool Lookup(Orthanc::DicomMap& dicom,
                std::string& parent,
                OrthancPlugins::MetadataMode mode,
                const std::string& orthancId,
                Orthanc::ResourceType level)
    {
      Content::iterator found = content_.find(std::make_pair(orthancId, level));
      
      if (found == content_.end())
      {
        std::unique_ptr<Info> info(new Info);
        if (!ReadResource(info->dicom_, info->parent_, mode, orthancId, level))
        {
          return false;
        }

        found = content_.insert(std::make_pair(std::make_pair(orthancId, level), info.release())).first;
      }

      assert(found != content_.end() &&
             found->second != NULL);
      dicom.Merge(found->second->dicom_);
      parent = found->second->parent_;

      return true;
    }


  public:
    ~MainDicomTagsCache()
    {
      for (Content::iterator it = content_.begin(); it != content_.end(); ++it)
      {
        assert(it->second != NULL);
        delete it->second;
      }
    }


    bool GetInstance(Orthanc::DicomMap& dicom,
                     OrthancPlugins::MetadataMode mode,
                     const std::string& instanceOrthancId)
    {
      std::string seriesOrthancId, studyOrthancId, nope;
      
      return (ReadResource(dicom, seriesOrthancId, mode, instanceOrthancId, Orthanc::ResourceType_Instance) &&
              Lookup(dicom, studyOrthancId, mode, seriesOrthancId, Orthanc::ResourceType_Series) &&
              Lookup(dicom, nope /* patient id is unused */, mode, studyOrthancId, Orthanc::ResourceType_Study));
    }

    bool GetSeries(Orthanc::DicomMap& dicom,
                   OrthancPlugins::MetadataMode mode,
                   const std::string& seriesOrthancId)
    {
      std::string studyOrthancId, nope;
      
      return (Lookup(dicom, studyOrthancId, mode, seriesOrthancId, Orthanc::ResourceType_Series) &&
              Lookup(dicom, nope /* patient id is unused */, mode, studyOrthancId, Orthanc::ResourceType_Study));
    }
  };
}


static void MergeJson(Json::Value &a, const Json::Value &b) {                                                                        
                                                                                                                  
  if (!a.isObject() || !b.isObject())
  {
    return;
  }

  Json::Value::Members members = b.getMemberNames();

  for (size_t i = 0; i < members.size(); i++)
  {
    std::string key = members[i];
    
    if (!a[key].isNull() && a[key].type() == Json::objectValue && b[key].type() == Json::objectValue)
    {
      MergeJson(a[key], b[key]);
    } 
    else
    {
      // const std::string& val = b[key].asString();
      a[key] = b[key];
    }
  }
}


static void WriteInstancesMetadataFromMainDicomTags(OrthancPlugins::DicomWebFormatter::HttpWriter& writer,
                                                    OrthancPlugins::MetadataMode mode,
                                                    MainDicomTagsCache& cache,
                                                    const std::string& seriesOrthancId,
                                                    const std::string& studyInstanceUid,
                                                    const std::string& seriesInstanceUid,
                                                    const std::string& wadoBase)
{
  assert(!seriesOrthancId.empty() &&
         !studyInstanceUid.empty() &&
         !seriesInstanceUid.empty() &&
         !wadoBase.empty());

  Json::Value instances;

#if 1
  Json::Value seriesMainDicomTags;
  Json::Value value;
  std::string seriesUri =  "/series/" + seriesOrthancId + "?dicomWeb";    // TODO: use requestedTags & include=RequestedTags
  if (!OrthancPlugins::RestApiGet(value, seriesUri, false))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError, "unable to get series");
  }
  seriesMainDicomTags = value[MAIN_DICOM_TAGS];
  std::string studyId = value["ParentStudy"].asString();

  std::string studyUri =  "/studies/" + studyId + "?dicomWeb";    // TODO: use requestedTags & include=RequestedTags
  if (!OrthancPlugins::RestApiGet(value, studyUri, false))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError, "unable to get study");
  }

  MergeJson(seriesMainDicomTags, value[MAIN_DICOM_TAGS]);
  MergeJson(seriesMainDicomTags, value[PATIENT_MAIN_DICOM_TAGS]);

  std::string seriesInstancesUri = "/series/" + seriesOrthancId + "/instances?dicomWeb";

  // DicomWebJson serialization takes a lot of time -> limit the content of the response to the bare minimum
  if (mode == OrthancPlugins::MetadataMode_Experimental)
  {
    seriesInstancesUri += "&include=RequestedTags&requestedTags=" + GetInstancesMainDicomTagsList();
  }
  else
  {
    seriesInstancesUri += "&include=MainDicomTags";
  }

  if (!OrthancPlugins::RestApiGet(instances, seriesInstancesUri, false))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError, "unable to get series instances");
  }

  if (instances.type() != Json::arrayValue)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }

  for (Json::ArrayIndex i = 0; i < instances.size(); ++i)
  {
    Json::Value instanceTags = seriesMainDicomTags;

    if (mode == OrthancPlugins::MetadataMode_Experimental)
    {
      MergeJson(instanceTags, instances[i][REQUESTED_TAGS]);
    }
    else
    {
      MergeJson(instanceTags, instances[i][MAIN_DICOM_TAGS]);
    }
    
    writer.AddDicomWebJson(instanceTags);
   
  }
#else  // keep it for XML output (or adapt ?)
  Orthanc::DicomMap seriesDicom;
  if (!cache.GetSeries(seriesDicom, mode, seriesOrthancId))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError, "unable to get series");
  }

  std::string seriesInstancesUri = "/series/" + seriesOrthancId + "/instances?full";

  if (mode == OrthancPlugins::MetadataMode_Experimental)
  {
    seriesInstancesUri += "&requestedTags=" + GetInstancesMainDicomTagsList();
  }

  if (!OrthancPlugins::RestApiGet(instances, seriesInstancesUri, false))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError, "unable to get series instances");
  }

  if (instances.type() != Json::arrayValue)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }

  for (Json::ArrayIndex i = 0; i < instances.size(); ++i)
  {
    std::unique_ptr<Orthanc::DicomMap> dicom(seriesDicom.Clone());
    if (mode == OrthancPlugins::MetadataMode_Experimental)
    {
      dicom->FromDicomAsJson(instances[i][REQUESTED_TAGS], true /* append */, false /* parseSequences */);
    }
    else
    {
      dicom->FromDicomAsJson(instances[i][MAIN_DICOM_TAGS], true /* append */, false /* parseSequences */);
    }

    // TODO_OPTI: optimize this call since it takes around 1ms per instance which is huge for a CT study with 5x1000 instances.
    writer.AddOrthancMap(*dicom);
  }
#endif
}


static void WriteInstanceMetadata(OrthancPlugins::DicomWebFormatter::HttpWriter& writer,
                                  OrthancPlugins::MetadataMode mode,
                                  MainDicomTagsCache& cache,
                                  const std::string& orthancId,
                                  Orthanc::DicomMap* dicom,
                                  const std::string& studyInstanceUid,
                                  const std::string& seriesInstanceUid,
                                  const std::string& wadoBase)
{
  assert(!orthancId.empty() &&
         !studyInstanceUid.empty() &&
         !seriesInstanceUid.empty() &&
         !wadoBase.empty());

  switch (mode)
  {
    case OrthancPlugins::MetadataMode_MainDicomTags:
    case OrthancPlugins::MetadataMode_Extrapolate:
    {
      // TODO: reactivate
      // Orthanc::DicomMap dicom;

      // if (!cache.GetInstance(dicom, mode, orthancId))
      // {
      //   throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource, "instance not found: " + orthancId);
      // }

      // writer.AddOrthancMap(dicom);
      // break;
    }

    case OrthancPlugins::MetadataMode_Full:
    {
      assert(dicom != NULL);

      const std::string bulkRoot = (wadoBase +
                                    "studies/" + studyInstanceUid +
                                    "/series/" + seriesInstanceUid + 
                                    "/instances/" + dicom->GetStringValue(Orthanc::DICOM_TAG_SOP_INSTANCE_UID, "", false) + "/bulk");

      // On a SSD drive, this version is twice slower than if using
      // cache (see below)

      static size_t instanceCounter = 0;
      static size_t totalTime1 = 0;
      static size_t totalTime2 = 0;

      instanceCounter++;
      boost::posix_time::ptime start2 = boost::posix_time::microsec_clock::universal_time();

      OrthancPlugins::MemoryBuffer dicomFile;
      if (dicomFile.RestApiGet("/instances/" + orthancId + "/file-until-pixel-data", false))
      {
        boost::posix_time::ptime stop2 = boost::posix_time::microsec_clock::universal_time();
        totalTime1 += (stop2-start2).total_microseconds();

        writer.AddDicom(dicomFile.GetData(), dicomFile.GetSize(), bulkRoot, true);

        stop2 = boost::posix_time::microsec_clock::universal_time();
        totalTime2 += (stop2-start2).total_microseconds();
      }

      if (instanceCounter % 100 == 0)
      {
        LOG(WARNING) << " i: " << instanceCounter << " " << totalTime1/instanceCounter << " " << totalTime2/instanceCounter;
      }


      break;
    }

    default:
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }
    
  
#if 0
  /**
   **/

  // TODO - Have a global setting to enable/disable caching of DICOMweb

  // TODO - Have a way to clear the "4444" attachments if Orthanc
  // version changes => Store Orthanc core version in a prefix or in
  // another attachment?
    
  OrthancPlugins::MemoryBuffer buffer;

  if (writer.IsXml())
  {
    // DICOMweb XML is not cached
    if (buffer.RestApiGet("/instances/" + orthancId + "/file", false))
    {
      writer.AddDicom(buffer.GetData(), buffer.GetSize(), bulkRoot);
    }
  }
  else
  {
    if (buffer.RestApiGet("/instances/" + orthancId + "/attachments/4444/data", false))
    {
      writer.AddDicomWebSerializedJson(buffer.GetData(), buffer.GetSize());
    }
    else if (buffer.RestApiGet("/instances/" + orthancId + "/file", false))
    {
      // "Ignore binary mode" in DICOMweb conversion if caching is
      // enabled, as the bulk root can change across executions

      std::string dicomweb;
      {
        // TODO - Avoid a global mutex => Need to change Orthanc SDK
        OrthancPlugins::DicomWebFormatter::Apply(
          dicomweb, OrthancPlugins::GetGlobalContext(), buffer.GetData(), buffer.GetSize(),
          false /* JSON */, OrthancPluginDicomWebBinaryMode_Ignore, "");
      }

      buffer.RestApiPut("/instances/" + orthancId + "/attachments/4444", dicomweb, false);
      writer.AddDicomWebSerializedJson(dicomweb.c_str(), dicomweb.size());
    }
  }
#endif
}

bool LocateResource(OrthancPluginRestOutput* output,
                    std::string& orthancId,
                    const std::string& studyInstanceUid,
                    const std::string& seriesInstanceUid,
                    const std::string& sopInstanceUid,
                    const std::string& level,
                    const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
    return false;
  }

  {
    Json::Value payload;
    Json::Value payloadQuery;

    payload["Level"] = level;
    
    if (!sopInstanceUid.empty())
    {
      payloadQuery["SOPInstanceUID"] = sopInstanceUid;
    }
    
    if (!seriesInstanceUid.empty())
    {
      payloadQuery["SeriesInstanceUID"] = seriesInstanceUid;
    }

    payloadQuery["StudyInstanceUID"] = studyInstanceUid;
    payload["Query"] = payloadQuery;

    std::map<std::string, std::string> httpHeaders;
    OrthancPlugins::GetHttpHeaders(httpHeaders, request);

    Json::Value resources;
    if (!OrthancPlugins::RestApiPost(resources, "/tools/find", payload, httpHeaders, true) ||
        resources.type() != Json::arrayValue)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
    }

    if (resources.size() == 0)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem,
                                      "Accessing an inexistent " + level + " with WADO-RS: " + studyInstanceUid + "/" + seriesInstanceUid + "/" + sopInstanceUid);
    }
    if (resources.size() > 1)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem,
                                      "Multiple " + level + " found for WADO-RS: " + studyInstanceUid + "/" + seriesInstanceUid + "/" + sopInstanceUid);
    }
    orthancId = resources[0].asString();
    return true;
  }
}



bool LocateStudy(OrthancPluginRestOutput* output,
                 std::string& orthancId,
                 std::string& studyInstanceUid,
                 const OrthancPluginHttpRequest* request)
{
  std::string sopInstanceUid;
  std::string seriesInstanceUid;
  studyInstanceUid = request->groups[0];

  return LocateResource(output,
                        orthancId,
                        studyInstanceUid,
                        seriesInstanceUid,
                        sopInstanceUid,
                        "Study",
                        request);
}


bool LocateSeries(OrthancPluginRestOutput* output,
                  std::string& orthancId,
                  std::string& studyInstanceUid,
                  std::string& seriesInstanceUid,
                  const OrthancPluginHttpRequest* request)
{
  std::string sopInstanceUid;
  studyInstanceUid = request->groups[0];
  seriesInstanceUid = request->groups[1];

  return LocateResource(output,
                        orthancId,
                        studyInstanceUid,
                        seriesInstanceUid,
                        sopInstanceUid,
                        "Series",
                        request);
}


bool LocateInstance(OrthancPluginRestOutput* output,
                    std::string& orthancId,
                    std::string& studyInstanceUid,
                    std::string& seriesInstanceUid,
                    std::string& sopInstanceUid,
                    const OrthancPluginHttpRequest* request)
{
  studyInstanceUid = request->groups[0];
  seriesInstanceUid = request->groups[1];
  sopInstanceUid = request->groups[2];

  return LocateResource(output,
                        orthancId,
                        studyInstanceUid,
                        seriesInstanceUid,
                        sopInstanceUid,
                        "Instance",
                        request);
}


void RetrieveDicomStudy(OrthancPluginRestOutput* output,
                        const char* url,
                        const OrthancPluginHttpRequest* request)
{
  bool transcode;
  Orthanc::DicomTransferSyntax targetSyntax;

  AcceptMultipartDicom(transcode, targetSyntax, request);
  
  std::string orthancId, studyInstanceUid;
  if (LocateStudy(output, orthancId, studyInstanceUid, request))
  {
    AnswerListOfDicomInstances(output, Orthanc::ResourceType_Study, orthancId, transcode, targetSyntax);
  }
}


void RetrieveDicomSeries(OrthancPluginRestOutput* output,
                         const char* url,
                         const OrthancPluginHttpRequest* request)
{
  bool transcode;
  Orthanc::DicomTransferSyntax targetSyntax;
  
  AcceptMultipartDicom(transcode, targetSyntax, request);
  
  std::string orthancId, studyInstanceUid, seriesInstanceUid;
  if (LocateSeries(output, orthancId, studyInstanceUid, seriesInstanceUid, request))
  {
    AnswerListOfDicomInstances(output, Orthanc::ResourceType_Series, orthancId, transcode, targetSyntax);
  }
}



void RetrieveDicomInstance(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request)
{
  bool transcode;
  Orthanc::DicomTransferSyntax targetSyntax;
  
  AcceptMultipartDicom(transcode, targetSyntax, request);
  
  std::string orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid;
  if (LocateInstance(output, orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid, request))
  {
    AnswerListOfDicomInstances(output, Orthanc::ResourceType_Instance, orthancId, transcode, targetSyntax);
  }
}


static void GetChildrenIdentifiers(std::list<std::string>& target,
                                   std::string& resourceDicomUid,
                                   Orthanc::ResourceType level,
                                   const std::string& orthancId)
{
  target.clear();

  const char* childrenTag = NULL;
  const char* dicomUidTag = NULL;
  std::string uri;

  switch (level)
  {
    case Orthanc::ResourceType_Study:
      uri = "/studies/" + orthancId;
      childrenTag = "Series";
      dicomUidTag = "StudyInstanceUID";
      break;
       
    case Orthanc::ResourceType_Series:
      uri = "/series/" + orthancId;
      childrenTag = "Instances";
      dicomUidTag = "SeriesInstanceUID";
      break;

    default:
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
  }

  assert(childrenTag != NULL && dicomUidTag != NULL);
  
  Json::Value resource;
  if (OrthancPlugins::RestApiGet(resource, uri, false))
  {
    if (resource.type() != Json::objectValue || !resource.isMember(childrenTag) 
      || !resource.isMember(MAIN_DICOM_TAGS) || !resource[MAIN_DICOM_TAGS].isMember(dicomUidTag))
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
    }

    const Json::Value& children = resource[childrenTag];
    resourceDicomUid = resource[MAIN_DICOM_TAGS][dicomUidTag].asString();

    for (Json::Value::ArrayIndex i = 0; i < children.size(); i++)
    {
      target.push_back(children[i].asString());
    }
  }  
}

typedef std::map<std::string, boost::shared_ptr<Orthanc::DicomMap> >  DicomMaps;

static void GetChildrenDicomMaps(DicomMaps& childrenDicomMaps,
                                 std::string& resourceDicomUid,
                                 Orthanc::ResourceType level,
                                 const std::string& orthancId)
{
  childrenDicomMaps.clear();

  const char* childrenRoute = NULL;
  const char* dicomUidTag = NULL;
  std::string uri;

  switch (level)
  {
    case Orthanc::ResourceType_Study:
      uri = "/studies/" + orthancId;
      childrenRoute = "series";
      dicomUidTag = "StudyInstanceUID";
      break;
       
    case Orthanc::ResourceType_Series:
      uri = "/series/" + orthancId;
      childrenRoute = "instances";
      dicomUidTag = "SeriesInstanceUID";
      break;

    default:
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
  }

  assert(childrenRoute != NULL && dicomUidTag != NULL);
  
  // get the resource itself
  Json::Value resource;
  if (OrthancPlugins::RestApiGet(resource, uri, false))
  {
    if (resource.type() != Json::objectValue 
      || !resource.isMember(MAIN_DICOM_TAGS) || !resource[MAIN_DICOM_TAGS].isMember(dicomUidTag))
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
    }

    resourceDicomUid = resource[MAIN_DICOM_TAGS][dicomUidTag].asString();

    // get the children resources
    Json::Value childResources;
    if (OrthancPlugins::RestApiGet(childResources, uri + "/" + childrenRoute + "?expand&full", false))
    {
      for (Json::Value::ArrayIndex i = 0; i < childResources.size(); i++)
      {
        const Json::Value& child = childResources[i];
        Orthanc::DicomMap dicom;
        dicom.FromDicomAsJson(child[MAIN_DICOM_TAGS], false /* append */, true /* parseSequences */);
        childrenDicomMaps[child["ID"].asString()] = boost::shared_ptr<Orthanc::DicomMap>(dicom.Clone());
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
    OrthancPluginSendHttpStatusCode(OrthancPlugins::GetGlobalContext(), output, 400 /* Bad request */);
  }
  else
  {
    const OrthancPlugins::MetadataMode mode =
      OrthancPlugins::Configuration::GetMetadataMode(Orthanc::ResourceType_Study);
    
    MainDicomTagsCache cache;

    std::string studyOrthancId, studyInstanceUid;
    if (LocateStudy(output, studyOrthancId, studyInstanceUid, request))
    {
      OrthancPlugins::DicomWebFormatter::HttpWriter writer(output, isXml);

      DicomMaps seriesDicomMaps;
      std::string studyDicomUid;
      GetChildrenDicomMaps(seriesDicomMaps, studyDicomUid, Orthanc::ResourceType_Study, studyOrthancId);

      for (DicomMaps::const_iterator s = seriesDicomMaps.begin(); s != seriesDicomMaps.end(); ++s)
      {
        DicomMaps instancesDicomMaps;
        std::string seriesDicomUid;
        GetChildrenDicomMaps(instancesDicomMaps, seriesDicomUid, Orthanc::ResourceType_Series, s->first);

        // TODO: parallelize (share code with RetrieveSeriesMetadata)
        for (DicomMaps::const_iterator i = instancesDicomMaps.begin(); i != instancesDicomMaps.end(); ++i)
        {
          WriteInstanceMetadata(writer, mode, cache, i->first, i->second.get(), studyInstanceUid, seriesDicomUid,
                                OrthancPlugins::Configuration::GetBasePublicUrl(request));
        }
      }

      writer.Send();
    }
  }
}


static const char* EXIT_WORKER_MESSAGE = "exit";

class InstanceToLoad : public Orthanc::IDynamicObject
{
public:
  std::string                  orthancId;
  std::string                  studyInstanceUid;
  std::string                  seriesInstanceUid;
  std::string                  bulkRoot;
  boost::mutex&                writerMutex;
  OrthancPlugins::DicomWebFormatter::HttpWriter& writer;
  bool                         isXml;
  OrthancPluginDicomWebBinaryMode mode;

  explicit InstanceToLoad(const std::string& orthancId, const std::string bulkRoot, boost::mutex& writerMutex, OrthancPlugins::DicomWebFormatter::HttpWriter& writer, bool isXml, OrthancPluginDicomWebBinaryMode mode, const std::string& studyInstanceUid, const std::string& seriesInstanceUid)
  : orthancId(orthancId),
    studyInstanceUid(studyInstanceUid),
    seriesInstanceUid(seriesInstanceUid),
    bulkRoot(bulkRoot),
    writerMutex(writerMutex),
    writer(writer),
    isXml(isXml),
    mode(mode)
  {}
};

class InstanceWorkerData
{
public:
  std::string id;
  Orthanc::SharedMessageQueue* instancesQueue;
  std::string wadoBase;

  InstanceWorkerData(const std::string& id, Orthanc::SharedMessageQueue* instancesQueue, const std::string& wadoBase)
  : id(id),
    instancesQueue(instancesQueue),
    wadoBase(wadoBase)
  {}
};

void InstanceWorkerThread(InstanceWorkerData* data)
{
  size_t instanceCounter = 0;
  size_t totalTime1 = 0;
  size_t totalTime2 = 0;
  size_t totalTime3 = 0;
  std::string threadId = data->id;

  while (true)
  {
    try
    {
      std::unique_ptr<InstanceToLoad> instanceToLoad(dynamic_cast<InstanceToLoad*>(data->instancesQueue->Dequeue(0)));

      if (instanceToLoad->orthancId == EXIT_WORKER_MESSAGE)
      {
        LOG(WARNING) << threadId << " i: " << instanceCounter << " " << totalTime1/instanceCounter << " " << totalTime2/instanceCounter << " " << totalTime3/instanceCounter;
        return;
      }
      instanceCounter++;
      boost::posix_time::ptime start2 = boost::posix_time::microsec_clock::universal_time();
      boost::posix_time::ptime stop2 = boost::posix_time::microsec_clock::universal_time();

      if (instanceToLoad->bulkRoot == "") // we are not in oneLargeQuery mode -> we must load the instance tags to get the SOPInstanceUID
      {
        Json::Value instanceResource;

        if (OrthancPlugins::RestApiGet(instanceResource, "/instances/" + instanceToLoad->orthancId, false))
        {
          instanceToLoad->bulkRoot = (data->wadoBase +
                              "studies/" + instanceToLoad->studyInstanceUid +
                              "/series/" + instanceToLoad->seriesInstanceUid + 
                              "/instances/" + instanceResource["MainDicomTags"]["SOPInstanceUID"].asString() + "/bulk");
        }
      }

      OrthancPlugins::MemoryBuffer dicomFile;
      if (dicomFile.RestApiGet("/instances/" + instanceToLoad->orthancId + "/file-until-pixel-data", false))
      {

        stop2 = boost::posix_time::microsec_clock::universal_time(); // LOG(WARNING) << data->id << " read file one instance " << (stop2-start2).total_microseconds() << "us";
        totalTime1 += (stop2-start2).total_microseconds();

        if (false)
        {        
          boost::mutex::scoped_lock lock(instanceToLoad->writerMutex);
          instanceToLoad->writer.AddDicom(dicomFile.GetData(), dicomFile.GetSize(), instanceToLoad->bulkRoot, true);
        }
        else
        {
          std::string content;
          OrthancPlugins::DicomWebFormatter::Apply(content, 
                                   OrthancPlugins::GetGlobalContext(), 
                                   dicomFile.GetData(), dicomFile.GetSize(),
                                   instanceToLoad->isXml, 
                                   instanceToLoad->mode, 
                                   instanceToLoad->bulkRoot, 
                                   true /*injectEmptyPixelData*/);

          stop2 = boost::posix_time::microsec_clock::universal_time(); // LOG(WARNING) << data->id << " formatted one instance " << (stop2-start2).total_microseconds() << "us";
          totalTime2 += (stop2-start2).total_microseconds();

          boost::mutex::scoped_lock lock(instanceToLoad->writerMutex);
          instanceToLoad->writer.AddDicomWebSerializedJson(content.data(), content.size());

          stop2 = boost::posix_time::microsec_clock::universal_time(); //LOG(WARNING) << data->id << " written one instance " << (stop2-start2).total_microseconds() << "us";
          totalTime3 += (stop2-start2).total_microseconds();
        }
      }
    }
    catch(...)
    {
      // ignore errors but don't exit the thread to make sure all workers end correctly
    }
  }
}

#include <boost/date_time/posix_time/posix_time.hpp>

void RetrieveSeriesMetadata(OrthancPluginRestOutput* output,
                            const char* url,
                            const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  boost::posix_time::ptime start = boost::posix_time::microsec_clock::universal_time();
  boost::posix_time::ptime stop = boost::posix_time::microsec_clock::universal_time();
  // stop = boost::posix_time::microsec_clock::universal_time(); LOG(WARNING) << "start " << (stop-start).total_milliseconds();

  bool isXml;
  if (!AcceptMetadata(request, isXml))
  {
    OrthancPluginSendHttpStatusCode(context, output, 400 /* Bad request */);
  }
  else
  {
    const OrthancPlugins::MetadataMode mode =
      OrthancPlugins::Configuration::GetMetadataMode(Orthanc::ResourceType_Series);
    
    MainDicomTagsCache cache;

    std::string seriesOrthancId, studyInstanceUid, seriesInstanceUid;

    if (LocateSeries(output, seriesOrthancId, studyInstanceUid, seriesInstanceUid, request))
    {
      OrthancPlugins::DicomWebFormatter::HttpWriter writer(output, isXml);
      if (mode == OrthancPlugins::MetadataMode_MainDicomTags || mode == OrthancPlugins::MetadataMode_Experimental)
      {
        WriteInstancesMetadataFromMainDicomTags(writer,
                                                mode,
                                                cache,
                                                seriesOrthancId,
                                                studyInstanceUid,
                                                seriesInstanceUid,
                                                OrthancPlugins::Configuration::GetBasePublicUrl(request));
      }
      else
      {
        DicomMaps instancesDicomMaps;
        std::list<std::string> instancesIds;
        std::string seriesDicomUid;

        size_t threadCount = 4;
        bool oneLargeQuery = false;

        if (oneLargeQuery)
        {
          GetChildrenDicomMaps(instancesDicomMaps, seriesDicomUid, Orthanc::ResourceType_Series, seriesOrthancId);
        }
        else
        {
          GetChildrenIdentifiers(instancesIds, seriesDicomUid, Orthanc::ResourceType_Series, seriesOrthancId);
        }

        stop = boost::posix_time::microsec_clock::universal_time(); LOG(WARNING) << "Get instances tags " << (stop-start).total_milliseconds();

        
        if (true || threadCount > 1)
        {
          // span a few workers to get the tags from the core and serialize them
          Orthanc::SharedMessageQueue instancesQueue;
          std::vector<boost::shared_ptr<boost::thread> > instancesWorkers;
          boost::mutex writerMutex;
          std::vector<boost::shared_ptr<InstanceWorkerData> > instancesWorkersData;
          std::string wadoBase = OrthancPlugins::Configuration::GetBasePublicUrl(request);

          for (size_t t; t < threadCount; t++)
          {
            InstanceWorkerData* threadData = new InstanceWorkerData(boost::lexical_cast<std::string>(t), &instancesQueue, wadoBase);
            instancesWorkersData.push_back(boost::shared_ptr<InstanceWorkerData>(threadData));
            instancesWorkers.push_back(boost::shared_ptr<boost::thread>(new boost::thread(InstanceWorkerThread, threadData)));
          }

          if (oneLargeQuery)
          {
            for (DicomMaps::const_iterator i = instancesDicomMaps.begin(); i != instancesDicomMaps.end(); ++i)
            {
              std::string bulkRoot = (wadoBase +
                                  "studies/" + studyInstanceUid +
                                  "/series/" + seriesInstanceUid + 
                                  "/instances/" + i->second->GetStringValue(Orthanc::DICOM_TAG_SOP_INSTANCE_UID, "", false) + "/bulk");

              instancesQueue.Enqueue(new InstanceToLoad(i->first, bulkRoot, writerMutex, writer, isXml, OrthancPluginDicomWebBinaryMode_BulkDataUri, studyInstanceUid, seriesInstanceUid));
            }
          }
          else
          {
            for (std::list<std::string>::const_iterator i = instancesIds.begin(); i != instancesIds.end(); ++i)
            {
              instancesQueue.Enqueue(new InstanceToLoad(*i, "", writerMutex, writer, isXml, OrthancPluginDicomWebBinaryMode_BulkDataUri, studyInstanceUid, seriesInstanceUid));
            }
          }

          stop = boost::posix_time::microsec_clock::universal_time(); LOG(WARNING) << "enqueued " << (stop-start).total_milliseconds();

          // send a dummy "exit" message to all workers such that they stop waiting for messages on the queue
          for (size_t i = 0; i < instancesWorkers.size(); i++)
          {
            instancesQueue.Enqueue(new InstanceToLoad(EXIT_WORKER_MESSAGE, "", writerMutex, writer, isXml, OrthancPluginDicomWebBinaryMode_BulkDataUri, studyInstanceUid, seriesInstanceUid));
          }

          stop = boost::posix_time::microsec_clock::universal_time(); LOG(WARNING) << "waiting " << (stop-start).total_milliseconds();

          for (size_t i = 0; i < instancesWorkers.size(); i++)
          {
            if (instancesWorkers[i]->joinable())
            {
              instancesWorkers[i]->join();
            }
          }

          instancesWorkers.clear();
        }
        else
        {
          size_t instanceCounter = 0;
          size_t totalTime1 = 0;

          for (DicomMaps::const_iterator i = instancesDicomMaps.begin(); i != instancesDicomMaps.end(); ++i)
          {
            boost::posix_time::ptime start2 = boost::posix_time::microsec_clock::universal_time();

            WriteInstanceMetadata(writer, mode, cache, i->first, i->second.get(), studyInstanceUid, seriesDicomUid,
                                  OrthancPlugins::Configuration::GetBasePublicUrl(request));

            boost::posix_time::ptime stop2 = boost::posix_time::microsec_clock::universal_time();
            totalTime1 += (stop2-start2).total_microseconds();
            instanceCounter++;
          }
          LOG(WARNING) << " i: " << instanceCounter << " " << totalTime1/instanceCounter;

        }
      }

      stop = boost::posix_time::microsec_clock::universal_time(); LOG(WARNING) << "before send " << (stop-start).total_milliseconds();

      writer.Send();

      stop = boost::posix_time::microsec_clock::universal_time(); LOG(WARNING) << "after send " << (stop-start).total_milliseconds();

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
    OrthancPluginSendHttpStatusCode(OrthancPlugins::GetGlobalContext(), output, 400 /* Bad request */);
  }
  else
  {
    MainDicomTagsCache cache;

    std::string orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid;
    if (LocateInstance(output, orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid, request))
    {
      OrthancPlugins::DicomWebFormatter::HttpWriter writer(output, isXml);
      WriteInstanceMetadata(writer, OrthancPlugins::MetadataMode_Full, cache, orthancId, NULL, studyInstanceUid,
                            seriesInstanceUid, OrthancPlugins::Configuration::GetBasePublicUrl(request));
      writer.Send();      
    }
  }
}


void RetrieveBulkData(OrthancPluginRestOutput* output,
                      const char* url,
                      const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (!AcceptBulkData(request))
  {
    OrthancPluginSendHttpStatusCode(context, output, 400 /* Bad request */);
    return;
  }

  std::string orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid;
  OrthancPlugins::MemoryBuffer content;
  if (LocateInstance(output, orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid, request) &&
      content.RestApiGet("/instances/" + orthancId + "/file", false))
  {
    std::string bulk(request->groups[3]);

    std::vector<std::string> path;
    Orthanc::Toolbox::TokenizeString(path, bulk, '/');

    // Map the bulk data URI to the Orthanc "/instances/.../content/..." built-in URI
    std::string orthanc = "/instances/" + orthancId + "/content";

    Orthanc::DicomTag tmp(0, 0);
    
    if (path.size() == 1 &&
        Orthanc::DicomTag::ParseHexadecimal(tmp, path[0].c_str()) &&
        tmp == Orthanc::DICOM_TAG_PIXEL_DATA)
    {
      // Accessing pixel data: Return the raw content of the fragments in a multipart stream.
      // TODO - Is this how DICOMweb should work?
      orthanc += "/" + Orthanc::DICOM_TAG_PIXEL_DATA.Format();

      Json::Value frames;
      if (OrthancPlugins::RestApiGet(frames, orthanc, false))
      {
        if (frames.type() != Json::arrayValue ||
            OrthancPluginStartMultipartAnswer(context, output, "related", "application/octet-stream") != 0)
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_Plugin);
        }

        for (Json::Value::ArrayIndex i = 0; i < frames.size(); i++)
        {
          std::string frame;
          
          if (frames[i].type() != Json::stringValue ||
              !OrthancPlugins::RestApiGetString(frame, orthanc + "/" + frames[i].asString(), false) ||
              OrthancPluginSendMultipartItem(context, output, frame.c_str(), frame.size()) != 0)
          {
            throw Orthanc::OrthancException(Orthanc::ErrorCode_Plugin);
          }
        }
      }
      else
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem);
      }      
    }
    else
    {
      if (path.size() % 2 != 1)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                        "Bulk data URI in WADO-RS should have an odd number of items: " + bulk);
      }

      for (size_t i = 0; i < path.size() / 2; i++)
      {
        int index;

        try
        {
          index = boost::lexical_cast<int>(path[2 * i + 1]);
        }
        catch (boost::bad_lexical_cast&)
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                          "Bad sequence index in bulk data URI: " + bulk);
        }

        orthanc += "/" + path[2 * i] + "/" + boost::lexical_cast<std::string>(index - 1);
      }

      orthanc += "/" + path.back();

      std::string result; 
      if (OrthancPlugins::RestApiGetString(result, orthanc, false))
      {
        if (OrthancPluginStartMultipartAnswer(context, output, "related", "application/octet-stream") != 0 ||
            OrthancPluginSendMultipartItem(context, output, result.c_str(), result.size()) != 0)
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_Plugin);
        }
      }
      else
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem);
      }
    }
  }
}
