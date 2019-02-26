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


#include "Plugin.h"

#include "Configuration.h"
#include "Dicom.h"
#include "DicomResults.h"

#include <Core/Toolbox.h>

#include <memory>


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



static bool AcceptMultipartDicom(const OrthancPluginHttpRequest* request)
{
  std::string accept;

  if (!OrthancPlugins::LookupHttpHeader(accept, request, "accept"))
  {
    return true;   // By default, return "multipart/related; type=application/dicom;"
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

  if (attributes.find("transfer-syntax") != attributes.end())
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest,
                                    "This WADO-RS plugin cannot change the transfer syntax to " + 
                                    attributes["transfer-syntax"]);
  }

  return true;
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

  if (application == "application/json" ||
      application == "application/dicom+json" ||
      application == "*/*")
  {
    return true;
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
                                       const std::string& publicId)
{
  if (level != Orthanc::ResourceType_Study &&
      level != Orthanc::ResourceType_Series)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
  }

  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  Json::Value instances;
  if (!OrthancPlugins::RestApiGet(instances, GetResourceUri(level, publicId) + "/instances", false))
  {
    // Internal error
    OrthancPluginSendHttpStatusCode(context, output, 400);
    return;
  }

  if (OrthancPluginStartMultipartAnswer(context, output, "related", "application/dicom"))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
  }
  
  for (Json::Value::ArrayIndex i = 0; i < instances.size(); i++)
  {
    std::string uri = "/instances/" + instances[i]["ID"].asString() + "/file";

    OrthancPlugins::MemoryBuffer dicom;
    if (dicom.RestApiGet(uri, false) &&
        OrthancPluginSendMultipartItem(context, output, dicom.GetData(), dicom.GetSize()) != 0)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
    }
  }
}





#if 0
#include <boost/thread/mutex.hpp>

class DicomWebFormatter : public boost::noncopyable
{
private:
  boost::mutex  mutex_;
  std::string   wadoBase_;

  static DicomWebFormatter& GetSingleton()
  {
    static DicomWebFormatter  formatter;
    return formatter;
  }


  static std::string FormatTag(uint16_t group,
                               uint16_t element)
  {
    char buf[16];
    sprintf(buf, "%04x%04x", group, element);
    return std::string(buf);
  }


  static void Callback(OrthancPluginDicomWebNode *node,
                       OrthancPluginDicomWebSetBinaryNode setter,
                       uint32_t levelDepth,
                       const uint16_t *levelTagGroup,
                       const uint16_t *levelTagElement,
                       const uint32_t *levelIndex,
                       uint16_t tagGroup,
                       uint16_t tagElement,
                       OrthancPluginValueRepresentation vr)
  {
    std::string uri = GetSingleton().wadoBase_;
    for (size_t i = 0; i < levelDepth; i++)
    {
      uri += ("/" + FormatTag(levelTagGroup[i], levelTagElement[i]) + "/" +
              boost::lexical_cast<std::string>(levelIndex[i]));
    }

    uri += "/" + FormatTag(tagGroup, tagElement);
    
    setter(node, OrthancPluginDicomWebBinaryMode_BulkDataUri, uri.c_str());
  }


public:
  class Locker : public boost::noncopyable
  {
  private:
    DicomWebFormatter&         that_;
    boost::mutex::scoped_lock  lock_;

  public:
    Locker(const std::string& wadoBase) :
      that_(GetSingleton()),
      lock_(that_.mutex_)
    {
      that_.wadoBase_ = wadoBase;
    }

    void Apply(std::string& target,
               OrthancPluginContext* context,
               const void* data,
               size_t size,
               bool xml)
    {
      OrthancPlugins::OrthancString s;

      if (xml)
      {
        s.Assign(OrthancPluginEncodeDicomWebXml(context, data, size, Callback));
      }
      else
      {
        s.Assign(OrthancPluginEncodeDicomWebJson(context, data, size, Callback));
      }

      s.ToString(target);
    }
  };
};


static bool GetDicomIdentifiers(std::string& studyInstanceUid,
                                std::string& seriesInstanceUid,
                                std::string& sopInstanceUid,
                                const std::string& orthancId)
{
  // TODO - Any way of speeding this up?

  static const char* const STUDY_INSTANCE_UID = "0020,000d";
  static const char* const SERIES_INSTANCE_UID = "0020,000e";
  static const char* const SOP_INSTANCE_UID = "0008,0018";
  
  Json::Value dicom;
  if (OrthancPlugins::RestApiGet(dicom, "/instances/" + orthancId + "/tags", false) &&
      dicom.isMember(STUDY_INSTANCE_UID) &&
      dicom.isMember(SERIES_INSTANCE_UID) &&
      dicom.isMember(SOP_INSTANCE_UID) &&
      dicom[STUDY_INSTANCE_UID].type() == Json::objectValue &&
      dicom[SERIES_INSTANCE_UID].type() == Json::objectValue &&
      dicom[SOP_INSTANCE_UID].type() == Json::objectValue &&
      dicom[STUDY_INSTANCE_UID].isMember("Value") &&
      dicom[SERIES_INSTANCE_UID].isMember("Value") &&
      dicom[SOP_INSTANCE_UID].isMember("Value") &&
      dicom[STUDY_INSTANCE_UID]["Value"].isString() &&
      dicom[SERIES_INSTANCE_UID]["Value"].isString() &&
      dicom[SOP_INSTANCE_UID]["Value"].isString())
  {
    studyInstanceUid = dicom[STUDY_INSTANCE_UID]["Value"].asString();
    seriesInstanceUid = dicom[SERIES_INSTANCE_UID]["Value"].asString();
    sopInstanceUid = dicom[SOP_INSTANCE_UID]["Value"].asString();
    return true;
  }
  else
  {
    return false;
  }
}
#endif



static void AnswerMetadata(OrthancPluginRestOutput* output,
                           const OrthancPluginHttpRequest* request,
                           Orthanc::ResourceType level,
                           const std::string& resource,
                           bool isInstance,
                           bool isXml)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

#if 1
  std::string tmp = GetResourceUri(level, resource);
  
  std::list<std::string> files;
  if (isInstance)
  {
    files.push_back(tmp + "/file");
  }
  else
  {
    Json::Value instances;
    if (!OrthancPlugins::RestApiGet(instances, tmp + "/instances", false))
    {
      // Internal error
      OrthancPluginSendHttpStatusCode(context, output, 400);
      return;
    }

    for (Json::Value::ArrayIndex i = 0; i < instances.size(); i++)
    {
      files.push_back("/instances/" + instances[i]["ID"].asString() + "/file");
    }
  }

  const std::string wadoBase = OrthancPlugins::Configuration::GetBaseUrl(request);

  OrthancPlugins::DicomResults results(output, wadoBase, *dictionary_, isXml, true);
  
  for (std::list<std::string>::const_iterator
         it = files.begin(); it != files.end(); ++it)
  {
    OrthancPlugins::MemoryBuffer content;
    if (content.RestApiGet(*it, false))
    {
      OrthancPlugins::ParsedDicomFile dicom(content);
      results.Add(dicom.GetFile());
    }
  }

  results.Answer();

#else
  std::list<std::string> instances;
  if (isInstance)
  {
    instances.push_back(resource);
  }
  else
  {
    Json::Value children;
    if (!OrthancPlugins::RestApiGet(children, GetResourceUri(level, resource) + "/instances", false))
    {
      // Internal error
      OrthancPluginSendHttpStatusCode(context, output, 400);
      return;
    }

    for (Json::Value::ArrayIndex i = 0; i < children.size(); i++)
    {
      instances.push_back(children[i]["ID"].asString());
    }
  }

  const std::string wadoBase = OrthancPlugins::Configuration::GetBaseUrl(request);

  // For JSON
  Orthanc::ChunkedBuffer buffer;
  buffer.AddChunk("[");
  bool first = true;

  if (isXml)
  {
    OrthancPluginStartMultipartAnswer(context, output, "related", "application/dicom+xml");
  }

  for (std::list<std::string>::const_iterator
         it = instances.begin(); it != instances.end(); ++it)
  {
    std::string studyInstanceUid, seriesInstanceUid, sopInstanceUid;
    if (GetDicomIdentifiers(studyInstanceUid, seriesInstanceUid, sopInstanceUid, *it))
    {
      OrthancPlugins::MemoryBuffer dicom;
      if (dicom.RestApiGet("/instances/" + *it + "/file", false))
      {
        std::string item;
        
        {
          // TODO - Avoid a global mutex
          DicomWebFormatter::Locker locker(wadoBase +
                                           "studies/" + studyInstanceUid +
                                           "/series/" + seriesInstanceUid + 
                                           "/instances/" + sopInstanceUid + "/bulk");
          locker.Apply(item, context, dicom.GetData(), dicom.GetSize(), isXml);
        }

        if (isXml)
        {
          OrthancPluginSendMultipartItem(context, output, item.c_str(), item.size());
        }
        else
        {
          if (!first)
          {
            buffer.AddChunk(",");
            first = false;
          }

          buffer.AddChunk(item);
        }
      }
    }
  }

  if (!isXml)
  {
    buffer.AddChunk("]");

    std::string answer;
    buffer.Flatten(answer);
    OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), "application/dicom+json");
  }
#endif
}



static bool LocateStudy(OrthancPluginRestOutput* output,
                        std::string& publicId,
                        const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
    return false;
  }

  try
  {
    OrthancPlugins::OrthancString tmp;
    tmp.Assign(OrthancPluginLookupStudy(context, request->groups[0]));
    tmp.ToString(publicId);
  }
  catch (Orthanc::OrthancException&)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem, 
                                    "Accessing an inexistent study with WADO-RS: " +
                                    std::string(request->groups[0]));
  }
  
  return true;
}


static bool LocateSeries(OrthancPluginRestOutput* output,
                         std::string& publicId,
                         const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
    return false;
  }

  try
  {
    OrthancPlugins::OrthancString tmp;
    tmp.Assign(OrthancPluginLookupSeries(context, request->groups[1]));
    tmp.ToString(publicId);
  }
  catch (Orthanc::OrthancException&)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem, 
                                    "Accessing an inexistent series with WADO-RS: " +
                                    std::string(request->groups[1]));
  }
  
  Json::Value study;
  if (!OrthancPlugins::RestApiGet(study, "/series/" + publicId + "/study", false))
  {
    OrthancPluginSendHttpStatusCode(context, output, 404);
    return false;
  }
  else if (study["MainDicomTags"]["StudyInstanceUID"].asString() != std::string(request->groups[0]))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem, 
                                    "No series " + std::string(request->groups[1]) + 
                                    " in study " + std::string(request->groups[0]));
  }
  else
  {
    return true;
  }
}


bool LocateInstance(OrthancPluginRestOutput* output,
                    std::string& publicId,
                    const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
    return false;
  }

  {
    OrthancPlugins::OrthancString tmp;
    tmp.Assign(OrthancPluginLookupInstance(context, request->groups[2]));

    if (tmp.GetContent() == NULL)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem,
                                      "Accessing an inexistent instance with WADO-RS: " + 
                                      std::string(request->groups[2]));
    }

    tmp.ToString(publicId);
  }
  
  Json::Value study, series;
  if (!OrthancPlugins::RestApiGet(series, "/instances/" + publicId + "/series", false) ||
      !OrthancPlugins::RestApiGet(study, "/instances/" + publicId + "/study", false))
  {
    OrthancPluginSendHttpStatusCode(context, output, 404);
    return false;
  }
  else if (study["MainDicomTags"]["StudyInstanceUID"].asString() != std::string(request->groups[0]) ||
           series["MainDicomTags"]["SeriesInstanceUID"].asString() != std::string(request->groups[1]))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem,
                                    "No instance " + std::string(request->groups[2]) + 
                                    " in study " + std::string(request->groups[0]) + " or " +
                                    " in series " + std::string(request->groups[1]));
  }
  else
  {
    return true;
  }
}


void RetrieveDicomStudy(OrthancPluginRestOutput* output,
                        const char* url,
                        const OrthancPluginHttpRequest* request)
{
  if (!AcceptMultipartDicom(request))
  {
    OrthancPluginSendHttpStatusCode(OrthancPlugins::GetGlobalContext(), output, 400 /* Bad request */);
  }
  else
  {
    std::string publicId;
    if (LocateStudy(output, publicId, request))
    {
      AnswerListOfDicomInstances(output, Orthanc::ResourceType_Study, publicId);
    }
  }
}


void RetrieveDicomSeries(OrthancPluginRestOutput* output,
                         const char* url,
                         const OrthancPluginHttpRequest* request)
{
  if (!AcceptMultipartDicom(request))
  {
    OrthancPluginSendHttpStatusCode(OrthancPlugins::GetGlobalContext(), output, 400 /* Bad request */);
  }
  else
  {
    std::string publicId;
    if (LocateSeries(output, publicId, request))
    {
      AnswerListOfDicomInstances(output, Orthanc::ResourceType_Series, publicId);
    }
  }
}



void RetrieveDicomInstance(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (!AcceptMultipartDicom(request))
  {
    OrthancPluginSendHttpStatusCode(context, output, 400 /* Bad request */);
  }
  else
  {
    std::string publicId;
    if (LocateInstance(output, publicId, request))
    {
      if (OrthancPluginStartMultipartAnswer(context, output, "related", "application/dicom"))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
      }

      OrthancPlugins::MemoryBuffer dicom;
      if (dicom.RestApiGet("/instances/" + publicId + "/file", false) &&
          OrthancPluginSendMultipartItem(context, output, dicom.GetData(), dicom.GetSize()) != 0)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
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
    std::string publicId;
    if (LocateStudy(output, publicId, request))
    {
      AnswerMetadata(output, request, Orthanc::ResourceType_Study, publicId, false, isXml);
    }
  }
}


void RetrieveSeriesMetadata(OrthancPluginRestOutput* output,
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
    std::string publicId;
    if (LocateSeries(output, publicId, request))
    {
      AnswerMetadata(output, request, Orthanc::ResourceType_Series, publicId, false, isXml);
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
    std::string publicId;
    if (LocateInstance(output, publicId, request))
    {
      AnswerMetadata(output, request, Orthanc::ResourceType_Instance, publicId, true, isXml);
    }
  }
}



static uint32_t Hex2Dec(char c)
{
  return (c >= '0' && c <= '9') ? c - '0' : c - 'a' + 10;
}


static bool ParseBulkTag(gdcm::Tag& tag,
                         const std::string& s)
{
  if (s.size() != 8)
  {
    return false;
  }

  for (size_t i = 0; i < 8; i++)
  {
    if ((s[i] < '0' || s[i] > '9') &&
        (s[i] < 'a' || s[i] > 'f'))
    {
      return false;
    }
  }

  uint32_t g = ((Hex2Dec(s[0]) << 12) +
                (Hex2Dec(s[1]) << 8) +
                (Hex2Dec(s[2]) << 4) +
                Hex2Dec(s[3]));

  uint32_t e = ((Hex2Dec(s[4]) << 12) +
                (Hex2Dec(s[5]) << 8) +
                (Hex2Dec(s[6]) << 4) +
                Hex2Dec(s[7]));

  tag = gdcm::Tag(g, e);
  return true;
}


static bool ExploreBulkData(std::string& content,
                            const std::vector<std::string>& path,
                            size_t position,
                            const gdcm::DataSet& dataset)
{
  gdcm::Tag tag;
  if (!ParseBulkTag(tag, path[position]) ||
      !dataset.FindDataElement(tag))
  {
    return false;
  }

  const gdcm::DataElement& element = dataset.GetDataElement(tag);

  if (position + 1 == path.size())
  {
    const gdcm::ByteValue* data = element.GetByteValue();
    if (!data)
    {
      content.clear();
    }
    else
    {
      content.assign(data->GetPointer(), data->GetLength());
    }

    return true;
  }

  return false;
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

  std::string uri;
  OrthancPlugins::MemoryBuffer content;
  if (LocateInstance(output, uri, request) &&
      content.RestApiGet(uri + "/file", false))
  {
    OrthancPlugins::ParsedDicomFile dicom(content);

    std::vector<std::string> path;
    Orthanc::Toolbox::TokenizeString(path, request->groups[3], '/');
      
    std::string result;
    if (path.size() % 2 == 1 &&
        ExploreBulkData(result, path, 0, dicom.GetDataSet()))
    {
      if (OrthancPluginStartMultipartAnswer(context, output, "related", "application/octet-stream") != 0 ||
          OrthancPluginSendMultipartItem(context, output, result.c_str(), result.size()) != 0)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_Plugin);
      }
    }
    else
    {
      OrthancPluginSendHttpStatusCode(context, output, 400 /* Bad request */);
    }      
  }
}
