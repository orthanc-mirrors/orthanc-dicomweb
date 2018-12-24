/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2019 Osimis S.A., Belgium
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
#include <json/value.h>

namespace OrthancPlugins
{
  class DicomResults
  {
  private:
    OrthancPluginRestOutput*  output_;
    std::string               wadoBase_;
    const gdcm::Dict&         dictionary_;
    Orthanc::ChunkedBuffer    jsonWriter_;  // Used for JSON output
    bool                      isFirst_; 
    bool                      isXml_;
    bool                      isBulkAccessible_;

    void AddInternal(const std::string& item);

    void AddInternal(const gdcm::DataSet& dicom);

  public:
    DicomResults(OrthancPluginRestOutput* output,
                 const std::string& wadoBase,
                 const gdcm::Dict& dictionary,
                 bool isXml,
                 bool isBulkAccessible);

    void Add(const gdcm::File& file)
    {
      AddInternal(file.GetDataSet());
    }

    void Add(const gdcm::DataSet& subset)
    {
      AddInternal(subset);
    }

    void AddFromOrthanc(const Json::Value& dicom,
                        const std::string& wadoUrl);

    void Answer();
  };
}
