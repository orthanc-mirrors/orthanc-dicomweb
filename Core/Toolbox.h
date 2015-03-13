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
#include <string>
#include <vector>
#include <json/value.h>
#include <map>

namespace OrthancPlugins
{
  struct MultipartItem
  {
    const char*   data_;
    size_t        size_;
    std::string   contentType_;
  };

  void ToLowerCase(std::string& s);

  void ToUpperCase(std::string& s);

  std::string StripSpaces(const std::string& source);

  void TokenizeString(std::vector<std::string>& result,
                      const std::string& source,
                      char separator);

  void ParseContentType(std::string& application,
                        std::map<std::string, std::string>& attributes,
                        const std::string& header);

  bool LookupHttpHeader(std::string& value,
                        const OrthancPluginHttpRequest* request,
                        const std::string& header);

  void ParseMultipartBody(std::vector<MultipartItem>& result,
                          const char* body,
                          const uint64_t bodySize,
                          const std::string& boundary);

  bool RestApiGetString(std::string& result,
                        OrthancPluginContext* context,
                        const std::string& uri);

  bool RestApiGetJson(Json::Value& result,
                      OrthancPluginContext* context,
                      const std::string& uri);
}
