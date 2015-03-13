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
      std::string path;

      {
        char* pathTmp = OrthancPluginGetConfigurationPath(context);
        if (pathTmp == NULL)
        {
          OrthancPluginLogError(context, "No configuration file is provided");
          return false;
        }

        path = std::string(pathTmp);

        OrthancPluginFreeString(context, pathTmp);
      }

      std::ifstream f(path.c_str());

      Json::Reader reader;
      if (!reader.parse(f, configuration) ||
          configuration.type() != Json::objectValue)
      {
        std::string s = "Unable to parse the configuration file: " + std::string(path);
        OrthancPluginLogError(context, s.c_str());
        return false;
      }

      return true;
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


    std::string  GetBaseUrl(const Json::Value& configuration,
                            const OrthancPluginHttpRequest* request)
    {
      std::string host;

      if (configuration.isMember("DicomWeb") &&
          configuration["DicomWeb"].type() == Json::objectValue)
      {
        host = GetStringValue(configuration["DicomWeb"], "Host", "");
        if (!host.empty())
        {
          return host;
        }
      }

      if (LookupHttpHeader(host, request, "host"))
      {
        return "http://" + host;
      }

      // Should never happen: The "host" header should always be present
      // in HTTP requests. Provide a default value anyway.
      return "http://localhost:8042/";
    }
  }
}
