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
#include <boost/locale.hpp>
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


  std::string ConvertToAscii(const std::string& source)
  {
    std::string result;

    result.reserve(source.size() + 1);
    for (size_t i = 0; i < source.size(); i++)
    {
      if (source[i] < 128 && source[i] >= 0 && !iscntrl(source[i]))
      {
        result.push_back(source[i]);
      }
    }

    return result;
  }

  
  std::string ConvertToUtf8(const std::string& source,
                            const Encoding sourceEncoding)
  {
    const char* encoding;

    // http://bradleyross.users.sourceforge.net/docs/dicom/doc/src-html/org/dcm4che2/data/SpecificCharacterSet.html
    switch (sourceEncoding)
    {
      case Encoding_Utf8:
        // Already in UTF-8: No conversion is required
        return source;

      case Encoding_Unknown:
      case Encoding_Ascii:
        return ConvertToAscii(source);

      case Encoding_Latin1:
        encoding = "ISO-8859-1";
        break;

      case Encoding_Latin2:
        encoding = "ISO-8859-2";
        break;

      case Encoding_Latin3:
        encoding = "ISO-8859-3";
        break;

      case Encoding_Latin4:
        encoding = "ISO-8859-4";
        break;

      case Encoding_Latin5:
        encoding = "ISO-8859-9";
        break;

      case Encoding_Cyrillic:
        encoding = "ISO-8859-5";
        break;

      case Encoding_Arabic:
        encoding = "ISO-8859-6";
        break;

      case Encoding_Greek:
        encoding = "ISO-8859-7";
        break;

      case Encoding_Hebrew:
        encoding = "ISO-8859-8";
        break;
        
      case Encoding_Japanese:
        encoding = "SHIFT-JIS";
        break;

      case Encoding_Chinese:
        encoding = "GB18030";
        break;

      case Encoding_Thai:
        encoding = "TIS620.2533-0";
        break;

      default:
        throw std::runtime_error("Unsupported encoding");
    }

    try
    {
      return boost::locale::conv::to_utf<char>(source, encoding);
    }
    catch (std::runtime_error&)
    {
      // Bad input string or bad encoding
      return ConvertToAscii(source);
    }
  }


  Encoding GetDicomEncoding(const char* specificCharacterSet)
  {
    std::string s = specificCharacterSet;
    ToUpperCase(s);

    // http://www.dabsoft.ch/dicom/3/C.12.1.1.2/
    // https://github.com/dcm4che/dcm4che/blob/master/dcm4che-core/src/main/java/org/dcm4che3/data/SpecificCharacterSet.java
    if (s == "ISO_IR 6" ||
        s == "ISO_IR 192" ||
        s == "ISO 2022 IR 6")
    {
      return Encoding_Utf8;
    }
    else if (s == "ISO_IR 100" ||
             s == "ISO 2022 IR 100")
    {
      return Encoding_Latin1;
    }
    else if (s == "ISO_IR 101" ||
             s == "ISO 2022 IR 101")
    {
      return Encoding_Latin2;
    }
    else if (s == "ISO_IR 109" ||
             s == "ISO 2022 IR 109")
    {
      return Encoding_Latin3;
    }
    else if (s == "ISO_IR 110" ||
             s == "ISO 2022 IR 110")
    {
      return Encoding_Latin4;
    }
    else if (s == "ISO_IR 148" ||
             s == "ISO 2022 IR 148")
    {
      return Encoding_Latin5;
    }
    else if (s == "ISO_IR 144" ||
             s == "ISO 2022 IR 144")
    {
      return Encoding_Cyrillic;
    }
    else if (s == "ISO_IR 127" ||
             s == "ISO 2022 IR 127")
    {
      return Encoding_Arabic;
    }
    else if (s == "ISO_IR 126" ||
             s == "ISO 2022 IR 126")
    {
      return Encoding_Greek;
    }
    else if (s == "ISO_IR 138" ||
             s == "ISO 2022 IR 138")
    {
      return Encoding_Hebrew;
    }
    else if (s == "ISO_IR 166" || s == "ISO 2022 IR 166")
    {
      return Encoding_Thai;
    }
    else if (s == "ISO_IR 13" || s == "ISO 2022 IR 13")
    {
      return Encoding_Japanese;
    }
    else if (s == "GB18030")
    {
      return Encoding_Chinese;
    }
    /*
      else if (s == "ISO 2022 IR 149")
      {
      TODO
      }
      else if (s == "ISO 2022 IR 159")
      {
      TODO
      }
      else if (s == "ISO 2022 IR 87")
      {
      TODO
      }
    */
    else
    {
      return Encoding_Unknown;
    }
  }
}
