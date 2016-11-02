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

#include "Plugin.h"
#include "DicomWebServers.h"
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


  static const boost::regex MULTIPART_HEADERS_ENDING("(.*?\r\n)\r\n(.*)");
  static const boost::regex MULTIPART_HEADERS_LINE(".*?\r\n");

  static void ParseMultipartHeaders(bool& hasLength /* out */,
                                    size_t& length /* out */,
                                    std::string& contentType /* out */,
                                    OrthancPluginContext* context,
                                    const char* startHeaders,
                                    const char* endHeaders)
  {
    hasLength = false;
    contentType = "application/octet-stream";

    // Loop over the HTTP headers of this multipart item
    boost::cregex_token_iterator it(startHeaders, endHeaders, MULTIPART_HEADERS_LINE, 0);
    boost::cregex_token_iterator iteratorEnd;

    for (; it != iteratorEnd; ++it)
    {
      const std::string line(*it);
      size_t colon = line.find(':');
      size_t eol = line.find('\r');

      if (colon != std::string::npos &&
          eol != std::string::npos &&
          colon < eol &&
          eol + 2 == line.length())
      {
        std::string key = Orthanc::Toolbox::StripSpaces(line.substr(0, colon));
        Orthanc::Toolbox::ToLowerCase(key);

        const std::string value = Orthanc::Toolbox::StripSpaces(line.substr(colon + 1, eol - colon - 1));

        if (key == "content-length")
        {
          try
          {
            int tmp = boost::lexical_cast<int>(value);
            if (tmp >= 0)
            {
              hasLength = true;
              length = tmp;
            }
          }
          catch (boost::bad_lexical_cast&)
          {
            OrthancPluginLogWarning(context, "Unable to parse the Content-Length of a multipart item");
          }
        }
        else if (key == "content-type")
        {
          contentType = value;
        }
      }
    }
  }


  static const char* ParseMultipartItem(std::vector<MultipartItem>& result,
                                        OrthancPluginContext* context,
                                        const char* start,
                                        const char* end,
                                        const boost::regex& nextSeparator)
  {
    // Just before "start", it is guaranteed that "--[BOUNDARY]\r\n" is present

    boost::cmatch what;
    if (!boost::regex_match(start, end, what, MULTIPART_HEADERS_ENDING, boost::match_perl))
    {
      // Cannot find the HTTP headers of this multipart item
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
    }

    // Some aliases for more clarity
    assert(what[1].first == start);
    const char* startHeaders = what[1].first;
    const char* endHeaders = what[1].second;
    const char* startBody = what[2].first;

    bool hasLength;
    size_t length;
    std::string contentType;
    ParseMultipartHeaders(hasLength, length, contentType, context, startHeaders, endHeaders);

    boost::cmatch separator;

    if (hasLength)
    {
      if (!boost::regex_match(startBody + length, end, separator, nextSeparator, boost::match_perl) ||
          startBody + length != separator[1].first)
      {
        // Cannot find the separator after skipping the "Content-Length" bytes
        throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
      }
    }
    else
    {
      if (!boost::regex_match(startBody, end, separator, nextSeparator, boost::match_perl))
      {
        // No more occurrence of the boundary separator
        throw OrthancPlugins::PluginException(OrthancPluginErrorCode_NetworkProtocol);
      }
    }

    MultipartItem item;
    item.data_ = startBody;
    item.size_ = separator[1].first - startBody;
    item.contentType_ = contentType;
    result.push_back(item);

    return separator[1].second;  // Return the end of the separator
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

    // Look for the first boundary separator in the body (note the "?"
    // to request non-greedy search)
    const boost::regex firstSeparator1("--" + boundary + "(--|\r\n).*");
    const boost::regex firstSeparator2(".*?\r\n--" + boundary + "(--|\r\n).*");

    // Look for the next boundary separator in the body (note the "?"
    // to request non-greedy search)
    const boost::regex nextSeparator(".*?(\r\n--" + boundary + ").*");

    const char* end = body + bodySize;

    boost::cmatch what;
    if (boost::regex_match(body, end, what, firstSeparator1, boost::match_perl | boost::match_single_line) ||
        boost::regex_match(body, end, what, firstSeparator2, boost::match_perl | boost::match_single_line))
    {
      const char* current = what[1].first;

      while (current != NULL &&
             current + 2 < end)
      {
        if (current[0] != '\r' ||
            current[1] != '\n')
        {
          // We reached a separator with a trailing "--", which
          // means that reading the multipart body is done
          break;
        }
        else
        {
          current = ParseMultipartItem(result, context, current + 2, end, nextSeparator);
        }
      }
    }
  }


  void ParseAssociativeArray(std::map<std::string, std::string>& target,
                             const Json::Value& value,
                             const std::string& key)
  {
    if (value.type() != Json::objectValue)
    {
      OrthancPlugins::Configuration::LogError("This is not a JSON object");
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
    }

    if (!value.isMember(key))
    {
      return;
    }

    const Json::Value& tmp = value[key];

    if (tmp.type() != Json::objectValue)
    {
      OrthancPlugins::Configuration::LogError("The field \"" + key + "\" of a JSON object is "
                                              "not a JSON associative array as expected");
      throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
    }

    Json::Value::Members names = tmp.getMemberNames();

    for (size_t i = 0; i < names.size(); i++)
    {
      if (tmp[names[i]].type() != Json::stringValue)
      {
        OrthancPlugins::Configuration::LogError("Some value in the associative array \"" + key + 
                                                "\" is not a string as expected");
        throw OrthancPlugins::PluginException(OrthancPluginErrorCode_BadFileFormat);
      }
      else
      {
        target[names[i]] = tmp[names[i]].asString();
      }
    }
  }


  namespace Configuration
  {
    // Assume Latin-1 encoding by default (as in the Orthanc core)
    static Orthanc::Encoding defaultEncoding_ = Orthanc::Encoding_Latin1;
    static OrthancConfiguration configuration_;


    void Initialize(OrthancPluginContext* context)
    {      
      OrthancPlugins::OrthancConfiguration global(context);
      global.GetSection(configuration_, "DicomWeb");

      std::string s;
      if (global.LookupStringValue(s, "DefaultEncoding"))
      {
        defaultEncoding_ = Orthanc::StringToEncoding(s.c_str());
      }

      OrthancPlugins::OrthancConfiguration servers;
      configuration_.GetSection(servers, "Servers");
      OrthancPlugins::DicomWebServers::GetInstance().Load(servers.GetJson());
    }


    OrthancPluginContext* GetContext()
    {
      return configuration_.GetContext();
    }


    std::string GetStringValue(const std::string& key,
                               const std::string& defaultValue)
    {
      return configuration_.GetStringValue(key, defaultValue);
    }


    bool GetBooleanValue(const std::string& key,
                         bool defaultValue)
    {
      return configuration_.GetBooleanValue(key, defaultValue);
    }


    unsigned int GetUnsignedIntegerValue(const std::string& key,
                                         unsigned int defaultValue)
    {
      return configuration_.GetUnsignedIntegerValue(key, defaultValue);
    }


    std::string GetRoot()
    {
      std::string root = configuration_.GetStringValue("Root", "/dicom-web/");

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


    std::string GetWadoRoot()
    {
      std::string root = configuration_.GetStringValue("WadoRoot", "/wado/");

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


    std::string  GetBaseUrl(const OrthancPluginHttpRequest* request)
    {
      std::string host = configuration_.GetStringValue("Host", "");
      bool ssl = configuration_.GetBooleanValue("Ssl", false);

      if (host.empty() &&
          !LookupHttpHeader(host, request, "host"))
      {
        // Should never happen: The "host" header should always be present
        // in HTTP requests. Provide a default value anyway.
        host = "localhost:8042";
      }

      return (ssl ? "https://" : "http://") + host + GetRoot();
    }


    std::string GetWadoUrl(const std::string& wadoBase,
                           const std::string& studyInstanceUid,
                           const std::string& seriesInstanceUid,
                           const std::string& sopInstanceUid)
    {
      if (studyInstanceUid.empty() ||
          seriesInstanceUid.empty() ||
          sopInstanceUid.empty())
      {
        return "";
      }
      else
      {
        return (wadoBase + 
                "studies/" + studyInstanceUid + 
                "/series/" + seriesInstanceUid + 
                "/instances/" + sopInstanceUid + "/");
      }
    }


    void LogError(const std::string& message)
    {
      OrthancPluginLogError(GetContext(), message.c_str());
    }


    void LogWarning(const std::string& message)
    {
      OrthancPluginLogWarning(GetContext(), message.c_str());
    }


    void LogInfo(const std::string& message)
    {
      OrthancPluginLogInfo(GetContext(), message.c_str());
    }


    Orthanc::Encoding GetDefaultEncoding()
    {
      return defaultEncoding_;
    }
  }
}
