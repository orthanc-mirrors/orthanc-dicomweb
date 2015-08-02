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


#pragma once

#include "../Orthanc/Core/ImageFormats/ImageAccessor.h"

#include <string>
#include <stdint.h>

namespace OrthancPlugins
{
  class JpegWriter
  {
  private:
    int  quality_;

  public:
    JpegWriter() : quality_(90)
    {
    }

    void SetQuality(uint8_t quality);

    uint8_t GetQuality() const
    {
      return quality_;
    }

    void WriteToFile(const char* filename,
                     unsigned int width,
                     unsigned int height,
                     unsigned int pitch,
                     Orthanc::PixelFormat format,
                     const void* buffer);

    void WriteToMemory(std::string& jpeg,
                       unsigned int width,
                       unsigned int height,
                       unsigned int pitch,
                       Orthanc::PixelFormat format,
                       const void* buffer);

    void WriteToFile(const char* filename,
                     const Orthanc::ImageAccessor& accessor)
    {
      WriteToFile(filename, accessor.GetWidth(), accessor.GetHeight(),
                  accessor.GetPitch(), accessor.GetFormat(), accessor.GetConstBuffer());
    }

    void WriteToMemory(std::string& jpeg,
                       const Orthanc::ImageAccessor& accessor)
    {
      WriteToMemory(jpeg, accessor.GetWidth(), accessor.GetHeight(),
                    accessor.GetPitch(), accessor.GetFormat(), accessor.GetConstBuffer());
    }
  };
}
