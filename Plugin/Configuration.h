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

#if (ORTHANC_PLUGINS_MINIMAL_MINOR_NUMBER >= 9 && ORTHANC_PLUGINS_MINIMAL_REVISION_NUMBER >= 5)
#  define REST_RETURN_TYPE     OrthancPluginErrorCode
#  define REST_RETURN_SUCCESS  OrthancPluginErrorCode_Success
#  define REST_RETURN_FAILURE  OrthancPluginErrorCode_Plugin
#else
#  define REST_RETURN_TYPE     int32_t
#  define REST_RETURN_SUCCESS  0
#  define REST_RETURN_FAILURE  -1
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
                      const std::string& uri);

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
