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


namespace
{
  enum WindowingMode
  {
    WindowingMode_WholeDynamics,
    WindowingMode_Linear,
    WindowingMode_LinearExact,
    WindowingMode_Sigmoid
  };

  class RenderingParameters : public boost::noncopyable
  {
  private:
    bool          hasViewport_;
    bool          hasQuality_;
    bool          hasWindowing_;
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
    float         windowCenter_;
    float         windowWidth_;
    WindowingMode windowingMode_;
    float         rescaleSlope_;
    float         rescaleIntercept_;

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
      hasWindowing_(false),
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
      flipX_(false),
      flipY_(false),
      quality_(90),   // Default quality for JPEG previews (the same as in Orthanc core)
      windowCenter_(128),
      windowWidth_(256),
      windowingMode_(WindowingMode_WholeDynamics),
      rescaleSlope_(1),
      rescaleIntercept_(0)
    {
      static const std::string VIEWPORT("\"viewport\" in WADO-RS Retrieve Rendered Transaction");
      static const std::string WINDOWING("\"windowing\" in WADO-RS Retrieve Rendered Transaction");
      
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

          hasSW_ = GetIntegerValue(tmp, tokens, 4, true, true, VIEWPORT);
          if (hasSW_)
          {
            sw_ = static_cast<unsigned int>(tmp < 0 ? -tmp : tmp);  // Take absolute value
            flipX_ = (tmp < 0);
          }

          hasSH_ = GetIntegerValue(tmp, tokens, 5, true, true, VIEWPORT);
          if (hasSH_)
          {
            sh_ = static_cast<unsigned int>(tmp < 0 ? -tmp : tmp);  // Take absolute value
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
        else if (key == "windowing")
        {
          hasWindowing_ = true;

          std::vector<std::string> tokens;
          Orthanc::Toolbox::TokenizeString(tokens, value, ',');

          if (tokens.size() != 3)
          {
            throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                            "The number arguments to " + WINDOWING + " must be 3");
          }

          try
          {
            windowCenter_ = boost::lexical_cast<float>(tokens[0]);
            windowWidth_ = boost::lexical_cast<float>(tokens[1]);
          }
          catch (boost::bad_lexical_cast&)
          {
            throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                            "The first and second arguments to " + WINDOWING + " must be floats: " + value);
          }

          if (tokens[2] == "linear")
          {
            windowingMode_ = WindowingMode_Linear;
          }
          else if (tokens[2] == "linear-exact")
          {
            windowingMode_ = WindowingMode_LinearExact;
          }
          else if (tokens[2] == "sigmoid")
          {
            windowingMode_ = WindowingMode_Sigmoid;
          }
          else
          {
            throw Orthanc::OrthancException(
              Orthanc::ErrorCode_ParameterOutOfRange,
              "The third argument to " + WINDOWING + " must be linear, linear-exact or sigmoid: " + tokens[2]);
          }
        }
      }
    }


    bool HasCustomization() const
    {
      return (hasViewport_ || hasQuality_ || hasWindowing_);
    }

    unsigned int GetTargetWidth(unsigned int sourceWidth) const
    {
      if (hasVW_)
      {
        return vw_;
      }
      else
      {
        return sourceWidth;
      }
    }

    unsigned int GetTargetHeight(unsigned int sourceHeight) const
    {
      if (hasVH_)
      {
        return vh_;
      }
      else
      {
        return sourceHeight;
      }
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

    bool IsWindowing() const
    {
      return hasWindowing_;
    }

    float GetWindowCenter() const
    {
      return windowCenter_;
    }

    float GetWindowWidth() const
    {
      return windowWidth_;
    }

    WindowingMode GetWindowingMode() const
    {
      return windowingMode_;
    }    

    void SetRescaleSlope(float v) 
    {
      rescaleSlope_ = v;
    }

    float GetRescaleSlope() const
    {
      return rescaleSlope_;
    }

    void SetRescaleIntercept(float v) 
    {
      rescaleIntercept_ = v;
    }

    float GetRescaleIntercept() const
    {
      return rescaleIntercept_;
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


template <Orthanc::PixelFormat SourceFormat>
static void ApplyWindowing(Orthanc::ImageAccessor& target,
                           const Orthanc::ImageAccessor& source,
                           float c,
                           float w,
                           WindowingMode mode,
                           float rescaleSlope,
                           float rescaleIntercept)
{
  assert(target.GetFormat() == Orthanc::PixelFormat_Grayscale8 &&
         source.GetFormat() == SourceFormat);

  if (source.GetWidth() != target.GetWidth() ||
      source.GetHeight() != target.GetHeight())
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_IncompatibleImageSize);
  }

  const unsigned int width = source.GetWidth();
  const unsigned int height = source.GetHeight();

  const float ymin = 0;
  const float ymax = 255;
  

  /**
     
     LINEAR:
     http://dicom.nema.org/MEDICAL/dicom/2019a/output/chtml/part03/sect_C.11.2.html#sect_C.11.2.1.2.1

     Python
     ------

     import sympy as sym
     x, c, w, ymin, ymax = sym.symbols('x c w ymin ymax')

     e = ((x - (c - 0.5)) / (w-1) + 0.5) * (ymax- ymin) + ymin
     print(sym.simplify(sym.collect(sym.expand(e), [ x, ymin, ymax ])))

     Result
     ------

     (x*(ymax - ymin) + ymax*(-c + 0.5*w) + ymin*(c + 0.5*w - 1.0))/(w - 1)

   **/

  const float linearXMin = (c - 0.5f - (w - 1.0f) / 2.0f);
  const float linearXMax = (c - 0.5f + (w - 1.0f) / 2.0f);
  const float linearYScaling = (ymax - ymin) / (w - 1.0f);
  const float linearYOffset = (ymax * (-c + 0.5f * w) + ymin * (c + 0.5f * w - 1.0f)) / (w - 1.0f);


  /**

     LINEAR-EXACT:
     http://dicom.nema.org/MEDICAL/dicom/2019a/output/chtml/part03/sect_C.11.2.html#sect_C.11.2.1.3.2

     Python
     ------

     import sympy as sym
     x, c, w, ymin, ymax = sym.symbols('x c w ymin ymax')

     e = (x - c) / w * (ymax- ymin) + ymin
     print(sym.simplify(sym.collect(sym.expand(e), [ x, ymin, ymax ])))

     Result
     ------

     (-c*ymax + x*(ymax - ymin) + ymin*(c + w))/w

   **/
  const float exactXMin = (c - w / 2.0f);
  const float exactXMax = (c + w / 2.0f);
  const float exactYScaling = (ymax - ymin) / w;
  const float exactYOffset = (-c * ymax + ymin * (c + w)) / w;
       

  float minValue = std::numeric_limits<float>::infinity();
  float maxValue = -std::numeric_limits<float>::infinity();
  float wholeDynamicsScale = 1;
 
  if (mode == WindowingMode_WholeDynamics)
  {
    for (unsigned int y = 0; y < height; y++)
    {
      for (unsigned int x = 0; x < width; x++)
      {
        float a = Orthanc::ImageTraits<SourceFormat>::GetFloatPixel(source, x, y);
        minValue = std::min(minValue, a);
        maxValue = std::max(maxValue, a);
      }
    }

    minValue = rescaleSlope * minValue + rescaleIntercept;
    maxValue = rescaleSlope * maxValue + rescaleIntercept;
    wholeDynamicsScale = 255.0f / (maxValue - minValue);
  }
  
  
  for (unsigned int y = 0; y < height; y++)
  {
    for (unsigned int x = 0; x < width; x++)
    {
      float a = Orthanc::ImageTraits<SourceFormat>::GetFloatPixel(source, x, y);
      a = rescaleSlope * a + rescaleIntercept;

      float b;

      switch (mode)
      {
        case WindowingMode_WholeDynamics:
          b = (a - minValue) * wholeDynamicsScale;
          break;

        case WindowingMode_Linear:
        {
          if (a <= linearXMin)
          {
            b = ymin;
          }
          else if (a > linearXMax)
          {
            b = ymax;
          }
          else
          {
            b = a * linearYScaling + linearYOffset;
          }

          break;
        }

        case WindowingMode_LinearExact:
        {
          if (a <= exactXMin)
          {
            b = ymin;
          }
          else if (a > exactXMax)
          {
            b = ymax;
          }
          else
          {
            b = a * exactYScaling + exactYOffset;
          }

          break;
        }

        case WindowingMode_Sigmoid:
        {
          // http://dicom.nema.org/MEDICAL/dicom/2019a/output/chtml/part03/sect_C.11.2.html#sect_C.11.2.1.3.1
          b = ymax / (1.0f + expf(-4.0f * (a - c) / w));
          break;
        }

        default:
          throw Orthanc::OrthancException(Orthanc::ErrorCode_NotImplemented);
      }

      Orthanc::ImageTraits<Orthanc::PixelFormat_Grayscale8>::SetFloatPixel(target, b, x, y);
    }
  }
}


static void ApplyRendering(Orthanc::ImageAccessor& target,
                           const Orthanc::ImageAccessor& source,
                           const RenderingParameters& parameters)
{
  Orthanc::ImageProcessing::Set(target, 0);

  Orthanc::ImageAccessor region;
  parameters.GetSourceRegion(region, source);
  
  Orthanc::Image scaled(target.GetFormat(), region.GetWidth(), region.GetHeight(), false);

  if (scaled.GetWidth() == 0 ||
      scaled.GetHeight() == 0)
  {
    return;
  }  

  switch (target.GetFormat())
  {
    case Orthanc::PixelFormat_RGB24:
      // Windowing is not taken into consideration for color images
      Orthanc::ImageProcessing::Convert(scaled, region);
      break;

    case Orthanc::PixelFormat_Grayscale8:
    {
      switch (source.GetFormat())
      {
        case Orthanc::PixelFormat_Grayscale16:
          ApplyWindowing<Orthanc::PixelFormat_Grayscale16>(scaled, region, parameters.GetWindowCenter(),
                                                           parameters.GetWindowWidth(),
                                                           parameters.GetWindowingMode(),
                                                           parameters.GetRescaleSlope(),
                                                           parameters.GetRescaleIntercept());
          break;

        case Orthanc::PixelFormat_SignedGrayscale16:
          ApplyWindowing<Orthanc::PixelFormat_SignedGrayscale16>(scaled, region, parameters.GetWindowCenter(),
                                                                 parameters.GetWindowWidth(),
                                                                 parameters.GetWindowingMode(),
                                                                 parameters.GetRescaleSlope(),
                                                                 parameters.GetRescaleIntercept());
          break;

        default:
          throw Orthanc::OrthancException(Orthanc::ErrorCode_NotImplemented);
      }
          
      break;
    }

    default:
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NotImplemented);
  }

  if (parameters.IsFlipX())
  {
    Orthanc::ImageProcessing::FlipX(scaled);
  }

  if (parameters.IsFlipY())
  {
    Orthanc::ImageProcessing::FlipY(scaled);
  }

  // Preserve the aspect ratio
  float cw = static_cast<float>(scaled.GetWidth());
  float ch = static_cast<float>(scaled.GetHeight());
  float r = std::min(
    static_cast<float>(target.GetWidth()) / cw,
    static_cast<float>(target.GetHeight()) / ch);

  unsigned int sw = std::min(static_cast<unsigned int>(boost::math::iround(cw * r)), target.GetWidth());  
  unsigned int sh = std::min(static_cast<unsigned int>(boost::math::iround(ch * r)), target.GetHeight());
  Orthanc::Image resized(target.GetFormat(), sw, sh, false);
  
  //Orthanc::ImageProcessing::SmoothGaussian5x5(scaled);
  Orthanc::ImageProcessing::Resize(resized, scaled);

  assert(target.GetWidth() >= resized.GetWidth() &&
         target.GetHeight() >= resized.GetHeight());
  unsigned int offsetX = (target.GetWidth() - resized.GetWidth()) / 2;
  unsigned int offsetY = (target.GetHeight() - resized.GetHeight()) / 2;

  target.GetRegion(region, offsetX, offsetY, resized.GetWidth(), resized.GetHeight());
  Orthanc::ImageProcessing::Copy(region, resized);
}


static void AnswerFrameRendered(OrthancPluginRestOutput* output,
                                int frame,
                                const OrthancPluginHttpRequest* request)
{
  static const char* const RESCALE_INTERCEPT = "0028,1052";
  static const char* const RESCALE_SLOPE = "0028,1053";

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

          Json::Value tags;
          buffer.DicomToJson(tags, OrthancPluginDicomToJsonFormat_Short, OrthancPluginDicomToJsonFlags_None, 255);
          
          if (tags.isMember(RESCALE_SLOPE) &&
              tags[RESCALE_SLOPE].type() == Json::stringValue)
          {
            try
            {
              parameters.SetRescaleSlope
                (boost::lexical_cast<float>
                 (Orthanc::Toolbox::StripSpaces(tags[RESCALE_SLOPE].asString())));
            }
            catch (boost::bad_lexical_cast&)
            {
            }
          }

          if (tags.isMember(RESCALE_INTERCEPT) &&
              tags[RESCALE_INTERCEPT].type() == Json::stringValue)
          {
            try
            {
              parameters.SetRescaleIntercept
                (boost::lexical_cast<float>
                 (Orthanc::Toolbox::StripSpaces(tags[RESCALE_INTERCEPT].asString())));
            }
            catch (boost::bad_lexical_cast&)
            {
            }
          }

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