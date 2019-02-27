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

#include "Configuration.h"

#include <Core/ChunkedBuffer.h>
#include <Core/Enumerations.h>
#include <Core/DicomFormat/DicomTag.h>
#include <Plugins/Samples/Common/OrthancPluginCppWrapper.h>

#include <gdcmReader.h>
#include <gdcmDataSet.h>
#include <pugixml.hpp>
#include <gdcmDict.h>
#include <list>


namespace OrthancPlugins
{
  class ParsedDicomFile
  {
  private:
    gdcm::Reader reader_;

    void Setup(const std::string& dicom);

    Orthanc::Encoding  GetEncoding() const;

  public:
    explicit ParsedDicomFile(const OrthancPlugins::MemoryBuffer& item);

    explicit ParsedDicomFile(const std::string& dicom)
    {
      Setup(dicom);
    }

    const gdcm::File& GetFile() const
    {
      return reader_.GetFile();
    }

    const gdcm::DataSet& GetDataSet() const
    {
      return reader_.GetFile().GetDataSet();
    }

    bool GetRawTag(std::string& result,
                   const gdcm::Tag& tag,
                   bool stripSpaces) const;

    std::string GetRawTagWithDefault(const gdcm::Tag& tag,
                                     const std::string& defaultValue,
                                     bool stripSpaces) const;

    std::string GetRawTagWithDefault(const Orthanc::DicomTag& tag,
                                     const std::string& defaultValue,
                                     bool stripSpaces) const;

    bool GetStringTag(std::string& result,
                      const gdcm::Dict& dictionary,
                      const gdcm::Tag& tag,
                      bool stripSpaces) const;

    bool GetIntegerTag(int& result,
                       const gdcm::Dict& dictionary,
                       const gdcm::Tag& tag) const;

    std::string GetWadoUrl(const OrthancPluginHttpRequest* request) const;
  };
}
