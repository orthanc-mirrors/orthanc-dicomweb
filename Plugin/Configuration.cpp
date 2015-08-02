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


#include "Configuration.h"

#include <fstream>
#include <json/reader.h>
#include <boost/regex.hpp>

#include "../Orthanc/Core/Toolbox.h"

namespace OrthancPlugins
{
  bool LookupHttpHeader(std::string& value,
                        const OrthancPluginHttpRequest* request,
                        const std::string& header)
  {
    value.clear();

    for (uint32_t i = 0; i < request->headersCount; i++)
    {
      std::string s = request->headersKeys[i];
      Orthanc::Toolbox::ToLowerCase(s);
      if (s == header)
      {
        value = request->headersValues[i];
        return true;
      }
    }

    return false;
  }


  void ParseContentType(std::string& application,
                        std::map<std::string, std::string>& attributes,
                        const std::string& header)
  {
    application.clear();
    attributes.clear();

    std::vector<std::string> tokens;
    Orthanc::Toolbox::TokenizeString(tokens, header, ';');

    assert(tokens.size() > 0);
    application = tokens[0];
    Orthanc::Toolbox::StripSpaces(application);
    Orthanc::Toolbox::ToLowerCase(application);

    boost::regex pattern("\\s*([^=]+)\\s*=\\s*([^=]+)\\s*");

    for (size_t i = 1; i < tokens.size(); i++)
    {
      boost::cmatch what;
      if (boost::regex_match(tokens[i].c_str(), what, pattern))
      {
        std::string key(what[1]);
        std::string value(what[2]);
        Orthanc::Toolbox::ToLowerCase(key);
        attributes[key] = value;
      }
    }
  }


  void ParseMultipartBody(std::vector<MultipartItem>& result,
                          const char* body,
                          const uint64_t bodySize,
                          const std::string& boundary)
  {
    result.clear();

    boost::regex header("\r?(\n?)--" + boundary + "(--|.*\r?\n\r?\n)");
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


  namespace Configuration
  {
    bool Read(Json::Value& configuration,
              OrthancPluginContext* context)
    {
      std::string s;

      {
        char* tmp = OrthancPluginGetConfiguration(context);
        if (tmp == NULL)
        {
          OrthancPluginLogError(context, "Error while retrieving the configuration from Orthanc");
          return false;
        }

        s.assign(tmp);
        OrthancPluginFreeString(context, tmp);      
      }

      Json::Reader reader;
      if (reader.parse(s, configuration))
      {
        return true;
      }
      else
      {
        OrthancPluginLogError(context, "Unable to parse the configuration");
        return false;
      }
    }


    std::string GetStringValue(const Json::Value& configuration,
                               const std::string& key,
                               const std::string& defaultValue)
    {
      if (configuration.type() != Json::objectValue ||
          !configuration.isMember(key) ||
          configuration[key].type() != Json::stringValue)
      {
        return defaultValue;
      }
      else
      {
        return configuration[key].asString();
      }
    }


    bool GetBoolValue(const Json::Value& configuration,
                      const std::string& key,
                      bool defaultValue)
    {
      if (configuration.type() != Json::objectValue ||
          !configuration.isMember(key) ||
          configuration[key].type() != Json::booleanValue)
      {
        return defaultValue;
      }
      else
      {
        return configuration[key].asBool();
      }
    }


    std::string GetRoot(const Json::Value& configuration)
    {
      std::string root;

      if (configuration.isMember("DicomWeb"))
      {
        root = GetStringValue(configuration["DicomWeb"], "Root", "");
      }

      if (root.empty())
      {
        root = "/dicom-web/";
      }

      // Make sure the root URI starts and ends with a slash
      if (root[0] != '/')
      {
        root = "/" + root;
      }
    
      if (root[root.length() - 1] != '/')
      {
        root += "/";
      }

      return root;
    }


    std::string  GetBaseUrl(const Json::Value& configuration,
                            const OrthancPluginHttpRequest* request)
    {
      std::string host;
      bool ssl = false;

      if (configuration.isMember("DicomWeb"))
      {
        host = GetStringValue(configuration["DicomWeb"], "Host", "");
        ssl = GetBoolValue(configuration["DicomWeb"], "Ssl", false);
      }

      if (host.empty() &&
          !LookupHttpHeader(host, request, "host"))
      {
        // Should never happen: The "host" header should always be present
        // in HTTP requests. Provide a default value anyway.
        host = "localhost:8042";
      }

      return (ssl ? "https://" : "http://") + host + GetRoot(configuration);
    }
  }
}
