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

#include "Toolbox.h"

#include <fstream>
#include <json/reader.h>

namespace OrthancPlugins
{
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
