/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2015 Sebastien Jodogne, Medical Physics
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


#include "StowRs.h"
#include "Plugin.h"

#include "Configuration.h"
#include "Dicom.h"
#include "../Orthanc/Core/Toolbox.h"


static void SetTag(gdcm::DataSet& dataset,
                   const gdcm::Tag& tag,
                   const gdcm::VR& vr,
                   const std::string& value)
{
  gdcm::DataElement element(tag);
  element.SetVR(vr);
  element.SetByteValue(value.c_str(), value.size());
  dataset.Insert(element);
}


static void SetSequenceTag(gdcm::DataSet& dataset,
                           const gdcm::Tag& tag,
                           gdcm::SmartPointer<gdcm::SequenceOfItems>& sequence)
{
  gdcm::DataElement element;
  element.SetTag(tag);
  element.SetVR(gdcm::VR::SQ);
  element.SetValue(*sequence);
  element.SetVLToUndefined();
  dataset.Insert(element);
}



bool IsXmlExpected(const OrthancPluginHttpRequest* request)
{
  std::string accept;

  if (!OrthancPlugins::LookupHttpHeader(accept, request, "accept"))
  {
    return true;   // By default, return XML Native DICOM Model
  }

  Orthanc::Toolbox::ToLowerCase(accept);
  if (accept == "application/json")
  {
    return false;
  }

  if (accept != "application/dicom+xml" &&
      accept != "application/xml" &&
      accept != "text/xml" &&
      accept != "*/*")
  {
    std::string s = "Unsupported return MIME type: " + accept + ", will return XML";
    OrthancPluginLogError(context_, s.c_str());
  }

  return true;
}



int32_t StowCallback(OrthancPluginRestOutput* output,
                     const char* url,
                     const OrthancPluginHttpRequest* request)
{
  try
  {
    const std::string wadoBase = OrthancPlugins::Configuration::GetBaseUrl(configuration_, request);


    if (request->method != OrthancPluginHttpMethod_Post)
    {
      OrthancPluginSendMethodNotAllowed(context_, output, "POST");
      return 0;
    }

    std::string expectedStudy;
    if (request->groupsCount == 1)
    {
      expectedStudy = request->groups[0];
    }

    if (expectedStudy.empty())
    {
      OrthancPluginLogInfo(context_, "STOW-RS request without study");
    }
    else
    {
      std::string s = "STOW-RS request restricted to study UID " + expectedStudy;
      OrthancPluginLogInfo(context_, s.c_str());
    }

    bool isXml = IsXmlExpected(request);

    std::string header;
    if (!OrthancPlugins::LookupHttpHeader(header, request, "content-type"))
    {
      OrthancPluginLogError(context_, "No content type in the HTTP header of a STOW-RS request");
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
      return 0;    
    }

    std::string application;
    std::map<std::string, std::string> attributes;
    OrthancPlugins::ParseContentType(application, attributes, header);

    if (application != "multipart/related" ||
        attributes.find("type") == attributes.end() ||
        attributes.find("boundary") == attributes.end())
    {
      std::string s = "Unable to parse the content type of a STOW-RS request (" + application + ")";
      OrthancPluginLogError(context_, s.c_str());
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
      return 0;
    }


    std::string boundary = attributes["boundary"]; 

    if (attributes["type"] != "application/dicom")
    {
      OrthancPluginLogError(context_, "The STOW-RS plugin currently only supports application/dicom");
      OrthancPluginSendHttpStatusCode(context_, output, 415 /* Unsupported media type */);
      return 0;
    }



    bool isFirst = true;
    gdcm::DataSet result;
    gdcm::SmartPointer<gdcm::SequenceOfItems> success = new gdcm::SequenceOfItems();
    gdcm::SmartPointer<gdcm::SequenceOfItems> failed = new gdcm::SequenceOfItems();
  
    std::vector<OrthancPlugins::MultipartItem> items;
    OrthancPlugins::ParseMultipartBody(items, request->body, request->bodySize, boundary);
    for (size_t i = 0; i < items.size(); i++)
    {
      if (!items[i].contentType_.empty() &&
          items[i].contentType_ != "application/dicom")
      {
        std::string s = "The STOW-RS request contains a part that is not application/dicom (it is: \"" + items[i].contentType_ + "\")";
          OrthancPluginLogError(context_, s.c_str());
        OrthancPluginSendHttpStatusCode(context_, output, 415 /* Unsupported media type */);
        return 0;
      }

      OrthancPlugins::ParsedDicomFile dicom(items[i]);

      std::string studyInstanceUid = dicom.GetTagWithDefault(OrthancPlugins::DICOM_TAG_STUDY_INSTANCE_UID, "", true);
      std::string sopClassUid = dicom.GetTagWithDefault(OrthancPlugins::DICOM_TAG_SOP_CLASS_UID, "", true);
      std::string sopInstanceUid = dicom.GetTagWithDefault(OrthancPlugins::DICOM_TAG_SOP_INSTANCE_UID, "", true);

      gdcm::Item item;
      item.SetVLToUndefined();
      gdcm::DataSet &status = item.GetNestedDataSet();

      SetTag(status, OrthancPlugins::DICOM_TAG_REFERENCED_SOP_CLASS_UID, gdcm::VR::UI, sopClassUid);
      SetTag(status, OrthancPlugins::DICOM_TAG_REFERENCED_SOP_INSTANCE_UID, gdcm::VR::UI, sopInstanceUid);

      if (!expectedStudy.empty() &&
          studyInstanceUid != expectedStudy)
      {
        std::string s = ("STOW-RS request restricted to study [" + expectedStudy + 
                         "]: Ignoring instance from study [" + studyInstanceUid + "]");
        OrthancPluginLogInfo(context_, s.c_str());

        SetTag(status, OrthancPlugins::DICOM_TAG_WARNING_REASON, gdcm::VR::US, "B006");  // Elements discarded
        success->AddItem(item);      
      }
      else
      {
        if (isFirst)
        {
          std::string url = wadoBase + "studies/" + studyInstanceUid;
          SetTag(result, OrthancPlugins::DICOM_TAG_RETRIEVE_URL, gdcm::VR::UT, url);
          isFirst = false;
        }

        OrthancPluginMemoryBuffer result;
        bool ok = OrthancPluginRestApiPost(context_, &result, "/instances", items[i].data_, items[i].size_) == 0;
        OrthancPluginFreeMemoryBuffer(context_, &result);

        if (ok)
        {
          std::string url = (wadoBase + 
                             "studies/" + studyInstanceUid +
                             "/series/" + dicom.GetTagWithDefault(OrthancPlugins::DICOM_TAG_SERIES_INSTANCE_UID, "", true) +
                             "/instances/" + sopInstanceUid);

          SetTag(status, OrthancPlugins::DICOM_TAG_RETRIEVE_URL, gdcm::VR::UT, url);
          success->AddItem(item);
        }
        else
        {
          OrthancPluginLogError(context_, "Orthanc was unable to store instance through STOW-RS request");
          SetTag(status, OrthancPlugins::DICOM_TAG_FAILURE_REASON, gdcm::VR::US, "0110");  // Processing failure
          failed->AddItem(item);
        }
      }
    }

    SetSequenceTag(result, OrthancPlugins::DICOM_TAG_FAILED_SOP_SEQUENCE, failed);
    SetSequenceTag(result, OrthancPlugins::DICOM_TAG_REFERENCED_SOP_SEQUENCE, success);

    OrthancPlugins::AnswerDicom(context_, output, wadoBase, *dictionary_, result, isXml, false);

    return 0;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}
