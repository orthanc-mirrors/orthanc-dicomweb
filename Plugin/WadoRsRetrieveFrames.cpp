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


#include "WadoRs.h"

#include <Core/Toolbox.h>
#include <Plugins/Samples/Common/OrthancPluginCppWrapper.h>

#include <memory>
#include <list>
#include <boost/algorithm/string/replace.hpp>
#include <boost/lexical_cast.hpp>


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


static void RemoveSurroundingQuotes(std::string& value)
{
  if (!value.empty() &&
      value[0] == '\"' &&
      value[value.size() - 1] == '\"')
  {
    value = value.substr(1, value.size() - 2);
  }  
}



static bool ParseTransferSyntax(Orthanc::DicomTransferSyntax& syntax,
                                const OrthancPluginHttpRequest* request)
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
        syntax = Orthanc::DicomTransferSyntax_LittleEndianExplicit;
        return true;
      }

      if (tokens[0] != "multipart/related")
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange, "expecting 'Accept: multipart/related' HTTP header");
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
          RemoveSurroundingQuotes(type);
        }

        if (parsed[0] == "transfer-syntax")
        {
          transferSyntax = parsed[1];
          RemoveSurroundingQuotes(transferSyntax);
        }
      }

      if (type == "application/octet-stream")
      {
        if (transferSyntax.empty() ||
            transferSyntax == "1.2.840.10008.1.2.1")
        {
          syntax = Orthanc::DicomTransferSyntax_LittleEndianExplicit;
          return true;
        }
        else if (transferSyntax == "1.2.840.10008.1.2")
        {
          syntax = Orthanc::DicomTransferSyntax_LittleEndianImplicit;
          return true;
        }
        else if (transferSyntax == "*")
        {
          // New in DICOMweb plugin 1.1.0
          return false;
        }
        else
        {
          throw Orthanc::OrthancException(
            Orthanc::ErrorCode_BadRequest,
            "DICOMweb RetrieveFrames: Cannot specify a transfer syntax (" + 
            transferSyntax + ") for default Little Endian uncompressed pixel data");
        }
      }
      else
      {
        /**
         * DICOM 2017c
         * http://dicom.nema.org/medical/dicom/current/output/html/part18.html#table_6.1.1.8-3b
         **/
        if (type == "image/jpeg" && (transferSyntax.empty() ||  // Default
                                     transferSyntax == "1.2.840.10008.1.2.4.70"))
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGProcess14SV1;
          return true;
        }
        else if (type == "image/jpeg" && transferSyntax == "1.2.840.10008.1.2.4.50")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGProcess1;
          return true;
        }
        else if (type == "image/jpeg" && transferSyntax == "1.2.840.10008.1.2.4.51")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGProcess2_4;
          return true;
        }
        else if (type == "image/jpeg" && transferSyntax == "1.2.840.10008.1.2.4.57")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGProcess14;
          return true;
        }
        else if (type == "image/x-dicom-rle" && (transferSyntax.empty() ||  // Default
                                                 transferSyntax == "1.2.840.10008.1.2.5"))
        {
          syntax = Orthanc::DicomTransferSyntax_RLELossless;
          return true;
        }
        else if (type == "image/x-jls" && (transferSyntax.empty() ||  // Default
                                           transferSyntax == "1.2.840.10008.1.2.4.80"))
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGLSLossless;
          return true;
        }
        else if (type == "image/x-jls" && transferSyntax == "1.2.840.10008.1.2.4.81")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGLSLossy;
          return true;
        }
        else if (type == "image/jp2" && (transferSyntax.empty() ||  // Default
                                         transferSyntax == "1.2.840.10008.1.2.4.90"))
        {
          syntax = Orthanc::DicomTransferSyntax_JPEG2000LosslessOnly;
          return true;
        }
        else if (type == "image/jp2" && transferSyntax == "1.2.840.10008.1.2.4.91")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEG2000;
          return true;
        }
        else if (type == "image/jpx" && (transferSyntax.empty() ||  // Default
                                         transferSyntax == "1.2.840.10008.1.2.4.92"))
        {
          syntax = Orthanc::DicomTransferSyntax_JPEG2000MulticomponentLosslessOnly;
          return true;
        }
        else if (type == "image/jpx" && transferSyntax == "1.2.840.10008.1.2.4.93")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEG2000Multicomponent;
          return true;
        }


        /**
         * Backward compatibility with DICOM 2014a
         * http://dicom.nema.org/medical/dicom/2014a/output/html/part18.html#table_6.5-1
         **/
        if (type == "image/dicom+jpeg" && transferSyntax == "1.2.840.10008.1.2.4.50")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGProcess1;
          return true;
        }
        else if (type == "image/dicom+jpeg" && transferSyntax == "1.2.840.10008.1.2.4.51")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGProcess2_4;
          return true;
        }
        else if (type == "image/dicom+jpeg" && transferSyntax == "1.2.840.10008.1.2.4.57")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGProcess14;
          return true;
        }
        else if (type == "image/dicom+jpeg" && (transferSyntax.empty() ||
                                                transferSyntax == "1.2.840.10008.1.2.4.70"))
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGProcess14SV1;
          return true;
        }
        else if (type == "image/dicom+rle" && (transferSyntax.empty() ||
                                               transferSyntax == "1.2.840.10008.1.2.5"))
        {
          syntax = Orthanc::DicomTransferSyntax_RLELossless;
          return true;
        }
        else if (type == "image/dicom+jpeg-ls" && (transferSyntax.empty() ||
                                                   transferSyntax == "1.2.840.10008.1.2.4.80"))
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGLSLossless;
          return true;
        }
        else if (type == "image/dicom+jpeg-ls" && transferSyntax == "1.2.840.10008.1.2.4.81")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEGLSLossy;
          return true;
        }
        else if (type == "image/dicom+jp2" && (transferSyntax.empty() ||
                                               transferSyntax == "1.2.840.10008.1.2.4.90"))
        {
          syntax = Orthanc::DicomTransferSyntax_JPEG2000LosslessOnly;
          return true;
        }
        else if (type == "image/dicom+jp2" && transferSyntax == "1.2.840.10008.1.2.4.91")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEG2000;
          return true;
        }
        else if (type == "image/dicom+jpx" && (transferSyntax.empty() ||
                                               transferSyntax == "1.2.840.10008.1.2.4.92"))
        {
          syntax = Orthanc::DicomTransferSyntax_JPEG2000MulticomponentLosslessOnly;
          return true;
        }
        else if (type == "image/dicom+jpx" && transferSyntax == "1.2.840.10008.1.2.4.93")
        {
          syntax = Orthanc::DicomTransferSyntax_JPEG2000Multicomponent;
          return true;
        }

        throw Orthanc::OrthancException(
          Orthanc::ErrorCode_BadRequest,
          "DICOMweb RetrieveFrames: Transfer syntax \"" + 
          transferSyntax + "\" is incompatible with media type \"" + type + "\"");
      }
    }
  }

  // By default, DICOMweb expectes Little Endian uncompressed pixel data
  syntax = Orthanc::DicomTransferSyntax_LittleEndianExplicit;
  return true;
}


static void ParseFrameList(std::list<unsigned int>& frames,
                           const OrthancPluginHttpRequest* request)
{
  frames.clear();

  if (request->groupsCount <= 3 ||
      request->groups[3] == NULL)
  {
    return;
  }

  std::string source(request->groups[3]);
  Orthanc::Toolbox::ToLowerCase(source);
  boost::replace_all(source, "%2c", ",");

  std::vector<std::string> tokens;
  Orthanc::Toolbox::TokenizeString(tokens, source, ',');

  for (size_t i = 0; i < tokens.size(); i++)
  {
    int frame = boost::lexical_cast<int>(tokens[i]);
    if (frame <= 0)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                      "Invalid frame number (must be > 0): " + tokens[i]);
    }

    frames.push_back(static_cast<unsigned int>(frame - 1));
  }
}                           


static const char* GetMimeType(const Orthanc::DicomTransferSyntax& syntax)
{
  // http://dicom.nema.org/medical/dicom/current/output/html/part18.html#table_6.1.1.8-3b
  // http://dicom.nema.org/MEDICAL/dicom/2019a/output/chtml/part18/chapter_6.html#table_6.1.1.8-3b

  switch (syntax)
  {
    case Orthanc::DicomTransferSyntax_LittleEndianImplicit:
      // The "transfer-syntax" info was added in version 1.1 of the plugin
      return "application/octet-stream; transfer-syntax=1.2.840.10008.1.2";

    case Orthanc::DicomTransferSyntax_LittleEndianExplicit:
      return "application/octet-stream; transfer-syntax=1.2.840.10008.1.2.1";

    case Orthanc::DicomTransferSyntax_JPEGProcess1:
      return "image/jpeg; transfer-syntax=1.2.840.10008.1.2.4.50";

    case Orthanc::DicomTransferSyntax_JPEGProcess2_4:
      return "image/jpeg; transfer-syntax=1.2.840.10008.1.2.4.51";
    
    case Orthanc::DicomTransferSyntax_JPEGProcess14:
      return "image/jpeg; transfer-syntax=1.2.840.10008.1.2.4.57";

    case Orthanc::DicomTransferSyntax_JPEGProcess14SV1:
      return "image/jpeg; transfer-syntax=1.2.840.10008.1.2.4.70";
    
    case Orthanc::DicomTransferSyntax_RLELossless:
      return "image/x-dicom-rle; transfer-syntax=1.2.840.10008.1.2.5";

    case Orthanc::DicomTransferSyntax_JPEGLSLossless:
      return "image/x-jls; transfer-syntax=1.2.840.10008.1.2.4.80";

    case Orthanc::DicomTransferSyntax_JPEGLSLossy:
      return "image/x-jls; transfer-syntax=1.2.840.10008.1.2.4.81";

    case Orthanc::DicomTransferSyntax_JPEG2000LosslessOnly:
      return "image/jp2; transfer-syntax=1.2.840.10008.1.2.4.90";

    case Orthanc::DicomTransferSyntax_JPEG2000:
      return "image/jp2; transfer-syntax=1.2.840.10008.1.2.4.91";

    case Orthanc::DicomTransferSyntax_JPEG2000MulticomponentLosslessOnly:
      return "image/jpx; transfer-syntax=1.2.840.10008.1.2.4.92";

    case Orthanc::DicomTransferSyntax_JPEG2000Multicomponent:
      return "image/jpx; transfer-syntax=1.2.840.10008.1.2.4.93";

    default:
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }
}


static void AnswerFrames(OrthancPluginRestOutput* output,
                         const OrthancPluginHttpRequest* request,
                         const OrthancPlugins::DicomInstance& instance,
                         const std::string& studyInstanceUid,
                         const std::string& seriesInstanceUid,
                         const std::string& sopInstanceUid,
                         const std::list<unsigned int>& frames,
                         Orthanc::DicomTransferSyntax outputSyntax)
{
  if (OrthancPluginStartMultipartAnswer(
        OrthancPlugins::GetGlobalContext(), 
        output, "related", GetMimeType(outputSyntax)) != OrthancPluginErrorCode_Success)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_Plugin,
                                    "Cannot start a multipart answer");
  }

  const std::string base = OrthancPlugins::Configuration::GetBaseUrl(request);
    
  for (std::list<unsigned int>::const_iterator
         frame = frames.begin(); frame != frames.end(); ++frame)
  {
    std::string content;
    instance.GetRawFrame(content, *frame);

    const char* data = content.empty() ? NULL : content.c_str();
    size_t size = content.size();
        
    OrthancPluginErrorCode error;

#if HAS_SEND_MULTIPART_ITEM_2 == 1
    std::string location = (
      OrthancPlugins::Configuration::GetWadoUrl(base, studyInstanceUid, seriesInstanceUid, sopInstanceUid) +
      "frames/" + boost::lexical_cast<std::string>(*frame + 1));
    const char *keys[] = { "Content-Location" };
    const char *values[] = { location.c_str() };
    error = OrthancPluginSendMultipartItem2(OrthancPlugins::GetGlobalContext(), output, data, size, 1, keys, values);
#else
    error = OrthancPluginSendMultipartItem(OrthancPlugins::GetGlobalContext(), output, data, size);
#endif

    if (error != OrthancPluginErrorCode_Success)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);      
    }
  }
}


static void RetrieveFrames(OrthancPluginRestOutput* output,
                           const OrthancPluginHttpRequest* request,
                           bool allFrames,
                           std::list<unsigned int>& frames)
{
  std::string orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid;
  OrthancPlugins::MemoryBuffer content;
  if (LocateInstance(output, orthancId, studyInstanceUid, seriesInstanceUid, sopInstanceUid, request) &&
      content.RestApiGet("/instances/" + orthancId + "/file", false))
  {
    if (allFrames)
    {
      OrthancPlugins::LogInfo("DICOMweb RetrieveFrames on " + orthancId + ", all frames");
    }
    else
    {
      std::string s = "DICOMweb RetrieveFrames on " + orthancId + ", frames: ";
      for (std::list<unsigned int>::const_iterator 
             frame = frames.begin(); frame != frames.end(); ++frame)
      {
        s += boost::lexical_cast<std::string>(*frame + 1) + " ";
      }

      OrthancPlugins::LogInfo(s);
    }

    Orthanc::DicomTransferSyntax targetSyntax;

    std::unique_ptr<OrthancPlugins::DicomInstance> instance;
    if (ParseTransferSyntax(targetSyntax, request))
    {
      OrthancPlugins::LogInfo("DICOMweb RetrieveFrames: Transcoding instance " + orthancId + 
                              " to transfer syntax " + Orthanc::GetTransferSyntaxUid(targetSyntax));

      instance.reset(OrthancPlugins::DicomInstance::Transcode(
                       content.GetData(), content.GetSize(), GetTransferSyntaxUid(targetSyntax)));
    }
    else
    {
      instance.reset(new OrthancPlugins::DicomInstance(content.GetData(), content.GetSize()));
    }

    if (instance.get() == NULL)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NullPointer);
    }

    if (allFrames)
    {
      frames.clear();
      for (unsigned int i = 0; i < instance->GetFramesCount(); i++)
      {
        frames.push_back(i + 1);  // Frames are numbered starting from 1
      }
    }

    AnswerFrames(output, request, *instance, studyInstanceUid, seriesInstanceUid,
                 sopInstanceUid, frames, targetSyntax);
  }    
}


void RetrieveAllFrames(OrthancPluginRestOutput* output,
                       const char* url,
                       const OrthancPluginHttpRequest* request)
{
  std::list<unsigned int> frames;
  RetrieveFrames(output, request, true, frames);
}


void RetrieveSelectedFrames(OrthancPluginRestOutput* output,
                            const char* url,
                            const OrthancPluginHttpRequest* request)
{
  std::list<unsigned int> frames;
  ParseFrameList(frames, request);
  RetrieveFrames(output, request, false, frames);
}
