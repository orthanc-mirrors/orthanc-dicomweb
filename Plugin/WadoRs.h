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


int32_t RetrieveDicomStudy(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request);

int32_t RetrieveDicomSeries(OrthancPluginRestOutput* output,
                            const char* url,
                            const OrthancPluginHttpRequest* request);

int32_t RetrieveDicomInstance(OrthancPluginRestOutput* output,
                              const char* url,
                              const OrthancPluginHttpRequest* request);

int32_t RetrieveStudyMetadata(OrthancPluginRestOutput* output,
                              const char* url,
                              const OrthancPluginHttpRequest* request);

int32_t RetrieveSeriesMetadata(OrthancPluginRestOutput* output,
                               const char* url,
                               const OrthancPluginHttpRequest* request);

int32_t RetrieveInstanceMetadata(OrthancPluginRestOutput* output,
                                 const char* url,
                                 const OrthancPluginHttpRequest* request);

int32_t RetrieveBulkData(OrthancPluginRestOutput* output,
                         const char* url,
                         const OrthancPluginHttpRequest* request);
