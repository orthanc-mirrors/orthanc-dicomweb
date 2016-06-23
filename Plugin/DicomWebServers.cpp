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


#include "DicomWebServers.h"

#include "Configuration.h"
#include "../Orthanc/Core/OrthancException.h"

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
}
