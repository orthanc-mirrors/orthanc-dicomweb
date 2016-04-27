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


#include "DicomWebPeers.h"

#include "Plugin.h"
#include "../Orthanc/Core/OrthancException.h"

namespace OrthancPlugins
{
  void DicomWebPeer::SetUrl(const std::string& url)
  {
    if (url.empty())
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
    }

    url_ = url;

    // Add trailing slash
    if (url_[url.size() - 1] != '/')
    {
      url_ += '/';
    }
  }


  void DicomWebPeers::Clear()
  {
    for (Peers::iterator it = peers_.begin(); it != peers_.end(); ++it)
    {
      delete it->second;
    }
  }


  void DicomWebPeers::Load(const Json::Value& configuration)
  {
    boost::mutex::scoped_lock lock(mutex_);

    Clear();

    if (!configuration.isMember("Peers"))
    {
      return;
    }

    bool ok = true;

    if (configuration["Peers"].type() != Json::objectValue)
    {
      ok = false;
    }
    else
    {
      Json::Value::Members members = configuration["Peers"].getMemberNames();

      for (size_t i = 0; i < members.size(); i++)
      {
        const Json::Value& peer = configuration["Peers"][members[i]];

        if (peer.type() != Json::arrayValue ||
            (peer.size() != 1 && peer.size() != 3) ||
            peer[0].type() != Json::stringValue ||
            (peer.size() == 3 && peer[1].type() != Json::stringValue) ||
            (peer.size() == 3 && peer[2].type() != Json::stringValue))
        {
          ok = false;
          break;
        }
        else
        {
          peers_[members[i]] = new DicomWebPeer(peer[0].asString(),
                                                peer[1].asString(),
                                                peer[2].asString());
        }
      }
    }

    if (!ok)
    {
      OrthancPluginLogError(context_, "Cannot parse the \"DicomWeb.Peers\" section of the configuration file");
      throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
    }
  }


  DicomWebPeers& DicomWebPeers::GetInstance()
  {
    static DicomWebPeers singleton;
    return singleton;
  }


  DicomWebPeer DicomWebPeers::GetPeer(const std::string& name)
  {
    boost::mutex::scoped_lock lock(mutex_);
    Peers::const_iterator peer = peers_.find(name);

    if (peer == peers_.end() ||
        peer->second == NULL)
    {
      std::string s = "Inexistent peer: " + name;
      OrthancPluginLogError(context_, s.c_str());
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InexistentItem);
    }
    else
    {
      return *peer->second;
    }
  }


  void DicomWebPeers::ListPeers(std::list<std::string>& peers)
  {
    boost::mutex::scoped_lock lock(mutex_);

    peers.clear();
    for (Peers::const_iterator it = peers_.begin(); it != peers_.end(); ++it)
    {
      peers.push_back(it->first);
    }
  }
}
