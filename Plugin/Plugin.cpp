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


#include "Plugin.h"

#include "QidoRs.h"
#include "StowRs.h"
#include "WadoRs.h"
#include "Wado.h"
#include "Configuration.h"


#include <gdcmDictEntry.h>
#include <gdcmDict.h>
#include <gdcmDicts.h>
#include <gdcmGlobal.h>


// Global state
OrthancPluginContext* context_ = NULL;
Json::Value configuration_;
const gdcm::Dict* dictionary_ = NULL;


static OrthancPluginErrorCode SwitchStudies(OrthancPluginRestOutput* output,
                                            const char* url,
                                            const OrthancPluginHttpRequest* request)
{
  switch (request->method)
  {
    case OrthancPluginHttpMethod_Get:
      // This is QIDO-RS
      return SearchForStudies(output, url, request);

    case OrthancPluginHttpMethod_Post:
      // This is STOW-RS
      return StowCallback(output, url, request);

    default:
      OrthancPluginSendMethodNotAllowed(context_, output, "GET,POST");
      return OrthancPluginErrorCode_Success;
  }
}


static OrthancPluginErrorCode SwitchStudy(OrthancPluginRestOutput* output,
                                          const char* url,
                                          const OrthancPluginHttpRequest* request)
{
  switch (request->method)
  {
    case OrthancPluginHttpMethod_Get:
      // This is WADO-RS
      return RetrieveDicomStudy(output, url, request);

    case OrthancPluginHttpMethod_Post:
      // This is STOW-RS
      return StowCallback(output, url, request);

    default:
      OrthancPluginSendMethodNotAllowed(context_, output, "GET,POST");
      return OrthancPluginErrorCode_Success;
  }
}


static void Register(const std::string& root,
                     const std::string& uri,
                     OrthancPluginRestCallback callback)
{
  assert(!uri.empty() && uri[0] != '/');
  std::string s = root + uri;
  OrthancPluginRegisterRestCallback(context_, s.c_str(), callback);
}



extern "C"
{
  ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* context)
  {
    context_ = context;

    /* Check the version of the Orthanc core */
    if (OrthancPluginCheckVersion(context_) == 0)
    {
      char info[1024];
      sprintf(info, "Your version of Orthanc (%s) must be above %d.%d.%d to run this plugin",
              context_->orthancVersion,
              ORTHANC_PLUGINS_MINIMAL_MAJOR_NUMBER,
              ORTHANC_PLUGINS_MINIMAL_MINOR_NUMBER,
              ORTHANC_PLUGINS_MINIMAL_REVISION_NUMBER);
      OrthancPluginLogError(context_, info);
      return -1;
    }

    {
      std::string version(context_->orthancVersion);
      if (version == "0.9.1")
      {
        OrthancPluginLogWarning(context_, "If using STOW-RS, the DICOMweb plugin can lead to "
                                "deadlocks in Orthanc version 0.9.1. Please upgrade Orthanc!");
      }
    }


    OrthancPluginSetDescription(context_, "Implementation of DICOM Web (QIDO-RS, STOW-RS and WADO-RS) and WADO.");

    // Read the configuration
    dictionary_ = &gdcm::Global::GetInstance().GetDicts().GetPublicDict();

    configuration_ = Json::objectValue;

    {
      Json::Value tmp;
      if (!OrthancPlugins::Configuration::Read(tmp, context) ||
          tmp.type() != Json::objectValue)
      {
        OrthancPluginLogError(context_, "Unable to read the configuration file");
        return -1;
      }

      if (tmp.isMember("DicomWeb") &&
          tmp["DicomWeb"].type() == Json::objectValue)
      {
        configuration_ = tmp["DicomWeb"];
      }
    }

    // Configure the DICOMweb callbacks
    if (OrthancPlugins::Configuration::GetBoolValue(configuration_, "Enable", true))
    {
      std::string root = OrthancPlugins::Configuration::GetRoot(configuration_);

      std::string message = "URI to the DICOMweb REST API: " + root;
      OrthancPluginLogWarning(context_, message.c_str());

      Register(root, "instances", SearchForInstances);    
      Register(root, "series", SearchForSeries);    
      Register(root, "studies", SwitchStudies);
      Register(root, "studies/([^/]*)", SwitchStudy);
      Register(root, "studies/([^/]*)/instances", SearchForInstances);    
      Register(root, "studies/([^/]*)/metadata", RetrieveStudyMetadata);
      Register(root, "studies/([^/]*)/series", SearchForSeries);    
      Register(root, "studies/([^/]*)/series/([^/]*)", RetrieveDicomSeries);
      Register(root, "studies/([^/]*)/series/([^/]*)/instances", SearchForInstances);    
      Register(root, "studies/([^/]*)/series/([^/]*)/instances/([^/]*)", RetrieveDicomInstance);
      Register(root, "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/bulk/(.*)", RetrieveBulkData);
      Register(root, "studies/([^/]*)/series/([^/]*)/instances/([^/]*)/metadata", RetrieveInstanceMetadata);
      Register(root, "studies/([^/]*)/series/([^/]*)/metadata", RetrieveSeriesMetadata);
    }
    else
    {
      OrthancPluginLogWarning(context_, "DICOMweb support is disabled");
    }

    // Configure the WADO callback
    if (OrthancPlugins::Configuration::GetBoolValue(configuration_, "EnableWado", true))
    {
      std::string wado = OrthancPlugins::Configuration::GetWadoRoot(configuration_);

      std::string message = "URI to the WADO API: " + wado;
      OrthancPluginLogWarning(context_, message.c_str());

      OrthancPluginRegisterRestCallback(context_, wado.c_str(), WadoCallback);
    }
    else
    {
      OrthancPluginLogWarning(context_, "WADO support is disabled");
    }

    return 0;
  }


  ORTHANC_PLUGINS_API void OrthancPluginFinalize()
  {
  }


  ORTHANC_PLUGINS_API const char* OrthancPluginGetName()
  {
    return "dicom-web";
  }


  ORTHANC_PLUGINS_API const char* OrthancPluginGetVersion()
  {
    return ORTHANC_DICOM_WEB_VERSION;
  }
}
