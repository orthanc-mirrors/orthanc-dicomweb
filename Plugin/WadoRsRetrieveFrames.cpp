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


#include "WadoRs.h"

#include "../Orthanc/Core/Toolbox.h"
#include "../Orthanc/Core/OrthancException.h"
#include "Dicom.h"
#include "Plugin.h"

#include <memory>
#include <list>
#include <gdcmImageReader.h>
#include <gdcmImageWriter.h>
#include <gdcmImageChangeTransferSyntax.h>
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
      std::string s = "Invalid frame number (must be > 0): " + tokens[i];
      OrthancPluginLogError(context_, s.c_str());
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
    }

    frames.push_back(static_cast<unsigned int>(frame - 1));
  }
}                           



static const char* GetMimeType(const gdcm::TransferSyntax& syntax)
{
  switch (syntax)
  {
    case gdcm::TransferSyntax::ImplicitVRLittleEndian:
      return "application/octet-stream";

    case gdcm::TransferSyntax::JPEGBaselineProcess1:
      return "image/dicom+jpeg; transfer-syntax=1.2.840.10008.1.2.4.50";

    case gdcm::TransferSyntax::JPEGExtendedProcess2_4:
      return "image/dicom+jpeg; transfer-syntax=1.2.840.10008.1.2.4.51";
    
    case gdcm::TransferSyntax::JPEGLosslessProcess14:
      return "image/dicom+jpeg; transfer-syntax=1.2.840.10008.1.2.4.57";

    case gdcm::TransferSyntax::JPEGLosslessProcess14_1:
      return "image/dicom+jpeg; transferSyntax=1.2.840.10008.1.2.4.70";
    
    case gdcm::TransferSyntax::RLELossless:
      return "image/dicom+rle; transferSyntax=1.2.840.10008.1.2.5";

    case gdcm::TransferSyntax::JPEGLSLossless:
      return "image/dicom+jpeg-ls; transferSyntax=1.2.840.10008.1.2.4.80";

    case gdcm::TransferSyntax::JPEGLSNearLossless:
      return "image/dicom+jpeg-ls; transfer-syntax=1.2.840.10008.1.2.4.81";

    case gdcm::TransferSyntax::JPEG2000Lossless:
      return "image/dicom+jp2; transferSyntax=1.2.840.10008.1.2.4.90";

    case gdcm::TransferSyntax::JPEG2000:
      return "image/dicom+jp2; transfer-syntax=1.2.840.10008.1.2.4.91";

    case gdcm::TransferSyntax::JPEG2000Part2Lossless:
      return "image/dicom+jpx; transferSyntax=1.2.840.10008.1.2.4.92";

    case gdcm::TransferSyntax::JPEG2000Part2:
      return "image/dicom+jpx; transfer-syntax=1.2.840.10008.1.2.4.93";

    default:
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }
}



static void AnswerSingleFrame(OrthancPluginRestOutput* output,
                              const OrthancPluginHttpRequest* request,
                              const OrthancPlugins::ParsedDicomFile& dicom,
                              const char* frame,
                              size_t size,
                              unsigned int frameIndex)
{
  OrthancPluginErrorCode error;

#if HAS_SEND_MULTIPART_ITEM_2 == 1
  std::string location = dicom.GetWadoUrl(request) + "frames/" + boost::lexical_cast<std::string>(frameIndex + 1);
  const char *keys[] = { "Content-Location" };
  const char *values[] = { location.c_str() };
  error = OrthancPluginSendMultipartItem2(context_, output, frame, size, 1, keys, values);
#else
  error = OrthancPluginSendMultipartItem(context_, output, frame, size);
#endif

  if (error != OrthancPluginErrorCode_Success)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_NetworkProtocol);      
  }
}



static bool AnswerFrames(OrthancPluginRestOutput* output,
                         const OrthancPluginHttpRequest* request,
                         const OrthancPlugins::ParsedDicomFile& dicom,
                         const gdcm::TransferSyntax& syntax,
                         std::list<unsigned int>& frames)
{
  if (!dicom.GetDataSet().FindDataElement(OrthancPlugins::DICOM_TAG_PIXEL_DATA))
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_IncompatibleImageFormat);
  }

  const gdcm::DataElement& pixelData = dicom.GetDataSet().GetDataElement(OrthancPlugins::DICOM_TAG_PIXEL_DATA);
  const gdcm::SequenceOfFragments* fragments = pixelData.GetSequenceOfFragments();

  if (OrthancPluginStartMultipartAnswer(context_, output, "related", GetMimeType(syntax)) != OrthancPluginErrorCode_Success)
  {
    return false;
  }

  if (fragments == NULL)
  {
    // Single-fragment image

    if (pixelData.GetByteValue() == NULL)
    {
      OrthancPluginLogError(context_, "Image was not properly decoded");
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);      
    }

    int width, height, bits;

    if (!dicom.GetIntegerTag(height, *dictionary_, OrthancPlugins::DICOM_TAG_ROWS) ||
        !dicom.GetIntegerTag(width, *dictionary_, OrthancPlugins::DICOM_TAG_COLUMNS) ||
        !dicom.GetIntegerTag(bits, *dictionary_, OrthancPlugins::DICOM_TAG_BITS_ALLOCATED))
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
    }

    size_t frameSize = height * width * bits / 8;
    
    if (pixelData.GetByteValue()->GetLength() % frameSize != 0)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);      
    }

    size_t framesCount = pixelData.GetByteValue()->GetLength() / frameSize;

    if (frames.empty())
    {
      // If no frame is provided, return all the frames (this is an extension)
      for (size_t i = 0; i < framesCount; i++)
      {
        frames.push_back(i);
      }
    }

    const char* buffer = pixelData.GetByteValue()->GetPointer();
    assert(sizeof(char) == 1);

    for (std::list<unsigned int>::const_iterator 
           frame = frames.begin(); frame != frames.end(); ++frame)
    {
      if (*frame >= framesCount)
      {
        std::string s = ("Trying to access frame number " + boost::lexical_cast<std::string>(*frame + 1) + 
                         " of an image with " + boost::lexical_cast<std::string>(framesCount) + " frames");
        OrthancPluginLogError(context_, s.c_str());
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
      }
      else
      {
        const char* p = buffer + (*frame) * frameSize;
        AnswerSingleFrame(output, request, dicom, p, frameSize, *frame);
      }
    }
  }
  else
  {
    // Multi-fragment image, we assume that each fragment corresponds to one frame

    if (frames.empty())
    {
      // If no frame is provided, return all the frames (this is an extension)
      for (size_t i = 0; i < fragments->GetNumberOfFragments(); i++)
      {
        frames.push_back(i);
      }
    }

    for (std::list<unsigned int>::const_iterator 
           frame = frames.begin(); frame != frames.end(); ++frame)
    {
      if (*frame >= fragments->GetNumberOfFragments())
      {
        std::string s = ("Trying to access frame number " + boost::lexical_cast<std::string>(*frame + 1) + 
                         " of an image with " + boost::lexical_cast<std::string>(fragments->GetNumberOfFragments()) + " frames");
        OrthancPluginLogError(context_, s.c_str());
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
      }
      else
      {
        AnswerSingleFrame(output, request, dicom,
                          fragments->GetFragment(*frame).GetByteValue()->GetPointer(),
                          fragments->GetFragment(*frame).GetByteValue()->GetLength(), *frame);
      }
    }
  }

  return true;
}



void RetrieveFrames(OrthancPluginRestOutput* output,
                    const char* url,
                    const OrthancPluginHttpRequest* request)
{
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
      for (std::list<unsigned int>::const_iterator 
             frame = frames.begin(); frame != frames.end(); ++frame)
      {
        s += boost::lexical_cast<std::string>(*frame + 1) + " ";
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

      AnswerFrames(output, request, *source, targetSyntax, frames);
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
      change.SetTransferSyntax(targetSyntax);

      std::stringstream stream(content);

      gdcm::ImageReader reader;
      reader.SetStream(stream);
      if (!reader.Read())
      {
        OrthancPluginLogError(context_, "Cannot decode the image");
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
      }

      change.SetInput(reader.GetImage());
      if (!change.Change())
      {
        OrthancPluginLogError(context_, "Cannot change the transfer syntax of the image");
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
      }

      gdcm::ImageWriter writer;
      writer.SetImage(change.GetOutput());
      writer.SetFile(reader.GetFile());
      
      std::stringstream ss;
      writer.SetStream(ss);
      if (!writer.Write())
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_NotEnoughMemory);
      }

      OrthancPlugins::ParsedDicomFile transcoded(ss.str());
      AnswerFrames(output, request, transcoded, targetSyntax, frames);
    }
  }    
}
