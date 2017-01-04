/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017 Osimis, Belgium
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


#include "DicomWebServers.h"

#include "Configuration.h"
#include "../Orthanc/Core/Toolbox.h"

namespace OrthancPlugins
{
  void DicomWebServers::Clear()
  {
    for (Servers::iterator it = servers_.begin(); it != servers_.end(); ++it)
    {
      delete it->second;
    }
  }


  void DicomWebServers::Load(const Json::Value& servers)
  {
    boost::mutex::scoped_lock lock(mutex_);

    Clear();

    bool ok = true;

    try
    {
      if (servers.type() != Json::objectValue)
      {
        ok = false;
      }
      else
      {
        Json::Value::Members members = servers.getMemberNames();

        for (size_t i = 0; i < members.size(); i++)
        {
          std::auto_ptr<Orthanc::WebServiceParameters> parameters(new Orthanc::WebServiceParameters);
          parameters->FromJson(servers[members[i]]);

          servers_[members[i]] = parameters.release();
        }
      }
    }
    catch (Orthanc::OrthancException& e)
    {
      OrthancPlugins::Configuration::LogError("Exception while parsing the \"DicomWeb.Servers\" section "
                                              "of the configuration file: " + std::string(e.What()));
      throw;
    }

    if (!ok)
    {
      OrthancPlugins::Configuration::LogError("Cannot parse the \"DicomWeb.Servers\" section of the configuration file");
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
    }
  }


  DicomWebServers& DicomWebServers::GetInstance()
  {
    static DicomWebServers singleton;
    return singleton;
  }


  Orthanc::WebServiceParameters DicomWebServers::GetServer(const std::string& name)
  {
    boost::mutex::scoped_lock lock(mutex_);
    Servers::const_iterator server = servers_.find(name);

    if (server == servers_.end() ||
        server->second == NULL)
    {
      OrthancPlugins::Configuration::LogError("Inexistent server: " + name);
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem);
    }
    else
    {
      return *server->second;
    }
  }


  void DicomWebServers::ListServers(std::list<std::string>& servers)
  {
    boost::mutex::scoped_lock lock(mutex_);

    servers.clear();
    for (Servers::const_iterator it = servers_.begin(); it != servers_.end(); ++it)
    {
      servers.push_back(it->first);
    }
  }


  static const char* ConvertToCString(const std::string& s)
  {
    if (s.empty())
    {
      return NULL;
    }
    else
    {
      return s.c_str();
    }
  }



  void CallServer(OrthancPlugins::MemoryBuffer& answerBody /* out */,
                  std::map<std::string, std::string>& answerHeaders /* out */,
                  const Orthanc::WebServiceParameters& server,
                  OrthancPluginHttpMethod method,
                  const std::map<std::string, std::string>& httpHeaders,
                  const std::string& uri,
                  const std::string& body)
  {
    answerBody.Clear();
    answerHeaders.clear();

    std::string url = server.GetUrl();
    assert(!url.empty() && url[url.size() - 1] == '/');

    // Remove the leading "/" in the URI if need be
    std::string tmp;
    if (!uri.empty() &&
        uri[0] == '/')
    {
      url += uri.substr(1);
    }
    else
    {
      url += uri;
    }

    std::vector<const char*> httpHeadersKeys(httpHeaders.size());
    std::vector<const char*> httpHeadersValues(httpHeaders.size());

    {
      size_t pos = 0;
      for (std::map<std::string, std::string>::const_iterator
             it = httpHeaders.begin(); it != httpHeaders.end(); ++it)
      {
        httpHeadersKeys[pos] = it->first.c_str();
        httpHeadersValues[pos] = it->second.c_str();
        pos += 1;
      }
    }

    const char* bodyContent = NULL;
    size_t bodySize = 0;

    if ((method == OrthancPluginHttpMethod_Put ||
         method == OrthancPluginHttpMethod_Post) &&
        !body.empty())
    {
      bodyContent = body.c_str();
      bodySize = body.size();
    }

    OrthancPluginContext* context = OrthancPlugins::Configuration::GetContext();

    uint16_t status = 0;
    MemoryBuffer answerHeadersTmp(context);
    OrthancPluginErrorCode code = OrthancPluginHttpClient(
      context, 
      /* Outputs */
      *answerBody, *answerHeadersTmp, &status, 
      method,
      url.c_str(), 
      /* HTTP headers*/
      httpHeaders.size(),
      httpHeadersKeys.empty() ? NULL : &httpHeadersKeys[0],
      httpHeadersValues.empty() ? NULL : &httpHeadersValues[0],
      bodyContent, bodySize,
      ConvertToCString(server.GetUsername()), /* Authentication */
      ConvertToCString(server.GetPassword()), 
      0,                                      /* Timeout */
      ConvertToCString(server.GetCertificateFile()),
      ConvertToCString(server.GetCertificateKeyFile()),
      ConvertToCString(server.GetCertificateKeyPassword()),
      server.IsPkcs11Enabled() ? 1 : 0);

    if (code != OrthancPluginErrorCode_Success ||
        (status < 200 || status >= 300))
    {
      OrthancPlugins::Configuration::LogError("Cannot issue an HTTP query to " + url + 
                                              " (HTTP status: " + boost::lexical_cast<std::string>(status) + ")");
      throw Orthanc::OrthancException(static_cast<Orthanc::ErrorCode>(code));
    }

    Json::Value json;
    answerHeadersTmp.ToJson(json);
    answerHeadersTmp.Clear();

    if (json.type() != Json::objectValue)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
    }

    Json::Value::Members members = json.getMemberNames();
    for (size_t i = 0; i < members.size(); i++)
    {
      const std::string& key = members[i];

      if (json[key].type() != Json::stringValue)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
      }
      else
      {
        answerHeaders[key] = json[key].asString();        
      }
    }
  }


  void UriEncode(std::string& uri,
                 const std::string& resource,
                 const std::map<std::string, std::string>& getArguments)
  {
    if (resource.find('?') != std::string::npos)
    {
      OrthancPlugins::Configuration::LogError("The GET arguments must be provided in a separate field "
                                              "(explicit \"?\" is disallowed): " + resource);
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
    }

    uri = resource;

    bool isFirst = true;
    for (std::map<std::string, std::string>::const_iterator
           it = getArguments.begin(); it != getArguments.end(); ++it)
    {
      if (isFirst)
      {
        uri += '?';
        isFirst = false;
      }
      else
      {
        uri += '&';
      }

      std::string key, value;
      Orthanc::Toolbox::UriEncode(key, it->first);
      Orthanc::Toolbox::UriEncode(value, it->second);

      if (value.empty())
      {
        uri += key;
      }
      else
      {
        uri += key + "=" + value;
      }
    }
  }
}
