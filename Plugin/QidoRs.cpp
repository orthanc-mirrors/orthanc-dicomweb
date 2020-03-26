/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2020 Osimis S.A., Belgium
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

#include <Core/DicomFormat/DicomMap.h>
#include <Core/DicomFormat/DicomTag.h>
#include <Core/Toolbox.h>
#include <Plugins/Samples/Common/OrthancPluginCppWrapper.h>

#include <list>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string/replace.hpp>


namespace
{
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
    bool                          filteredStudyInstanceUid_;
    bool                          filteredSeriesInstanceUid_;


    static void AddResultAttributesForLevel(std::set<Orthanc::DicomTag>& result,
                                            Orthanc::ResourceType level)
    {
      switch (level)
      {
        case Orthanc::ResourceType_Study:
          // http://dicom.nema.org/medical/dicom/2019a/output/html/part18.html#table_6.7.1-2
          //result.insert(Orthanc::DicomTag(0x0008, 0x0005));  // Specific Character Set  => SPECIAL CASE
          result.insert(Orthanc::DicomTag(0x0008, 0x0020));  // Study Date
          result.insert(Orthanc::DicomTag(0x0008, 0x0030));  // Study Time
          result.insert(Orthanc::DicomTag(0x0008, 0x0050));  // Accession Number
          result.insert(Orthanc::DicomTag(0x0008, 0x0056));  // Instance Availability
          //result.insert(Orthanc::DicomTag(0x0008, 0x0061));  // Modalities in Study  => SPECIAL CASE
          result.insert(Orthanc::DicomTag(0x0008, 0x0090));  // Referring Physician's Name
          result.insert(Orthanc::DicomTag(0x0008, 0x0201));  // Timezone Offset From UTC
          //result.insert(Orthanc::DicomTag(0x0008, 0x1190));  // Retrieve URL  => SPECIAL CASE
          result.insert(Orthanc::DicomTag(0x0010, 0x0010));  // Patient's Name
          result.insert(Orthanc::DicomTag(0x0010, 0x0020));  // Patient ID
          result.insert(Orthanc::DicomTag(0x0010, 0x0030));  // Patient's Birth Date
          result.insert(Orthanc::DicomTag(0x0010, 0x0040));  // Patient's Sex
          result.insert(Orthanc::DicomTag(0x0020, 0x000D));  // Study Instance UID
          result.insert(Orthanc::DicomTag(0x0020, 0x0010));  // Study ID
          //result.insert(Orthanc::DicomTag(0x0020, 0x1206));  // Number of Study Related Series  => SPECIAL CASE
          //result.insert(Orthanc::DicomTag(0x0020, 0x1208));  // Number of Study Related Instances  => SPECIAL CASE
          break;

        case Orthanc::ResourceType_Series:
          // http://dicom.nema.org/medical/dicom/2019a/output/html/part18.html#table_6.7.1-2a
          //result.insert(Orthanc::DicomTag(0x0008, 0x0005));  // Specific Character Set  => SPECIAL CASE
          result.insert(Orthanc::DicomTag(0x0008, 0x0060));  // Modality
          result.insert(Orthanc::DicomTag(0x0008, 0x0201));  // Timezone Offset From UTC
          result.insert(Orthanc::DicomTag(0x0008, 0x103E));  // Series Description
          //result.insert(Orthanc::DicomTag(0x0008, 0x1190));  // Retrieve URL  => SPECIAL CASE
          result.insert(Orthanc::DicomTag(0x0020, 0x000E));  // Series Instance UID
          result.insert(Orthanc::DicomTag(0x0020, 0x0011));  // Series Number
          //result.insert(Orthanc::DicomTag(0x0020, 0x1209));  // Number of Series Related Instances  => SPECIAL CASE
          result.insert(Orthanc::DicomTag(0x0040, 0x0244));  // Performed Procedure Step Start Date
          result.insert(Orthanc::DicomTag(0x0040, 0x0245));  // Performed Procedure Step Start Time
          result.insert(Orthanc::DicomTag(0x0040, 0x0275));  // Request Attribute Sequence
          break;

        case Orthanc::ResourceType_Instance:
          // http://dicom.nema.org/medical/dicom/2019a/output/html/part18.html#table_6.7.1-2b
          //result.insert(Orthanc::DicomTag(0x0008, 0x0005));  // Specific Character Set  => SPECIAL CASE
          result.insert(Orthanc::DicomTag(0x0008, 0x0016));  // SOP Class UID
          result.insert(Orthanc::DicomTag(0x0008, 0x0018));  // SOP Instance UID
          result.insert(Orthanc::DicomTag(0x0008, 0x0056));  // Instance Availability
          result.insert(Orthanc::DicomTag(0x0008, 0x0201));  // Timezone Offset From UTC
          result.insert(Orthanc::DicomTag(0x0008, 0x1190));  // Retrieve URL
          result.insert(Orthanc::DicomTag(0x0020, 0x0013));  // Instance Number
          result.insert(Orthanc::DicomTag(0x0028, 0x0010));  // Rows
          result.insert(Orthanc::DicomTag(0x0028, 0x0011));  // Columns
          result.insert(Orthanc::DicomTag(0x0028, 0x0100));  // Bits Allocated
          result.insert(Orthanc::DicomTag(0x0028, 0x0008));  // Number of Frames
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
      includeAllFields_(false),
      filteredStudyInstanceUid_(false),
      filteredSeriesInstanceUid_(false)
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
            // The following lines are new in DICOMweb > 1.0, and
            // allow to query against a list of multiple values
            // http://dicom.nema.org/MEDICAL/dicom/2019a/output/chtml/part18/sect_6.7.html#sect_6.7.1.1.1
            boost::replace_all(value, "\\", "");  // Remove backslashes from source request

            // Replace commas by backslashes
            boost::replace_all(value, ",", "\\");
            boost::replace_all(value, "%2c", "\\");
            boost::replace_all(value, "%2C", "\\");
            
            AddFilter(tag, value, false);
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
                   const std::string& constraint,
                   bool isFromPath)
    {
      filters_[tag] = constraint;

      if (!isFromPath)
      {
        if (tag == Orthanc::DICOM_TAG_STUDY_INSTANCE_UID)
        {
          filteredStudyInstanceUid_ = true;
        }
        else if (tag == Orthanc::DICOM_TAG_SERIES_INSTANCE_UID)
        {
          filteredSeriesInstanceUid_ = true;
        }
      }
    }

    void Print(std::ostream& out) const 
    {
      for (Filters::const_iterator it = filters_.begin(); 
           it != filters_.end(); ++it)
      {
        printf("Filter [%04x,%04x] = [%s]\n", it->first.GetGroup(), it->first.GetElement(), it->second.c_str());
      }
      printf("QIDO on StudyInstanceUID: %d\n", filteredStudyInstanceUid_);
      printf("QIDO on SeriesInstanceUID: %d\n\n", filteredSeriesInstanceUid_);
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

      bool caseSensitive;
      if (OrthancPlugins::Configuration::LookupBooleanValue(caseSensitive, "QidoCaseSensitive"))
      {
        result["CaseSensitive"] = caseSensitive;
      }

      result["Expand"] = false;
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


    void ComputeDerivedTags(Orthanc::DicomMap& target,
                            std::string& someInstance,
                            Orthanc::ResourceType level,
                            const std::string& resource) const
    {
      static const char* const INSTANCES = "Instances";      
      static const char* const MAIN_DICOM_TAGS = "MainDicomTags";
      static const char* const MODALITY = "Modality";
      
      switch (level)
      {
        case Orthanc::ResourceType_Instance:
          someInstance = resource;
          break;

        case Orthanc::ResourceType_Study:
        {
          Json::Value series;
          if (OrthancPlugins::RestApiGet(series, "/studies/" + resource + "/series?expand", false) &&
              series.type() == Json::arrayValue)
          {
            // Collect the Modality of all the child series, and 
            std::set<std::string> modalities;
            unsigned int countInstances = 0;
            
            for (Json::Value::ArrayIndex i = 0; i < series.size(); i++)
            {
              if (series[i].type() == Json::objectValue)
              {
                if (series[i].isMember(MAIN_DICOM_TAGS) &&
                    series[i][MAIN_DICOM_TAGS].type() == Json::objectValue &&
                    series[i][MAIN_DICOM_TAGS].isMember(MODALITY) &&
                    series[i][MAIN_DICOM_TAGS][MODALITY].type() == Json::stringValue)
                {
                  modalities.insert(series[i][MAIN_DICOM_TAGS][MODALITY].asString());
                }
                
                if (series[i].isMember(INSTANCES) &&
                    series[i][INSTANCES].type() == Json::arrayValue)
                {
                  if (series[i][INSTANCES].size() > 0 &&
                      series[i][INSTANCES][0].type() == Json::stringValue)
                  {
                    someInstance = series[i][INSTANCES][0].asString();
                  }
                  
                  countInstances += series[i][INSTANCES].size();
                }
              }
            }

            target.SetValue(Orthanc::DICOM_TAG_NUMBER_OF_STUDY_RELATED_SERIES,
                            boost::lexical_cast<std::string>(series.size()), false);
            
            target.SetValue(Orthanc::DICOM_TAG_NUMBER_OF_STUDY_RELATED_INSTANCES,
                            boost::lexical_cast<std::string>(countInstances), false);
            
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

            target.SetValue(Orthanc::DICOM_TAG_MODALITIES_IN_STUDY, s, false);
          }
          else
          {
            target.SetValue(Orthanc::DICOM_TAG_MODALITIES_IN_STUDY, "", false);
            target.SetValue(Orthanc::DICOM_TAG_NUMBER_OF_STUDY_RELATED_SERIES, "0", false);
            target.SetValue(Orthanc::DICOM_TAG_NUMBER_OF_STUDY_RELATED_INSTANCES, "0", false);
          }

          break;
        }

        case Orthanc::ResourceType_Series:
        {
          Json::Value series;
          if (OrthancPlugins::RestApiGet(series, "/series/" + resource, false) &&
              series.type() == Json::objectValue &&
              series.isMember(INSTANCES) &&
              series[INSTANCES].type() == Json::arrayValue)
          {
            if (series[INSTANCES].size() > 0 &&
                series[INSTANCES][0].type() == Json::stringValue)
            {
              someInstance = series[INSTANCES][0].asString();
            }
            
            // Number of Series Related Instances
            target.SetValue(Orthanc::DICOM_TAG_NUMBER_OF_SERIES_RELATED_INSTANCES, 
                            boost::lexical_cast<std::string>(series[INSTANCES].size()), false);
          }
          else
          {
            // Should never happen
            target.SetValue(Orthanc::DICOM_TAG_NUMBER_OF_SERIES_RELATED_INSTANCES, "0", false);
          }

          break;
        }

        default:
          throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
      }
    }                              


    void ExtractFields(Orthanc::DicomMap& result,
                       const Orthanc::DicomMap& source,
                       const std::string& wadoBase,
                       Orthanc::ResourceType level) const
    {
      std::set<Orthanc::DicomTag> fields;

      for (std::list<Orthanc::DicomTag>::const_iterator
             it = includeFields_.begin(); it != includeFields_.end(); ++it)
      {
        fields.insert(*it);
      }

      // The list of attributes for this query level
      AddResultAttributesForLevel(fields, level);

      // All other attributes passed as query keys
      for (Filters::const_iterator it = filters_.begin();
           it != filters_.end(); ++it)
      {
        fields.insert(it->first);
      }

      // For instances and series, add all Study-level attributes if
      // {StudyInstanceUID} is not specified.
      if (!filteredStudyInstanceUid_ &&
          (level == Orthanc::ResourceType_Instance ||
           level == Orthanc::ResourceType_Series))
      {
        AddResultAttributesForLevel(fields, Orthanc::ResourceType_Study);
      }

      // For instances, add all Series-level attributes if
      // {SeriesInstanceUID} is not specified.
      if (!filteredSeriesInstanceUid_ &&
          level == Orthanc::ResourceType_Instance)
      {
        AddResultAttributesForLevel(fields, Orthanc::ResourceType_Series);
      }

      // Copy all the required fields to the target
      for (std::set<Orthanc::DicomTag>::const_iterator
             it = fields.begin(); it != fields.end(); ++it)
      {
        std::string value;
        if (source.LookupStringValue(value, *it, false /* no binary */))
        {
          result.SetValue(*it, value, false);
        }
      }

      // Set the retrieve URL for WADO-RS
      std::string url = (wadoBase + "studies/" +
                         source.GetStringValue(Orthanc::DICOM_TAG_STUDY_INSTANCE_UID, "", false));

      if (level == Orthanc::ResourceType_Series || 
          level == Orthanc::ResourceType_Instance)
      {
        url += "/series/" + source.GetStringValue(Orthanc::DICOM_TAG_SERIES_INSTANCE_UID, "", false);
      }

      if (level == Orthanc::ResourceType_Instance)
      {
        url += "/instances/" + source.GetStringValue(Orthanc::DICOM_TAG_SOP_INSTANCE_UID, "", false);
      }
    
      static const Orthanc::DicomTag DICOM_TAG_RETRIEVE_URL(0x0008, 0x1190);
      result.SetValue(DICOM_TAG_RETRIEVE_URL, url, false);
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

  LOG(INFO) << "Body of the call from QIDO-RS to /tools/find: " << find.toStyledString();
  
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

  std::string wadoBase = OrthancPlugins::Configuration::GetBaseUrl(request);

  OrthancPlugins::DicomWebFormatter::HttpWriter writer(
    output, OrthancPlugins::Configuration::IsXmlExpected(request));

  // Fix of issue #13
  for (Json::Value::ArrayIndex i = 0; i < resources.size(); i++)
  {
    const std::string resource = resources[i].asString();

    Orthanc::DicomMap derivedTags;
    std::string someInstance;
    matcher.ComputeDerivedTags(derivedTags, someInstance, level, resource);
    
    Json::Value tags;
    if (!someInstance.empty() &&
        OrthancPlugins::RestApiGet(tags, "/instances/" + someInstance + "/tags", false))
    {
      Orthanc::DicomMap source;
      source.FromDicomAsJson(tags);

      std::string wadoUrl = OrthancPlugins::Configuration::GetWadoUrl(
        wadoBase,
        source.GetStringValue(Orthanc::DICOM_TAG_STUDY_INSTANCE_UID, "", false),
        source.GetStringValue(Orthanc::DICOM_TAG_SERIES_INSTANCE_UID, "", false),
        source.GetStringValue(Orthanc::DICOM_TAG_SOP_INSTANCE_UID, "", false));

      Orthanc::DicomMap target;
      target.Assign(derivedTags);
      matcher.ExtractFields(target, source, wadoBase, level);

      writer.AddOrthancMap(target);
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
      matcher.AddFilter(Orthanc::DICOM_TAG_STUDY_INSTANCE_UID, request->groups[0], true);
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
      matcher.AddFilter(Orthanc::DICOM_TAG_STUDY_INSTANCE_UID, request->groups[0], true);
    }

    if (request->groupsCount == 2)
    {
      // The "SeriesInstanceUID" is provided by the regular expression
      matcher.AddFilter(Orthanc::DICOM_TAG_SERIES_INSTANCE_UID, request->groups[1], true);
    }

    ApplyMatcher(output, request, matcher, Orthanc::ResourceType_Instance);
  }
}
