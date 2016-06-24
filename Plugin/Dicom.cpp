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


#include "Dicom.h"

#include "Plugin.h"
#include "ChunkedBuffer.h"

#include "../Orthanc/Core/Toolbox.h"

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


  const char* GetVRName(bool& isSequence,
                        const gdcm::Dict& dictionary,
                        const gdcm::Tag& tag)
  {
    return GetVRName(isSequence, dictionary, tag, gdcm::VR::INVALID);
  }


  static const char* GetVRName(bool& isSequence,
                               const gdcm::Dict& dictionary,
                               const gdcm::DataElement& element)
  {
    return GetVRName(isSequence, dictionary, element.GetTag(), element.GetVR());
  }


  static bool ConvertDicomStringToUtf8(std::string& result,
                                       const gdcm::Dict& dictionary,
                                       const gdcm::File* file,
                                       const gdcm::DataElement& element,
                                       const Orthanc::Encoding sourceEncoding)
  {
    const gdcm::ByteValue* data = element.GetByteValue();
    if (!data)
    {
      return false;
    }

    if (file != NULL)
    {
      bool isSequence;
      std::string vr = GetVRName(isSequence, dictionary, element);
      if (!isSequence && (
            vr == "FL" ||
            vr == "FD" ||
            vr == "SL" ||
            vr == "SS" ||
            vr == "UL" ||
            vr == "US"
            ))
      {      
        gdcm::StringFilter f;
        f.SetFile(*file);
        result = f.ToString(element.GetTag());
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
      result = Orthanc::Toolbox::ConvertToUtf8(tmp, sourceEncoding);
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
      /* "GDCM cannot read this DICOM instance of length " +
         boost::lexical_cast<std::string>(dicom.size()) */
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
    }
  }


  ParsedDicomFile::ParsedDicomFile(const OrthancPlugins::MultipartItem& item)
  {
    std::string dicom(item.data_, item.data_ + item.size_);
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


  bool ParsedDicomFile::GetStringTag(std::string& result,
                                     const gdcm::Dict& dictionary,
                                     const gdcm::Tag& tag,
                                     bool stripSpaces) const
  {
    if (!GetDataSet().FindDataElement(tag))
    {
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_InexistentTag);
    }

    const gdcm::DataElement& element = GetDataSet().GetDataElement(tag);

    if (!ConvertDicomStringToUtf8(result, dictionary, &GetFile(), element, GetEncoding()))
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


  const char* GetKeyword(const gdcm::Dict& dictionary,
                         const gdcm::Tag& tag)
  {
    const gdcm::DictEntry &entry = dictionary.GetDictEntry(tag);
    const char* keyword = entry.GetKeyword();

    if (strlen(keyword) != 0)
    {
      return keyword;
    }

    if (tag == DICOM_TAG_RETRIEVE_URL)
    {
      return "RetrieveURL";
    }

    return NULL;
  }



  static bool IsBulkData(const std::string& vr)
  {
    /**
     * Full list of VR (Value Representations) that are admissible for
     * being retrieved as bulk data. We commented out some of them, as
     * they correspond to strings and not to binary data.
     **/
    return (//vr == "FL" ||
            //vr == "FD" ||
            //vr == "IS" ||
      vr == "LT" ||
      vr == "OB" ||
      vr == "OD" ||
      vr == "OF" ||
      vr == "OW" ||
      //vr == "SL" ||
      //vr == "SS" ||
      //vr == "ST" ||
      //vr == "UL" ||
      vr == "UN" ||
      //vr == "US" ||
      vr == "UT");
  }


  static std::string GetWadoUrl(const std::string& wadoBase,
                                const gdcm::DataSet& dicom)
  {
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
  


  static void DicomToXmlInternal(pugi::xml_node& target,
                                 const gdcm::Dict& dictionary,
                                 const gdcm::File* file,
                                 const gdcm::DataSet& dicom,
                                 const Orthanc::Encoding sourceEncoding,
                                 const std::string& bulkUri)
  {
    for (gdcm::DataSet::ConstIterator it = dicom.Begin();
         it != dicom.End(); ++it)  // "*it" represents a "gdcm::DataElement"
    {
      char path[16];
      sprintf(path, "%04x%04x", it->GetTag().GetGroup(), it->GetTag().GetElement());

      pugi::xml_node node = target.append_child("DicomAttribute");
      node.append_attribute("tag").set_value(FormatTag(it->GetTag()).c_str());

      bool isSequence = false;
      std::string vr;
      if (it->GetTag() == DICOM_TAG_RETRIEVE_URL)
      {
        // The VR of this attribute has changed from UT to UR.
        vr = "UR";
      }
      else
      {
        vr = GetVRName(isSequence, dictionary, *it);
      }

      node.append_attribute("vr").set_value(vr.c_str());

      const char* keyword = GetKeyword(dictionary, it->GetTag());
      if (keyword != NULL)
      {
        node.append_attribute("keyword").set_value(keyword);
      }

      if (isSequence)
      {
        gdcm::SmartPointer<gdcm::SequenceOfItems> seq = it->GetValueAsSQ();
        if (seq.GetPointer() != NULL)
        {
          for (gdcm::SequenceOfItems::SizeType i = 1; i <= seq->GetNumberOfItems(); i++)
          {
            pugi::xml_node item = node.append_child("Item");
            std::string number = boost::lexical_cast<std::string>(i);
            item.append_attribute("number").set_value(number.c_str());

            std::string childUri;
            if (!bulkUri.empty())
            {
              childUri = bulkUri + std::string(path) + "/" + number + "/";
            }

            DicomToXmlInternal(item, dictionary, file, seq->GetItem(i).GetNestedDataSet(), sourceEncoding, childUri);
          }
        }
      }
      else if (IsBulkData(vr))
      {
        // Bulk data
        if (!bulkUri.empty())
        {
          pugi::xml_node value = node.append_child("BulkData");
          std::string uri = bulkUri + std::string(path);
          value.append_attribute("uri").set_value(uri.c_str());
        }
      }
      else
      {
        // Deal with other value representations
        pugi::xml_node value = node.append_child("Value");
        value.append_attribute("number").set_value("1");

        std::string tmp;
        if (ConvertDicomStringToUtf8(tmp, dictionary, file, *it, sourceEncoding)) 
        {
          value.append_child(pugi::node_pcdata).set_value(tmp.c_str());
        }
        else
        {
          value.append_child(pugi::node_pcdata).set_value("");
        }
      }
    }
  }


  static void DicomToXml(pugi::xml_document& target,
                         const gdcm::Dict& dictionary,
                         const gdcm::File* file,
                         const gdcm::DataSet& dicom,
                         const std::string& bulkUriRoot)
  {
    pugi::xml_node root = target.append_child("NativeDicomModel");
    root.append_attribute("xmlns").set_value("http://dicom.nema.org/PS3.19/models/NativeDICOM");
    root.append_attribute("xsi:schemaLocation").set_value("http://dicom.nema.org/PS3.19/models/NativeDICOM");
    root.append_attribute("xmlns:xsi").set_value("http://www.w3.org/2001/XMLSchema-instance");

    Orthanc::Encoding encoding = DetectEncoding(dicom);
    DicomToXmlInternal(root, dictionary, file, dicom, encoding, bulkUriRoot);

    pugi::xml_node decl = target.prepend_child(pugi::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("utf-8");
  }


  static void DicomToJsonInternal(Json::Value& target,
                                  const gdcm::Dict& dictionary,
                                  const gdcm::File* file,
                                  const gdcm::DataSet& dicom,
                                  const std::string& bulkUri,
                                  Orthanc::Encoding sourceEncoding)
  {
    target = Json::objectValue;

    for (gdcm::DataSet::ConstIterator it = dicom.Begin();
         it != dicom.End(); ++it)  // "*it" represents a "gdcm::DataElement"
    {
      char path[16];
      sprintf(path, "%04x%04x", it->GetTag().GetGroup(), it->GetTag().GetElement());

      Json::Value node = Json::objectValue;

      bool isSequence = false;
      std::string vr;
      if (it->GetTag() == DICOM_TAG_RETRIEVE_URL)
      {
        // The VR of this attribute has changed from UT to UR.
        vr = "UR";
      }
      else
      {
        vr = GetVRName(isSequence, dictionary, *it);
      }

      node["vr"] = vr.c_str();

      bool ok = true;
      if (isSequence)
      {
        // Deal with sequences
        node["Value"] = Json::arrayValue;

        gdcm::SmartPointer<gdcm::SequenceOfItems> seq = it->GetValueAsSQ();
        if (seq.GetPointer() != NULL)
        {
          for (gdcm::SequenceOfItems::SizeType i = 1; i <= seq->GetNumberOfItems(); i++)
          {
            Json::Value child;

            std::string childUri;
            if (!bulkUri.empty())
            {
              std::string number = boost::lexical_cast<std::string>(i);
              childUri = bulkUri + std::string(path) + "/" + number + "/";
            }

            DicomToJsonInternal(child, dictionary, file, seq->GetItem(i).GetNestedDataSet(), childUri, sourceEncoding);
            node["Value"].append(child);
          }
        }

        ok = true;
      }
      else if (IsBulkData(vr))
      {
        // Bulk data
        if (!bulkUri.empty())
        {
          node["BulkDataURI"] = bulkUri + std::string(path);
          ok = true;
        }
      }
      else
      {
        // Deal with other value representations
        node["Value"] = Json::arrayValue;

        std::string value;
        if (ConvertDicomStringToUtf8(value, dictionary, file, *it, sourceEncoding)) 
        {
          node["Value"].append(value.c_str());
        }
        else
        {
          node["Value"].append("");
        }

        ok = true;
      }

      if (ok)
      {
        target[FormatTag(it->GetTag())] = node;
      }
    }
  }


  static void DicomToJson(Json::Value& target,
                          const gdcm::Dict& dictionary,
                          const gdcm::File* file,
                          const gdcm::DataSet& dicom,
                          const std::string& bulkUriRoot)
  {
    Orthanc::Encoding encoding = DetectEncoding(dicom);
    DicomToJsonInternal(target, dictionary, file, dicom, bulkUriRoot, encoding);
  }


  void GenerateSingleDicomAnswer(std::string& result,
                                 const std::string& wadoBase,
                                 const gdcm::Dict& dictionary,
                                 const gdcm::File* file,  // Can be NULL
                                 const gdcm::DataSet& dicom,
                                 bool isXml,
                                 bool isBulkAccessible)
  {
    std::string bulkUriRoot;
    if (isBulkAccessible)
    {
      bulkUriRoot = GetWadoUrl(wadoBase, dicom) + "bulk/";
    }

    if (isXml)
    {
      pugi::xml_document doc;
      DicomToXml(doc, dictionary, file, dicom, bulkUriRoot);
    
      ChunkedBufferWriter writer;
      doc.save(writer, "  ", pugi::format_default, pugi::encoding_utf8);

      writer.Flatten(result);
    }
    else
    {
      Json::Value v;
      DicomToJson(v, dictionary, file, dicom, bulkUriRoot);

      Json::FastWriter writer;
      result = writer.write(v); 
    }
  }


  void AnswerDicom(OrthancPluginContext* context,
                   OrthancPluginRestOutput* output,
                   const std::string& wadoBase,
                   const gdcm::Dict& dictionary,
                   const gdcm::DataSet& dicom,
                   bool isXml,
                   bool isBulkAccessible)
  {
    std::string answer;
    GenerateSingleDicomAnswer(answer, wadoBase, dictionary, NULL, dicom, isXml, isBulkAccessible);
    OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), 
                              isXml ? "application/dicom+xml" : "application/json");
  }


  std::string ParsedDicomFile::GetWadoUrl(const OrthancPluginHttpRequest* request) const
  {
    const std::string base = OrthancPlugins::Configuration::GetBaseUrl(request);
    return OrthancPlugins::GetWadoUrl(base, GetDataSet());
  }


  static inline uint16_t GetCharValue(char c)
  {
    if (c >= '0' && c <= '9')
      return c - '0';
    else if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    else
      return 0;
  }

  static inline uint16_t GetTagValue(const char* c)
  {
    return ((GetCharValue(c[0]) << 12) + 
            (GetCharValue(c[1]) << 8) + 
            (GetCharValue(c[2]) << 4) + 
            GetCharValue(c[3]));
  }


  gdcm::Tag ParseTag(const gdcm::Dict& dictionary,
                     const std::string& key)
  {
    if (key.find('.') != std::string::npos)
    {
      OrthancPlugins::Configuration::LogError("This DICOMweb plugin does not support hierarchical queries: " + key);
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NotImplemented);
    }

    if (key.size() == 8 &&  // This is the DICOMweb convention
        isxdigit(key[0]) &&
        isxdigit(key[1]) &&
        isxdigit(key[2]) &&
        isxdigit(key[3]) &&
        isxdigit(key[4]) &&
        isxdigit(key[5]) &&
        isxdigit(key[6]) &&
        isxdigit(key[7]))        
    {
      return gdcm::Tag(GetTagValue(key.c_str()),
                       GetTagValue(key.c_str() + 4));
    }
    else if (key.size() == 9 &&  // This is the Orthanc convention
             isxdigit(key[0]) &&
             isxdigit(key[1]) &&
             isxdigit(key[2]) &&
             isxdigit(key[3]) &&
             key[4] == ',' &&
             isxdigit(key[5]) &&
             isxdigit(key[6]) &&
             isxdigit(key[7]) &&
             isxdigit(key[8]))        
    {
      return gdcm::Tag(GetTagValue(key.c_str()),
                       GetTagValue(key.c_str() + 5));
    }
    else
    {
      gdcm::Tag tag;
      dictionary.GetDictEntryByKeyword(key.c_str(), tag);

      if (tag.IsIllegal() || tag.IsPrivate())
      {
        if (key.find('.') != std::string::npos)
        {
          OrthancPlugins::Configuration::LogError("This QIDO-RS implementation does not support search over sequences: " + key);
          throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NotImplemented);
        }
        else
        {
          OrthancPlugins::Configuration::LogError("Illegal tag name in QIDO-RS: " + key);
          throw OrthancPlugins::PluginException(OrthancPluginErrorCode_UnknownDicomTag);
        }
      }

      return tag;
    }
  }
}
