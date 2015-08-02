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


#include "Dicom.h"

#include "ChunkedBuffer.h"

#include <gdcmDictEntry.h>
#include <gdcmStringFilter.h>
#include <boost/lexical_cast.hpp>
#include <json/writer.h>

#include "../Orthanc/Core/OrthancException.h"
#include "../Orthanc/Core/Toolbox.h"

namespace OrthancPlugins
{
  namespace
  {
    class ChunkedBufferWriter : public pugi::xml_writer
    {
    private:
      ChunkedBuffer buffer_;

    public:
      virtual void write(const void *data, size_t size)
      {
        if (size > 0)
        {
          buffer_.AddChunk(reinterpret_cast<const char*>(data), size);
        }
      }

      void Flatten(std::string& s)
      {
        buffer_.Flatten(s);
      }
    };
  }



  void ParsedDicomFile::Setup(const std::string& dicom)
  {
    // Prepare a memory stream over the DICOM instance
    std::stringstream stream(dicom);

    // Parse the DICOM instance using GDCM
    reader_.SetStream(stream);
    if (!reader_.Read())
    {
      throw Orthanc::OrthancException("GDCM cannot read this DICOM instance of length " + 
                                      boost::lexical_cast<std::string>(dicom.size()));
    }
  }


  ParsedDicomFile::ParsedDicomFile(const OrthancPlugins::MultipartItem& item)
  {
    std::string dicom(item.data_, item.data_ + item.size_);
    Setup(dicom);
  }


  static bool GetTag(std::string& result,
                     const gdcm::DataSet& dataset,
                     const gdcm::Tag& tag,
                     bool stripSpaces)
  {
    if (dataset.FindDataElement(tag))
    {
      const gdcm::ByteValue* value = dataset.GetDataElement(tag).GetByteValue();
      if (value)
      {
        result = std::string(value->GetPointer(), value->GetLength());

        if (stripSpaces)
        {
          result = Orthanc::Toolbox::StripSpaces(result);
        }

        return true;
      }
    }

    return false;
  }


  static std::string GetTagWithDefault(const gdcm::DataSet& dataset,
                                       const gdcm::Tag& tag,
                                       const std::string& defaultValue,
                                       bool stripSpaces)
  {
    std::string result;
    if (!GetTag(result, dataset, tag, false))
    {
      result = defaultValue;
    }

    if (stripSpaces)
    {
      result = Orthanc::Toolbox::StripSpaces(result);
    }

    return result;
  }


  bool ParsedDicomFile::GetTag(std::string& result,
                               const gdcm::Tag& tag,
                               bool stripSpaces) const
  {
    return OrthancPlugins::GetTag(result, GetDataSet(), tag, stripSpaces);
  }


  std::string ParsedDicomFile::GetTagWithDefault(const gdcm::Tag& tag,
                                                 const std::string& defaultValue,
                                                 bool stripSpaces) const
  {
    return OrthancPlugins::GetTagWithDefault(GetDataSet(), tag, defaultValue, stripSpaces);
  }


  static std::string FormatTag(const gdcm::Tag& tag)
  {
    char tmp[16];
    sprintf(tmp, "%04X%04X", tag.GetGroup(), tag.GetElement());
    return std::string(tmp);
  }


  static const char* GetKeyword(const gdcm::Dict& dictionary,
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

    //throw Orthanc::OrthancException("Unknown keyword for tag: " + FormatTag(tag));
    return NULL;
  }



  static const char* GetVRName(bool& isSequence,
                               const gdcm::Dict& dictionary,
                               const gdcm::DataElement& element)
  {
    gdcm::VR vr = element.GetVR();
    if (vr == gdcm::VR::INVALID)
    {
      const gdcm::DictEntry &entry = dictionary.GetDictEntry(element.GetTag());
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


  static std::string GetBulkUriRoot(const std::string& wadoBase,
                                    const gdcm::DataSet& dicom)
  {
    std::string study, series, instance;

    if (!GetTag(study, dicom, DICOM_TAG_STUDY_INSTANCE_UID, true) ||
        !GetTag(series, dicom, DICOM_TAG_SERIES_INSTANCE_UID, true) ||
        !GetTag(instance, dicom, DICOM_TAG_SOP_INSTANCE_UID, true))
    {
      return "";
    }
    else
    {
      return wadoBase + "studies/" + study + "/series/" + series + "/instances/" + instance + "/bulk/";
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
      // Assume Latin-1 encoding (TODO add a parameter as in Orthanc)
      return Orthanc::Encoding_Latin1;
    }

    std::string tmp(data->GetPointer(), data->GetLength());
    tmp = Orthanc::Toolbox::StripSpaces(tmp);

    Orthanc::Encoding encoding;
    if (Orthanc::GetDicomEncoding(encoding, tmp.c_str()))
    {
      return encoding;
    }
    else
    {
      // Assume Latin-1 encoding (TODO add a parameter as in Orthanc)
      return Orthanc::Encoding_Latin1;
    }
  }


  static bool ConvertDicomStringToUf8(std::string& result,
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

    result = Orthanc::Toolbox::StripSpaces(result);
    return true;
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

      const char* keyword = GetKeyword(dictionary, it->GetTag());
      if (keyword != NULL)
      {
        node.append_attribute("keyword").set_value(keyword);
      }

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
        if (ConvertDicomStringToUf8(tmp, dictionary, file, *it, sourceEncoding)) 
        {
          value.append_child(pugi::node_pcdata).set_value(tmp.c_str());
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
      }
      else if (IsBulkData(vr))
      {
        // Bulk data
        if (!bulkUri.empty())
        {
          node["BulkDataURI"] = bulkUri + std::string(path);
        }
      }
      else
      {
        // Deal with other value representations
        node["Value"] = Json::arrayValue;

        std::string value;
        if (ConvertDicomStringToUf8(value, dictionary, file, *it, sourceEncoding)) 
        {
          node["Value"].append(value.c_str());
        }
      }

      target[FormatTag(it->GetTag())] = node;
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
      bulkUriRoot = GetBulkUriRoot(wadoBase, dicom);
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
}
