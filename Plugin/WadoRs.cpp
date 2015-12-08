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
  try
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
  catch (Orthanc::OrthancException& e)
  {
    OrthancPluginLogError(context_, e.What());
    return OrthancPluginErrorCode_Plugin;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return OrthancPluginErrorCode_Plugin;
  }
}


OrthancPluginErrorCode RetrieveDicomSeries(OrthancPluginRestOutput* output,
                                           const char* url,
                                           const OrthancPluginHttpRequest* request)
{
  try
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
  catch (Orthanc::OrthancException& e)
  {
    OrthancPluginLogError(context_, e.What());
    return OrthancPluginErrorCode_Plugin;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return OrthancPluginErrorCode_Plugin;
  }
}



OrthancPluginErrorCode RetrieveDicomInstance(OrthancPluginRestOutput* output,
                                             const char* url,
                                             const OrthancPluginHttpRequest* request)
{
  try
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
  catch (Orthanc::OrthancException& e)
  {
    OrthancPluginLogError(context_, e.What());
    return OrthancPluginErrorCode_Plugin;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return OrthancPluginErrorCode_Plugin;
  }
}



OrthancPluginErrorCode RetrieveStudyMetadata(OrthancPluginRestOutput* output,
                                             const char* url,
                                             const OrthancPluginHttpRequest* request)
{
  try
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
  catch (Orthanc::OrthancException& e)
  {
    OrthancPluginLogError(context_, e.What());
    return OrthancPluginErrorCode_Plugin;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return OrthancPluginErrorCode_Plugin;
  }
}


OrthancPluginErrorCode RetrieveSeriesMetadata(OrthancPluginRestOutput* output,
                                              const char* url,
                                              const OrthancPluginHttpRequest* request)
{
  try
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
  catch (Orthanc::OrthancException& e)
  {
    OrthancPluginLogError(context_, e.What());
    return OrthancPluginErrorCode_Plugin;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return OrthancPluginErrorCode_Plugin;
  }
}


OrthancPluginErrorCode RetrieveInstanceMetadata(OrthancPluginRestOutput* output,
                                                const char* url,
                                                const OrthancPluginHttpRequest* request)
{
  try
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
  catch (Orthanc::OrthancException& e)
  {
    OrthancPluginLogError(context_, e.What());
    return OrthancPluginErrorCode_Plugin;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return OrthancPluginErrorCode_Plugin;
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

OrthancPluginErrorCode RetrieveBulkData(OrthancPluginRestOutput* output,
                                        const char* url,
                                        const OrthancPluginHttpRequest* request)
{
  try
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
  catch (Orthanc::OrthancException& e)
  {
    OrthancPluginLogError(context_, e.What());
    return OrthancPluginErrorCode_Plugin;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return OrthancPluginErrorCode_Plugin;
  }
}





#include <gdcmImageReader.h>
#include <gdcmImageWriter.h>
#include <gdcmImageChangeTransferSyntax.h>
#include <gdcmJPEG2000Codec.h>


OrthancPluginErrorCode RetrieveFrames(OrthancPluginRestOutput* output,
                                      const char* url,
                                      const OrthancPluginHttpRequest* request)
{
  // curl http://localhost:8042/dicom-web/studies/1.3.51.0.1.1.192.168.29.133.1681753.1681732/series/1.3.12.2.1107.5.2.33.37097.2012041612474981424569674.0.0.0/instances/1.3.12.2.1107.5.2.33.37097.2012041612485517294169680/frames/0

  // http://gdcm.sourceforge.net/html/CompressLossyJPEG_8cs-example.html

  try
  {
    std::string uri, content;
    if (LocateInstance(output, uri, request) &&
        OrthancPlugins::RestApiGetString(content, context_, uri + "/file"))
    {
      //OrthancPlugins::ParsedDicomFile dicom(content);
      {
        FILE* fp = fopen("/tmp/toto.dcm", "wb");
        fwrite(content.c_str(), content.size(), 1, fp);
        fclose(fp);
      }

      printf("RetrieveFrames: [%s] [%s]\n", uri.c_str(), request->groups[3]);

      gdcm::ImageChangeTransferSyntax change;
      change.SetTransferSyntax(gdcm::TransferSyntax::JPEG2000Lossless);

      gdcm::JPEG2000Codec codec;
      if (!codec.CanCode(change.GetTransferSyntax()))
      {
        return OrthancPluginErrorCode_Plugin;
      }

      //codec.SetLossless(true);
      change.SetUserCodec(&codec);

      gdcm::ImageReader reader;
      //reader.SetFile(dicom.GetFile());
      reader.SetFileName("/tmp/toto.dcm");
      printf("Read: %d\n", reader.Read());

      change.SetInput(reader.GetImage());
      printf("Change: %d\n", change.Change());

      gdcm::ImageWriter writer;
      writer.SetImage(change.GetOutput());
      writer.SetFile(reader.GetFile());
      
      writer.SetFileName("/tmp/tutu.dcm");
      printf("Write: %d\n", writer.Write());
    }    

    return OrthancPluginErrorCode_Success;
  }
  catch (Orthanc::OrthancException& e)
  {
    OrthancPluginLogError(context_, e.What());
    return OrthancPluginErrorCode_Plugin;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return OrthancPluginErrorCode_Plugin;
  }
}
