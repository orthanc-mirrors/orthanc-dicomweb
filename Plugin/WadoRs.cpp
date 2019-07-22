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


#include "Configuration.h"
#include "DicomWebFormatter.h"

#include <Core/ChunkedBuffer.h>
#include <Core/Toolbox.h>
#include <Plugins/Samples/Common/OrthancPluginCppWrapper.h>

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

  static const char* const TRANSFER_SYNTAX = "transfer-syntax";

  /**
   * The "*" case below is related to Google Healthcare API:
   * https://groups.google.com/d/msg/orthanc-users/w1Ekrsc6-U8/T2a_DoQ5CwAJ
   **/
  if (attributes.find(TRANSFER_SYNTAX) != attributes.end() &&
      attributes[TRANSFER_SYNTAX] != "*")
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
  if (OrthancPlugins::RestApiGet(dicom, "/instances/" + orthancId + "/tags?short", false) &&
      dicom.isMember(STUDY_INSTANCE_UID) &&
      dicom.isMember(SERIES_INSTANCE_UID) &&
      dicom.isMember(SOP_INSTANCE_UID) &&
      dicom[STUDY_INSTANCE_UID].type() == Json::stringValue &&
      dicom[SERIES_INSTANCE_UID].type() == Json::stringValue &&
      dicom[SOP_INSTANCE_UID].type() == Json::stringValue)
  {
    studyInstanceUid = dicom[STUDY_INSTANCE_UID].asString();
    seriesInstanceUid = dicom[SERIES_INSTANCE_UID].asString();
    sopInstanceUid = dicom[SOP_INSTANCE_UID].asString();
    return true;
  }
  else
  {
    return false;
  }
}



static void AnswerMetadata(OrthancPluginRestOutput* output,
                           const OrthancPluginHttpRequest* request,
                           Orthanc::ResourceType level,
                           const std::string& resource,
                           bool isInstance,
                           bool isXml)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

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

  OrthancPlugins::DicomWebFormatter::HttpWriter writer(output, isXml);

  for (std::list<std::string>::const_iterator
         it = instances.begin(); it != instances.end(); ++it)
  {
    std::string studyInstanceUid, seriesInstanceUid, sopInstanceUid;
    if (GetDicomIdentifiers(studyInstanceUid, seriesInstanceUid, sopInstanceUid, *it))
    {
      const std::string bulkRoot = (wadoBase +
                                    "studies/" + studyInstanceUid +
                                    "/series/" + seriesInstanceUid + 
                                    "/instances/" + sopInstanceUid + "/bulk");
      
      OrthancPlugins::MemoryBuffer dicom;
      if (dicom.RestApiGet("/instances/" + *it + "/file", false))
      {
        writer.AddDicom(dicom.GetData(), dicom.GetSize(), bulkRoot);
      }
    }
  }

  writer.Send();
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
                                    "Instance " + std::string(request->groups[2]) + 
                                    " is not both in study " + std::string(request->groups[0]) +
                                    " and in series " + std::string(request->groups[1]));
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

  std::string publicId;
  OrthancPlugins::MemoryBuffer content;
  if (LocateInstance(output, publicId, request) &&
      content.RestApiGet("/instances/" + publicId + "/file", false))
  {
    std::string bulk(request->groups[3]);

    std::vector<std::string> path;
    Orthanc::Toolbox::TokenizeString(path, bulk, '/');

    // Map the bulk data URI to the Orthanc "/instances/.../content/..." built-in URI
    std::string orthanc = "/instances/" + publicId + "/content";

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
