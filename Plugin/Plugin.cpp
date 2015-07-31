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
#include "../Core/Configuration.h"


#include <gdcmDictEntry.h>
#include <gdcmDict.h>
#include <gdcmDicts.h>
#include <gdcmGlobal.h>


// Global state
OrthancPluginContext* context_ = NULL;
Json::Value configuration_;
const gdcm::Dict* dictionary_ = NULL;

static std::string root_ = "/dicom-web/";


static int32_t SwitchStudies(OrthancPluginRestOutput* output,
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
      return 0;
  }
}


static int32_t SwitchStudy(OrthancPluginRestOutput* output,
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
      return 0;
  }
}


static void Register(const std::string& uri,
                     OrthancPluginRestCallback callback)
{
  assert(!uri.empty() && uri[0] != '/');
  std::string s = root_ + uri;
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


    dictionary_ = &gdcm::Global::GetInstance().GetDicts().GetPublicDict();


    if (!OrthancPlugins::Configuration::Read(configuration_, context) ||
        configuration_.type() != Json::objectValue)
    {
      OrthancPluginLogError(context_, "Unable to read the configuration file");
      return -1;
    }

    if (configuration_.isMember("DicomWeb") &&
        configuration_["DicomWeb"].isMember("Root"))
    {
      if (configuration_["DicomWeb"]["Root"].type() == Json::stringValue)
      {
        root_ = configuration_["DicomWeb"]["Root"].asString();
      }
      else
      {
        OrthancPluginLogError(context_, "Bad data type for DicomWeb::Root inside the configuration file");
        return -1;
      }
    }

    // Make sure the root URI starts and ends with a slash
    if (root_.empty())
    {
      root_ = "/";
    }
    else
    {
      if (root_[0] != '/')
      {
        root_ = "/" + root_;
      }
    
      if (root_[root_.length() - 1] != '/')
      {
        root_ += "/";
      }
    }

    {
      std::string message = "URI to the DICOMweb REST API: " + root_;
      OrthancPluginLogWarning(context_, message.c_str());
    }

    OrthancPluginSetDescription(context_, "Implementation of DICOM Web (QIDO-RS, STOW-RS and WADO-RS).");

    Register("instances", SearchForInstances);    
    Register("series", SearchForSeries);    
    Register("studies", SwitchStudies);
    Register("studies/([^/]*)", SwitchStudy);
    Register("studies/([^/]*)/instances", SearchForInstances);    
    Register("studies/([^/]*)/metadata", RetrieveStudyMetadata);
    Register("studies/([^/]*)/series", SearchForSeries);    
    Register("studies/([^/]*)/series/([^/]*)", RetrieveDicomSeries);
    Register("studies/([^/]*)/series/([^/]*)/instances", SearchForInstances);    
    Register("studies/([^/]*)/series/([^/]*)/instances/([^/]*)", RetrieveDicomInstance);
    Register("studies/([^/]*)/series/([^/]*)/instances/([^/]*)/bulk/(.*)", RetrieveBulkData);
    Register("studies/([^/]*)/series/([^/]*)/instances/([^/]*)/metadata", RetrieveInstanceMetadata);
    Register("studies/([^/]*)/series/([^/]*)/metadata", RetrieveSeriesMetadata);

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
