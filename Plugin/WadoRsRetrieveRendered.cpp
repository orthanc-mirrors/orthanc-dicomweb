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

#include <Core/Images/Image.h>
#include <Core/Images/ImageProcessing.h>
#include <Core/Images/ImageTraits.h>
#include <Core/Toolbox.h>
#include <Plugins/Samples/Common/OrthancPluginCppWrapper.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/math/special_functions/round.hpp>



namespace Orthanc
{
  namespace ImageProcessing
  {
    template <PixelFormat Format>
    void ResizeInternal(ImageAccessor& target,
                        const ImageAccessor& source)
    {     
      const unsigned int sourceHeight = source.GetHeight();
      const unsigned int sourceWidth = source.GetWidth();
      const unsigned int targetHeight = target.GetHeight();
      const unsigned int targetWidth = target.GetWidth();

      if (targetWidth == 0 || targetHeight == 0)
      {
        return;
      }

      if (sourceWidth == 0 || sourceHeight == 0)
      {
        // Avoids division by zero below
        Set(target, 0);
        return;
      }
      
      const float scaleX = static_cast<float>(sourceWidth) / static_cast<float>(targetWidth);
      const float scaleY = static_cast<float>(sourceHeight) / static_cast<float>(targetHeight);


      /**
       * Create two lookup tables to quickly know the (x,y) position
       * in the source image, given the (x,y) position in the target
       * image.
       **/
      
      std::vector<unsigned int>  lookupX(targetWidth);
      
      for (unsigned int x = 0; x < targetWidth; x++)
      {
        int sourceX = std::floor((static_cast<float>(x) + 0.5f) * scaleX);
        if (sourceX < 0)
        {
          sourceX = 0;  // Should never happen
        }
        else if (sourceX >= static_cast<int>(sourceWidth))
        {
          sourceX = sourceWidth - 1;
        }

        lookupX[x] = static_cast<unsigned int>(sourceX);
      }
      
      std::vector<unsigned int>  lookupY(targetHeight);
      
      for (unsigned int y = 0; y < targetHeight; y++)
      {
        int sourceY = std::floor((static_cast<float>(y) + 0.5f) * scaleY);
        if (sourceY < 0)
        {
          sourceY = 0;  // Should never happen
        }
        else if (sourceY >= static_cast<int>(sourceHeight))
        {
          sourceY = sourceHeight - 1;
        }

        lookupY[y] = static_cast<unsigned int>(sourceY);
      }


      /**
       * Actual resizing
       **/
      
      for (unsigned int targetY = 0; targetY < targetHeight; targetY++)
      {
        unsigned int sourceY = lookupY[targetY];

        for (unsigned int targetX = 0; targetX < targetWidth; targetX++)
        {
          unsigned int sourceX = lookupX[targetX];

          typename ImageTraits<Format>::PixelType pixel;
          ImageTraits<Format>::GetPixel(pixel, source, sourceX, sourceY);
          ImageTraits<Format>::SetPixel(target, pixel, targetX, targetY);
        }
      }            
    }

    
    void Resize(ImageAccessor& target,
                const ImageAccessor& source)
    {
      if (source.GetFormat() != source.GetFormat())
      {
        throw OrthancException(ErrorCode_IncompatibleImageFormat);
      }

      if (source.GetWidth() == target.GetWidth() &&
          source.GetHeight() == target.GetHeight())
      {
        Copy(target, source);
        return;
      }
      
      switch (source.GetFormat())
      {
        case PixelFormat_Grayscale8:
          ResizeInternal<PixelFormat_Grayscale8>(target, source);
          break;

        case PixelFormat_RGB24:
          ResizeInternal<PixelFormat_RGB24>(target, source);
          break;

        default:
          throw OrthancException(ErrorCode_NotImplemented);
      }
    }
  }
}


namespace
{
  class RenderingParameters : public boost::noncopyable
  {
  private:
    bool          hasViewport_;
    bool          hasQuality_;
    bool          hasVW_;
    bool          hasVH_;
    bool          hasSW_;
    bool          hasSH_;
    unsigned int  vw_;
    unsigned int  vh_;
    unsigned int  sx_;
    unsigned int  sy_;
    unsigned int  sw_;
    unsigned int  sh_;
    bool          flipX_;
    bool          flipY_;
    unsigned int  quality_;

    static bool GetIntegerValue(int& target,
                                std::vector<std::string>& tokens,
                                size_t index,
                                bool allowNegative,
                                bool allowFloat,
                                const std::string& message)
    {
      if (index >= tokens.size() ||
          tokens[index].empty())
      {
        return false;
      }          
      
      try
      {
        if (allowFloat)
        {
          float value = boost::lexical_cast<float>(tokens[index]);
          target = boost::math::iround(value);
        }
        else
        {
          target = boost::lexical_cast<int>(tokens[index]);
        }
      }
      catch (boost::bad_lexical_cast&)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                        "Out-of-range value for " + message + ": " + tokens[index]);
      }

      if (!allowNegative && target < 0)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                        "Negative values disallowed for " + message + ": " + tokens[index]);
      }
      
      return true;
    }
    
  public:
    RenderingParameters(const OrthancPluginHttpRequest* request) :
      hasViewport_(false),
      hasQuality_(false),
      hasVW_(false),
      hasVH_(false),
      hasSW_(false),
      hasSH_(false),
      vw_(0),
      vh_(0),
      sx_(0),
      sy_(0),
      sw_(0),
      sh_(0),
      quality_(90)   // Default quality for JPEG previews (the same as in Orthanc core)
    {
      static const std::string VIEWPORT("\"viewport\" in WADO-RS Retrieve Rendered Transaction");
      
      for (uint32_t i = 0; i < request->getCount; i++)
      {
        const std::string key = request->getKeys[i];
        const std::string value = request->getValues[i];
        
        if (key == "viewport")
        {
          hasViewport_ = true;

          std::vector<std::string> tokens;
          Orthanc::Toolbox::TokenizeString(tokens, value, ',');
          if (tokens.size() != 2 &&
              tokens.size() != 6)
          {
            throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                            "The number arguments to " + VIEWPORT + " must be 2 or 6");
          }

          int tmp;

          hasVW_ = GetIntegerValue(tmp, tokens, 0, false, false, VIEWPORT);
          if (hasVW_)
          {
            assert(tmp >= 0);
            vw_ = static_cast<unsigned int>(tmp);
          }

          hasVH_ = GetIntegerValue(tmp, tokens, 1, false, false, VIEWPORT);
          if (hasVH_)
          {
            assert(tmp >= 0);
            vh_ = static_cast<unsigned int>(tmp);
          }

          if (GetIntegerValue(tmp, tokens, 2, true, true, VIEWPORT))
          {
            sx_ = static_cast<unsigned int>(tmp < 0 ? -tmp : tmp);  // Take absolute value
          }
          else
          {
            sx_ = 0;  // Default is zero
          }

          if (GetIntegerValue(tmp, tokens, 3, true, true, VIEWPORT))
          {
            sy_ = static_cast<unsigned int>(tmp < 0 ? -tmp : tmp);  // Take absolute value
          }
          else
          {
            sy_ = 0;  // Default is zero
          }

          hasSW_ = GetIntegerValue(tmp, tokens, 0, true, true, VIEWPORT);
          if (hasSW_)
          {
            vw_ = static_cast<unsigned int>(tmp < 0 ? -tmp : tmp);  // Take absolute value
            flipX_ = (tmp < 0);
          }

          hasSH_ = GetIntegerValue(tmp, tokens, 1, true, true, VIEWPORT);
          if (hasSH_)
          {
            vh_ = static_cast<unsigned int>(tmp < 0 ? -tmp : tmp);  // Take absolute value
            flipY_ = (tmp < 0);
          }
        }
        else if (key == "quality")
        {
          hasQuality_ = true;

          bool ok = false;
          int q;
          try
          {
            q = boost::lexical_cast<int>(value);
            ok = (q >= 1 && q <= 100);
          }
          catch (boost::bad_lexical_cast&)
          {
          }

          if (!ok)
          {
            throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                            "The value of \"quality\" in WADO-RS Retrieve Rendered Transaction "
                                            "must be between 1 and 100, found: " + value);
          }

          quality_ = static_cast<unsigned int>(q);
        }
      }
    }


    bool HasCustomization() const
    {
      return (hasViewport_ || hasQuality_);
    }

    unsigned int GetTargetWidth(unsigned int sourceWidth) const
    {
      return (hasVW_ ? vw_ : sourceWidth);
    }

    unsigned int GetTargetHeight(unsigned int sourceHeight) const
    {
      return (hasVH_ ? vh_ : sourceHeight);
    }

    bool IsFlipX() const
    {
      return flipX_;
    }

    bool IsFlipY() const
    {
      return flipY_;
    }    

    void GetSourceRegion(Orthanc::ImageAccessor& region,
                         const Orthanc::ImageAccessor& source) const
    {
      if (sx_ >= source.GetWidth() ||
          sy_ >= source.GetHeight())
      {
        region.AssignEmpty(source.GetFormat());
      }
      else
      {
        unsigned int right = source.GetWidth();
        if (hasSW_ &&
            sx_ + sw_ < source.GetWidth())
        {
          right = sx_ + sw_;
        }

        unsigned int bottom = source.GetHeight();
        if (hasSH_ &&
            sy_ + sh_ < source.GetHeight())
        {
          bottom = sy_ + sh_;
        }

        assert(sx_ <= right &&
               sy_ <= bottom &&
               right <= source.GetWidth() &&
               bottom <= source.GetHeight());
        source.GetRegion(region, sx_, sy_, right - sx_, bottom - sy_);
      }
    }

    unsigned int GetQuality() const
    {
      return quality_;
    }
  };
}


static Orthanc::PixelFormat Convert(OrthancPluginPixelFormat format)
{
  switch (format)
  {
    case OrthancPluginPixelFormat_BGRA32:
      return Orthanc::PixelFormat_BGRA32;

    case OrthancPluginPixelFormat_Float32:
      return Orthanc::PixelFormat_Float32;

    case OrthancPluginPixelFormat_Grayscale16:
      return Orthanc::PixelFormat_Grayscale16;

    case OrthancPluginPixelFormat_Grayscale32:
      return Orthanc::PixelFormat_Grayscale32;

    case OrthancPluginPixelFormat_Grayscale64:
      return Orthanc::PixelFormat_Grayscale64;

    case OrthancPluginPixelFormat_Grayscale8:
      return Orthanc::PixelFormat_Grayscale8;

    case OrthancPluginPixelFormat_RGB24:
      return Orthanc::PixelFormat_RGB24;

    case OrthancPluginPixelFormat_RGB48:
      return Orthanc::PixelFormat_RGB48;

    case OrthancPluginPixelFormat_RGBA32:
      return Orthanc::PixelFormat_RGBA32;

    case OrthancPluginPixelFormat_SignedGrayscale16:
      return Orthanc::PixelFormat_SignedGrayscale16;

    default:
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
  }
}


static void ApplyRendering(Orthanc::ImageAccessor& target,
                           const Orthanc::ImageAccessor& source,
                           const RenderingParameters& parameters)
{
  Orthanc::Image tmp(target.GetFormat(), source.GetWidth(), source.GetHeight(), false);
  Orthanc::ImageProcessing::Convert(tmp, source);
  Orthanc::ImageProcessing::Resize(target, tmp);
}


static void AnswerFrameRendered(OrthancPluginRestOutput* output,
                                int frame,
                                const OrthancPluginHttpRequest* request)
{
  OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(context, output, "GET");
  }
  else
  {
    std::string instanceId;
    if (LocateInstance(output, instanceId, request))
    {
      Orthanc::MimeType mime = Orthanc::MimeType_Jpeg;  // This is the default in DICOMweb
      
      for (uint32_t i = 0; i < request->headersCount; i++)
      {
        if (boost::iequals(request->headersKeys[i], "Accept") &&
            !boost::iequals(request->headersValues[i], "*/*"))
        {
          try
          {
            // TODO - Support conversion to GIF
        
            mime = Orthanc::StringToMimeType(request->headersValues[i]);
            if (mime != Orthanc::MimeType_Png &&
                mime != Orthanc::MimeType_Jpeg)
            {
              throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
            }
          }
          catch (Orthanc::OrthancException&)
          {
            LOG(ERROR) << "Unsupported MIME type in WADO-RS rendered frame: " << request->headersValues[i];
            throw;
          }
        }
      }

      RenderingParameters parameters(request);
      
      OrthancPlugins::MemoryBuffer buffer;
      bool badFrameError = false;
      
      if (parameters.HasCustomization())
      {
        if (frame <= 0)
        {
          badFrameError = true;
        }
        else
        {
          buffer.GetDicomInstance(instanceId);

          OrthancPlugins::OrthancImage dicom;
          dicom.DecodeDicomImage(buffer.GetData(), buffer.GetSize(), static_cast<unsigned int>(frame - 1));

          Orthanc::PixelFormat targetFormat;
          OrthancPluginPixelFormat sdkFormat;
          if (dicom.GetPixelFormat() == OrthancPluginPixelFormat_RGB24)
          {
            targetFormat = Orthanc::PixelFormat_RGB24;
            sdkFormat = OrthancPluginPixelFormat_RGB24;
          }
          else
          {
            targetFormat = Orthanc::PixelFormat_Grayscale8;
            sdkFormat = OrthancPluginPixelFormat_Grayscale8;
          }

          Orthanc::ImageAccessor source;
          source.AssignReadOnly(Convert(dicom.GetPixelFormat()),
                                dicom.GetWidth(), dicom.GetHeight(), dicom.GetPitch(), dicom.GetBuffer());
          
          Orthanc::Image target(targetFormat, parameters.GetTargetWidth(source.GetWidth()),
                                parameters.GetTargetHeight(source.GetHeight()), false);

          ApplyRendering(target, source, parameters);
          
          switch (mime)
          {
            case Orthanc::MimeType_Png:
              OrthancPluginCompressAndAnswerPngImage(OrthancPlugins::GetGlobalContext(), output, sdkFormat,
                                                     target.GetWidth(), target.GetHeight(), target.GetPitch(), target.GetBuffer());
              break;
              
            case Orthanc::MimeType_Jpeg:
              OrthancPluginCompressAndAnswerJpegImage(OrthancPlugins::GetGlobalContext(), output, sdkFormat,
                                                      target.GetWidth(), target.GetHeight(), target.GetPitch(), target.GetBuffer(),
                                                      parameters.GetQuality());
              break;

            default:
              throw Orthanc::OrthancException(Orthanc::ErrorCode_NotImplemented);
          }
        }
      }
      else
      {
        // No customization of the rendering. Return the default
        // preview of Orthanc.
        std::map<std::string, std::string> headers;
        headers["Accept"] = Orthanc::EnumerationToString(mime);

        // In DICOMweb, the "frame" parameter is in the range [1..N],
        // whereas Orthanc uses range [0..N-1], hence the "-1" below.
        if (buffer.RestApiGet("/instances/" + instanceId + "/frames/" +
                              boost::lexical_cast<std::string>(frame - 1) + "/preview", headers, false))
        {
          OrthancPluginAnswerBuffer(context, output, buffer.GetData(),
                                    buffer.GetSize(), Orthanc::EnumerationToString(mime));
        }
        else
        {
          badFrameError = true;
        }
      }

      if (badFrameError)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                        "Inexistent frame index in this image: " + boost::lexical_cast<std::string>(frame));
      }
    }
    else
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem, "Inexistent instance");
    }
  }
}


void RetrieveInstanceRendered(OrthancPluginRestOutput* output,
                              const char* url,
                              const OrthancPluginHttpRequest* request)
{
  AnswerFrameRendered(output, 1 /* first frame */, request);
}


void RetrieveFrameRendered(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request)
{
  assert(request->groupsCount == 4);
  const char* frame = request->groups[3];

  AnswerFrameRendered(output, boost::lexical_cast<int>(frame), request);
}
