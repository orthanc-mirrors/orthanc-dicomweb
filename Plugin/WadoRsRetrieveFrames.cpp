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


#include "WadoRs.h"

#include "Dicom.h"
#include "Plugin.h"

#include <Core/Toolbox.h>

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


static void RemoveSurroundingQuotes(std::string& value)
{
  if (!value.empty() &&
      value[0] == '\"' &&
      value[value.size() - 1] == '\"')
  {
    value = value.substr(1, value.size() - 2);
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
        if (transferSyntax.empty())
        {
          return gdcm::TransferSyntax(gdcm::TransferSyntax::ImplicitVRLittleEndian);
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
          return gdcm::TransferSyntax::JPEGLosslessProcess14_1;
        }
        else if (type == "image/jpeg" && transferSyntax == "1.2.840.10008.1.2.4.50")
        {
          return gdcm::TransferSyntax::JPEGBaselineProcess1;
        }
        else if (type == "image/jpeg" && transferSyntax == "1.2.840.10008.1.2.4.51")
        {
          return gdcm::TransferSyntax::JPEGExtendedProcess2_4;
        }
        else if (type == "image/jpeg" && transferSyntax == "1.2.840.10008.1.2.4.57")
        {
          return gdcm::TransferSyntax::JPEGLosslessProcess14;
        }
        else if (type == "image/x-dicom-rle" && (transferSyntax.empty() ||  // Default
                                                 transferSyntax == "1.2.840.10008.1.2.5"))
        {
          return gdcm::TransferSyntax::RLELossless;
        }
        else if (type == "image/x-jls" && (transferSyntax.empty() ||  // Default
                                           transferSyntax == "1.2.840.10008.1.2.4.80"))
        {
          return gdcm::TransferSyntax::JPEGLSLossless;
        }
        else if (type == "image/x-jls" && transferSyntax == "1.2.840.10008.1.2.4.81")
        {
          return gdcm::TransferSyntax::JPEGLSNearLossless;
        }
        else if (type == "image/jp2" && (transferSyntax.empty() ||  // Default
                                         transferSyntax == "1.2.840.10008.1.2.4.90"))
        {
          return gdcm::TransferSyntax::JPEG2000Lossless;
        }
        else if (type == "image/jp2" && transferSyntax == "1.2.840.10008.1.2.4.91")
        {
          return gdcm::TransferSyntax::JPEG2000;
        }
        else if (type == "image/jpx" && (transferSyntax.empty() ||  // Default
                                         transferSyntax == "1.2.840.10008.1.2.4.92"))
        {
          return gdcm::TransferSyntax::JPEG2000Part2Lossless;
        }
        else if (type == "image/jpx" && transferSyntax == "1.2.840.10008.1.2.4.93")
        {
          return gdcm::TransferSyntax::JPEG2000Part2;
        }


        /**
         * Backward compatibility with DICOM 2014a
         * http://dicom.nema.org/medical/dicom/2014a/output/html/part18.html#table_6.5-1
         **/
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

        throw Orthanc::OrthancException(
          Orthanc::ErrorCode_BadRequest,
          "DICOMweb RetrieveFrames: Transfer syntax \"" + 
          transferSyntax + "\" is incompatible with media type \"" + type + "\"");
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
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                      "Invalid frame number (must be > 0): " + tokens[i]);
    }

    frames.push_back(static_cast<unsigned int>(frame - 1));
  }
}                           



static const char* GetMimeType(const gdcm::TransferSyntax& syntax)
{
  // http://dicom.nema.org/medical/dicom/current/output/html/part18.html#table_6.1.1.8-3b
  
  switch (syntax)
  {
    case gdcm::TransferSyntax::ImplicitVRLittleEndian:
      return "application/octet-stream";

    case gdcm::TransferSyntax::JPEGBaselineProcess1:
      return "image/jpeg; transfer-syntax=1.2.840.10008.1.2.4.50";

    case gdcm::TransferSyntax::JPEGExtendedProcess2_4:
      return "image/jpeg; transfer-syntax=1.2.840.10008.1.2.4.51";
    
    case gdcm::TransferSyntax::JPEGLosslessProcess14:
      return "image/jpeg; transfer-syntax=1.2.840.10008.1.2.4.57";

    case gdcm::TransferSyntax::JPEGLosslessProcess14_1:
      return "image/jpeg; transferSyntax=1.2.840.10008.1.2.4.70";
    
    case gdcm::TransferSyntax::RLELossless:
      return "image/x-dicom-rle; transferSyntax=1.2.840.10008.1.2.5";

    case gdcm::TransferSyntax::JPEGLSLossless:
      return "image/x-jls; transferSyntax=1.2.840.10008.1.2.4.80";

    case gdcm::TransferSyntax::JPEGLSNearLossless:
      return "image/x-jls; transfer-syntax=1.2.840.10008.1.2.4.81";

    case gdcm::TransferSyntax::JPEG2000Lossless:
      return "image/jp2; transferSyntax=1.2.840.10008.1.2.4.90";

    case gdcm::TransferSyntax::JPEG2000:
      return "image/jp2; transfer-syntax=1.2.840.10008.1.2.4.91";

    case gdcm::TransferSyntax::JPEG2000Part2Lossless:
      return "image/jpx; transferSyntax=1.2.840.10008.1.2.4.92";

    case gdcm::TransferSyntax::JPEG2000Part2:
      return "image/jpx; transfer-syntax=1.2.840.10008.1.2.4.93";

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
  error = OrthancPluginSendMultipartItem2(OrthancPlugins::GetGlobalContext(), output, frame, size, 1, keys, values);
#else
  error = OrthancPluginSendMultipartItem(OrthancPlugins::GetGlobalContext(), output, frame, size);
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

  if (OrthancPluginStartMultipartAnswer(OrthancPlugins::GetGlobalContext(), 
                                        output, "related", GetMimeType(syntax)) != OrthancPluginErrorCode_Success)
  {
    return false;
  }

  if (fragments == NULL)
  {
    // Single-fragment image

    if (pixelData.GetByteValue() == NULL)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError,
                                      "Image was not properly decoded");
    }

    int width, height, bits, samplesPerPixel;

    if (!dicom.GetIntegerTag(height, *dictionary_, OrthancPlugins::DICOM_TAG_ROWS) ||
        !dicom.GetIntegerTag(width, *dictionary_, OrthancPlugins::DICOM_TAG_COLUMNS) ||
        !dicom.GetIntegerTag(bits, *dictionary_, OrthancPlugins::DICOM_TAG_BITS_ALLOCATED) || 
        !dicom.GetIntegerTag(samplesPerPixel, *dictionary_, OrthancPlugins::DICOM_TAG_SAMPLES_PER_PIXEL))
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
    }

    size_t frameSize = height * width * bits * samplesPerPixel / 8;
    
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
        throw Orthanc::OrthancException(
          Orthanc::ErrorCode_ParameterOutOfRange,
          "Trying to access frame number " + boost::lexical_cast<std::string>(*frame + 1) + 
          " of an image with " + boost::lexical_cast<std::string>(framesCount) + " frames");
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
        // TODO A frame is not a fragment, looks like a bug
        throw Orthanc::OrthancException(
          Orthanc::ErrorCode_ParameterOutOfRange,
          "Trying to access frame number " + 
          boost::lexical_cast<std::string>(*frame + 1) + 
          " of an image with " + 
          boost::lexical_cast<std::string>(fragments->GetNumberOfFragments()) + 
          " frames");
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
  std::string publicId;
  OrthancPlugins::MemoryBuffer content;
  if (LocateInstance(output, publicId, request) &&
      content.RestApiGet("/instances/" + publicId + "/file", false) &&
      OrthancPlugins::RestApiGet(header, "/instances/" + publicId + "/header?simplify", false))
  {
    {
      std::string s = "DICOMweb RetrieveFrames on " + publicId + ", frames: ";
      for (std::list<unsigned int>::const_iterator 
             frame = frames.begin(); frame != frames.end(); ++frame)
      {
        s += boost::lexical_cast<std::string>(*frame + 1) + " ";
      }

      OrthancPlugins::LogInfo(s);
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
        OrthancPlugins::LogInfo("DICOMweb RetrieveFrames: Transcoding instance " + publicId + 
                                " from transfer syntax " + std::string(sourceSyntax.GetString()) + 
                                " to " + std::string(targetSyntax.GetString()));
      }

      gdcm::ImageChangeTransferSyntax change;
      change.SetTransferSyntax(targetSyntax);

      // TODO Avoid this unnecessary memcpy by defining a stream over the MemoryBuffer
      std::string dicom(content.GetData(), content.GetData() + content.GetSize());
      std::stringstream stream(dicom);

      gdcm::ImageReader reader;
      reader.SetStream(stream);
      if (!reader.Read())
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat,
                                        "Cannot decode the image");
      }

      change.SetInput(reader.GetImage());
      if (!change.Change())
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError,
                                        "Cannot change the transfer syntax of the image");
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
