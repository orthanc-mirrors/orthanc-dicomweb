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


#include "Dicom.h"

#include "Plugin.h"
#include "ChunkedBuffer.h"

#include <Core/Toolbox.h>

#include <gdcmDictEntry.h>
#include <gdcmStringFilter.h>
#include <boost/lexical_cast.hpp>
#include <json/writer.h>


namespace OrthancPlugins
{
  static std::string MyStripSpaces(const std::string& source)
  {
    size_t first = 0;

    while (first < source.length() &&
           (isspace(source[first]) || 
            source[first] == '\0'))
    {
      first++;
    }

    if (first == source.length())
    {
      // String containing only spaces
      return "";
    }

    size_t last = source.length();
    while (last > first &&
           (isspace(source[last - 1]) ||
            source[last - 1] == '\0'))
    {
      last--;
    }          
    
    assert(first <= last);
    return source.substr(first, last - first);
  }


  static const char* GetVRName(bool& isSequence,
                               const gdcm::Dict& dictionary,
                               const gdcm::Tag& tag,
                               gdcm::VR vr)
  {
    if (vr == gdcm::VR::INVALID)
    {
      const gdcm::DictEntry &entry = dictionary.GetDictEntry(tag);
      vr = entry.GetVR();

      if (vr == gdcm::VR::OB_OW)
      {
        vr = gdcm::VR::OB;
      }
    }

    isSequence = (vr == gdcm::VR::SQ);

    const char* str = gdcm::VR::GetVRString(vr);
    if (isSequence)
    {
      return str;
    }

    if (str == NULL ||
        strlen(str) != 2 ||
        !(str[0] >= 'A' && str[0] <= 'Z') ||
        !(str[1] >= 'A' && str[1] <= 'Z'))
    {
      return "UN";
    }
    else
    {
      return str;
    }
  }


  static const char* GetVRName(bool& isSequence,
                               const gdcm::Dict& dictionary,
                               const gdcm::DataElement& element)
  {
    return GetVRName(isSequence, dictionary, element.GetTag(), element.GetVR());
  }


  template <int T>
  static void ConvertNumberTag(std::string& target,
                               const gdcm::DataElement& source)
  {
    if (source.IsEmpty())
    {
      target.clear();
    }
    else
    {
      typename gdcm::Element<T, gdcm::VM::VM1_n> element;

      element.Set(source.GetValue());

      for (unsigned int i = 0; i < element.GetLength(); i++)
      {
        if (i != 0)
        {
          target += "\\";
        }
      
        target = boost::lexical_cast<std::string>(element.GetValue());
      }
    }
  }


  static bool ConvertDicomStringToUtf8(std::string& result,
                                       const gdcm::Dict& dictionary,
                                       const gdcm::DataElement& element,
                                       const Orthanc::Encoding sourceEncoding)
  {
    const gdcm::ByteValue* data = element.GetByteValue();
    if (!data)
    {
      return false;
    }

    bool isSequence;
    std::string vr = GetVRName(isSequence, dictionary, element);

    if (!isSequence)
    {
      if (vr == "FL")
      {
        ConvertNumberTag<gdcm::VR::FL>(result, element);
        return true;
      }
      else if (vr == "FD")
      {
        ConvertNumberTag<gdcm::VR::FD>(result, element);
        return true;
      }
      else if (vr == "SL")
      {
        ConvertNumberTag<gdcm::VR::SL>(result, element);
        return true;
      }
      else if (vr == "SS")
      {
        ConvertNumberTag<gdcm::VR::SS>(result, element);
        return true;
      }
      else if (vr == "UL")
      {
        ConvertNumberTag<gdcm::VR::UL>(result, element);
        return true;
      }
      else if (vr == "US")
      {
        ConvertNumberTag<gdcm::VR::US>(result, element);
        return true;
      }
    }

    if (sourceEncoding == Orthanc::Encoding_Utf8)
    {
      result.assign(data->GetPointer(), data->GetLength());
    }
    else
    {
      std::string tmp(data->GetPointer(), data->GetLength());
      result = Orthanc::Toolbox::ConvertToUtf8(tmp, sourceEncoding, false);
    }

    result = MyStripSpaces(result);
    return true;
  }



  void ParsedDicomFile::Setup(const std::string& dicom)
  {
    // Prepare a memory stream over the DICOM instance
    std::stringstream stream(dicom);

    // Parse the DICOM instance using GDCM
    reader_.SetStream(stream);

    if (!reader_.Read())
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat,
                                      "GDCM cannot decode this DICOM instance of length " +
                                      boost::lexical_cast<std::string>(dicom.size()));
    }
  }


  ParsedDicomFile::ParsedDicomFile(const OrthancPlugins::MemoryBuffer& buffer)
  {
    // TODO Avoid this unnecessary memcpy by defining a stream over the MemoryBuffer
    std::string dicom(buffer.GetData(), buffer.GetData() + buffer.GetSize());
    Setup(dicom);
  }


  static bool GetRawTag(std::string& result,
                        const gdcm::DataSet& dataset,
                        const gdcm::Tag& tag,
                        bool stripSpaces)
  {
    if (dataset.FindDataElement(tag))
    {
      const gdcm::ByteValue* value = dataset.GetDataElement(tag).GetByteValue();
      if (value)
      {
        result.assign(value->GetPointer(), value->GetLength());

        if (stripSpaces)
        {
          result = MyStripSpaces(result);
        }

        return true;
      }
    }

    return false;
  }


  bool ParsedDicomFile::GetRawTag(std::string& result,
                                  const gdcm::Tag& tag,
                                  bool stripSpaces) const
  {
    return OrthancPlugins::GetRawTag(result, GetDataSet(), tag, stripSpaces);
  }


  std::string ParsedDicomFile::GetRawTagWithDefault(const gdcm::Tag& tag,
                                                    const std::string& defaultValue,
                                                    bool stripSpaces) const
  {
    std::string result;
    if (!GetRawTag(result, tag, stripSpaces))
    {
      return defaultValue;
    }
    else
    {
      return result;
    }
  }


  std::string ParsedDicomFile::GetRawTagWithDefault(const Orthanc::DicomTag& tag,
                                                    const std::string& defaultValue,
                                                    bool stripSpaces) const
  {
    gdcm::Tag t(tag.GetGroup(), tag.GetElement());
    return GetRawTagWithDefault(t, defaultValue, stripSpaces);
  }


  bool ParsedDicomFile::GetStringTag(std::string& result,
                                     const gdcm::Dict& dictionary,
                                     const gdcm::Tag& tag,
                                     bool stripSpaces) const
  {
    if (!GetDataSet().FindDataElement(tag))
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentTag);
    }

    const gdcm::DataElement& element = GetDataSet().GetDataElement(tag);

    if (!ConvertDicomStringToUtf8(result, dictionary, element, GetEncoding()))
    {
      return false;
    }

    if (stripSpaces)
    {
      result = MyStripSpaces(result);
    }

    return true;
  }


  bool ParsedDicomFile::GetIntegerTag(int& result,
                                      const gdcm::Dict& dictionary,
                                      const gdcm::Tag& tag) const
  {
    std::string tmp;
    if (!GetStringTag(tmp, dictionary, tag, true))
    {
      return false;
    }

    try
    {
      result = boost::lexical_cast<int>(tmp);
      return true;
    }
    catch (boost::bad_lexical_cast&)
    {
      return false;
    }
  }


  std::string FormatTag(const gdcm::Tag& tag)
  {
    char tmp[16];
    sprintf(tmp, "%04X%04X", tag.GetGroup(), tag.GetElement());
    return std::string(tmp);
  }


  static std::string GetWadoUrl(const std::string& wadoBase,
                                const gdcm::DataSet& dicom)
  {
    static const gdcm::Tag DICOM_TAG_STUDY_INSTANCE_UID(0x0020, 0x000d);
    static const gdcm::Tag DICOM_TAG_SERIES_INSTANCE_UID(0x0020, 0x000e);
    static const gdcm::Tag DICOM_TAG_SOP_INSTANCE_UID(0x0008, 0x0018);

    std::string study, series, instance;

    if (!GetRawTag(study, dicom, DICOM_TAG_STUDY_INSTANCE_UID, true) ||
        !GetRawTag(series, dicom, DICOM_TAG_SERIES_INSTANCE_UID, true) ||
        !GetRawTag(instance, dicom, DICOM_TAG_SOP_INSTANCE_UID, true))
    {
      return "";
    }
    else
    {
      return Configuration::GetWadoUrl(wadoBase, study, series, instance);
    }
  }


  static Orthanc::Encoding DetectEncoding(const gdcm::DataSet& dicom)
  {
    static const gdcm::Tag DICOM_TAG_SPECIFIC_CHARACTER_SET(0x0008, 0x0005);

    if (!dicom.FindDataElement(DICOM_TAG_SPECIFIC_CHARACTER_SET))
    {
      return Orthanc::Encoding_Ascii;
    }

    const gdcm::DataElement& element = 
      dicom.GetDataElement(DICOM_TAG_SPECIFIC_CHARACTER_SET);

    const gdcm::ByteValue* data = element.GetByteValue();
    if (!data)
    {
      return Configuration::GetDefaultEncoding();
    }

    std::string tmp(data->GetPointer(), data->GetLength());
    tmp = MyStripSpaces(tmp);

    Orthanc::Encoding encoding;
    if (Orthanc::GetDicomEncoding(encoding, tmp.c_str()))
    {
      return encoding;
    }
    else
    {
      return Configuration::GetDefaultEncoding();
    }
  }


  Orthanc::Encoding  ParsedDicomFile::GetEncoding() const
  {
    return DetectEncoding(GetDataSet());
  }
  

  std::string ParsedDicomFile::GetWadoUrl(const OrthancPluginHttpRequest* request) const
  {
    const std::string base = OrthancPlugins::Configuration::GetBaseUrl(request);
    return OrthancPlugins::GetWadoUrl(base, GetDataSet());
  }
}
