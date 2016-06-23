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


#include "QidoRs.h"

#include "Plugin.h"
#include "StowRs.h"  // For IsXmlExpected()
#include "Dicom.h"
#include "DicomResults.h"
#include "Configuration.h"
#include "../Orthanc/Core/Toolbox.h"
#include "../Orthanc/Core/OrthancException.h"

#include <gdcmTag.h>
#include <list>
#include <stdexcept>
#include <boost/lexical_cast.hpp>
#include <gdcmDict.h>
#include <gdcmDicts.h>
#include <gdcmGlobal.h>
#include <gdcmDictEntry.h>
#include <boost/regex.hpp>
#include <boost/algorithm/string/replace.hpp>


namespace
{
  static std::string FormatOrthancTag(const gdcm::Tag& tag)
  {
    char b[16];
    sprintf(b, "%04x,%04x", tag.GetGroup(), tag.GetElement());
    return std::string(b);
  }


  static std::string GetOrthancTag(const Json::Value& source,
                                   const gdcm::Tag& tag,
                                   const std::string& defaultValue)
  {
    std::string s = FormatOrthancTag(tag);
      
    if (source.isMember(s) &&
        source[s].type() == Json::objectValue &&
        source[s].isMember("Value") &&
        source[s].isMember("Type") &&
        source[s]["Type"] == "String" &&
        source[s]["Value"].type() == Json::stringValue)
    {
      return source[s]["Value"].asString();
    }
    else
    {
      return defaultValue;
    }
  }


  enum QueryLevel
  {
    QueryLevel_Study,
    QueryLevel_Series,
    QueryLevel_Instance
  };


  class ModuleMatcher
  {
  public:
    typedef std::map<gdcm::Tag, std::string>  Filters;

  private:
    bool                  fuzzy_;
    unsigned int          offset_;
    unsigned int          limit_;
    std::list<gdcm::Tag>  includeFields_;
    bool                  includeAllFields_;
    Filters               filters_;


    static void AddResultAttributesForLevel(std::list<gdcm::Tag>& result,
                                            QueryLevel level)
    {
      switch (level)
      {
        case QueryLevel_Study:
          // http://medical.nema.org/medical/dicom/current/output/html/part18.html#table_6.7.1-2
          result.push_back(gdcm::Tag(0x0008, 0x0005));  // Specific Character Set
          result.push_back(gdcm::Tag(0x0008, 0x0020));  // Study Date
          result.push_back(gdcm::Tag(0x0008, 0x0030));  // Study Time
          result.push_back(gdcm::Tag(0x0008, 0x0050));  // Accession Number
          result.push_back(gdcm::Tag(0x0008, 0x0056));  // Instance Availability
          //result.push_back(gdcm::Tag(0x0008, 0x0061));  // Modalities in Study  => SPECIAL CASE
          result.push_back(gdcm::Tag(0x0008, 0x0090));  // Referring Physician's Name
          result.push_back(gdcm::Tag(0x0008, 0x0201));  // Timezone Offset From UTC
          //result.push_back(gdcm::Tag(0x0008, 0x1190));  // Retrieve URL  => SPECIAL CASE
          result.push_back(gdcm::Tag(0x0010, 0x0010));  // Patient's Name
          result.push_back(gdcm::Tag(0x0010, 0x0020));  // Patient ID
          result.push_back(gdcm::Tag(0x0010, 0x0030));  // Patient's Birth Date
          result.push_back(gdcm::Tag(0x0010, 0x0040));  // Patient's Sex
          result.push_back(gdcm::Tag(0x0020, 0x000D));  // Study Instance UID
          result.push_back(gdcm::Tag(0x0020, 0x0010));  // Study ID
          //result.push_back(gdcm::Tag(0x0020, 0x1206));  // Number of Study Related Series  => SPECIAL CASE
          //result.push_back(gdcm::Tag(0x0020, 0x1208));  // Number of Study Related Instances  => SPECIAL CASE
          break;

        case QueryLevel_Series:
          // http://medical.nema.org/medical/dicom/current/output/html/part18.html#table_6.7.1-2a
          result.push_back(gdcm::Tag(0x0008, 0x0005));  // Specific Character Set
          result.push_back(gdcm::Tag(0x0008, 0x0060));  // Modality
          result.push_back(gdcm::Tag(0x0008, 0x0201));  // Timezone Offset From UTC
          result.push_back(gdcm::Tag(0x0008, 0x103E));  // Series Description
          //result.push_back(gdcm::Tag(0x0008, 0x1190));  // Retrieve URL  => SPECIAL CASE
          result.push_back(gdcm::Tag(0x0020, 0x000E));  // Series Instance UID
          result.push_back(gdcm::Tag(0x0020, 0x0011));  // Series Number
          //result.push_back(gdcm::Tag(0x0020, 0x1209));  // Number of Series Related Instances  => SPECIAL CASE
          result.push_back(gdcm::Tag(0x0040, 0x0244));  // Performed Procedure Step Start Date
          result.push_back(gdcm::Tag(0x0040, 0x0245));  // Performed Procedure Step Start Time
          result.push_back(gdcm::Tag(0x0040, 0x0275));  // Request Attribute Sequence
          break;

        case QueryLevel_Instance:
          // http://medical.nema.org/medical/dicom/current/output/html/part18.html#table_6.7.1-2b
          result.push_back(gdcm::Tag(0x0008, 0x0005));  // Specific Character Set
          result.push_back(gdcm::Tag(0x0008, 0x0016));  // SOP Class UID
          result.push_back(gdcm::Tag(0x0008, 0x0018));  // SOP Instance UID
          result.push_back(gdcm::Tag(0x0008, 0x0056));  // Instance Availability
          result.push_back(gdcm::Tag(0x0008, 0x0201));  // Timezone Offset From UTC
          result.push_back(gdcm::Tag(0x0008, 0x1190));  // Retrieve URL
          result.push_back(gdcm::Tag(0x0020, 0x0013));  // Instance Number
          result.push_back(gdcm::Tag(0x0028, 0x0010));  // Rows
          result.push_back(gdcm::Tag(0x0028, 0x0011));  // Columns
          result.push_back(gdcm::Tag(0x0028, 0x0100));  // Bits Allocated
          result.push_back(gdcm::Tag(0x0028, 0x0008));  // Number of Frames
          break;

        default:
          throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
      }
    }


  public:
    ModuleMatcher(const OrthancPluginHttpRequest* request) :
    fuzzy_(false),
    offset_(0),
    limit_(0),
    includeAllFields_(false)
    {
      for (uint32_t i = 0; i < request->getCount; i++)
      {
        std::string key(request->getKeys[i]);
        std::string value(request->getValues[i]);

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
            OrthancPlugins::Configuration::LogError("Not a proper value for fuzzy matching (true or false): " + value);
            throw Orthanc::OrthancException(Orthanc::ErrorCode_BadRequest);
          }
        }
        else if (key == "includefield")
        {
          if (key == "all")
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
              includeFields_.push_back(OrthancPlugins::ParseTag(*dictionary_, tags[i]));
            }
          }
        }
        else
        {
          filters_[OrthancPlugins::ParseTag(*dictionary_, key)] = value;
        }
      }
    }

    unsigned int GetLimit() const
    {
      return limit_;
    }

    unsigned int GetOffset() const
    {
      return offset_;
    }

    void AddFilter(const gdcm::Tag& tag,
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
                          QueryLevel level) const
    {
      result = Json::objectValue;

      switch (level)
      {
        case QueryLevel_Study:
          result["Level"] = "Study";
          break;

        case QueryLevel_Series:
          result["Level"] = "Series";
          break;

        case QueryLevel_Instance:
          result["Level"] = "Instance";
          break;

        default:
          throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
      }

      result["Expand"] = false;
      result["CaseSensitive"] = true;
      result["Query"] = Json::objectValue;

      for (Filters::const_iterator it = filters_.begin(); 
           it != filters_.end(); ++it)
      {
        result["Query"][FormatOrthancTag(it->first)] = it->second;
      }
    }


    void ComputeDerivedTags(Filters& target,
                            QueryLevel level,
                            const std::string& resource) const
    {
      target.clear();

      switch (level)
      {
        case QueryLevel_Study:
        {
          Json::Value series, instances;
          if (OrthancPlugins::RestApiGetJson(series, OrthancPlugins::Configuration::GetContext(), "/studies/" + resource + "/series?expand") &&
              OrthancPlugins::RestApiGetJson(instances, OrthancPlugins::Configuration::GetContext(), "/studies/" + resource + "/instances"))
          {
            // Number of Study Related Series
            target[gdcm::Tag(0x0020, 0x1206)] = boost::lexical_cast<std::string>(series.size());

            // Number of Study Related Instances
            target[gdcm::Tag(0x0020, 0x1208)] = boost::lexical_cast<std::string>(instances.size());

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

            target[gdcm::Tag(0x0008, 0x0061)] = s;  // Modalities in Study
          }
          else
          {
            target[gdcm::Tag(0x0008, 0x0061)] = "";   // Modalities in Study
            target[gdcm::Tag(0x0020, 0x1206)] = "0";  // Number of Study Related Series
            target[gdcm::Tag(0x0020, 0x1208)] = "0";  // Number of Study Related Instances
          }

          break;
        }

        case QueryLevel_Series:
        {
          Json::Value instances;
          if (OrthancPlugins::RestApiGetJson(instances, OrthancPlugins::Configuration::GetContext(), "/series/" + resource + "/instances"))
          {
            // Number of Series Related Instances
            target[gdcm::Tag(0x0020, 0x1209)] = boost::lexical_cast<std::string>(instances.size());
          }
          else
          {
            target[gdcm::Tag(0x0020, 0x1209)] = "0";  // Number of Series Related Instances
          }

          break;
        }

        default:
          break;
      }
    }                              


    void ExtractFields(gdcm::DataSet& result,
                       const OrthancPlugins::ParsedDicomFile& dicom,
                       const std::string& wadoBase,
                       QueryLevel level) const
    {
      std::list<gdcm::Tag> fields = includeFields_;

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
      if ((level == QueryLevel_Instance  || level == QueryLevel_Series) 
          && filters_.find(OrthancPlugins::DICOM_TAG_STUDY_INSTANCE_UID) == filters_.end()
        )
      {
        AddResultAttributesForLevel(fields, QueryLevel_Study);
      }

      // For instances, add all Series-level attributes if
      // {SeriesInstanceUID} is not specified.
      if (level == QueryLevel_Instance
          && filters_.find(OrthancPlugins::DICOM_TAG_SERIES_INSTANCE_UID) == filters_.end()
        )
      {
        AddResultAttributesForLevel(fields, QueryLevel_Series);
      }

      // Copy all the required fields to the target
      for (std::list<gdcm::Tag>::const_iterator
             it = fields.begin(); it != fields.end(); ++it)
      {
        if (dicom.GetDataSet().FindDataElement(*it))
        {
          const gdcm::DataElement& element = dicom.GetDataSet().GetDataElement(*it);
          result.Replace(element);
        }
      }

      // Set the retrieve URL for WADO-RS
      std::string url = (wadoBase + "studies/" + 
                         dicom.GetRawTagWithDefault(OrthancPlugins::DICOM_TAG_STUDY_INSTANCE_UID, "", true));

      if (level == QueryLevel_Series || level == QueryLevel_Instance)
      {
        url += "/series/" + dicom.GetRawTagWithDefault(OrthancPlugins::DICOM_TAG_SERIES_INSTANCE_UID, "", true);
      }

      if (level == QueryLevel_Instance)
      {
        url += "/instances/" + dicom.GetRawTagWithDefault(OrthancPlugins::DICOM_TAG_SOP_INSTANCE_UID, "", true);
      }
    
      gdcm::DataElement element(OrthancPlugins::DICOM_TAG_RETRIEVE_URL);
      element.SetByteValue(url.c_str(), url.size());
      result.Replace(element);
    }


    void ExtractFields(Json::Value& result,
                       const Json::Value& source,
                       const std::string& wadoBase,
                       QueryLevel level) const
    {
      result = Json::objectValue;
      std::list<gdcm::Tag> fields = includeFields_;

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
      if ((level == QueryLevel_Instance  || level == QueryLevel_Series) 
          && filters_.find(OrthancPlugins::DICOM_TAG_STUDY_INSTANCE_UID) == filters_.end()
        )
      {
        AddResultAttributesForLevel(fields, QueryLevel_Study);
      }

      // For instances, add all Series-level attributes if
      // {SeriesInstanceUID} is not specified.
      if (level == QueryLevel_Instance
          && filters_.find(OrthancPlugins::DICOM_TAG_SERIES_INSTANCE_UID) == filters_.end()
        )
      {
        AddResultAttributesForLevel(fields, QueryLevel_Series);
      }

      // Copy all the required fields to the target
      for (std::list<gdcm::Tag>::const_iterator
             it = fields.begin(); it != fields.end(); ++it)
      {
        std::string tag = FormatOrthancTag(*it);
        if (source.isMember(tag))
        {
          result[tag] = source[tag];
        }
      }

      // Set the retrieve URL for WADO-RS
      std::string url = (wadoBase + "studies/" + 
                         GetOrthancTag(source, OrthancPlugins::DICOM_TAG_STUDY_INSTANCE_UID, ""));

      if (level == QueryLevel_Series || level == QueryLevel_Instance)
      {
        url += "/series/" + GetOrthancTag(source, OrthancPlugins::DICOM_TAG_SERIES_INSTANCE_UID, "");
      }

      if (level == QueryLevel_Instance)
      {
        url += "/instances/" + GetOrthancTag(source, OrthancPlugins::DICOM_TAG_SOP_INSTANCE_UID, "");
      }
    
      Json::Value tmp = Json::objectValue;
      tmp["Name"] = "RetrieveURL";
      tmp["Type"] = "String";
      tmp["Value"] = url;

      result[FormatOrthancTag(OrthancPlugins::DICOM_TAG_RETRIEVE_URL)] = tmp;
    }
  };
}



static void ApplyMatcher(OrthancPluginRestOutput* output,
                         const OrthancPluginHttpRequest* request,
                         const ModuleMatcher& matcher,
                         QueryLevel level)
{
  Json::Value find;
  matcher.ConvertToOrthanc(find, level);

  Json::FastWriter writer;
  std::string body = writer.write(find);
  
  Json::Value resources;
  if (!OrthancPlugins::RestApiPostJson(resources, OrthancPlugins::Configuration::GetContext(), "/tools/find", body) ||
      resources.type() != Json::arrayValue)
  {
    throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
  }

  typedef std::list< std::pair<std::string, std::string> > ResourcesAndInstances;

  ResourcesAndInstances resourcesAndInstances;
  std::string root = (level == QueryLevel_Study ? "/studies/" : "/series/");
    
  for (Json::Value::ArrayIndex i = 0; i < resources.size(); i++)
  {
    const std::string resource = resources[i].asString();

    if (level == QueryLevel_Study ||
        level == QueryLevel_Series)
    {
      // Find one child instance of this resource
      Json::Value tmp;
      if (OrthancPlugins::RestApiGetJson(tmp, OrthancPlugins::Configuration::GetContext(), root + resource + "/instances") &&
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

  OrthancPlugins::DicomResults results(OrthancPlugins::Configuration::GetContext(), output, wadoBase, *dictionary_, IsXmlExpected(request), true);

#if 0
  // Implementation up to version 0.2 of the plugin. Each instance is
  // downloaded and decoded using GDCM, which slows down things
  // wrt. the new implementation below that directly uses the Orthanc
  // pre-computed JSON summary.
  for (ResourcesAndInstances::const_iterator
         it = resourcesAndInstances.begin(); it != resourcesAndInstances.end(); ++it)
  {
    ModuleMatcher::Filters derivedTags;
    matcher.ComputeDerivedTags(derivedTags, level, it->first);

    std::string file;
    if (OrthancPlugins::RestApiGetString(file, OrthancPlugins::Configuration::GetContext(), "/instances/" + it->second + "/file"))
    {
      OrthancPlugins::ParsedDicomFile dicom(file);

      std::auto_ptr<gdcm::DataSet> result(new gdcm::DataSet);
      matcher.ExtractFields(*result, dicom, wadoBase, level);

      // Inject the derived tags
      ModuleMatcher::Filters derivedTags;
      matcher.ComputeDerivedTags(derivedTags, level, it->first);

      for (ModuleMatcher::Filters::const_iterator
             tag = derivedTags.begin(); tag != derivedTags.end(); ++tag)
      {
        gdcm::DataElement element(tag->first);
        element.SetByteValue(tag->second.c_str(), tag->second.size());
        result->Replace(element);
      }

      results.Add(dicom.GetFile(), *result);
    }
  }

#else
  // Fix of issue #13
  for (ResourcesAndInstances::const_iterator
         it = resourcesAndInstances.begin(); it != resourcesAndInstances.end(); ++it)
  {
    Json::Value tags;
    if (OrthancPlugins::RestApiGetJson(tags, OrthancPlugins::Configuration::GetContext(), "/instances/" + it->second + "/tags"))
    {
      std::string wadoUrl = OrthancPlugins::Configuration::GetWadoUrl(
        wadoBase, 
        GetOrthancTag(tags, OrthancPlugins::DICOM_TAG_STUDY_INSTANCE_UID, ""),
        GetOrthancTag(tags, OrthancPlugins::DICOM_TAG_SERIES_INSTANCE_UID, ""),
        GetOrthancTag(tags, OrthancPlugins::DICOM_TAG_SOP_INSTANCE_UID, ""));

      Json::Value result;
      matcher.ExtractFields(result, tags, wadoBase, level);

      // Inject the derived tags
      ModuleMatcher::Filters derivedTags;
      matcher.ComputeDerivedTags(derivedTags, level, it->first);

      for (ModuleMatcher::Filters::const_iterator
             tag = derivedTags.begin(); tag != derivedTags.end(); ++tag)
      {
        Json::Value tmp = Json::objectValue;
        tmp["Name"] = OrthancPlugins::GetKeyword(*dictionary_, tag->first);
        tmp["Type"] = "String";
        tmp["Value"] = tag->second;
        result[FormatOrthancTag(tag->first)] = tmp;
      }

      results.AddFromOrthanc(result, wadoUrl);
    }
  }
#endif

  results.Answer();
}



void SearchForStudies(OrthancPluginRestOutput* output,
                      const char* url,
                      const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "GET");
  }
  else
  {
    ModuleMatcher matcher(request);
    ApplyMatcher(output, request, matcher, QueryLevel_Study);
  }
}


void SearchForSeries(OrthancPluginRestOutput* output,
                     const char* url,
                     const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "GET");
  }
  else
  {
    ModuleMatcher matcher(request);

    if (request->groupsCount == 1)
    {
      // The "StudyInstanceUID" is provided by the regular expression
      matcher.AddFilter(OrthancPlugins::DICOM_TAG_STUDY_INSTANCE_UID, request->groups[0]);
    }

    ApplyMatcher(output, request, matcher, QueryLevel_Series);
  }
}


void SearchForInstances(OrthancPluginRestOutput* output,
                        const char* url,
                        const OrthancPluginHttpRequest* request)
{
  if (request->method != OrthancPluginHttpMethod_Get)
  {
    OrthancPluginSendMethodNotAllowed(OrthancPlugins::Configuration::GetContext(), output, "GET");
  }
  else
  {
    ModuleMatcher matcher(request);

    if (request->groupsCount == 1 || request->groupsCount == 2)
    {
      // The "StudyInstanceUID" is provided by the regular expression
      matcher.AddFilter(OrthancPlugins::DICOM_TAG_STUDY_INSTANCE_UID, request->groups[0]);
    }

    if (request->groupsCount == 2)
    {
      // The "SeriesInstanceUID" is provided by the regular expression
      matcher.AddFilter(OrthancPlugins::DICOM_TAG_SERIES_INSTANCE_UID, request->groups[1]);
    }

    ApplyMatcher(output, request, matcher, QueryLevel_Instance);
  }
}
