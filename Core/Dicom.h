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

#include "Toolbox.h"

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

  class ParsedDicomFile
  {
  private:
    gdcm::Reader reader_;

    void Setup(const std::string& dicom);

  public:
    ParsedDicomFile(const OrthancPlugins::MultipartItem& item);

    ParsedDicomFile(const std::string& dicom)
    {
      Setup(dicom);
    }

    const gdcm::DataSet& GetDataSet() const
    {
      return reader_.GetFile().GetDataSet();
    }

    bool GetTag(std::string& result,
                const gdcm::Tag& tag,
                bool stripSpaces) const;

    std::string GetTagWithDefault(const gdcm::Tag& tag,
                                  const std::string& defaultValue,
                                  bool stripSpaces) const;
  };


  void GenerateSingleDicomAnswer(std::string& result,
                                 const gdcm::Dict& dictionary,
                                 const gdcm::DataSet& dicom,
                                 bool isXml,
                                 bool isBulkAccessible);

  void AnswerDicom(OrthancPluginContext* context,
                   OrthancPluginRestOutput* output,
                   const gdcm::Dict& dictionary,
                   const gdcm::DataSet& dicom,
                   bool isXml,
                   bool isBulkAccessible);
}
