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


#pragma once

#include <orthanc/OrthancCPlugin.h>
#include <json/value.h>

#if (ORTHANC_PLUGINS_MINIMAL_MAJOR_NUMBER <= 0 && \
     ORTHANC_PLUGINS_MINIMAL_MINOR_NUMBER <= 9 && \
     ORTHANC_PLUGINS_MINIMAL_REVISION_NUMBER <= 6)
#  define HAS_SEND_MULTIPART_ITEM_2   0
#else
#  define HAS_SEND_MULTIPART_ITEM_2   1
#endif

namespace OrthancPlugins
{
  struct MultipartItem
  {
    const char*   data_;
    size_t        size_;
    std::string   contentType_;
  };

  bool LookupHttpHeader(std::string& value,
                        const OrthancPluginHttpRequest* request,
                        const std::string& header);

  void ParseContentType(std::string& application,
                        std::map<std::string, std::string>& attributes,
                        const std::string& header);

  void ParseMultipartBody(std::vector<MultipartItem>& result,
                          const char* body,
                          const uint64_t bodySize,
                          const std::string& boundary);

  bool RestApiGetString(std::string& result,
                        OrthancPluginContext* context,
                        const std::string& uri,
                        bool applyPlugins = false);

  bool RestApiGetJson(Json::Value& result,
                      OrthancPluginContext* context,
                      const std::string& uri,
                      bool applyPlugins = false);

  bool RestApiPostString(std::string& result,
                         OrthancPluginContext* context,
                         const std::string& uri,
                         const std::string& body);

  bool RestApiPostJson(Json::Value& result,
                       OrthancPluginContext* context,
                       const std::string& uri,
                       const std::string& body);

  namespace Configuration
  {
    bool Read(Json::Value& configuration,
              OrthancPluginContext* context);

    std::string GetStringValue(const Json::Value& configuration,
                               const std::string& key,
                               const std::string& defaultValue);
    
    bool GetBoolValue(const Json::Value& configuration,
                      const std::string& key,
                      bool defaultValue);

    std::string GetRoot(const Json::Value& configuration);

    std::string GetWadoRoot(const Json::Value& configuration);
      
    std::string GetBaseUrl(const Json::Value& configuration,
                           const OrthancPluginHttpRequest* request);
  }
}
