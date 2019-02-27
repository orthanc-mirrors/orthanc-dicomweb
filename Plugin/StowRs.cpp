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


#include "StowRs.h"
#include "Plugin.h"

#include "Configuration.h"
#include "DicomWebFormatter.h"

#include <Core/Toolbox.h>

#include <stdexcept>


bool IsXmlExpected(const OrthancPluginHttpRequest* request)
{
  std::string accept;

  if (!OrthancPlugins::LookupHttpHeader(accept, request, "accept"))
  {
    return false;   // By default, return DICOM+JSON
  }

  Orthanc::Toolbox::ToLowerCase(accept);
  if (accept == "application/dicom+json" ||
      accept == "application/json" ||
      accept == "*/*")
  {
    return false;
  }
  else if (accept == "application/dicom+xml" ||
           accept == "application/xml" ||
           accept == "text/xml")
  {
    return true;
  }
  else
  {
    OrthancPlugins::LogError("Unsupported return MIME type: " + accept +
                             ", will return DICOM+JSON");
    return false;
  }
}



void StowCallback(OrthancPluginRestOutput* output,
                  const char* url,
                  const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  const std::string wadoBase = OrthancPlugins::Configuration::GetBaseUrl(request);

  if (request->method != OrthancPluginHttpMethod_Post)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "POST");
    return;
  }

  std::string expectedStudy;
  if (request->groupsCount == 1)
  {
    expectedStudy = request->groups[0];
  }

  if (expectedStudy.empty())
  {
    OrthancPlugins::LogInfo("STOW-RS request without study");
  }
  else
  {
    OrthancPlugins::LogInfo("STOW-RS request restricted to study UID " + expectedStudy);
  }

  std::string header;
  if (!OrthancPlugins::LookupHttpHeader(header, request, "content-type"))
  {
    OrthancPlugins::LogError("No content type in the HTTP header of a STOW-RS request");
    OrthancPluginSendHttpStatusCode(context, output, 400 /* Bad request */);
    return;
  }

  std::string application;
  std::map<std::string, std::string> attributes;
  OrthancPlugins::ParseContentType(application, attributes, header);

  if (application != "multipart/related" ||
      attributes.find("type") == attributes.end() ||
      attributes.find("boundary") == attributes.end())
  {
    OrthancPlugins::LogError("Unable to parse the content type of a STOW-RS request (" + application + ")");
    OrthancPluginSendHttpStatusCode(context, output, 400 /* Bad request */);
    return;
  }


  std::string boundary = attributes["boundary"]; 

  if (attributes["type"] != "application/dicom")
  {
    OrthancPlugins::LogError("The STOW-RS plugin currently only supports application/dicom");
    OrthancPluginSendHttpStatusCode(context, output, 415 /* Unsupported media type */);
    return;
  }


  bool isFirst = true;

  Json::Value result = Json::objectValue;
  Json::Value success = Json::arrayValue;
  Json::Value failed = Json::arrayValue;
  
  std::vector<OrthancPlugins::MultipartItem> items;
  OrthancPlugins::ParseMultipartBody(items, request->body, request->bodySize, boundary);

  for (size_t i = 0; i < items.size(); i++)
  {
    OrthancPlugins::LogInfo("Detected multipart item with content type \"" + 
                            items[i].contentType_ + "\" of size " + 
                            boost::lexical_cast<std::string>(items[i].size_));
  }  

  for (size_t i = 0; i < items.size(); i++)
  {
    if (!items[i].contentType_.empty() &&
        items[i].contentType_ != "application/dicom")
    {
      OrthancPlugins::LogError("The STOW-RS request contains a part that is not "
                               "\"application/dicom\" (it is: \"" + items[i].contentType_ + "\")");
      OrthancPluginSendHttpStatusCode(context, output, 415 /* Unsupported media type */);
      return;
    }

    Json::Value dicom;

    try
    {
      OrthancPlugins::OrthancString s;
      s.Assign(OrthancPluginDicomBufferToJson(context, items[i].data_, items[i].size_,
                                              OrthancPluginDicomToJsonFormat_Short,
                                              OrthancPluginDicomToJsonFlags_None, 256));
      s.ToJson(dicom);
    }
    catch (Orthanc::OrthancException&)
    {
      // Bad DICOM file => TODO add to error
      OrthancPlugins::LogWarning("STOW-RS cannot parse an incoming DICOM file");
      continue;
    }           

    if (dicom.type() != Json::objectValue ||
        !dicom.isMember(Orthanc::DICOM_TAG_SERIES_INSTANCE_UID.Format()) ||
        !dicom.isMember(Orthanc::DICOM_TAG_SOP_CLASS_UID.Format()) ||
        !dicom.isMember(Orthanc::DICOM_TAG_SOP_INSTANCE_UID.Format()) ||
        !dicom.isMember(Orthanc::DICOM_TAG_STUDY_INSTANCE_UID.Format()) ||
        dicom[Orthanc::DICOM_TAG_SERIES_INSTANCE_UID.Format()].type() != Json::stringValue ||
        dicom[Orthanc::DICOM_TAG_SOP_CLASS_UID.Format()].type() != Json::stringValue ||
        dicom[Orthanc::DICOM_TAG_SOP_INSTANCE_UID.Format()].type() != Json::stringValue ||
        dicom[Orthanc::DICOM_TAG_STUDY_INSTANCE_UID.Format()].type() != Json::stringValue)
    {
      OrthancPlugins::LogWarning("STOW-RS: Missing a mandatory tag in incoming DICOM file");
      continue;
    }

    const std::string seriesInstanceUid = dicom[Orthanc::DICOM_TAG_SERIES_INSTANCE_UID.Format()].asString();
    const std::string sopClassUid = dicom[Orthanc::DICOM_TAG_SOP_CLASS_UID.Format()].asString();
    const std::string sopInstanceUid = dicom[Orthanc::DICOM_TAG_SOP_INSTANCE_UID.Format()].asString();
    const std::string studyInstanceUid = dicom[Orthanc::DICOM_TAG_STUDY_INSTANCE_UID.Format()].asString();

    Json::Value item = Json::objectValue;
    item[OrthancPlugins::DICOM_TAG_REFERENCED_SOP_CLASS_UID.Format()] = sopClassUid;
    item[OrthancPlugins::DICOM_TAG_REFERENCED_SOP_INSTANCE_UID.Format()] = sopInstanceUid;

    if (!expectedStudy.empty() &&
        studyInstanceUid != expectedStudy)
    {
      OrthancPlugins::LogInfo("STOW-RS request restricted to study [" + expectedStudy + 
                              "]: Ignoring instance from study [" + studyInstanceUid + "]");

      /*item[OrthancPlugins::DICOM_TAG_WARNING_REASON.Format()] =
        boost::lexical_cast<std::string>(0xB006);  // Elements discarded
        success.append(item);*/
    }
    else
    {
      if (isFirst)
      {
        std::string url = wadoBase + "studies/" + studyInstanceUid;
        result[OrthancPlugins::DICOM_TAG_RETRIEVE_URL.Format()] = url;
        isFirst = false;
      }

      OrthancPlugins::MemoryBuffer tmp;
      bool ok = tmp.RestApiPost("/instances", items[i].data_, items[i].size_, false);
      tmp.Clear();

      if (ok)
      {
        std::string url = (wadoBase + 
                           "studies/" + studyInstanceUid +
                           "/series/" + seriesInstanceUid +
                           "/instances/" + sopInstanceUid);

        item[OrthancPlugins::DICOM_TAG_RETRIEVE_URL.Format()] = url;
        success.append(item);      
      }
      else
      {
        OrthancPlugins::LogError("Orthanc was unable to store instance through STOW-RS request");
        item[OrthancPlugins::DICOM_TAG_FAILURE_REASON.Format()] =
          boost::lexical_cast<std::string>(0x0110);  // Processing failure
        failed.append(item);      
      }
    }
  }

  result[OrthancPlugins::DICOM_TAG_FAILED_SOP_SEQUENCE.Format()] = failed;
  result[OrthancPlugins::DICOM_TAG_REFERENCED_SOP_SEQUENCE.Format()] = success;

  const bool isXml = IsXmlExpected(request);
  std::string answer;
  
  {
    OrthancPlugins::DicomWebFormatter::Locker locker(OrthancPluginDicomWebBinaryMode_Ignore, "");
    locker.Apply(answer, context, result, isXml);
  }

  OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(),
                            isXml ? "application/dicom+xml" : "application/dicom+json");
}
