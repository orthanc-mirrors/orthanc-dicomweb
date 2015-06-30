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

#include "ChunkedBuffer.h"

#include <orthanc/OrthancCPlugin.h>
#include <gdcmDataSet.h>
#include <gdcmDict.h>
#include <gdcmFile.h>

namespace OrthancPlugins
{
  class DicomResults
  {
  private:
    OrthancPluginContext*     context_;
    OrthancPluginRestOutput*  output_;
    const gdcm::Dict&         dictionary_;
    ChunkedBuffer             jsonWriter_;  // Used for JSON output
    bool                      isFirst_; 
    bool                      isXml_;
    bool                      isBulkAccessible_;

    void AddInternal(const gdcm::File* file,
                     const gdcm::DataSet& dicom);

  public:
    DicomResults(OrthancPluginContext* context,
                 OrthancPluginRestOutput* output,
                 const gdcm::Dict& dictionary,
                 bool isXml,
                 bool isBulkAccessible);

    void Add(const gdcm::File& file)
    {
      AddInternal(&file, file.GetDataSet());
    }

    void Add(const gdcm::File& file,
             const gdcm::DataSet& subset)
    {
      AddInternal(&file, subset);
    }

    void Answer();
  };
}
