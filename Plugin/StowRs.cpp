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


#include "StowRs.h"

#include "Configuration.h"
#include "DicomWebFormatter.h"


namespace OrthancPlugins
{
  StowServer::StowServer(OrthancPluginContext* context,
                         const std::map<std::string, std::string>& headers,
                         const std::string& expectedStudy) :
    context_(context),
    xml_(Configuration::IsXmlExpected(headers)),
    wadoBase_(Configuration::GetBaseUrl(headers)),
    expectedStudy_(expectedStudy),
    isFirst_(true),
    result_(Json::objectValue),
    success_(Json::arrayValue),
    failed_(Json::arrayValue)
  { 
    std::string tmp, contentType, subType, boundary;
    if (!Orthanc::MultipartStreamReader::GetMainContentType(tmp, headers) ||
        !Orthanc::MultipartStreamReader::ParseMultipartContentType(contentType, subType, boundary, tmp))
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_UnsupportedMediaType,
                                      "The STOW-RS server expects a multipart body in its request");
    }

    if (contentType != "multipart/related")
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_UnsupportedMediaType,
                                      "The Content-Type of a STOW-RS request must be \"multipart/related\"");
    }

    if (subType != "application/dicom")
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_UnsupportedMediaType,
                                      "The STOW-RS plugin currently only supports \"application/dicom\" subtype");
    }

    parser_.reset(new Orthanc::MultipartStreamReader(boundary));
    parser_->SetHandler(*this);
  }


  void StowServer::HandlePart(const Orthanc::MultipartStreamReader::HttpHeaders& headers,
                              const void* part,
                              size_t size)
  {
    std::string contentType;

    if (!Orthanc::MultipartStreamReader::GetMainContentType(contentType, headers) ||
        contentType != "application/dicom")
    {
      throw Orthanc::OrthancException(
        Orthanc::ErrorCode_UnsupportedMediaType,
        "The STOW-RS request contains a part that is not "
        "\"application/dicom\" (it is: \"" + contentType + "\")");
    }

    Json::Value dicom;
    bool ok = false;

    try
    {
      OrthancString s;
      s.Assign(OrthancPluginDicomBufferToJson(context_, part, size,
                                              OrthancPluginDicomToJsonFormat_Short,
                                              OrthancPluginDicomToJsonFlags_None, 256));

      if (s.GetContent() != NULL)
      {
        ok = true;
        s.ToJson(dicom);
      }
    }
    catch (Orthanc::OrthancException&)
    {
    }           

    if (!ok)
    {
      // Bad DICOM file => TODO add to error
      LogWarning("STOW-RS cannot parse an incoming DICOM file");
      return;
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
      LogWarning("STOW-RS: Missing a mandatory tag in incoming DICOM file");
      return;
    }

    const std::string seriesInstanceUid = dicom[Orthanc::DICOM_TAG_SERIES_INSTANCE_UID.Format()].asString();
    const std::string sopClassUid = dicom[Orthanc::DICOM_TAG_SOP_CLASS_UID.Format()].asString();
    const std::string sopInstanceUid = dicom[Orthanc::DICOM_TAG_SOP_INSTANCE_UID.Format()].asString();
    const std::string studyInstanceUid = dicom[Orthanc::DICOM_TAG_STUDY_INSTANCE_UID.Format()].asString();

    Json::Value item = Json::objectValue;
    item[DICOM_TAG_REFERENCED_SOP_CLASS_UID.Format()] = sopClassUid;
    item[DICOM_TAG_REFERENCED_SOP_INSTANCE_UID.Format()] = sopInstanceUid;
      
    if (!expectedStudy_.empty() &&
        studyInstanceUid != expectedStudy_)
    {
      LogInfo("STOW-RS request restricted to study [" + expectedStudy_ + 
              "]: Ignoring instance from study [" + studyInstanceUid + "]");

      /*item[DICOM_TAG_WARNING_REASON.Format()] =
        boost::lexical_cast<std::string>(0xB006);  // Elements discarded
        success.append(item);*/
    }
    else
    {
      if (isFirst_)
      {
        std::string url = wadoBase_ + "studies/" + studyInstanceUid;
        result_[DICOM_TAG_RETRIEVE_URL.Format()] = url;
        isFirst_ = false;
      }

      MemoryBuffer tmp;
      bool ok = tmp.RestApiPost("/instances", part, size, false);
      tmp.Clear();

      if (ok)
      {
        std::string url = (wadoBase_ + 
                           "studies/" + studyInstanceUid +
                           "/series/" + seriesInstanceUid +
                           "/instances/" + sopInstanceUid);

        item[DICOM_TAG_RETRIEVE_URL.Format()] = url;
        success_.append(item);      
      }
      else
      {
        LogError("Orthanc was unable to store one instance in a STOW-RS request");
        item[DICOM_TAG_FAILURE_REASON.Format()] =
          boost::lexical_cast<std::string>(0x0110);  // Processing failure
        failed_.append(item);
      }
    }
  }


  void StowServer::AddChunk(const void* data,
                            size_t size)
  {
    assert(parser_.get() != NULL);
    parser_->AddChunk(data, size);
  }


  void StowServer::Execute(OrthancPluginRestOutput* output)
  {
    assert(parser_.get() != NULL);
    parser_->CloseStream();

    result_[DICOM_TAG_FAILED_SOP_SEQUENCE.Format()] = failed_;
    result_[DICOM_TAG_REFERENCED_SOP_SEQUENCE.Format()] = success_;
    
    std::string answer;
    
    DicomWebFormatter::Apply(answer, context_, result_, xml_,
                             OrthancPluginDicomWebBinaryMode_Ignore, "");
      
    OrthancPluginAnswerBuffer(context_, output, answer.c_str(), answer.size(),
                              xml_ ? "application/dicom+xml" : "application/dicom+json");
  };

  
  IChunkedRequestReader* StowServer::PostCallback(const char* url,
                                                  const OrthancPluginHttpRequest* request)
  {
    OrthancPluginContext* context = GetGlobalContext();
  
    if (request->method != OrthancPluginHttpMethod_Post)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
    }

    std::map<std::string, std::string> headers;
    for (uint32_t i = 0; i < request->headersCount; i++)
    {
      headers[request->headersKeys[i]] = request->headersValues[i];
    }

    std::string expectedStudy;
    if (request->groupsCount == 1)
    {
      expectedStudy = request->groups[0];
    }

    if (expectedStudy.empty())
    {
      LogInfo("STOW-RS request without study");
    }
    else
    {
      LogInfo("STOW-RS request restricted to study UID " + expectedStudy);
    }

    return new StowServer(context, headers, expectedStudy);
  }
}
