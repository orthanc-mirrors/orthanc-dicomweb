/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * In addition, as a special exception, the copyright holders of this
 * program give permission to link the code of its release with the
 * OpenSSL project's "OpenSSL" library (or with modified versions of it
 * that use the same license as the "OpenSSL" library), and distribute
 * the linked executables. You must obey the GNU General Public License
 * in all respects for all of the code used other than "OpenSSL". If you
 * modify file(s) with this exception, you may extend this exception to
 * your version of the file(s), but you are not obligated to do so. If
 * you do not wish to do so, delete this exception statement from your
 * version. If you delete this exception statement from all source files
 * in the program, then also delete it here.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/


#include "OrthancPluginCppWrapper.h"

#include <json/reader.h>


namespace OrthancPlugins
{
  const char* PluginException::GetErrorDescription(OrthancPluginContext* context) const
  {
    const char* description = OrthancPluginGetErrorDescription(context, code_);
    if (description)
    {
      return description;
    }
    else
    {
      return "No description available";
    }
  }


  MemoryBuffer::MemoryBuffer(OrthancPluginContext* context) : 
    context_(context)
  {
    buffer_.data = NULL;
    buffer_.size = 0;
  }


  void MemoryBuffer::Clear()
  {
    if (buffer_.data != NULL)
    {
      OrthancPluginFreeMemoryBuffer(context_, &buffer_);
      buffer_.data = NULL;
      buffer_.size = 0;
    }
  }


  void MemoryBuffer::ToString(std::string& target) const
  {
    if (buffer_.size == 0)
    {
      target.clear();
    }
    else
    {
      target.assign(reinterpret_cast<const char*>(buffer_.data), buffer_.size);
    }
  }


  void MemoryBuffer::ToJson(Json::Value& target) const
  {
    if (buffer_.data == NULL ||
        buffer_.size == 0)
    {
      OrthancPluginLogError(context_, "Cannot convert an empty memory buffer to JSON");
      throw PluginException(OrthancPluginErrorCode_InternalError);
    }

    const char* tmp = reinterpret_cast<const char*>(buffer_.data);

    Json::Reader reader;
    if (!reader.parse(tmp, tmp + buffer_.size, target))
    {
      OrthancPluginLogError(context_, "Cannot convert some memory buffer to JSON");
      throw PluginException(OrthancPluginErrorCode_BadFileFormat);
    }
  }


  bool MemoryBuffer::RestApiGet(const std::string& uri,
                                bool applyPlugins)
  {
    Clear();

    OrthancPluginErrorCode error;

    if (applyPlugins)
    {
      error = OrthancPluginRestApiGetAfterPlugins(context_, &buffer_, uri.c_str());
    }
    else
    {
      error = OrthancPluginRestApiGet(context_, &buffer_, uri.c_str());
    }

    if (error == OrthancPluginErrorCode_Success)
    {
      return true;
    }
    else if (error == OrthancPluginErrorCode_UnknownResource)
    {
      return false;
    }
    else
    {
      throw PluginException(error);
    }
  }

  
  bool MemoryBuffer::RestApiPost(const std::string& uri,
                                 const char* body,
                                 size_t bodySize,
                                 bool applyPlugins)
  {
    Clear();

    OrthancPluginErrorCode error;

    if (applyPlugins)
    {
      error = OrthancPluginRestApiPostAfterPlugins(context_, &buffer_, uri.c_str(), body, bodySize);
    }
    else
    {
      error = OrthancPluginRestApiPost(context_, &buffer_, uri.c_str(), body, bodySize);
    }

    if (error == OrthancPluginErrorCode_Success)
    {
      return true;
    }
    else if (error == OrthancPluginErrorCode_UnknownResource)
    {
      return false;
    }
    else
    {
      throw PluginException(error);
    }
  }


  bool MemoryBuffer::RestApiPut(const std::string& uri,
                                const char* body,
                                size_t bodySize,
                                bool applyPlugins)
  {
    Clear();

    OrthancPluginErrorCode error;

    if (applyPlugins)
    {
      error = OrthancPluginRestApiPutAfterPlugins(context_, &buffer_, uri.c_str(), body, bodySize);
    }
    else
    {
      error = OrthancPluginRestApiPut(context_, &buffer_, uri.c_str(), body, bodySize);
    }

    if (error == OrthancPluginErrorCode_Success)
    {
      return true;
    }
    else if (error == OrthancPluginErrorCode_UnknownResource)
    {
      return false;
    }
    else
    {
      throw PluginException(error);
    }
  }


  OrthancString::OrthancString(OrthancPluginContext* context,
                               char* str) :
    context_(context),
    str_(str)
  {
  }


  void OrthancString::Clear()
  {
    if (str_ != NULL)
    {
      OrthancPluginFreeString(context_, str_);
      str_ = NULL;
    }
  }


  void OrthancString::ToString(std::string& target) const
  {
    if (str_ == NULL)
    {
      target.clear();
    }
    else
    {
      target.assign(str_);
    }
  }


  void OrthancString::ToJson(Json::Value& target) const
  {
    if (str_ == NULL)
    {
      OrthancPluginLogError(context_, "Cannot convert an empty memory buffer to JSON");
      throw PluginException(OrthancPluginErrorCode_InternalError);
    }

    Json::Reader reader;
    if (!reader.parse(str_, target))
    {
      OrthancPluginLogError(context_, "Cannot convert some memory buffer to JSON");
      throw PluginException(OrthancPluginErrorCode_BadFileFormat);
    }
  }
  

  OrthancConfiguration::OrthancConfiguration(OrthancPluginContext* context) : 
    context_(context)
  {
    OrthancString str(context, OrthancPluginGetConfiguration(context));

    if (str.GetContent() == NULL)
    {
      OrthancPluginLogError(context, "Cannot access the Orthanc configuration");
      throw PluginException(OrthancPluginErrorCode_InternalError);
    }

    str.ToJson(configuration_);

    if (configuration_.type() != Json::objectValue)
    {
      OrthancPluginLogError(context, "Unable to read the Orthanc configuration");
      throw PluginException(OrthancPluginErrorCode_InternalError);
    }
  }


  OrthancPluginContext* OrthancConfiguration::GetContext() const
  {
    if (context_ == NULL)
    {
      throw PluginException(OrthancPluginErrorCode_Plugin);
    }
    else
    {
      return context_;
    }
  }


  std::string OrthancConfiguration::GetPath(const std::string& key) const
  {
    if (path_.empty())
    {
      return key;
    }
    else
    {
      return path_ + "." + key;
    }
  }


  void OrthancConfiguration::GetSection(OrthancConfiguration& target,
                                        const std::string& key) const
  {
    assert(configuration_.type() == Json::objectValue);

    target.context_ = context_;
    target.path_ = GetPath(key);

    if (!configuration_.isMember(key))
    {
      target.configuration_ = Json::objectValue;
    }
    else
    {
      if (configuration_[key].type() != Json::objectValue)
      {
        if (context_ != NULL)
        {
          std::string s = "The configuration section \"" + target.path_ + "\" is not an associative array as expected";
          OrthancPluginLogError(context_, s.c_str());
        }

        throw PluginException(OrthancPluginErrorCode_BadFileFormat);
      }

      target.configuration_ = configuration_[key];
    }
  }


  bool OrthancConfiguration::LookupStringValue(std::string& target,
                                               const std::string& key) const
  {
    assert(configuration_.type() == Json::objectValue);

    if (!configuration_.isMember(key))
    {
      return false;
    }

    if (configuration_[key].type() != Json::stringValue)
    {
      if (context_ != NULL)
      {
        std::string s = "The configuration option \"" + GetPath(key) + "\" is not a string as expected";
        OrthancPluginLogError(context_, s.c_str());
      }

      throw PluginException(OrthancPluginErrorCode_BadFileFormat);
    }

    target = configuration_[key].asString();
    return true;
  }


  bool OrthancConfiguration::LookupIntegerValue(int& target,
                                                const std::string& key) const
  {
    assert(configuration_.type() == Json::objectValue);

    if (!configuration_.isMember(key))
    {
      return false;
    }

    switch (configuration_[key].type())
    {
      case Json::intValue:
        target = configuration_[key].asInt();
        return true;
        
      case Json::uintValue:
        target = configuration_[key].asUInt();
        return true;
        
      default:
        if (context_ != NULL)
        {
          std::string s = "The configuration option \"" + GetPath(key) + "\" is not an integer as expected";
          OrthancPluginLogError(context_, s.c_str());
        }

        throw PluginException(OrthancPluginErrorCode_BadFileFormat);
    }
  }


  bool OrthancConfiguration::LookupUnsignedIntegerValue(unsigned int& target,
                                                        const std::string& key) const
  {
    int tmp;
    if (!LookupIntegerValue(tmp, key))
    {
      return false;
    }

    if (tmp < 0)
    {
      if (context_ != NULL)
      {
        std::string s = "The configuration option \"" + GetPath(key) + "\" is not a positive integer as expected";
        OrthancPluginLogError(context_, s.c_str());
      }

      throw PluginException(OrthancPluginErrorCode_BadFileFormat);
    }
    else
    {
      target = static_cast<unsigned int>(tmp);
      return true;
    }
  }


  bool OrthancConfiguration::LookupBooleanValue(bool& target,
                                                const std::string& key) const
  {
    assert(configuration_.type() == Json::objectValue);

    if (!configuration_.isMember(key))
    {
      return false;
    }

    if (configuration_[key].type() != Json::booleanValue)
    {
      if (context_ != NULL)
      {
        std::string s = "The configuration option \"" + GetPath(key) + "\" is not a Boolean as expected";
        OrthancPluginLogError(context_, s.c_str());
      }

      throw PluginException(OrthancPluginErrorCode_BadFileFormat);
    }

    target = configuration_[key].asBool();
    return true;
  }


  bool OrthancConfiguration::LookupFloatValue(float& target,
                                              const std::string& key) const
  {
    assert(configuration_.type() == Json::objectValue);

    if (!configuration_.isMember(key))
    {
      return false;
    }

    switch (configuration_[key].type())
    {
      case Json::realValue:
        target = configuration_[key].asFloat();
        return true;
        
      case Json::intValue:
        target = configuration_[key].asInt();
        return true;
        
      case Json::uintValue:
        target = configuration_[key].asUInt();
        return true;
        
      default:
        if (context_ != NULL)
        {
          std::string s = "The configuration option \"" + GetPath(key) + "\" is not an integer as expected";
          OrthancPluginLogError(context_, s.c_str());
        }

        throw PluginException(OrthancPluginErrorCode_BadFileFormat);
    }
  }

  
  std::string OrthancConfiguration::GetStringValue(const std::string& key,
                                                   const std::string& defaultValue) const
  {
    std::string tmp;
    if (LookupStringValue(tmp, key))
    {
      return tmp;
    }
    else
    {
      return defaultValue;
    }
  }


  int OrthancConfiguration::GetIntegerValue(const std::string& key,
                                            int defaultValue) const
  {
    int tmp;
    if (LookupIntegerValue(tmp, key))
    {
      return tmp;
    }
    else
    {
      return defaultValue;
    }
  }


  unsigned int OrthancConfiguration::GetUnsignedIntegerValue(const std::string& key,
                                                             unsigned int defaultValue) const
  {
    unsigned int tmp;
    if (LookupUnsignedIntegerValue(tmp, key))
    {
      return tmp;
    }
    else
    {
      return defaultValue;
    }
  }


  bool OrthancConfiguration::GetBooleanValue(const std::string& key,
                                             bool defaultValue) const
  {
    bool tmp;
    if (LookupBooleanValue(tmp, key))
    {
      return tmp;
    }
    else
    {
      return defaultValue;
    }
  }


  float OrthancConfiguration::GetFloatValue(const std::string& key,
                                            float defaultValue) const
  {
    float tmp;
    if (LookupFloatValue(tmp, key))
    {
      return tmp;
    }
    else
    {
      return defaultValue;
    }
  }


  bool RestApiDelete(OrthancPluginContext* context,
                     const std::string& uri,
                     bool applyPlugins)
  {
    OrthancPluginErrorCode error;

    if (applyPlugins)
    {
      error = OrthancPluginRestApiDeleteAfterPlugins(context, uri.c_str());
    }
    else
    {
      error = OrthancPluginRestApiDelete(context, uri.c_str());
    }

    if (error == OrthancPluginErrorCode_Success)
    {
      return true;
    }
    else if (error == OrthancPluginErrorCode_UnknownResource)
    {
      return false;
    }
    else
    {
      throw PluginException(error);
    }
  }
}

