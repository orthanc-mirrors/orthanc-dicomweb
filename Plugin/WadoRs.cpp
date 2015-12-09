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


#include "Plugin.h"

#include <boost/lexical_cast.hpp>

#include "Configuration.h"
#include "Dicom.h"
#include "DicomResults.h"
#include "../Orthanc/Core/Toolbox.h"
#include "../Orthanc/Core/OrthancException.h"

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
    std::string s = "This WADO-RS plugin cannot generate the following content type: " + accept;
    OrthancPluginLogError(context_, s.c_str());
    return false;
  }

  if (attributes.find("type") != attributes.end())
  {
    std::string s = attributes["type"];
    Orthanc::Toolbox::ToLowerCase(s);
    if (s != "application/dicom")
    {
      std::string s = "This WADO-RS plugin only supports application/dicom return type for DICOM retrieval (" + accept + ")";
      OrthancPluginLogError(context_, s.c_str());
      return false;
    }
  }

  if (attributes.find("transfer-syntax") != attributes.end())
  {
    std::string s = "This WADO-RS plugin cannot change the transfer syntax to " + attributes["transfer-syntax"];
    OrthancPluginLogError(context_, s.c_str());
    return false;
  }

  return true;
}



static bool AcceptMetadata(const OrthancPluginHttpRequest* request,
                           bool& isXml)
{
  isXml = true;

  std::string accept;

  if (!OrthancPlugins::LookupHttpHeader(accept, request, "accept"))
  {
    // By default, return "multipart/related; type=application/dicom+xml;"
    return true;
  }

  std::string application;
  std::map<std::string, std::string> attributes;
  OrthancPlugins::ParseContentType(application, attributes, accept);

  if (application == "application/json")
  {
    isXml = false;
    return true;
  }

  if (application != "multipart/related" &&
      application != "*/*")
  {
    std::string s = "This WADO-RS plugin cannot generate the following content type: " + accept;
    OrthancPluginLogError(context_, s.c_str());
    return false;
  }

  if (attributes.find("type") != attributes.end())
  {
    std::string s = attributes["type"];
    Orthanc::Toolbox::ToLowerCase(s);
    if (s != "application/dicom+xml")
    {
      std::string s = "This WADO-RS plugin only supports application/json or application/dicom+xml return types for metadata (" + accept + ")";
      OrthancPluginLogError(context_, s.c_str());
      return false;
    }
  }

  if (attributes.find("transfer-syntax") != attributes.end())
  {
    std::string s = "This WADO-RS plugin cannot change the transfer syntax to " + attributes["transfer-syntax"];
    OrthancPluginLogError(context_, s.c_str());
    return false;
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
    std::string s = "This WADO-RS plugin cannot generate the following bulk data type: " + accept;
    OrthancPluginLogError(context_, s.c_str());
    return false;
  }

  if (attributes.find("type") != attributes.end())
  {
    std::string s = attributes["type"];
    Orthanc::Toolbox::ToLowerCase(s);
    if (s != "application/octet-stream")
    {
      std::string s = "This WADO-RS plugin only supports application/octet-stream return type for bulk data retrieval (" + accept + ")";
      OrthancPluginLogError(context_, s.c_str());
      return false;
    }
  }

  if (attributes.find("ra,ge") != attributes.end())
  {
    std::string s = "This WADO-RS plugin does not support Range retrieval, it can only return entire bulk data object";
    OrthancPluginLogError(context_, s.c_str());
    return false;
  }

  return true;
}


static OrthancPluginErrorCode AnswerListOfDicomInstances(OrthancPluginRestOutput* output,
                                                         const std::string& resource)
{
  Json::Value instances;
  if (!OrthancPlugins::RestApiGetJson(instances, context_, resource + "/instances"))
  {
    // Internal error
    OrthancPluginSendHttpStatusCode(context_, output, 400);
    return OrthancPluginErrorCode_Success;
  }

  if (OrthancPluginStartMultipartAnswer(context_, output, "related", "application/dicom"))
  {
    return OrthancPluginErrorCode_Plugin;
  }
  
  for (Json::Value::ArrayIndex i = 0; i < instances.size(); i++)
  {
    std::string uri = "/instances/" + instances[i]["ID"].asString() + "/file";
    std::string dicom;
    if (OrthancPlugins::RestApiGetString(dicom, context_, uri) &&
        OrthancPluginSendMultipartItem(context_, output, dicom.c_str(), dicom.size()) != 0)
    {
      return OrthancPluginErrorCode_Plugin;
    }
  }

  return OrthancPluginErrorCode_Success;
}



static void AnswerMetadata(OrthancPluginRestOutput* output,
                           const OrthancPluginHttpRequest* request,
                           const std::string& resource,
                           bool isInstance,
                           bool isXml)
{
  std::list<std::string> files;
  if (isInstance)
  {
    files.push_back(resource + "/file");
  }
  else
  {
    Json::Value instances;
    if (!OrthancPlugins::RestApiGetJson(instances, context_, resource + "/instances"))
    {
      // Internal error
      OrthancPluginSendHttpStatusCode(context_, output, 400);
      return;
    }

    for (Json::Value::ArrayIndex i = 0; i < instances.size(); i++)
    {
      files.push_back("/instances/" + instances[i]["ID"].asString() + "/file");
    }
  }

  const std::string wadoBase = OrthancPlugins::Configuration::GetBaseUrl(configuration_, request);
  OrthancPlugins::DicomResults results(context_, output, wadoBase, *dictionary_, isXml, true);
  
  for (std::list<std::string>::const_iterator
         it = files.begin(); it != files.end(); ++it)
  {
    std::string content; 
    if (OrthancPlugins::RestApiGetString(content, context_, *it))
    {
      OrthancPlugins::ParsedDicomFile dicom(content);
      results.Add(dicom.GetFile());
    }
  }

  results.Answer();
}




static bool LocateStudy(OrthancPluginRestOutput* output,
                        std::string& uri,
                        const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context_, output, "GET");
    return false;
  }

  std::string id;

  {
    char* tmp = OrthancPluginLookupStudy(context_, request->groups[0]);
    if (tmp == NULL)
    {
      std::string s = "Accessing an inexistent study with WADO-RS: " + std::string(request->groups[0]);
      OrthancPluginLogError(context_, s.c_str());
      OrthancPluginSendHttpStatusCode(context_, output, 404);
      return false;
    }

    id.assign(tmp);
    OrthancPluginFreeString(context_, tmp);
  }
  
  uri = "/studies/" + id;
  return true;
}


static bool LocateSeries(OrthancPluginRestOutput* output,
                         std::string& uri,
                         const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context_, output, "GET");
    return false;
  }

  std::string id;

  {
    char* tmp = OrthancPluginLookupSeries(context_, request->groups[1]);
    if (tmp == NULL)
    {
      std::string s = "Accessing an inexistent series with WADO-RS: " + std::string(request->groups[1]);
      OrthancPluginLogError(context_, s.c_str());
      OrthancPluginSendHttpStatusCode(context_, output, 404);
      return false;
    }

    id.assign(tmp);
    OrthancPluginFreeString(context_, tmp);
  }
  
  Json::Value study;
  if (!OrthancPlugins::RestApiGetJson(study, context_, "/series/" + id + "/study"))
  {
    OrthancPluginSendHttpStatusCode(context_, output, 404);
    return false;
  }

  if (study["MainDicomTags"]["StudyInstanceUID"].asString() != std::string(request->groups[0]))
  {
    std::string s = "No series " + std::string(request->groups[1]) + " in study " + std::string(request->groups[0]);
    OrthancPluginLogError(context_, s.c_str());
    OrthancPluginSendHttpStatusCode(context_, output, 404);
    return false;
  }
  
  uri = "/series/" + id;
  return true;
}


static bool LocateInstance(OrthancPluginRestOutput* output,
                           std::string& uri,
                           const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context_, output, "GET");
    return false;
  }

  std::string id;

  {
    char* tmp = OrthancPluginLookupInstance(context_, request->groups[2]);
    if (tmp == NULL)
    {
      std::string s = "Accessing an inexistent instance with WADO-RS: " + std::string(request->groups[2]);
      OrthancPluginLogError(context_, s.c_str());
      OrthancPluginSendHttpStatusCode(context_, output, 404);
      return false;
    }

    id.assign(tmp);
    OrthancPluginFreeString(context_, tmp);
  }
  
  Json::Value study, series;
  if (!OrthancPlugins::RestApiGetJson(series, context_, "/instances/" + id + "/series") ||
      !OrthancPlugins::RestApiGetJson(study, context_, "/instances/" + id + "/study"))
  {
    OrthancPluginSendHttpStatusCode(context_, output, 404);
    return false;
  }

  if (study["MainDicomTags"]["StudyInstanceUID"].asString() != std::string(request->groups[0]) ||
      series["MainDicomTags"]["SeriesInstanceUID"].asString() != std::string(request->groups[1]))
  {
    std::string s = ("No instance " + std::string(request->groups[2]) + 
                     " in study " + std::string(request->groups[0]) + " or " +
                     " in series " + std::string(request->groups[1]));
    OrthancPluginLogError(context_, s.c_str());
    OrthancPluginSendHttpStatusCode(context_, output, 404);
    return false;
  }

  uri = "/instances/" + id;
  return true;
}


OrthancPluginErrorCode RetrieveDicomStudy(OrthancPluginRestOutput* output,
                                          const char* url,
                                          const OrthancPluginHttpRequest* request)
{
  if (!AcceptMultipartDicom(request))
  {
    OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
    return OrthancPluginErrorCode_Success;
  }

  std::string uri;
  if (LocateStudy(output, uri, request))
  {
    AnswerListOfDicomInstances(output, uri);
  }

  return OrthancPluginErrorCode_Success;
}


OrthancPluginErrorCode RetrieveDicomSeries(OrthancPluginRestOutput* output,
                                           const char* url,
                                           const OrthancPluginHttpRequest* request)
{
  if (!AcceptMultipartDicom(request))
  {
    OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
    return OrthancPluginErrorCode_Success;
  }

  std::string uri;
  if (LocateSeries(output, uri, request))
  {
    AnswerListOfDicomInstances(output, uri);
  }

  return OrthancPluginErrorCode_Success;
}



OrthancPluginErrorCode RetrieveDicomInstance(OrthancPluginRestOutput* output,
                                             const char* url,
                                             const OrthancPluginHttpRequest* request)
{
  if (!AcceptMultipartDicom(request))
  {
    OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
    return OrthancPluginErrorCode_Success;
  }

  std::string uri;
  if (LocateInstance(output, uri, request))
  {
    if (OrthancPluginStartMultipartAnswer(context_, output, "related", "application/dicom"))
    {
      return OrthancPluginErrorCode_Plugin;
    }
  
    std::string dicom;
    if (OrthancPlugins::RestApiGetString(dicom, context_, uri + "/file") &&
        OrthancPluginSendMultipartItem(context_, output, dicom.c_str(), dicom.size()) != 0)
    {
      return OrthancPluginErrorCode_Plugin;
    }
  }

  return OrthancPluginErrorCode_Success;
}



OrthancPluginErrorCode RetrieveStudyMetadata(OrthancPluginRestOutput* output,
                                             const char* url,
                                             const OrthancPluginHttpRequest* request)
{
  bool isXml;
  if (!AcceptMetadata(request, isXml))
  {
    OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
    return OrthancPluginErrorCode_Success;
  }

  std::string uri;
  if (LocateStudy(output, uri, request))
  {
    AnswerMetadata(output, request, uri, false, isXml);
  }

  return OrthancPluginErrorCode_Success;
}


OrthancPluginErrorCode RetrieveSeriesMetadata(OrthancPluginRestOutput* output,
                                              const char* url,
                                              const OrthancPluginHttpRequest* request)
{
  bool isXml;
  if (!AcceptMetadata(request, isXml))
  {
    OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
    return OrthancPluginErrorCode_Success;
  }

  std::string uri;
  if (LocateSeries(output, uri, request))
  {
    AnswerMetadata(output, request, uri, false, isXml);
  }

  return OrthancPluginErrorCode_Success;
}


OrthancPluginErrorCode RetrieveInstanceMetadata(OrthancPluginRestOutput* output,
                                                const char* url,
                                                const OrthancPluginHttpRequest* request)
{
  bool isXml;
  if (!AcceptMetadata(request, isXml))
  {
    OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
    return OrthancPluginErrorCode_Success;
  }

  std::string uri;
  if (LocateInstance(output, uri, request))
  {
    AnswerMetadata(output, request, uri, true, isXml);
  }

  return OrthancPluginErrorCode_Success;
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

OrthancPluginErrorCode RetrieveBulkData(OrthancPluginRestOutput* output,
                                        const char* url,
                                        const OrthancPluginHttpRequest* request)
{
  if (!AcceptBulkData(request))
  {
    OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
    return OrthancPluginErrorCode_Success;
  }

  std::string uri, content;
  if (LocateInstance(output, uri, request) &&
      OrthancPlugins::RestApiGetString(content, context_, uri + "/file"))
  {
    OrthancPlugins::ParsedDicomFile dicom(content);

    std::vector<std::string> path;
    Orthanc::Toolbox::TokenizeString(path, request->groups[3], '/');
      
    std::string result;
    if (path.size() % 2 == 1 &&
        ExploreBulkData(result, path, 0, dicom.GetDataSet()))
    {
      if (OrthancPluginStartMultipartAnswer(context_, output, "related", "application/octet-stream") != 0 ||
          OrthancPluginSendMultipartItem(context_, output, result.c_str(), result.size()) != 0)
      {
        return OrthancPluginErrorCode_Plugin;
      }
    }
    else
    {
      OrthancPluginSendHttpStatusCode(context_, output, 400 /* Bad request */);
    }      
  }

  return OrthancPluginErrorCode_Success;
}





#include <gdcmImageReader.h>
#include <gdcmImageWriter.h>
#include <gdcmImageChangeTransferSyntax.h>
#include <gdcmJPEG2000Codec.h>
#include <boost/algorithm/string/replace.hpp>


static void TokenizeAndNormalize(std::vector<std::string>& tokens,
                                 const std::string& source,
                                 char separator)
{
  Orthanc::Toolbox::TokenizeString(tokens, source, separator);

  for (size_t i = 0; i < tokens.size(); i++)
  {
    tokens[i] = Orthanc::Toolbox::StripSpaces(tokens[i]);
    Orthanc::Toolbox::ToLowerCase(tokens[i]);
  }
}



static gdcm::TransferSyntax ParseTransferSyntax(const OrthancPluginHttpRequest* request)
{
  for (uint32_t i = 0; i < request->headersCount; i++)
  {
    std::string key(request->headersKeys[i]);
    Orthanc::Toolbox::ToLowerCase(key);

    if (key == "accept")
    {
      std::vector<std::string> tokens;
      TokenizeAndNormalize(tokens, request->headersValues[i], ';');

      if (tokens.size() == 0 ||
          tokens[0] == "*/*")
      {
        return gdcm::TransferSyntax::ImplicitVRLittleEndian;
      }

      if (tokens[0] != "multipart/related")
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
      }

      std::string type("application/octet-stream");
      std::string transferSyntax;
      
      for (size_t j = 1; j < tokens.size(); j++)
      {
        std::vector<std::string> parsed;
        TokenizeAndNormalize(parsed, tokens[j], '=');

        if (parsed.size() != 2)
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest);
        }

        if (parsed[0] == "type")
        {
          type = parsed[1];
        }

        if (parsed[0] == "transfer-syntax")
        {
          transferSyntax = parsed[1];
        }
      }

      if (type == "application/octet-stream")
      {
        if (transferSyntax.empty())
        {
          return gdcm::TransferSyntax(gdcm::TransferSyntax::ImplicitVRLittleEndian);
        }
        else
        {
          std::string s = ("DICOMweb RetrieveFrames: Cannot specify a transfer syntax (" + 
                           transferSyntax + ") for default Little Endian uncompressed pixel data");
          OrthancPluginLogError(context_, s.c_str());
          throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest);
        }
      }
      else
      {
        // http://dicom.nema.org/medical/dicom/current/output/html/part18.html#table_6.5-1
        if (type == "image/dicom+jpeg" && transferSyntax == "1.2.840.10008.1.2.4.50")
        {
          return gdcm::TransferSyntax::JPEGBaselineProcess1;
        }
        else if (type == "image/dicom+jpeg" && transferSyntax == "1.2.840.10008.1.2.4.51")
        {
          return gdcm::TransferSyntax::JPEGExtendedProcess2_4;
        }
        else if (type == "image/dicom+jpeg" && transferSyntax == "1.2.840.10008.1.2.4.57")
        {
          return gdcm::TransferSyntax::JPEGLosslessProcess14;
        }
        else if (type == "image/dicom+jpeg" && (transferSyntax.empty() ||
                                                transferSyntax == "1.2.840.10008.1.2.4.70"))
        {
          return gdcm::TransferSyntax::JPEGLosslessProcess14_1;
        }
        else if (type == "image/dicom+rle" && (transferSyntax.empty() ||
                                               transferSyntax == "1.2.840.10008.1.2.5"))
        {
          return gdcm::TransferSyntax::RLELossless;
        }
        else if (type == "image/dicom+jpeg-ls" && (transferSyntax.empty() ||
                                                   transferSyntax == "1.2.840.10008.1.2.4.80"))
        {
          return gdcm::TransferSyntax::JPEGLSLossless;
        }
        else if (type == "image/dicom+jpeg-ls" && transferSyntax == "1.2.840.10008.1.2.4.81")
        {
          return gdcm::TransferSyntax::JPEGLSNearLossless;
        }
        else if (type == "image/dicom+jp2" && (transferSyntax.empty() ||
                                               transferSyntax == "1.2.840.10008.1.2.4.90"))
        {
          return gdcm::TransferSyntax::JPEG2000Lossless;
        }
        else if (type == "image/dicom+jp2" && transferSyntax == "1.2.840.10008.1.2.4.91")
        {
          return gdcm::TransferSyntax::JPEG2000;
        }
        else if (type == "image/dicom+jpx" && (transferSyntax.empty() ||
                                               transferSyntax == "1.2.840.10008.1.2.4.92"))
        {
          return gdcm::TransferSyntax::JPEG2000Part2Lossless;
        }
        else if (type == "image/dicom+jpx" && transferSyntax == "1.2.840.10008.1.2.4.93")
        {
          return gdcm::TransferSyntax::JPEG2000Part2;
        }
        else
        {
          std::string s = ("DICOMweb RetrieveFrames: Transfer syntax \"" + 
                           transferSyntax + "\" is incompatible with media type \"" + type + "\"");
          OrthancPluginLogError(context_, s.c_str());
          throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest);
        }
      }
    }
  }

  // By default, DICOMweb expectes Little Endian uncompressed pixel data
  return gdcm::TransferSyntax::ImplicitVRLittleEndian;
}


static void ParseFrameList(std::list<unsigned int>& frames,
                           const OrthancPluginHttpRequest* request)
{
  frames.clear();

  if (request->groupsCount < 3)
  {
    frames.push_back(0);
    return;
  }

  std::string source(request->groups[3]);
  Orthanc::Toolbox::ToLowerCase(source);
  boost::replace_all(source, "%2c", ",");

  std::vector<std::string> tokens;
  Orthanc::Toolbox::TokenizeString(tokens, source, ',');

  for (size_t i = 0; i < tokens.size(); i++)
  {
    frames.push_back(boost::lexical_cast<unsigned int>(tokens[i]));
  }
}                           



static bool AnswerFrames(OrthancPluginRestOutput* output,
                         const gdcm::DataSet& dicom,
                         const std::list<unsigned int>& frames)
{
  if (!dicom.FindDataElement(OrthancPlugins::DICOM_TAG_PIXEL_DATA))
  {
    return OrthancPluginErrorCode_IncompatibleImageFormat;
  }

  const gdcm::DataElement& pixelData = dicom.GetDataElement(OrthancPlugins::DICOM_TAG_PIXEL_DATA);
  const gdcm::SequenceOfFragments* fragments = pixelData.GetSequenceOfFragments();

  if (fragments == NULL)
  {
    printf("Single-frame image\n");
    printf("%d\n", pixelData.GetByteValue()->GetLength());
  }
  else
  {
    printf("Multi-frame image (%d fragments)\n", fragments->GetNumberOfFragments());

    for (gdcm::SequenceOfFragments::SizeType i = 0; i < fragments->GetNumberOfFragments(); i++)
    {
      printf("%d: %d\n", i, fragments->GetFragment(i).GetByteValue()->GetLength());
    }
  }

  return true;
}


OrthancPluginErrorCode RetrieveFrames(OrthancPluginRestOutput* output,
                                      const char* url,
                                      const OrthancPluginHttpRequest* request)
{
  // storescu -xs localhost 4242 ~/Subversion/orthanc-tests/Database/Multiframe.dcm 

  // curl http://localhost:8042/dicom-web/studies/1.3.51.0.1.1.192.168.29.133.1681753.1681732/series/1.3.12.2.1107.5.2.33.37097.2012041612474981424569674.0.0.0/instances/1.3.12.2.1107.5.2.33.37097.2012041612485517294169680/frames/0
  // curl http://localhost:8042/dicom-web/studies/1.3.46.670589.7.5.8.80001255161.20000323.151537.1/series/1.3.46.670589.7.5.7.80001255161.20000323.151537.1/instances/1.3.46.670589.7.5.1.981501.20000323.16172540.1.1.13/frames/0
 

  // http://gdcm.sourceforge.net/html/CompressLossyJPEG_8cs-example.html

  gdcm::TransferSyntax targetSyntax(ParseTransferSyntax(request));

  std::list<unsigned int> frames;
  ParseFrameList(frames, request);

  Json::Value header;
  std::string uri, content;
  if (LocateInstance(output, uri, request) &&
      OrthancPlugins::RestApiGetString(content, context_, uri + "/file") &&
      OrthancPlugins::RestApiGetJson(header, context_, uri + "/header?simplify"))
  {
    {
      std::string s = "DICOMweb RetrieveFrames on " + uri + ", frames: ";
      for (std::list<unsigned int>::const_iterator it = frames.begin(); it != frames.end(); ++it)
      {
        s += boost::lexical_cast<std::string>(*it) + " ";
      }
      OrthancPluginLogInfo(context_, s.c_str());
    }

    std::auto_ptr<OrthancPlugins::ParsedDicomFile> source;

    gdcm::TransferSyntax sourceSyntax;

    if (header.type() == Json::objectValue &&
        header.isMember("TransferSyntaxUID"))
    {
      sourceSyntax = gdcm::TransferSyntax::GetTSType(header["TransferSyntaxUID"].asCString());
    }
    else
    {
      source.reset(new OrthancPlugins::ParsedDicomFile(content));
      sourceSyntax = source->GetFile().GetHeader().GetDataSetTransferSyntax();
    }

    if (sourceSyntax == targetSyntax ||
        (targetSyntax == gdcm::TransferSyntax::ImplicitVRLittleEndian &&
         sourceSyntax == gdcm::TransferSyntax::ExplicitVRLittleEndian))
    {
      // No need to change the transfer syntax

      if (source.get() == NULL)
      {
        source.reset(new OrthancPlugins::ParsedDicomFile(content));
      }

      AnswerFrames(output, source->GetFile().GetDataSet(), frames);
    }
    else
    {
      // Need to convert the transfer syntax

      {
        std::string s = ("DICOMweb RetrieveFrames: Transcoding " + uri + " from transfer syntax " + 
                         std::string(sourceSyntax.GetString()) + " to " + std::string(targetSyntax.GetString()));
        OrthancPluginLogInfo(context_, s.c_str());
      }

      gdcm::ImageChangeTransferSyntax change;
      change.SetTransferSyntax(gdcm::TransferSyntax::JPEG2000Lossless);

      gdcm::JPEG2000Codec codec;
      if (!codec.CanCode(change.GetTransferSyntax()))
      {
        return OrthancPluginErrorCode_Plugin;
      }

      //codec.SetLossless(true);
      change.SetUserCodec(&codec);

      std::stringstream stream(content);

      gdcm::ImageReader reader;
      reader.SetStream(stream);
      printf("Read: %d\n", reader.Read());

      change.SetInput(reader.GetImage());
      printf("Change: %d\n", change.Change());

      gdcm::ImageWriter writer;
      writer.SetImage(change.GetOutput());
      writer.SetFile(reader.GetFile());
      
      std::stringstream ss;
      writer.SetStream(ss);
      printf("Write: %d\n", writer.Write());

      gdcm::Reader reader2;
      reader2.SetStream(ss);
      printf("Read: %d\n", reader2.Read());

      AnswerFrames(output, reader2.GetFile().GetDataSet(), frames);
    }
  }    

  return OrthancPluginErrorCode_Success;
}
