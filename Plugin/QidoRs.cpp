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


#include "QidoRs.h"

#include "Plugin.h"
#include "StowRs.h"  // For IsXmlExpected()
#include "Dicom.h"
#include "DicomResults.h"
#include "Configuration.h"
#include "../Orthanc/Core/Toolbox.h"

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

  enum QueryLevel
  {
    QueryLevel_Study,
    QueryLevel_Series,
    QueryLevel_Instance
  };


  class ModuleMatcher
  {
  private:
    typedef std::map<gdcm::Tag, std::string>  Filters;

    const gdcm::Dict&     dictionary_;
    bool                  fuzzy_;
    unsigned int          offset_;
    unsigned int          limit_;
    std::list<gdcm::Tag>  includeFields_;
    bool                  includeAllFields_;
    Filters               filters_;



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


    gdcm::Tag  ParseTag(const std::string& key) const
    {
      if (key.size() == 8 &&
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
      else
      {
        gdcm::Tag tag;
        dictionary_.GetDictEntryByKeyword(key.c_str(), tag);

        if (tag.IsIllegal() || tag.IsPrivate())
        {
          if (key.find('.') != std::string::npos)
          {
            throw std::runtime_error("This QIDO-RS implementation does not support search over sequences: " + key);
          }
          else
          {
            throw std::runtime_error("Illegal tag name in QIDO-RS: " + key);
          }
        }

        return tag;
      }
    }


    static bool IsWildcard(const std::string& constraint)
    {
      return (constraint.find('-') != std::string::npos ||
              constraint.find('*') != std::string::npos ||
              constraint.find('\\') != std::string::npos ||
              constraint.find('?') != std::string::npos);
    }

    static bool ApplyRangeConstraint(const std::string& value,
                                     const std::string& constraint)
    {
      size_t separator = constraint.find('-');
      std::string lower(constraint.substr(0, separator));
      std::string upper(constraint.substr(separator + 1));
      std::string v(value);

      Orthanc::Toolbox::ToLowerCase(lower);
      Orthanc::Toolbox::ToLowerCase(upper);
      Orthanc::Toolbox::ToLowerCase(v);

      if (lower.size() == 0 && upper.size() == 0)
      {
        return false;
      }

      if (lower.size() == 0)
      {
        return v <= upper;
      }

      if (upper.size() == 0)
      {
        return v >= lower;
      }
    
      return (v >= lower && v <= upper);
    }


    static bool ApplyListConstraint(const std::string& value,
                                    const std::string& constraint)
    {
      std::string v1(value);
      Orthanc::Toolbox::ToLowerCase(v1);

      std::vector<std::string> items;
      Orthanc::Toolbox::TokenizeString(items, constraint, '\\');

      for (size_t i = 0; i < items.size(); i++)
      {
        std::string lower(items[i]);
        Orthanc::Toolbox::ToLowerCase(lower);
        if (lower == v1)
        {
          return true;
        }
      }

      return false;
    }


    static std::string WildcardToRegularExpression(const std::string& source)
    {
      std::string result = source;

      // Escape all special characters
      boost::replace_all(result, "\\", "\\\\");
      boost::replace_all(result, "^", "\\^");
      boost::replace_all(result, ".", "\\.");
      boost::replace_all(result, "$", "\\$");
      boost::replace_all(result, "|", "\\|");
      boost::replace_all(result, "(", "\\(");
      boost::replace_all(result, ")", "\\)");
      boost::replace_all(result, "[", "\\[");
      boost::replace_all(result, "]", "\\]");
      boost::replace_all(result, "+", "\\+");
      boost::replace_all(result, "/", "\\/");
      boost::replace_all(result, "{", "\\{");
      boost::replace_all(result, "}", "\\}");

      // Convert wildcards '*' and '?' to their regex equivalents
      boost::replace_all(result, "?", ".");
      boost::replace_all(result, "*", ".*");

      return result;
    }


    static bool Matches(const std::string& value,
                        const std::string& constraint)
    {
      // http://www.itk.org/Wiki/DICOM_QueryRetrieve_Explained
      // http://dicomiseasy.blogspot.be/2012/01/dicom-queryretrieve-part-i.html  

      if (constraint.find('-') != std::string::npos)
      {
        return ApplyRangeConstraint(value, constraint);
      }
    
      if (constraint.find('\\') != std::string::npos)
      {
        return ApplyListConstraint(value, constraint);
      }

      if (constraint.find('*') != std::string::npos ||
          constraint.find('?') != std::string::npos)
      {
        boost::regex pattern(WildcardToRegularExpression(constraint),
                             boost::regex::icase /* case insensitive search */);
        return boost::regex_match(value, pattern);
      }
      else
      {
        std::string v(value), c(constraint);
        Orthanc::Toolbox::ToLowerCase(v);
        Orthanc::Toolbox::ToLowerCase(c);
        return v == c;
      }
    }



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
          result.push_back(gdcm::Tag(0x0008, 0x0061));  // Modalities in Study
          result.push_back(gdcm::Tag(0x0008, 0x0090));  // Referring Physician's Name
          result.push_back(gdcm::Tag(0x0008, 0x0201));  // Timezone Offset From UTC
          //result.push_back(gdcm::Tag(0x0008, 0x1190));  // Retrieve URL  => SPECIAL CASE
          result.push_back(gdcm::Tag(0x0010, 0x0010));  // Patient's Name
          result.push_back(gdcm::Tag(0x0010, 0x0020));  // Patient ID
          result.push_back(gdcm::Tag(0x0010, 0x0030));  // Patient's Birth Date
          result.push_back(gdcm::Tag(0x0010, 0x0040));  // Patient's Sex
          result.push_back(gdcm::Tag(0x0020, 0x000D));  // Study Instance UID
          result.push_back(gdcm::Tag(0x0020, 0x0010));  // Study ID
          result.push_back(gdcm::Tag(0x0020, 0x1206));  // Number of Study Related Series
          result.push_back(gdcm::Tag(0x0020, 0x1208));  // Number of Study Related Instances
          break;

        case QueryLevel_Series:
          // http://medical.nema.org/medical/dicom/current/output/html/part18.html#table_6.7.1-2a
          result.push_back(gdcm::Tag(0x0008, 0x0005));  // Specific Character Set
          result.push_back(gdcm::Tag(0x0008, 0x0056));  // Modality
          result.push_back(gdcm::Tag(0x0008, 0x0201));  // Timezone Offset From UTC
          result.push_back(gdcm::Tag(0x0008, 0x103E));  // Series Description
          //result.push_back(gdcm::Tag(0x0008, 0x1190));  // Retrieve URL  => SPECIAL CASE
          result.push_back(gdcm::Tag(0x0020, 0x000E));  // Series Instance UID
          result.push_back(gdcm::Tag(0x0020, 0x0011));  // Series Number
          result.push_back(gdcm::Tag(0x0020, 0x1209));  // Number of Series Related Instances
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
          throw std::runtime_error("Internal error");
      }
    }



  public:
    ModuleMatcher(const OrthancPluginHttpRequest* request) :
      dictionary_(gdcm::Global::GetInstance().GetDicts().GetPublicDict()),
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
            throw std::runtime_error("Not a proper value for fuzzy matching (true or false): " + value);
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
              includeFields_.push_back(ParseTag(tags[i]));
            }
          }
        }
        else
        {
          filters_[ParseTag(key)] = value;
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

    bool LookupExactFilter(std::string& constraint,
                           const gdcm::Tag& tag) const
    {
      Filters::const_iterator it = filters_.find(tag);
      if (it != filters_.end() &&
          !IsWildcard(it->second))
      {
        constraint = it->second;
        return true;
      }
      else
      {
        return false;
      }
    }

    bool Matches(const OrthancPlugins::ParsedDicomFile& dicom) const
    {
      for (Filters::const_iterator it = filters_.begin();
           it != filters_.end(); ++it)
      {
        std::string value;
        if (!dicom.GetTag(value, it->first, true))
        {
          return false;
        }

        if (!Matches(value, it->second))
        {
          return false;
        }
      }

      return true;
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
             it = fields.begin(); it != fields.end(); it++)
      {
        if (dicom.GetDataSet().FindDataElement(*it))
        {
          const gdcm::DataElement& element = dicom.GetDataSet().GetDataElement(*it);
          result.Replace(element);
        }
      }

      // Set the retrieve URL for WADO-RS
      std::string url = (wadoBase + "studies/" + 
                         dicom.GetTagWithDefault(OrthancPlugins::DICOM_TAG_STUDY_INSTANCE_UID, "", true));

      if (level == QueryLevel_Series || level == QueryLevel_Instance)
      {
        url += "/series/" + dicom.GetTagWithDefault(OrthancPlugins::DICOM_TAG_SERIES_INSTANCE_UID, "", true);
      }

      if (level == QueryLevel_Instance)
      {
        url += "/instances/" + dicom.GetTagWithDefault(OrthancPlugins::DICOM_TAG_SOP_INSTANCE_UID, "", true);
      }
    
      gdcm::DataElement element(OrthancPlugins::DICOM_TAG_RETRIEVE_URL);
      element.SetByteValue(url.c_str(), url.size());
      result.Replace(element);
    }
  };




  class CandidateResources
  {
  private:
    typedef std::set<std::string>  Resources;

    bool        all_;
    QueryLevel  level_;
    Resources   resources_;

    static bool CallLookup(std::string& orthancId,
                           const std::string& dicomId,
                           char* (lookup) (OrthancPluginContext*, const char*))
    {
      bool result = false;

      char* tmp = lookup(context_, dicomId.c_str());
      if (tmp != NULL)
      {
        orthancId = tmp;
        result = true;
      }

      OrthancPluginFreeString(context_, tmp);

      return result;
    }


    void FilterByIdentifierInternal(const ModuleMatcher& matcher,
                                    const gdcm::Tag& tag,
                                    char* (lookup) (OrthancPluginContext*, const char*))
    {
      std::string orthancId, dicomId;

      if (!matcher.LookupExactFilter(dicomId, tag))
      {
        // There is no restriction at this level
        return;
      }

      if (CallLookup(orthancId, dicomId, lookup) &&
          (all_ || resources_.find(orthancId) != resources_.end()))
      {
        // There remains a single candidate resource
        resources_.clear();
        resources_.insert(orthancId);
      }
      else
      {
        // No matching resource remains
        resources_.clear();            
      }

      all_ = false;
    }


    bool PickOneInstance(std::string& instance,
                         const std::string& resource) const
    {
      if (level_ == QueryLevel_Instance)
      {
        instance = resource;
        return true;
      }

      std::string uri;
      if (level_ == QueryLevel_Study)
      {
        uri = "/studies/" + resource + "/instances";
      }
      else
      {
        assert(level_ == QueryLevel_Series);
        uri = "/series/" + resource + "/instances";
      }

      Json::Value instances;
      if (!OrthancPlugins::RestApiGetJson(instances, context_, uri) ||
          instances.type() != Json::arrayValue ||
          instances.size() == 0)
      {
        return false;
      }
      else
      {
        instance = instances[0]["ID"].asString();
        return true;
      }
    }


  public:
    CandidateResources() : all_(true), level_(QueryLevel_Study)
    {
    }

    void GoDown()
    {
      std::string baseUri;
      std::string nextLevel;
      switch (level_)
      {
        case QueryLevel_Study:
          baseUri = "/studies/";
          nextLevel = "Series";
          break;

        case QueryLevel_Series:
          baseUri = "/series/";
          nextLevel = "Instances";
          break;

        default:
          throw std::runtime_error("Internal error");
      }


      if (!all_)
      {
        Resources  children;
      
        for (Resources::const_iterator it = resources_.begin();
             it != resources_.end(); it++)
        {
          Json::Value tmp;
          if (OrthancPlugins::RestApiGetJson(tmp, context_, baseUri + *it) &&
              tmp.type() == Json::objectValue &&
              tmp.isMember(nextLevel) &&
              tmp[nextLevel].type() == Json::arrayValue)
          {
            for (Json::Value::ArrayIndex i = 0; i < tmp[nextLevel].size(); i++)
            {
              children.insert(tmp[nextLevel][i].asString());
            }
          }
        }

        resources_ = children;
      }


      switch (level_)
      {
        case QueryLevel_Study:
          level_ = QueryLevel_Series;
          break;

        case QueryLevel_Series:
          level_ = QueryLevel_Instance;
          break;

        default:
          throw std::runtime_error("Internal error");
      }
    }


    void FilterByIdentifier(const ModuleMatcher& matcher)
    {
      switch (level_)
      {
        case QueryLevel_Study:
          FilterByIdentifierInternal(matcher, OrthancPlugins::DICOM_TAG_STUDY_INSTANCE_UID,
                                     OrthancPluginLookupStudy);
          FilterByIdentifierInternal(matcher, OrthancPlugins::DICOM_TAG_ACCESSION_NUMBER,
                                     OrthancPluginLookupStudyWithAccessionNumber);
          break;

        case QueryLevel_Series:
          FilterByIdentifierInternal(matcher, OrthancPlugins::DICOM_TAG_SERIES_INSTANCE_UID,
                                     OrthancPluginLookupSeries);
          break;

        case QueryLevel_Instance:
          FilterByIdentifierInternal(matcher, OrthancPlugins::DICOM_TAG_SOP_INSTANCE_UID,
                                     OrthancPluginLookupInstance);
          break;

        default:
          throw std::runtime_error("Internal error");
      }
    }


    void Flatten(std::list<std::string>& result) const
    {
      std::string instance;

      result.clear();

      if (all_)
      {
        std::string uri;
        switch (level_)
        {
          case QueryLevel_Study:
            uri = "/studies/";
            break;

          case QueryLevel_Series:
            uri = "/series/";
            break;

          case QueryLevel_Instance:
            uri = "/instances/";
            break;

          default:
            throw std::runtime_error("Internal error");
        }

        Json::Value tmp;
        if (OrthancPlugins::RestApiGetJson(tmp, context_, uri) &&
            tmp.type() == Json::arrayValue)
        {
          for (Json::Value::ArrayIndex i = 0; i < tmp.size(); i++)
          {
            if (PickOneInstance(instance, tmp[i].asString()))
            {
              result.push_back(instance);
            }
          }
        }
      }
      else
      {
        for (Resources::const_iterator 
               it = resources_.begin(); it != resources_.end(); it++)
        {
          if (PickOneInstance(instance, *it))
          {
            result.push_back(instance);
          }
        }
      }
    }
  };
}




static void ApplyMatcher(OrthancPluginRestOutput* output,
                         const OrthancPluginHttpRequest* request,
                         const ModuleMatcher& matcher,
                         const CandidateResources& candidates,
                         QueryLevel level)
{
  std::list<std::string> resources;
  candidates.Flatten(resources);

  std::string wadoBase = OrthancPlugins::Configuration::GetBaseUrl(configuration_, request);

  OrthancPlugins::DicomResults results(context_, output, wadoBase, *dictionary_, IsXmlExpected(request), true);

  for (std::list<std::string>::const_iterator
         it = resources.begin(); it != resources.end(); it++)
  {
    std::string file;
    if (OrthancPlugins::RestApiGetString(file, context_, "/instances/" + *it + "/file"))
    {
      OrthancPlugins::ParsedDicomFile dicom(file);
      if (matcher.Matches(dicom))
      {
        std::auto_ptr<gdcm::DataSet> result(new gdcm::DataSet);
        matcher.ExtractFields(*result, dicom, wadoBase, level);
        results.Add(dicom.GetFile(), *result);
      }
    }
  }

  results.Answer();
}



int32_t SearchForStudies(OrthancPluginRestOutput* output,
                         const char* url,
                         const OrthancPluginHttpRequest* request)
{
  try
  {
    if (request->method != OrthancPluginHttpMethod_Get)
    {
      OrthancPluginSendMethodNotAllowed(context_, output, "GET");
      return 0;
    }

    ModuleMatcher matcher(request);

    CandidateResources candidates;
    candidates.FilterByIdentifier(matcher);

    ApplyMatcher(output, request, matcher, candidates, QueryLevel_Study);

    return 0;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
  catch (boost::bad_lexical_cast& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}


int32_t SearchForSeries(OrthancPluginRestOutput* output,
                        const char* url,
                        const OrthancPluginHttpRequest* request)
{
  try
  {
    if (request->method != OrthancPluginHttpMethod_Get)
    {
      OrthancPluginSendMethodNotAllowed(context_, output, "GET");
      return 0;
    }

    ModuleMatcher matcher(request);

    if (request->groupsCount == 1)
    {
      // The "StudyInstanceUID" is provided by the regular expression
      matcher.AddFilter(OrthancPlugins::DICOM_TAG_STUDY_INSTANCE_UID, request->groups[0]);
    }

    CandidateResources candidates;
    candidates.FilterByIdentifier(matcher);
    candidates.GoDown();
    candidates.FilterByIdentifier(matcher);

    ApplyMatcher(output, request, matcher, candidates, QueryLevel_Series);

    return 0;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
  catch (boost::bad_lexical_cast& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}


int32_t SearchForInstances(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request)
{
  try
  {
    if (request->method != OrthancPluginHttpMethod_Get)
    {
      OrthancPluginSendMethodNotAllowed(context_, output, "GET");
      return 0;
    }

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

    CandidateResources candidates;
    candidates.FilterByIdentifier(matcher);
    candidates.GoDown();
    candidates.FilterByIdentifier(matcher);
    candidates.GoDown();
    candidates.FilterByIdentifier(matcher);

    ApplyMatcher(output, request, matcher, candidates, QueryLevel_Instance);

    return 0;
  }
  catch (std::runtime_error& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
  catch (boost::bad_lexical_cast& e)
  {
    OrthancPluginLogError(context_, e.what());
    return -1;
  }
}
