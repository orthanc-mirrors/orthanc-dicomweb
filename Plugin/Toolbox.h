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

  // http://www.dabsoft.ch/dicom/3/C.12.1.1.2/
  enum Encoding
  {
    Encoding_Unknown,
    Encoding_Ascii,
    Encoding_Utf8,
    Encoding_Latin1,
    Encoding_Latin2,
    Encoding_Latin3,
    Encoding_Latin4,
    Encoding_Latin5,                        // Turkish
    Encoding_Cyrillic,
    Encoding_Arabic,
    Encoding_Greek,
    Encoding_Hebrew,
    Encoding_Thai,                          // TIS 620-2533
    Encoding_Japanese,                      // JIS X 0201 (Shift JIS): Katakana
    Encoding_Chinese                        // GB18030 - Chinese simplified
    //Encoding_JapaneseKanji,               // Multibyte - JIS X 0208: Kanji
    //Encoding_JapaneseSupplementaryKanji,  // Multibyte - JIS X 0212: Supplementary Kanji set
    //Encoding_Korean,                      // Multibyte - KS X 1001: Hangul and Hanja
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

  std::string ConvertToAscii(const std::string& source);

  std::string ConvertToUtf8(const std::string& source,
                            const Encoding sourceEncoding);

  Encoding GetDicomEncoding(const char* specificCharacterSet);
}
