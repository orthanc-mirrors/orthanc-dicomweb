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


#include "Configuration.h"

#include <fstream>
#include <json/reader.h>
#include <boost/regex.hpp>
#include <boost/lexical_cast.hpp>

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
                          OrthancPluginContext* context,
                          const char* body,
                          const uint64_t bodySize,
                          const std::string& boundary)
  {
    // Reference:
    // https://www.w3.org/Protocols/rfc1341/7_2_Multipart.html

    result.clear();

    const boost::regex separator("(^|\r\n)--" + boundary + "(--|\r\n)");
    const boost::regex encapsulation("(.*)\r\n\r\n(.*)");
 
    std::vector< std::pair<const char*, const char*> > parts;
    
    const char* start = body;
    const char* end = body + bodySize;

    boost::cmatch what;
    boost::match_flag_type flags = boost::match_perl | boost::match_single_line;
    while (boost::regex_search(start, end, what, separator, flags))   
    {
      if (start != body)  // Ignore the first separator
      {
        parts.push_back(std::make_pair(start, what[0].first));
      }

      if (*what[2].first == '-')
      {
        // This is the last separator (there is a trailing "--")
        break;
      }

      start = what[0].second;
      flags |= boost::match_prev_avail;
    }

    for (size_t i = 0; i < parts.size(); i++)
    {
      if (boost::regex_match(parts[i].first, parts[i].second, what, encapsulation, boost::match_perl))
      {
        size_t dicomSize = what[2].second - what[2].first;

        std::string contentType = "application/octet-stream";
        std::vector<std::string> headers;

        {
          std::string tmp;
          tmp.assign(what[1].first, what[1].second);
          Orthanc::Toolbox::TokenizeString(headers, tmp, '\n');
        }

        bool valid = true;

        for (size_t j = 0; j < headers.size(); j++)
        {
          std::vector<std::string> tokens;
          Orthanc::Toolbox::TokenizeString(tokens, headers[j], ':');

          if (tokens.size() == 2)
          {
            std::string key = Orthanc::Toolbox::StripSpaces(tokens[0]);
            std::string value = Orthanc::Toolbox::StripSpaces(tokens[1]);
            Orthanc::Toolbox::ToLowerCase(key);

            if (key == "content-type")
            {
              contentType = value;
            }
            else if (key == "content-length")
            {
              try
              {
                size_t s = boost::lexical_cast<size_t>(value);
                if (s != dicomSize)
                {
                  valid = false;
                }
              }
              catch (boost::bad_lexical_cast&)
              {
                valid = false;
              }
            }
          }
        }

        if (valid)
        {
          MultipartItem item;
          item.data_ = what[2].first;
          item.size_ = dicomSize;
          item.contentType_ = contentType;
          result.push_back(item);          
        }
        else
        {
          OrthancPluginLogWarning(context, "Ignoring a badly-formatted item in a multipart body");
        }
      }      
    }
  }


  bool RestApiGetString(std::string& result,
                        OrthancPluginContext* context,
                        const std::string& uri,
                        bool applyPlugins)
  {
    OrthancPluginMemoryBuffer buffer;
    OrthancPluginErrorCode code;

    if (applyPlugins)
    {
      code = OrthancPluginRestApiGetAfterPlugins(context, &buffer, uri.c_str());
    }
    else
    {
      code = OrthancPluginRestApiGet(context, &buffer, uri.c_str());
    }

    if (code != OrthancPluginErrorCode_Success)
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
                      const std::string& uri,
                      bool applyPlugins)
  {
    std::string content;
    if (!RestApiGetString(content, context, uri, applyPlugins))
    {
      return false;
    }
    else
    {
      Json::Reader reader;
      return reader.parse(content, result);
    }
  }


  bool RestApiPostString(std::string& result,
                         OrthancPluginContext* context,
                         const std::string& uri,
                         const std::string& body)
  {
    OrthancPluginMemoryBuffer buffer;
    OrthancPluginErrorCode code = OrthancPluginRestApiPost(context, &buffer, uri.c_str(), body.c_str(), body.size());

    if (code != OrthancPluginErrorCode_Success)
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


  bool RestApiPostJson(Json::Value& result,
                       OrthancPluginContext* context,
                       const std::string& uri,
                       const std::string& body)
  {
    std::string content;
    if (!RestApiPostString(content, context, uri, body))
    {
      return false;
    }
    else
    {
      Json::Reader reader;
      return reader.parse(content, result);
    }
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
      std::string root = GetStringValue(configuration, "Root", "/dicom-web/");

      // Make sure the root URI starts and ends with a slash
      if (root.size() == 0 ||
          root[0] != '/')
      {
        root = "/" + root;
      }
    
      if (root[root.length() - 1] != '/')
      {
        root += "/";
      }

      return root;
    }


    std::string GetWadoRoot(const Json::Value& configuration)
    {
      std::string root = GetStringValue(configuration, "WadoRoot", "/wado/");

      // Make sure the root URI starts with a slash
      if (root.size() == 0 ||
          root[0] != '/')
      {
        root = "/" + root;
      }

      // Remove the trailing slash, if any
      if (root[root.length() - 1] == '/')
      {
        root = root.substr(0, root.length() - 1);
      }

      return root;
    }


    std::string  GetBaseUrl(const Json::Value& configuration,
                            const OrthancPluginHttpRequest* request)
    {
      std::string host = GetStringValue(configuration, "Host", "");
      bool ssl = GetBoolValue(configuration, "Ssl", false);

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
