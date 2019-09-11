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


#include "QidoRs.h"

#include "Configuration.h"
#include "DicomWebFormatter.h"

#include <Core/DicomFormat/DicomTag.h>
#include <Core/Toolbox.h>
#include <Plugins/Samples/Common/OrthancPluginCppWrapper.h>

#include <list>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string/replace.hpp>


namespace
{
  static std::string GetOrthancTag(const Json::Value& source,
                                   const Orthanc::DicomTag& tag,
                                   const std::string& defaultValue)
  {
    const std::string s = tag.Format();

    if (source.isMember(s))
    {
      switch (source[s].type())
      {
        case Json::stringValue:
          return source[s].asString();

          // The conversions below should *not* be necessary

        case Json::intValue:
          return boost::lexical_cast<std::string>(source[s].asInt());

        case Json::uintValue:
          return boost::lexical_cast<std::string>(source[s].asUInt());

        case Json::realValue:
          return boost::lexical_cast<std::string>(source[s].asFloat());

        default:
          return defaultValue;
      }
    }
    else
    {
      return defaultValue;
    }
  }


  class ModuleMatcher
  {
  public:
    typedef std::map<Orthanc::DicomTag, std::string>  Filters;

  private:
    bool                          fuzzy_;
    unsigned int                  offset_;
    unsigned int                  limit_;
    std::list<Orthanc::DicomTag>  includeFields_;
    bool                          includeAllFields_;
    Filters                       filters_;


    static void AddResultAttributesForLevel(std::list<Orthanc::DicomTag>& result,
                                            Orthanc::ResourceType level)
    {
      switch (level)
      {
        case Orthanc::ResourceType_Study:
          // http://medical.nema.org/medical/dicom/current/output/html/part18.html#table_6.7.1-2
          //result.push_back(Orthanc::DicomTag(0x0008, 0x0005));  // Specific Character Set  => SPECIAL CASE
          result.push_back(Orthanc::DicomTag(0x0008, 0x0020));  // Study Date
          result.push_back(Orthanc::DicomTag(0x0008, 0x0030));  // Study Time
          result.push_back(Orthanc::DicomTag(0x0008, 0x0050));  // Accession Number
          result.push_back(Orthanc::DicomTag(0x0008, 0x0056));  // Instance Availability
          //result.push_back(Orthanc::DicomTag(0x0008, 0x0061));  // Modalities in Study  => SPECIAL CASE
          result.push_back(Orthanc::DicomTag(0x0008, 0x0090));  // Referring Physician's Name
          result.push_back(Orthanc::DicomTag(0x0008, 0x0201));  // Timezone Offset From UTC
          //result.push_back(Orthanc::DicomTag(0x0008, 0x1190));  // Retrieve URL  => SPECIAL CASE
          result.push_back(Orthanc::DicomTag(0x0010, 0x0010));  // Patient's Name
          result.push_back(Orthanc::DicomTag(0x0010, 0x0020));  // Patient ID
          result.push_back(Orthanc::DicomTag(0x0010, 0x0030));  // Patient's Birth Date
          result.push_back(Orthanc::DicomTag(0x0010, 0x0040));  // Patient's Sex
          result.push_back(Orthanc::DicomTag(0x0020, 0x000D));  // Study Instance UID
          result.push_back(Orthanc::DicomTag(0x0020, 0x0010));  // Study ID
          //result.push_back(Orthanc::DicomTag(0x0020, 0x1206));  // Number of Study Related Series  => SPECIAL CASE
          //result.push_back(Orthanc::DicomTag(0x0020, 0x1208));  // Number of Study Related Instances  => SPECIAL CASE
          break;

        case Orthanc::ResourceType_Series:
          // http://medical.nema.org/medical/dicom/current/output/html/part18.html#table_6.7.1-2a
          //result.push_back(Orthanc::DicomTag(0x0008, 0x0005));  // Specific Character Set  => SPECIAL CASE
          result.push_back(Orthanc::DicomTag(0x0008, 0x0060));  // Modality
          result.push_back(Orthanc::DicomTag(0x0008, 0x0201));  // Timezone Offset From UTC
          result.push_back(Orthanc::DicomTag(0x0008, 0x103E));  // Series Description
          //result.push_back(Orthanc::DicomTag(0x0008, 0x1190));  // Retrieve URL  => SPECIAL CASE
          result.push_back(Orthanc::DicomTag(0x0020, 0x000E));  // Series Instance UID
          result.push_back(Orthanc::DicomTag(0x0020, 0x0011));  // Series Number
          //result.push_back(Orthanc::DicomTag(0x0020, 0x1209));  // Number of Series Related Instances  => SPECIAL CASE
          result.push_back(Orthanc::DicomTag(0x0040, 0x0244));  // Performed Procedure Step Start Date
          result.push_back(Orthanc::DicomTag(0x0040, 0x0245));  // Performed Procedure Step Start Time
          result.push_back(Orthanc::DicomTag(0x0040, 0x0275));  // Request Attribute Sequence
          break;

        case Orthanc::ResourceType_Instance:
          // http://medical.nema.org/medical/dicom/current/output/html/part18.html#table_6.7.1-2b
          //result.push_back(Orthanc::DicomTag(0x0008, 0x0005));  // Specific Character Set  => SPECIAL CASE
          result.push_back(Orthanc::DicomTag(0x0008, 0x0016));  // SOP Class UID
          result.push_back(Orthanc::DicomTag(0x0008, 0x0018));  // SOP Instance UID
          result.push_back(Orthanc::DicomTag(0x0008, 0x0056));  // Instance Availability
          result.push_back(Orthanc::DicomTag(0x0008, 0x0201));  // Timezone Offset From UTC
          result.push_back(Orthanc::DicomTag(0x0008, 0x1190));  // Retrieve URL
          result.push_back(Orthanc::DicomTag(0x0020, 0x0013));  // Instance Number
          result.push_back(Orthanc::DicomTag(0x0028, 0x0010));  // Rows
          result.push_back(Orthanc::DicomTag(0x0028, 0x0011));  // Columns
          result.push_back(Orthanc::DicomTag(0x0028, 0x0100));  // Bits Allocated
          result.push_back(Orthanc::DicomTag(0x0028, 0x0008));  // Number of Frames
          break;

        default:
          throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
      }
    }


  public:
    explicit ModuleMatcher(const OrthancPluginHttpRequest* request) :
      fuzzy_(false),
      offset_(0),
      limit_(0),
      includeAllFields_(false)
    {
      std::string args;
      
      for (uint32_t i = 0; i < request->getCount; i++)
      {
        std::string key(request->getKeys[i]);
        std::string value(request->getValues[i]);
        args += " [" + key + "=" + value + "]";

        if (key == "limit")
        {
          limit_ = boost::lexical_cast<unsigned int>(value);
        }
        else if (key == "offset")
        {
          offset_ = boost::lexical_cast<unsigned int>(value);
        }
        else if (key == "fuzzymatching")
        {
          if (value == "true")
          {
            fuzzy_ = true;
          }
          else if (value == "false")
          {
            fuzzy_ = false;
          }
          else
          {
            throw Orthanc::OrthancException(
              Orthanc::ErrorCode_BadRequest,
              "Not a proper value for fuzzy matching (true or false): " + value);
          }
        }
        else if (key == "includefield")
        {
          if (value == "all")
          {
            includeAllFields_ = true;
          }
          else
          {
            // Split a comma-separated list of tags
            std::vector<std::string> tags;
            Orthanc::Toolbox::TokenizeString(tags, value, ',');
            
            for (size_t i = 0; i < tags.size(); i++)
            {
              Orthanc::DicomTag tag(0, 0);
              if (OrthancPlugins::ParseTag(tag, tags[i]))
              {
                includeFields_.push_back(tag);
              }
            }
          }
        }
        else
        {
          Orthanc::DicomTag tag(0, 0);
          if (OrthancPlugins::ParseTag(tag, key))
          {
            filters_[tag] = value;
          }
        }
      }

      OrthancPlugins::LogInfo("Arguments of QIDO-RS request:" + args);
    }

    unsigned int GetLimit() const
    {
      return limit_;
    }

    unsigned int GetOffset() const
    {
      return offset_;
    }

    void AddFilter(const Orthanc::DicomTag& tag,
                   const std::string& constraint)
    {
      filters_[tag] = constraint;
    }

    void Print(std::ostream& out) const 
    {
      for (Filters::const_iterator it = filters_.begin(); 
           it != filters_.end(); ++it)
      {
        printf("Filter [%04x,%04x] = [%s]\n", it->first.GetGroup(), it->first.GetElement(), it->second.c_str());
      }
    }

    void ConvertToOrthanc(Json::Value& result,
                          Orthanc::ResourceType level) const
    {
      result = Json::objectValue;

      switch (level)
      {
        case Orthanc::ResourceType_Study:
          result["Level"] = "Study";
          break;

        case Orthanc::ResourceType_Series:
          result["Level"] = "Series";
          break;

        case Orthanc::ResourceType_Instance:
          result["Level"] = "Instance";
          break;

        default:
          throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
      }

      result["Expand"] = false;
      result["CaseSensitive"] = OrthancPlugins::Configuration::GetBooleanValue("QidoCaseSensitive", true);
      result["Query"] = Json::objectValue;
      result["Limit"] = limit_;
      result["Since"] = offset_;

      if (offset_ != 0 &&
          !OrthancPlugins::CheckMinimalOrthancVersion(1, 3, 0))
      {
        OrthancPlugins::LogError(
          "QIDO-RS request with \"offset\" argument: "
          "Only available if the Orthanc core version is >= 1.3.0");
      }
      
      for (Filters::const_iterator it = filters_.begin(); 
           it != filters_.end(); ++it)
      {
        result["Query"][it->first.Format()] = it->second;
      }
    }


    void ComputeDerivedTags(Filters& target,
                            Orthanc::ResourceType level,
                            const std::string& resource) const
    {
      target.clear();

      switch (level)
      {
        case Orthanc::ResourceType_Study:
        {
          Json::Value series, instances;
          if (OrthancPlugins::RestApiGet(series, "/studies/" + resource + "/series?expand", false) &&
              OrthancPlugins::RestApiGet(instances, "/studies/" + resource + "/instances", false))
          {
            target[Orthanc::DICOM_TAG_NUMBER_OF_STUDY_RELATED_SERIES] = 
              boost::lexical_cast<std::string>(series.size());
            target[Orthanc::DICOM_TAG_NUMBER_OF_STUDY_RELATED_INSTANCES] = 
              boost::lexical_cast<std::string>(instances.size());

            // Collect the Modality of all the child series
            std::set<std::string> modalities;
            for (Json::Value::ArrayIndex i = 0; i < series.size(); i++)
            {
              if (series[i].isMember("MainDicomTags") &&
                  series[i]["MainDicomTags"].isMember("Modality"))
              {
                modalities.insert(series[i]["MainDicomTags"]["Modality"].asString());
              }
            }

            std::string s;
            for (std::set<std::string>::const_iterator 
                   it = modalities.begin(); it != modalities.end(); ++it)
            {
              if (!s.empty())
              {
                s += "\\";
              }

              s += *it;
            }

            target[Orthanc::DICOM_TAG_MODALITIES_IN_STUDY] = s;
          }
          else
          {
            target[Orthanc::DICOM_TAG_MODALITIES_IN_STUDY] = "";
            target[Orthanc::DICOM_TAG_NUMBER_OF_STUDY_RELATED_SERIES] = "0";
            target[Orthanc::DICOM_TAG_NUMBER_OF_STUDY_RELATED_INSTANCES] = "0"; 
          }

          break;
        }

        case Orthanc::ResourceType_Series:
        {
          Json::Value instances;
          if (OrthancPlugins::RestApiGet(instances, "/series/" + resource + "/instances", false))
          {
            // Number of Series Related Instances
            target[Orthanc::DICOM_TAG_NUMBER_OF_SERIES_RELATED_INSTANCES] = 
              boost::lexical_cast<std::string>(instances.size());
          }
          else
          {
            target[Orthanc::DICOM_TAG_NUMBER_OF_SERIES_RELATED_INSTANCES] = "0";
          }

          break;
        }

        default:
          break;
      }
    }                              


    void ExtractFields(Json::Value& result,
                       const Json::Value& source,
                       const std::string& wadoBase,
                       Orthanc::ResourceType level) const
    {
      result = Json::objectValue;
      std::list<Orthanc::DicomTag> fields = includeFields_;

      // The list of attributes for this query level
      AddResultAttributesForLevel(fields, level);

      // All other attributes passed as query keys
      for (Filters::const_iterator it = filters_.begin();
           it != filters_.end(); ++it)
      {
        fields.push_back(it->first);
      }

      // For instances and series, add all Study-level attributes if
      // {StudyInstanceUID} is not specified.
      if ((level == Orthanc::ResourceType_Instance  || level == Orthanc::ResourceType_Series) 
          && filters_.find(Orthanc::DICOM_TAG_STUDY_INSTANCE_UID) == filters_.end()
        )
      {
        AddResultAttributesForLevel(fields, Orthanc::ResourceType_Study);
      }

      // For instances, add all Series-level attributes if
      // {SeriesInstanceUID} is not specified.
      if (level == Orthanc::ResourceType_Instance
          && filters_.find(Orthanc::DICOM_TAG_SERIES_INSTANCE_UID) == filters_.end()
        )
      {
        AddResultAttributesForLevel(fields, Orthanc::ResourceType_Series);
      }

      // Copy all the required fields to the target
      for (std::list<Orthanc::DicomTag>::const_iterator
             it = fields.begin(); it != fields.end(); ++it)
      {
        // Complies to the JSON produced internally by Orthanc
        char tag[16];
        sprintf(tag, "%04x,%04x", it->GetGroup(), it->GetElement());
        
        if (source.isMember(tag))
        {
          result[tag] = source[tag];
        }
      }

      // Set the retrieve URL for WADO-RS
      std::string url = (wadoBase + "studies/" + 
                         GetOrthancTag(source, Orthanc::DICOM_TAG_STUDY_INSTANCE_UID, ""));

      if (level == Orthanc::ResourceType_Series || 
          level == Orthanc::ResourceType_Instance)
      {
        url += "/series/" + GetOrthancTag(source, Orthanc::DICOM_TAG_SERIES_INSTANCE_UID, "");
      }

      if (level == Orthanc::ResourceType_Instance)
      {
        url += "/instances/" + GetOrthancTag(source, Orthanc::DICOM_TAG_SOP_INSTANCE_UID, "");
      }
    
      static const Orthanc::DicomTag DICOM_TAG_RETRIEVE_URL(0x0008, 0x1190);
      result[DICOM_TAG_RETRIEVE_URL.Format()] = url;
    }
  };
}



static void ApplyMatcher(OrthancPluginRestOutput* output,
                         const OrthancPluginHttpRequest* request,
                         const ModuleMatcher& matcher,
                         Orthanc::ResourceType level)
{
  Json::Value find;
  matcher.ConvertToOrthanc(find, level);

  std::string body;

  {
    Json::FastWriter writer;
    body = writer.write(find);
  }
  
  Json::Value resources;
  if (!OrthancPlugins::RestApiPost(resources, "/tools/find", body, false) ||
      resources.type() != Json::arrayValue)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }

  typedef std::list< std::pair<std::string, std::string> > ResourcesAndInstances;

  ResourcesAndInstances resourcesAndInstances;
  std::string root = (level == Orthanc::ResourceType_Study ? "/studies/" : "/series/");
    
  for (Json::Value::ArrayIndex i = 0; i < resources.size(); i++)
  {
    const std::string resource = resources[i].asString();

    if (level == Orthanc::ResourceType_Study ||
        level == Orthanc::ResourceType_Series)
    {
      // Find one child instance of this resource
      Json::Value tmp;
      if (OrthancPlugins::RestApiGet(tmp, root + resource + "/instances", false) &&
          tmp.type() == Json::arrayValue &&
          tmp.size() > 0)
      {
        resourcesAndInstances.push_back(std::make_pair(resource, tmp[0]["ID"].asString()));
      }
    }
    else
    {
      resourcesAndInstances.push_back(std::make_pair(resource, resource));
    }
  }
  
  std::string wadoBase = OrthancPlugins::Configuration::GetBaseUrl(request);

  OrthancPlugins::DicomWebFormatter::HttpWriter writer(
    output, OrthancPlugins::Configuration::IsXmlExpected(request));

  // Fix of issue #13
  for (ResourcesAndInstances::const_iterator
         it = resourcesAndInstances.begin(); it != resourcesAndInstances.end(); ++it)
  {
    Json::Value tags;
    if (OrthancPlugins::RestApiGet(tags, "/instances/" + it->second + "/tags?short", false))
    {
      std::string wadoUrl = OrthancPlugins::Configuration::GetWadoUrl(
        wadoBase, 
        GetOrthancTag(tags, Orthanc::DICOM_TAG_STUDY_INSTANCE_UID, ""),
        GetOrthancTag(tags, Orthanc::DICOM_TAG_SERIES_INSTANCE_UID, ""),
        GetOrthancTag(tags, Orthanc::DICOM_TAG_SOP_INSTANCE_UID, ""));

      Json::Value result;
      matcher.ExtractFields(result, tags, wadoBase, level);

      // Inject the derived tags
      ModuleMatcher::Filters derivedTags;
      matcher.ComputeDerivedTags(derivedTags, level, it->first);

      for (ModuleMatcher::Filters::const_iterator
             tag = derivedTags.begin(); tag != derivedTags.end(); ++tag)
      {
        result[tag->first.Format()] = tag->second;
      }

      writer.AddOrthancJson(result);
    }
  }

  writer.Send();
}



void SearchForStudies(OrthancPluginRestOutput* output,
                      const char* url,
                      const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::GetGlobalContext(), output, "GET");
  }
  else
  {
    ModuleMatcher matcher(request);
    ApplyMatcher(output, request, matcher, Orthanc::ResourceType_Study);
  }
}


void SearchForSeries(OrthancPluginRestOutput* output,
                     const char* url,
                     const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::GetGlobalContext(), output, "GET");
  }
  else
  {
    ModuleMatcher matcher(request);

    if (request->groupsCount == 1)
    {
      // The "StudyInstanceUID" is provided by the regular expression
      matcher.AddFilter(Orthanc::DICOM_TAG_STUDY_INSTANCE_UID, request->groups[0]);
    }

    ApplyMatcher(output, request, matcher, Orthanc::ResourceType_Series);
  }
}


void SearchForInstances(OrthancPluginRestOutput* output,
                        const char* url,
                        const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::GetGlobalContext(), output, "GET");
  }
  else
  {
    ModuleMatcher matcher(request);

    if (request->groupsCount == 1 || 
        request->groupsCount == 2)
    {
      // The "StudyInstanceUID" is provided by the regular expression
      matcher.AddFilter(Orthanc::DICOM_TAG_STUDY_INSTANCE_UID, request->groups[0]);
    }

    if (request->groupsCount == 2)
    {
      // The "SeriesInstanceUID" is provided by the regular expression
      matcher.AddFilter(Orthanc::DICOM_TAG_SERIES_INSTANCE_UID, request->groups[1]);
    }

    ApplyMatcher(output, request, matcher, Orthanc::ResourceType_Instance);
  }
}
