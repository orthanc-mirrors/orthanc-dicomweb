/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
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


#include "WadoUri.h"
#include "Plugin.h"

#include "../Orthanc/Core/OrthancException.h"
#include "Configuration.h"

#include <string>

static bool MapWadoToOrthancIdentifier(std::string& orthanc,
                                       char* (*func) (OrthancPluginContext*, const char*),
                                       const std::string& dicom)
{
  char* tmp = func(OrthancPlugins::Configuration::GetContext(), dicom.c_str());

  if (tmp)
  {
    orthanc = tmp;
    OrthancPluginFreeString(OrthancPlugins::Configuration::GetContext(), tmp);
    return true;
  }
  else
  {
    return false;
  }
}


static bool LocateInstance(std::string& instance,
                           std::string& contentType,
                           const OrthancPluginHttpRequest* request)
{
  std::string requestType, studyUid, seriesUid, objectUid;

  for (uint32_t i = 0; i < request->getCount; i++)
  {
    std::string key(request->getKeys[i]);
    std::string value(request->getValues[i]);

    if (key == "studyUID")
    {
      studyUid = value;
    }
    else if (key == "seriesUID")
    {
      seriesUid = value;
    }
    else if (key == "objectUID")  // In WADO-URI, "objectUID" corresponds to "SOPInstanceUID"
    {
      objectUid = value;
    }
    else if (key == "requestType")
    {
      requestType = value;
    }
    else if (key == "contentType")
    {
      contentType = value;
    }
  }

  if (requestType != "WADO")
  {
    OrthancPlugins::Configuration::LogError("WADO-URI: Invalid requestType: \"" + requestType + "\"");
    return false;
  }

  if (objectUid.empty())
  {
    OrthancPlugins::Configuration::LogError("WADO-URI: No SOPInstanceUID provided");
    return false;
  }

  if (!MapWadoToOrthancIdentifier(instance, OrthancPluginLookupInstance, objectUid))
  {
    OrthancPlugins::Configuration::LogError("WADO-URI: No such SOPInstanceUID in Orthanc: \"" + objectUid + "\"");
    return false;
  }

  /**
   * Below are only sanity checks to ensure that the possibly provided
   * "seriesUID" and "studyUID" match that of the provided instance.
   **/

  if (!seriesUid.empty())
  {
    std::string series;
    if (!MapWadoToOrthancIdentifier(series, OrthancPluginLookupSeries, seriesUid))
    {
      OrthancPlugins::Configuration::LogError("WADO-URI: No such SeriesInstanceUID in Orthanc: \"" + seriesUid + "\"");
      return false;
    }
    else
    {
      Json::Value info;
      if (!OrthancPlugins::RestApiGetJson(info, OrthancPlugins::Configuration::GetContext(), "/instances/" + instance + "/series") ||
          info["MainDicomTags"]["SeriesInstanceUID"] != seriesUid)
      {
        OrthancPlugins::Configuration::LogError("WADO-URI: Instance " + objectUid + " does not belong to series " + seriesUid);
        return false;
      }
    }
  }
  
  if (!studyUid.empty())
  {
    std::string study;
    if (!MapWadoToOrthancIdentifier(study, OrthancPluginLookupStudy, studyUid))
    {
      OrthancPlugins::Configuration::LogError("WADO-URI: No such StudyInstanceUID in Orthanc: \"" + studyUid + "\"");
      return false;
    }
    else
    {
      Json::Value info;
      if (!OrthancPlugins::RestApiGetJson(info, OrthancPlugins::Configuration::GetContext(), "/instances/" + instance + "/study") ||
          info["MainDicomTags"]["StudyInstanceUID"] != studyUid)
      {
        OrthancPlugins::Configuration::LogError("WADO-URI: Instance " + objectUid + " does not belong to study " + studyUid);
        return false;
      }
    }
  }
  
  return true;
}


static void AnswerDicom(OrthancPluginRestOutput* output,
                        const std::string& instance)
{
  std::string uri = "/instances/" + instance + "/file";

  std::string dicom;
  if (OrthancPlugins::RestApiGetString(dicom, OrthancPlugins::Configuration::GetContext(), uri))
  {
    OrthancPluginAnswerBuffer(OrthancPlugins::Configuration::GetContext(), output, dicom.c_str(), dicom.size(), "application/dicom");
  }
  else
  {
    OrthancPlugins::Configuration::LogError("WADO-URI: Unable to retrieve DICOM file from " + uri);
    throw Orthanc::OrthancException(Orthanc::ErrorCode_Plugin);
  }
}


static bool RetrievePngPreview(std::string& png,
                               const std::string& instance)
{
  std::string uri = "/instances/" + instance + "/preview";

  if (OrthancPlugins::RestApiGetString(png, OrthancPlugins::Configuration::GetContext(), uri, true))
  {
    return true;
  }
  else
  {
    OrthancPlugins::Configuration::LogError("WADO-URI: Unable to generate a preview image for " + uri);
    return false;
  }
}


static void AnswerPngPreview(OrthancPluginRestOutput* output,
                             const std::string& instance)
{
  std::string png;
  if (RetrievePngPreview(png, instance))
  {
    OrthancPluginAnswerBuffer(OrthancPlugins::Configuration::GetContext(), output, png.c_str(), png.size(), "image/png");
  }
  else
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_Plugin);
  }
}


static void AnswerJpegPreview(OrthancPluginRestOutput* output,
                              const std::string& instance)
{
  // Retrieve the preview in the PNG format
  std::string png;
  if (!RetrievePngPreview(png, instance))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_Plugin);
  }

  // Decode the PNG file
  OrthancPluginImage* image = OrthancPluginUncompressImage(
    OrthancPlugins::Configuration::GetContext(), png.c_str(), png.size(), OrthancPluginImageFormat_Png);

  // Convert to JPEG
  OrthancPluginCompressAndAnswerJpegImage(
    OrthancPlugins::Configuration::GetContext(), output, 
    OrthancPluginGetImagePixelFormat(OrthancPlugins::Configuration::GetContext(), image),
    OrthancPluginGetImageWidth(OrthancPlugins::Configuration::GetContext(), image),
    OrthancPluginGetImageHeight(OrthancPlugins::Configuration::GetContext(), image),
    OrthancPluginGetImagePitch(OrthancPlugins::Configuration::GetContext(), image),
    OrthancPluginGetImageBuffer(OrthancPlugins::Configuration::GetContext(), image), 
    90 /*quality*/);

  OrthancPluginFreeImage(OrthancPlugins::Configuration::GetContext(), image);
}


void WadoUriCallback(OrthancPluginRestOutput* output,
                     const char* url,
                     const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "GET");
    return;
  }

  std::string instance;
  std::string contentType = "image/jpg";  // By default, JPEG image will be returned
  if (!LocateInstance(instance, contentType, request))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource);
  }

  if (contentType == "application/dicom")
  {
    AnswerDicom(output, instance);
  }
  else if (contentType == "image/png")
  {
    AnswerPngPreview(output, instance);
  }
  else if (contentType == "image/jpeg" ||
           contentType == "image/jpg")
  {
    AnswerJpegPreview(output, instance);
  }
  else
  {
    OrthancPlugins::Configuration::LogError("WADO-URI: Unsupported content type: \"" + contentType + "\"");
    throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest);
  }
}
