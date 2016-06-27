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


#include "DicomResults.h"

#include "Dicom.h"
#include "../Orthanc/Core/Toolbox.h"
#include "../Orthanc/Plugins/Samples/Common/OrthancPluginCppWrapper.h"

#include <boost/lexical_cast.hpp>
#include <boost/noncopyable.hpp>

namespace OrthancPlugins
{
  DicomResults::DicomResults(OrthancPluginContext* context,
                             OrthancPluginRestOutput* output,
                             const std::string& wadoBase,
                             const gdcm::Dict& dictionary,
                             bool isXml,
                             bool isBulkAccessible) :
    context_(context),
    output_(output),
    wadoBase_(wadoBase),
    dictionary_(dictionary),
    isFirst_(true),
    isXml_(isXml),
    isBulkAccessible_(isBulkAccessible)
  {
    if (isXml_ &&
        OrthancPluginStartMultipartAnswer(context_, output_, "related", "application/dicom+xml") != 0)
    {
      OrthancPlugins::Configuration::LogError("Unable to create a multipart stream of DICOM+XML answers");
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
    }

    jsonWriter_.AddChunk("[\n");
  }


  void DicomResults::AddInternal(const std::string& item)
  {
    if (isXml_)
    {
      if (OrthancPluginSendMultipartItem(context_, output_, item.c_str(), item.size()) != 0)
      {
        OrthancPlugins::Configuration::LogError("Unable to create a multipart stream of DICOM+XML answers");
        throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
      }
    }
    else
    {
      if (!isFirst_)
      {
        jsonWriter_.AddChunk(",\n");
      }

      jsonWriter_.AddChunk(item);
    }

    isFirst_ = false;
  }


  void DicomResults::AddInternal(const gdcm::File* file,
                                 const gdcm::DataSet& dicom)
  {
    std::string item;

    if (isXml_)
    {
      GenerateSingleDicomAnswer(item, wadoBase_, dictionary_, file, dicom, true, isBulkAccessible_);
    }
    else
    {
      GenerateSingleDicomAnswer(item, wadoBase_, dictionary_, file, dicom, false, isBulkAccessible_);
    }

    AddInternal(item);

    isFirst_ = false;
  }



  namespace
  {
    class ITagVisitor : public boost::noncopyable
    {
    public:
      virtual ~ITagVisitor()
      {
      }

      virtual void Visit(const gdcm::Tag& tag,
                         bool isSequence,
                         const std::string& vr,
                         const std::string& type,
                         const Json::Value& value) = 0;

      static void Apply(ITagVisitor& visitor,
                        const Json::Value& source,
                        const gdcm::Dict& dictionary)
      {
        if (source.type() != Json::objectValue)
        {
          throw OrthancPlugins::PluginException(OrthancPluginErrorCode_InternalError);
        }

        Json::Value::Members members = source.getMemberNames();
        for (size_t i = 0; i < members.size(); i++)
        {
          if (members[i].size() != 9 ||
              members[i][4] != ',' ||
              source[members[i]].type() != Json::objectValue ||
              !source[members[i]].isMember("Value") ||
              !source[members[i]].isMember("Type") ||
              source[members[i]]["Type"].type() != Json::stringValue)
          {
            throw OrthancPlugins::PluginException(OrthancPluginErrorCode_InternalError);
          }        

          const Json::Value& value = source[members[i]]["Value"];
          const std::string type = source[members[i]]["Type"].asString();

          gdcm::Tag tag(OrthancPlugins::ParseTag(dictionary, members[i]));

          bool isSequence = false;
          std::string vr = GetVRName(isSequence, dictionary, tag);

          if (tag == DICOM_TAG_RETRIEVE_URL)
          {
            // The VR of this attribute has changed from UT to UR.
            vr = "UR";
          }
          else
          {
            vr = GetVRName(isSequence, dictionary, tag);
          }

          visitor.Visit(tag, isSequence, vr, type, value);
        }
      }
    };


    class TagVisitorBase : public ITagVisitor
    {
    protected:
      const Json::Value&  source_;
      const gdcm::Dict&   dictionary_;
      const std::string&  bulkUri_;

    public:
      TagVisitorBase(const Json::Value&  source,
                     const gdcm::Dict&   dictionary,
                     const std::string&  bulkUri) :
        source_(source),
        dictionary_(dictionary),
        bulkUri_(bulkUri)
      {
      }
    };


    class JsonVisitor : public TagVisitorBase
    {
    private:
      Json::Value&   target_;

    public:
      JsonVisitor(Json::Value&        target,
                  const Json::Value&  source,
                  const gdcm::Dict&   dictionary,
                  const std::string&  bulkUri) :
        TagVisitorBase(source, dictionary, bulkUri),
        target_(target)
      {
        target_ = Json::objectValue;
      }

      virtual void Visit(const gdcm::Tag& tag,
                         bool isSequence,
                         const std::string& vr,
                         const std::string& type,
                         const Json::Value& value)
      {
        const std::string formattedTag = OrthancPlugins::FormatTag(tag);

        Json::Value node = Json::objectValue;
        node["vr"] = vr;

        bool ok = false;
        if (isSequence)
        {
          // Deal with sequences
          if (type != "Sequence" ||
              value.type() != Json::arrayValue)
          {
            throw OrthancPlugins::PluginException(OrthancPluginErrorCode_InternalError);
          }

          node["Value"] = Json::arrayValue;

          for (Json::Value::ArrayIndex i = 0; i < value.size(); i++)
          {
            if (value[i].type() != Json::objectValue)
            {
              throw OrthancPlugins::PluginException(OrthancPluginErrorCode_InternalError);
            }

            Json::Value child;

            std::string childUri;
            if (!bulkUri_.empty())
            {
              std::string number = boost::lexical_cast<std::string>(i);
              childUri = bulkUri_ + formattedTag + "/" + number + "/";
            }

            JsonVisitor visitor(child, value[i], dictionary_, childUri);
            JsonVisitor::Apply(visitor, value[i], dictionary_);

            node["Value"].append(child);
          }

          ok = true;
        }
        else if (type == "String" &&
                 value.type() == Json::stringValue)
        {
          // Deal with string representations
          node["Value"] = Json::arrayValue;
          node["Value"].append(value.asString());
          ok = true;
        }
        else
        {
          // Bulk data
          if (!bulkUri_.empty())
          {
            node["BulkDataURI"] = bulkUri_ + formattedTag;
            ok = true;
          }
        }

        if (ok)
        {
          target_[formattedTag] = node;
        }
      }
    };


    class XmlVisitor : public TagVisitorBase
    {
    private:
      pugi::xml_node&  target_;

    public:
      XmlVisitor(pugi::xml_node&     target,
                 const Json::Value&  source,
                 const gdcm::Dict&   dictionary,
                 const std::string&  bulkUri) :
        TagVisitorBase(source, dictionary, bulkUri),
        target_(target)
      {
      }

      virtual void Visit(const gdcm::Tag& tag,
                         bool isSequence,
                         const std::string& vr,
                         const std::string& type,
                         const Json::Value& value)
      {
        const std::string formattedTag = OrthancPlugins::FormatTag(tag);

        pugi::xml_node node = target_.append_child("DicomAttribute");
        node.append_attribute("tag").set_value(formattedTag.c_str());
        node.append_attribute("vr").set_value(vr.c_str());

        const char* keyword = GetKeyword(dictionary_, tag);
        if (keyword != NULL)
        {
          node.append_attribute("keyword").set_value(keyword);
        }

        if (isSequence)
        {
          // Deal with sequences
          if (type != "Sequence" ||
              value.type() != Json::arrayValue)
          {
            throw OrthancPlugins::PluginException(OrthancPluginErrorCode_InternalError);
          }

          for (Json::Value::ArrayIndex i = 0; i < value.size(); i++)
          {
            if (value[i].type() != Json::objectValue)
            {
              throw OrthancPlugins::PluginException(OrthancPluginErrorCode_InternalError);
            }

            pugi::xml_node child = node.append_child("Item");
            std::string number = boost::lexical_cast<std::string>(i + 1);
            child.append_attribute("number").set_value(number.c_str());

            std::string childUri;
            if (!bulkUri_.empty())
            {
              childUri = bulkUri_ + formattedTag + "/" + number + "/";
            }

            XmlVisitor visitor(child, value[i], dictionary_, childUri);
            XmlVisitor::Apply(visitor, value[i], dictionary_);
          }
        }
        else if (type == "String" &&
                 value.type() == Json::stringValue)
        {
          // Deal with string representations
          pugi::xml_node item = node.append_child("Value");
          item.append_attribute("number").set_value("1");
          item.append_child(pugi::node_pcdata).set_value(value.asCString());
        }
        else
        {
          // Bulk data
          if (!bulkUri_.empty())
          {
            pugi::xml_node value = node.append_child("BulkData");
            std::string uri = bulkUri_ + formattedTag;
            value.append_attribute("uri").set_value(uri.c_str());
          }
        }
      }
    };
  }


  static void OrthancToDicomWebXml(pugi::xml_document& target,
                                   const Json::Value& source,
                                   const gdcm::Dict& dictionary,
                                   const std::string& bulkUriRoot)
  {
    pugi::xml_node root = target.append_child("NativeDicomModel");
    root.append_attribute("xmlns").set_value("http://dicom.nema.org/PS3.19/models/NativeDICOM");
    root.append_attribute("xsi:schemaLocation").set_value("http://dicom.nema.org/PS3.19/models/NativeDICOM");
    root.append_attribute("xmlns:xsi").set_value("http://www.w3.org/2001/XMLSchema-instance");

    XmlVisitor visitor(root, source, dictionary, bulkUriRoot);
    ITagVisitor::Apply(visitor, source, dictionary);

    pugi::xml_node decl = target.prepend_child(pugi::node_declaration);
    decl.append_attribute("version").set_value("1.0");
    decl.append_attribute("encoding").set_value("utf-8");
  }


  void DicomResults::AddFromOrthanc(const Json::Value& dicom,
                                    const std::string& wadoUrl)
  { 
    std::string bulkUriRoot;
    if (isBulkAccessible_)
    {
      bulkUriRoot = wadoUrl + "bulk/";
    }

    if (isXml_)
    {
      pugi::xml_document doc;
      OrthancToDicomWebXml(doc, dicom, dictionary_, bulkUriRoot);
    
      ChunkedBufferWriter writer;
      doc.save(writer, "  ", pugi::format_default, pugi::encoding_utf8);

      std::string item;
      writer.Flatten(item);

      AddInternal(item);
    }
    else
    {
      Json::Value v;
      JsonVisitor visitor(v, dicom, dictionary_, bulkUriRoot);
      ITagVisitor::Apply(visitor, dicom, dictionary_);

      Json::FastWriter writer;
      AddInternal(writer.write(v));
    }
  }


  void DicomResults::Answer()
  {
    if (isXml_)
    {
      // Nothing to do in this case
    }
    else
    {
      jsonWriter_.AddChunk("]\n");

      std::string answer;
      jsonWriter_.Flatten(answer);
      OrthancPluginAnswerBuffer(context_, output_, answer.c_str(), answer.size(), "application/json");
    }
  }
}
