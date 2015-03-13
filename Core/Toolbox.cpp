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


#include "Toolbox.h"

#include <string>
#include <algorithm>
#include <boost/regex.hpp>
#include <json/reader.h>


namespace OrthancPlugins
{
  void ToLowerCase(std::string& s)
  {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  }


  void ToUpperCase(std::string& s)
  {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
  }


  std::string StripSpaces(const std::string& source)
  {
    size_t first = 0;

    while (first < source.length() &&
           isspace(source[first]))
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

  

  void TokenizeString(std::vector<std::string>& result,
                      const std::string& value,
                      char separator)
  {
    result.clear();

    std::string currentItem;

    for (size_t i = 0; i < value.size(); i++)
    {
      if (value[i] == separator)
      {
        result.push_back(currentItem);
        currentItem.clear();
      }
      else
      {
        currentItem.push_back(value[i]);
      }
    }

    result.push_back(currentItem);
  }



  void ParseContentType(std::string& application,
                        std::map<std::string, std::string>& attributes,
                        const std::string& header)
  {
    application.clear();
    attributes.clear();

    std::vector<std::string> tokens;
    TokenizeString(tokens, header, ';');

    assert(tokens.size() > 0);
    application = tokens[0];
    StripSpaces(application);
    ToLowerCase(application);

    boost::regex pattern("\\s*([^=]+)\\s*=\\s*([^=]+)\\s*");

    for (size_t i = 1; i < tokens.size(); i++)
    {
      boost::cmatch what;
      if (boost::regex_match(tokens[i].c_str(), what, pattern))
      {
        std::string key(what[1]);
        std::string value(what[2]);
        ToLowerCase(key);
        attributes[key] = value;
      }
    }
  }


  bool LookupHttpHeader(std::string& value,
                        const OrthancPluginHttpRequest* request,
                        const std::string& header)
  {
    value.clear();

    for (uint32_t i = 0; i < request->headersCount; i++)
    {
      std::string s = request->headersKeys[i];
      ToLowerCase(s);
      if (s == header)
      {
        value = request->headersValues[i];
        return true;
      }
    }

    return false;
  }



  void ParseMultipartBody(std::vector<MultipartItem>& result,
                          const char* body,
                          const uint64_t bodySize,
                          const std::string& boundary)
  {
    result.clear();

    boost::regex header("(\n?)--" + boundary + "(--|.*\n\n)");
    boost::regex pattern(".*^Content-Type\\s*:\\s*([^\\s]*).*",
                         boost::regex::icase /* case insensitive */);
    
    boost::cmatch what;
    boost::match_flag_type flags = (boost::match_perl | 
                                    boost::match_not_dot_null);
    const char* start = body;
    const char* end = body + bodySize;
    std::string currentType;

    while (boost::regex_search(start, end, what, header, flags))   
    {
      if (start != body)
      {
        MultipartItem item;
        item.data_ = start;
        item.size_ = what[0].first - start;
        item.contentType_ = currentType;

        result.push_back(item);
      }

      boost::cmatch contentType;
      if (boost::regex_match(what[0].first, what[0].second, contentType, pattern))
      {
        currentType = contentType[1];
      }
      else
      {
        currentType.clear();
      }
    
      start = what[0].second;
      flags |= boost::match_prev_avail;
    }
  }


  bool RestApiGetString(std::string& result,
                        OrthancPluginContext* context,
                        const std::string& uri)
  {
    OrthancPluginMemoryBuffer buffer;
    int code = OrthancPluginRestApiGet(context, &buffer, uri.c_str());
    if (code)
    {
      // Error
      return false;
    }

    bool ok = true;

    try
    {
      if (buffer.size)
      {
        result.assign(reinterpret_cast<const char*>(buffer.data), buffer.size);
      }
      else
      {
        result.clear();
      }
    }
    catch (std::bad_alloc&)
    {
      ok = false;
    }

    OrthancPluginFreeMemoryBuffer(context, &buffer);

    return ok;
  }


  bool RestApiGetJson(Json::Value& result,
                      OrthancPluginContext* context,
                      const std::string& uri)
  {
    std::string content;
    RestApiGetString(content, context, uri);
    
    Json::Reader reader;
    return reader.parse(content, result);
  }
}
