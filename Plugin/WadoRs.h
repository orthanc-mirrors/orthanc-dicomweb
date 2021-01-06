/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2021 Osimis S.A., Belgium
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

#include "Configuration.h"


bool LocateStudy(OrthancPluginRestOutput* output,
                 std::string& uri,
                 std::string& studyInstanceUid,
                 const OrthancPluginHttpRequest* request);

bool LocateSeries(OrthancPluginRestOutput* output,
                  std::string& uri,
                  std::string& studyInstanceUid,
                  std::string& seriesInstanceUid,
                  const OrthancPluginHttpRequest* request);

bool LocateInstance(OrthancPluginRestOutput* output,
                    std::string& uri,
                    std::string& studyInstanceUid,
                    std::string& seriesInstanceUid,
                    std::string& sopInstanceUid,
                    const OrthancPluginHttpRequest* request);

void RetrieveDicomStudy(OrthancPluginRestOutput* output,
                        const char* url,
                        const OrthancPluginHttpRequest* request);

void RetrieveDicomSeries(OrthancPluginRestOutput* output,
                         const char* url,
                         const OrthancPluginHttpRequest* request);

void RetrieveDicomInstance(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request);

void RetrieveStudyMetadata(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request);

void RetrieveSeriesMetadata(OrthancPluginRestOutput* output,
                            const char* url,
                            const OrthancPluginHttpRequest* request);

void RetrieveInstanceMetadata(OrthancPluginRestOutput* output,
                              const char* url,
                              const OrthancPluginHttpRequest* request);

void RetrieveBulkData(OrthancPluginRestOutput* output,
                      const char* url,
                      const OrthancPluginHttpRequest* request);

void RetrieveAllFrames(OrthancPluginRestOutput* output,
                       const char* url,
                       const OrthancPluginHttpRequest* request);

void RetrieveSelectedFrames(OrthancPluginRestOutput* output,
                            const char* url,
                            const OrthancPluginHttpRequest* request);

void RetrieveInstanceRendered(OrthancPluginRestOutput* output,
                              const char* url,
                              const OrthancPluginHttpRequest* request);

void RetrieveFrameRendered(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request);

void RetrieveSeriesRendered(OrthancPluginRestOutput* output,
                            const char* url,
                            const OrthancPluginHttpRequest* request);

void RetrieveStudyRendered(OrthancPluginRestOutput* output,
                           const char* url,
                           const OrthancPluginHttpRequest* request);
