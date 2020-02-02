/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2020 Osimis S.A., Belgium
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



static void WriteInstanceMetadata(OrthancPlugins::DicomWebFormatter::HttpWriter& writer,
                                  const std::string& orthancId,
                                  const std::string& studyInstanceUid,
                                  const std::string& seriesInstanceUid,
                                  const std::string& sopInstanceUid,
                                  const std::string& wadoBase)
{
  assert(!orthancId.empty() &&
         !studyInstanceUid.empty() &&
         !seriesInstanceUid.empty() &&
         !sopInstanceUid.empty() &&
         !wadoBase.empty());

  const std::string bulkRoot = (wadoBase +
                                "studies/" + studyInstanceUid +
                                "/series/" + seriesInstanceUid + 
                                "/instances/" + sopInstanceUid + "/bulk");
  
#if 1
  // On a SSD drive, this version is twice slower than if using
  // cache (see below)
    
  OrthancPlugins::MemoryBuffer dicom;
  if (dicom.RestApiGet("/instances/" + orthancId + "/file", false))
  {
    writer.AddDicom(dicom.GetData(), dicom.GetSize(), bulkRoot);
  }
#else

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
        OrthancPlugins::DicomWebFormatter::Locker locker(OrthancPluginDicomWebBinaryMode_Ignore, "");
        locker.Apply(dicomweb, OrthancPlugins::GetGlobalContext(),
                     buffer.GetData(), buffer.GetSize(), false /* JSON */);
      }

      buffer.RestApiPut("/instances/" + orthancId + "/attachments/4444", dicomweb, false);
      writer.AddDicomWebSerializedJson(dicomweb.c_str(), dicomweb.size());
    }
  }
#endif
}



static bool LocateStudy(OrthancPluginRestOutput* output,
                        std::string& orthancId,
                        std::string& studyInstanceUid,
                        const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
    return false;
  }

  studyInstanceUid = request->groups[0];

  try
  {
    OrthancPlugins::OrthancString tmp;
    tmp.Assign(OrthancPluginLookupStudy(context, studyInstanceUid.c_str()));

    if (tmp.GetContent() != NULL)
    {
      tmp.ToString(orthancId);
      return true;
    }
  }
  catch (Orthanc::OrthancException&)
  {
  }

  throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem, 
                                  "Accessing an inexistent study with WADO-RS: " + studyInstanceUid);
}


bool LocateSeries(OrthancPluginRestOutput* output,
                  std::string& orthancId,
                  std::string& studyInstanceUid,
                  std::string& seriesInstanceUid,
                  const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
    return false;
  }

  studyInstanceUid = request->groups[0];
  seriesInstanceUid = request->groups[1];

  bool found = false;

  try
  {
    OrthancPlugins::OrthancString tmp;
    tmp.Assign(OrthancPluginLookupSeries(context, seriesInstanceUid.c_str()));

    if (tmp.GetContent() != NULL)
    {
      tmp.ToString(orthancId);
      found = true;
    }
  }
  catch (Orthanc::OrthancException&)
  {
  }

  if (!found)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem, 
                                    "Accessing an inexistent series with WADO-RS: " + seriesInstanceUid);
  }
  
  Json::Value study;
  if (!OrthancPlugins::RestApiGet(study, "/series/" + orthancId + "/study", false))
  {
    OrthancPluginSendHttpStatusCode(context, output, 404);
    return false;
  }
  else if (study["MainDicomTags"]["StudyInstanceUID"].asString() != studyInstanceUid)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem, 
                                    "No series " + seriesInstanceUid + " in study " + studyInstanceUid);
  }
  else
  {
    return true;
  }
}


bool LocateInstance(OrthancPluginRestOutput* output,
                    std::string& orthancId,
                    std::string& studyInstanceUid,
                    std::string& seriesInstanceUid,
                    std::string& sopInstanceUid,
                    const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
    return false;
  }

  studyInstanceUid = request->groups[0];
  seriesInstanceUid = request->groups[1];
  sopInstanceUid = request->groups[2];
  
  {
    OrthancPlugins::OrthancString tmp;
    tmp.Assign(OrthancPluginLookupInstance(context, sopInstanceUid.c_str()));

    if (tmp.GetContent() == NULL)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem,
                                      "Accessing an inexistent instance with WADO-RS: " + sopInstanceUid);
    }

    tmp.ToString(orthancId);
  }
  
  Json::Value study, series;
  if (!OrthancPlugins::RestApiGet(series, "/instances/" + orthancId + "/series", false) ||
      !OrthancPlugins::RestApiGet(study, "/instances/" + orthancId + "/study", false))
  {
    OrthancPluginSendHttpStatusCode(context, output, 404);
    return false;
  }
  else if (study["MainDicomTags"]["StudyInstanceUID"].asString() != studyInstanceUid ||
           series["MainDicomTags"]["SeriesInstanceUID"].asString() != seriesInstanceUid)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem,
                                    "Instance " + sopInstanceUid + 
                                    " is not both in study " + studyInstanceUid +
                                    " and in series " + seriesInstanceUid);
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
    std::string orthancId, studyInstanceUid;
    if (LocateStudy(output, orthancId, studyInstanceUid, request))
    {
      AnswerListOfDicomInstances(output, Orthanc::ResourceType_Study, orthancId);
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
    std::string orthancId, studyInstanceUid, seriesInstanceUid;
    if (LocateSeries(output, orthancId, studyInstanceUid, seriesInstanceUid, request))
    {
      AnswerListOfDicomInstances(output, Orthanc::ResourceType_Series, orthancId);
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
    std::string orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid;
    if (LocateInstance(output, orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid, request))
    {
      if (OrthancPluginStartMultipartAnswer(context, output, "related", "application/dicom"))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
      }

      OrthancPlugins::MemoryBuffer dicom;
      if (dicom.RestApiGet("/instances/" + orthancId + "/file", false) &&
          OrthancPluginSendMultipartItem(context, output, dicom.GetData(), dicom.GetSize()) != 0)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
      }
    }
  }
}



namespace
{
  class Identifier
  {
  private:
    std::string  orthancId_;
    std::string  dicomUid_;

  public:
    Identifier(const std::string& orthancId,
               const std::string& dicomUid) :
      orthancId_(orthancId),
      dicomUid_(dicomUid)
    {
    }

    const std::string& GetOrthancId() const
    {
      return orthancId_;
    }

    const std::string& GetDicomUid() const
    {
      return dicomUid_;
    }
  };
}


static void GetChildrenIdentifiers(std::list<Identifier>& target,
                                   Orthanc::ResourceType level,
                                   const std::string& orthancId)
{
  static const char* const ID = "ID";
  static const char* const MAIN_DICOM_TAGS = "MainDicomTags";
  static const char* const SERIES_INSTANCE_UID = "SeriesInstanceUID";
  static const char* const SOP_INSTANCE_UID = "SOPInstanceUID";

  target.clear();

  const char* tag = NULL;
  std::string uri;

  switch (level)
  {
    case Orthanc::ResourceType_Study:
      uri = "/studies/" + orthancId + "/series";
      tag = SERIES_INSTANCE_UID;
      break;
       
    case Orthanc::ResourceType_Series:
      uri = "/series/" + orthancId + "/instances";
      tag = SOP_INSTANCE_UID;
      break;

    default:
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
  }

  assert(tag != NULL);
  
  Json::Value children;
  if (OrthancPlugins::RestApiGet(children, uri, false))
  {
    if (children.type() != Json::arrayValue)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
    }

    for (Json::Value::ArrayIndex i = 0; i < children.size(); i++)
    {
      if (children[i].type() != Json::objectValue ||
          !children[i].isMember(ID) ||
          !children[i].isMember(MAIN_DICOM_TAGS) ||
          children[i][ID].type() != Json::stringValue ||
          children[i][MAIN_DICOM_TAGS].type() != Json::objectValue ||
          !children[i][MAIN_DICOM_TAGS].isMember(tag) ||
          children[i][MAIN_DICOM_TAGS][tag].type() != Json::stringValue)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);
      }
      else
      {
        target.push_back(Identifier(children[i][ID].asString(),
                                    children[i][MAIN_DICOM_TAGS][tag].asString()));
                                    
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
    std::string studyOrthancId, studyInstanceUid;
    if (LocateStudy(output, studyOrthancId, studyInstanceUid, request))
    {
      OrthancPlugins::DicomWebFormatter::HttpWriter writer(output, isXml);

      std::list<Identifier> series;
      GetChildrenIdentifiers(series, Orthanc::ResourceType_Study, studyOrthancId);

      for (std::list<Identifier>::const_iterator a = series.begin(); a != series.end(); ++a)
      {
        std::list<Identifier> instances;
        GetChildrenIdentifiers(instances, Orthanc::ResourceType_Series, a->GetOrthancId());

        for (std::list<Identifier>::const_iterator b = instances.begin(); b != instances.end(); ++b)
        {
          WriteInstanceMetadata(writer, b->GetOrthancId(), studyInstanceUid, a->GetDicomUid(), b->GetDicomUid(),
                                OrthancPlugins::Configuration::GetBaseUrl(request));
        }
      }

      writer.Send();
    }
  }
}


void RetrieveSeriesMetadata(OrthancPluginRestOutput* output,
                            const char* url,
                            const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();
  
  bool isXml;
  if (!AcceptMetadata(request, isXml))
  {
    OrthancPluginSendHttpStatusCode(context, output, 400 /* Bad request */);
  }
  else
  {
    std::string seriesOrthancId, studyInstanceUid, seriesInstanceUid;
    if (LocateSeries(output, seriesOrthancId, studyInstanceUid, seriesInstanceUid, request))
    {
      OrthancPlugins::DicomWebFormatter::HttpWriter writer(output, isXml);
      
      std::list<Identifier> instances;
      GetChildrenIdentifiers(instances, Orthanc::ResourceType_Series, seriesOrthancId);

      for (std::list<Identifier>::const_iterator a = instances.begin(); a != instances.end(); ++a)
      {
        WriteInstanceMetadata(writer, a->GetOrthancId(), studyInstanceUid, seriesInstanceUid, a->GetDicomUid(),
                              OrthancPlugins::Configuration::GetBaseUrl(request));
      }

      writer.Send();
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
    std::string orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid;
    if (LocateInstance(output, orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid, request))
    {
      OrthancPlugins::DicomWebFormatter::HttpWriter writer(output, isXml);
      WriteInstanceMetadata(writer, orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid,
                            OrthancPlugins::Configuration::GetBaseUrl(request));
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
