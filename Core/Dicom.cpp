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
#include <boost/lexical_cast.hpp>
#include <json/writer.h>

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
      throw std::runtime_error("GDCM cannot read this DICOM instance");
    }
  }


  ParsedDicomFile::ParsedDicomFile(const OrthancPlugins::MultipartItem& item)
  {
    std::string dicom(item.data_, item.data_ + item.size_);
    Setup(dicom);
  }


  bool ParsedDicomFile::GetTag(std::string& result,
                               const gdcm::Tag& tag,
                               bool stripSpaces) const
  {
    const gdcm::DataSet& dataset = GetDataSet();

    if (dataset.FindDataElement(tag))
    {
      const gdcm::ByteValue* value = dataset.GetDataElement(tag).GetByteValue();
      if (value)
      {
        result = std::string(value->GetPointer(), value->GetLength());

        if (stripSpaces)
        {
          result = OrthancPlugins::StripSpaces(result);
        }

        return true;
      }
    }

    return false;
  }


  std::string ParsedDicomFile::GetTagWithDefault(const gdcm::Tag& tag,
                                                 const std::string& defaultValue,
                                                 bool stripSpaces) const
  {
    std::string result;
    if (!GetTag(result, tag, false))
    {
      result = defaultValue;
    }

    if (stripSpaces)
    {
      result = OrthancPlugins::StripSpaces(result);
    }

    return result;
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

    throw std::runtime_error("Unknown keyword for tag: " + FormatTag(tag));
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
    }

    isSequence = (vr == gdcm::VR::SQ);

    return gdcm::VR::GetVRString(vr);
  }


  static void DicomToXmlInternal(pugi::xml_node& target,
                                 const gdcm::Dict& dictionary,
                                 const gdcm::DataSet& dicom)
  {
    for (gdcm::DataSet::ConstIterator it = dicom.Begin();
         it != dicom.End(); ++it)  // "*it" represents a "gdcm::DataElement"
    {
      pugi::xml_node node = target.append_child("DicomAttribute");
      node.append_attribute("tag").set_value(FormatTag(it->GetTag()).c_str());
      node.append_attribute("keyword").set_value(GetKeyword(dictionary, it->GetTag()));

      bool isSequence = false;
      if (it->GetTag() == DICOM_TAG_RETRIEVE_URL)
      {
        // The VR of this attribute has changed from UT to UR.
        node.append_attribute("vr").set_value("UR");
      }
      else
      {
        node.append_attribute("vr").set_value(GetVRName(isSequence, dictionary, *it));
      }

      if (isSequence)
      {
        gdcm::SmartPointer<gdcm::SequenceOfItems> seq = it->GetValueAsSQ();

        for (gdcm::SequenceOfItems::SizeType i = 1; i <= seq->GetNumberOfItems(); i++)
        {
          pugi::xml_node item = node.append_child("Item");
          std::string number = boost::lexical_cast<std::string>(i);
          item.append_attribute("number").set_value(number.c_str());
          DicomToXmlInternal(item, dictionary, seq->GetItem(i).GetNestedDataSet());
        }
      }
      else
      {
        // Deal with other value representations
        pugi::xml_node value = node.append_child("Value");
        value.append_attribute("number").set_value("1");

        const gdcm::ByteValue* data = it->GetByteValue();
        if (data)
        {
          std::string tmp(data->GetPointer(), data->GetLength());
          tmp = OrthancPlugins::StripSpaces(tmp);
          value.append_child(pugi::node_pcdata).set_value(tmp.c_str());
        }
      }
    }
  }


  void DicomToXml(pugi::xml_document& target,
                  const gdcm::Dict& dictionary,
                  const gdcm::DataSet& dicom)
  {
    pugi::xml_node root = target.append_child("NativeDicomModel");
    root.append_attribute("xmlns").set_value("http://dicom.nema.org/PS3.19/models/NativeDICOM");
    root.append_attribute("xsi:schemaLocation").set_value("http://dicom.nema.org/PS3.19/models/NativeDICOM");
    root.append_attribute("xmlns:xsi").set_value("http://www.w3.org/2001/XMLSchema-instance");

    DicomToXmlInternal(root, dictionary, dicom);

    pugi::xml_node decl = target.prepend_child(pugi::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("utf-8");
  }




  void DicomToJson(Json::Value& target,
                   const gdcm::Dict& dictionary,
                   const gdcm::DataSet& dicom)
  {
    target = Json::objectValue;

    for (gdcm::DataSet::ConstIterator it = dicom.Begin();
         it != dicom.End(); ++it)  // "*it" represents a "gdcm::DataElement"
    {
      Json::Value node = Json::objectValue;

      bool isSequence = false;
      if (it->GetTag() == DICOM_TAG_RETRIEVE_URL)
      {
        // The VR of this attribute has changed from UT to UR.
        node["vr"] = "UR";
      }
      else
      {
        node["vr"] = GetVRName(isSequence, dictionary, *it);
      }

      if (isSequence)
      {
        // Deal with sequences
        node["Value"] = Json::arrayValue;

        gdcm::SmartPointer<gdcm::SequenceOfItems> seq = it->GetValueAsSQ();

        for (gdcm::SequenceOfItems::SizeType i = 1; i <= seq->GetNumberOfItems(); i++)
        {
          Json::Value child;
          DicomToJson(child, dictionary, seq->GetItem(i).GetNestedDataSet());
          node["Value"].append(child);
        }
      }
      else
      {
        // Deal with other value representations
        node["Value"] = Json::arrayValue;

        const gdcm::ByteValue* data = it->GetByteValue();
        if (data)
        {
          std::string tmp(data->GetPointer(), data->GetLength());
          node["Value"].append(OrthancPlugins::StripSpaces(tmp));
        }
      }

      target[FormatTag(it->GetTag())] = node;
    }
  }


  void GenerateSingleDicomAnswer(std::string& result,
                                 const gdcm::Dict& dictionary,
                                 const gdcm::DataSet& dicom,
                                 bool isXml)
  {
    if (isXml)
    {
      pugi::xml_document doc;
      DicomToXml(doc, dictionary, dicom);
    
      ChunkedBufferWriter writer;
      doc.save(writer, "  ", pugi::format_default, pugi::encoding_utf8);

      writer.Flatten(result);
    }
    else
    {
      Json::Value v;
      DicomToJson(v, dictionary, dicom);

      Json::FastWriter writer;
      result = writer.write(v); 
    }
  }


  void AnswerDicom(OrthancPluginContext* context,
                   OrthancPluginRestOutput* output,
                   const gdcm::Dict& dictionary,
                   const gdcm::DataSet& dicom,
                   bool isXml)
  {
    std::string answer;
    GenerateSingleDicomAnswer(answer, dictionary, dicom, isXml);
    OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), 
                              isXml ? "application/dicom+xml" : "application/json");
  }
}
