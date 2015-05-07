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


    dictionary_ = &gdcm::Global::GetInstance().GetDicts().GetPublicDict();


    if (!OrthancPlugins::Configuration::Read(configuration_, context) ||
        configuration_.type() != Json::objectValue)
    {
      OrthancPluginLogError(context_, "Unable to read the configuration file");
      return -1;
    }

    OrthancPluginSetDescription(context_, "Implementation of DICOM Web (QIDO-RS, STOW-RS and WADO-RS).");

    // WADO-RS callbacks
    OrthancPluginRegisterRestCallback(context, "/wado-rs/studies/([^/]*)", RetrieveDicomStudy);
    OrthancPluginRegisterRestCallback(context, "/wado-rs/studies/([^/]*)/series/([^/]*)", RetrieveDicomSeries);
    OrthancPluginRegisterRestCallback(context, "/wado-rs/studies/([^/]*)/series/([^/]*)/instances/([^/]*)", RetrieveDicomInstance);
    OrthancPluginRegisterRestCallback(context, "/wado-rs/studies/([^/]*)/metadata", RetrieveStudyMetadata);
    OrthancPluginRegisterRestCallback(context, "/wado-rs/studies/([^/]*)/series/([^/]*)/metadata", RetrieveSeriesMetadata);
    OrthancPluginRegisterRestCallback(context, "/wado-rs/studies/([^/]*)/series/([^/]*)/instances/([^/]*)/metadata", RetrieveInstanceMetadata);
    OrthancPluginRegisterRestCallback(context, "/wado-rs/studies/([^/]*)/series/([^/]*)/instances/([^/]*)/bulk/(.*)", RetrieveBulkData);

    // STOW-RS callbacks
    OrthancPluginRegisterRestCallback(context, "/stow-rs/studies", StowCallback);
    OrthancPluginRegisterRestCallback(context, "/stow-rs/studies/([^/]*)", StowCallback);

    // QIDO-RS callbacks
    OrthancPluginRegisterRestCallback(context, "/qido-rs/studies", SearchForStudies);    

    OrthancPluginRegisterRestCallback(context, "/qido-rs/studies/([^/]*)/series", SearchForSeries);    
    OrthancPluginRegisterRestCallback(context, "/qido-rs/series", SearchForSeries);    

    OrthancPluginRegisterRestCallback(context, "/qido-rs/studies/([^/]*)/series/([^/]*)/instances", SearchForInstances);    
    OrthancPluginRegisterRestCallback(context, "/qido-rs/studies/([^/]*)/instances", SearchForInstances);    
    OrthancPluginRegisterRestCallback(context, "/qido-rs/instances", SearchForInstances);    

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
