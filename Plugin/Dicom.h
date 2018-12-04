/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2018 Osimis S.A., Belgium
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
#include <Plugins/Samples/Common/OrthancPluginCppWrapper.h>

#include <gdcmReader.h>
#include <gdcmDataSet.h>
#include <pugixml.hpp>
#include <gdcmDict.h>
#include <list>


namespace OrthancPlugins
{
  static const gdcm::Tag DICOM_TAG_SOP_CLASS_UID(0x0008, 0x0016);
  static const gdcm::Tag DICOM_TAG_SOP_INSTANCE_UID(0x0008, 0x0018);
  static const gdcm::Tag DICOM_TAG_STUDY_INSTANCE_UID(0x0020, 0x000d);
  static const gdcm::Tag DICOM_TAG_SERIES_INSTANCE_UID(0x0020, 0x000e);
  static const gdcm::Tag DICOM_TAG_REFERENCED_SOP_CLASS_UID(0x0008, 0x1150);
  static const gdcm::Tag DICOM_TAG_REFERENCED_SOP_INSTANCE_UID(0x0008, 0x1155);
  static const gdcm::Tag DICOM_TAG_RETRIEVE_URL(0x0008, 0x1190);
  static const gdcm::Tag DICOM_TAG_FAILED_SOP_SEQUENCE(0x0008, 0x1198);
  static const gdcm::Tag DICOM_TAG_FAILURE_REASON(0x0008, 0x1197);
  static const gdcm::Tag DICOM_TAG_WARNING_REASON(0x0008, 0x1196);
  static const gdcm::Tag DICOM_TAG_REFERENCED_SOP_SEQUENCE(0x0008, 0x1199);
  static const gdcm::Tag DICOM_TAG_ACCESSION_NUMBER(0x0008, 0x0050);
  static const gdcm::Tag DICOM_TAG_SPECIFIC_CHARACTER_SET(0x0008, 0x0005);
  static const gdcm::Tag DICOM_TAG_PIXEL_DATA(0x7fe0, 0x0010);
  static const gdcm::Tag DICOM_TAG_SAMPLES_PER_PIXEL(0x0028, 0x0002);
  static const gdcm::Tag DICOM_TAG_COLUMNS(0x0028, 0x0011);
  static const gdcm::Tag DICOM_TAG_ROWS(0x0028, 0x0010);
  static const gdcm::Tag DICOM_TAG_BITS_ALLOCATED(0x0028, 0x0100);

  class ParsedDicomFile
  {
  private:
    gdcm::Reader reader_;

    void Setup(const std::string& dicom);

  public:
    explicit ParsedDicomFile(const OrthancPlugins::MultipartItem& item);

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

    bool GetStringTag(std::string& result,
                      const gdcm::Dict& dictionary,
                      const gdcm::Tag& tag,
                      bool stripSpaces) const;

    bool GetIntegerTag(int& result,
                       const gdcm::Dict& dictionary,
                       const gdcm::Tag& tag) const;

    Orthanc::Encoding  GetEncoding() const;

    std::string GetWadoUrl(const OrthancPluginHttpRequest* request) const;
  };


  const char* GetVRName(bool& isSequence /* out */,
                        const gdcm::Dict& dictionary,
                        const gdcm::Tag& tag);

  void GenerateSingleDicomAnswer(std::string& result,
                                 const std::string& wadoBase,
                                 const gdcm::Dict& dictionary,
                                 const gdcm::DataSet& dicom,
                                 bool isXml,
                                 bool isBulkAccessible);

  void AnswerDicom(OrthancPluginRestOutput* output,
                   const std::string& wadoBase,
                   const gdcm::Dict& dictionary,
                   const gdcm::DataSet& dicom,
                   bool isXml,
                   bool isBulkAccessible);

  gdcm::Tag ParseTag(const gdcm::Dict& dictionary,
                     const std::string& key);

  std::string FormatTag(const gdcm::Tag& tag);

  const char* GetKeyword(const gdcm::Dict& dictionary,
                         const gdcm::Tag& tag);

  class ChunkedBufferWriter : public pugi::xml_writer
  {
  private:
    Orthanc::ChunkedBuffer buffer_;

  public:
    virtual void write(const void *data, size_t size)
    {
      if (size > 0)
      {
        buffer_.AddChunk(reinterpret_cast<const char*>(data), size);
      }
    }

    void Flatten(std::string& s)
    {
      buffer_.Flatten(s);
    }
  };
}
