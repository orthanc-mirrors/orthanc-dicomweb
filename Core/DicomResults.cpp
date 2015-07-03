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


#include "DicomResults.h"

#include "Dicom.h"

namespace OrthancPlugins
{
  DicomResults::DicomResults(OrthancPluginContext* context,
                             OrthancPluginRestOutput* output,
                             const gdcm::Dict& dictionary,
                             bool isXml,
                             bool isBulkAccessible) :
    context_(context),
    output_(output),
    dictionary_(dictionary),
    isFirst_(true),
    isXml_(isXml),
    isBulkAccessible_(isBulkAccessible)
  {
    if (isXml_ &&
        OrthancPluginStartMultipartAnswer(context_, output_, "related", "application/dicom+xml") != 0)
    {
      throw std::runtime_error("Unable to create a multipart stream of DICOM+XML answers");
    }

    jsonWriter_.AddChunk("[\n");
  }


  void DicomResults::AddInternal(const gdcm::File* file,
                                 const gdcm::DataSet& dicom)
  {
    if (isXml_)
    {
      std::string answer;
      GenerateSingleDicomAnswer(answer, dictionary_, file, dicom, true, isBulkAccessible_);

      if (OrthancPluginSendMultipartItem(context_, output_, answer.c_str(), answer.size()) != 0)
      {
        throw std::runtime_error("Unable to write an item to a multipart stream of DICOM+XML answers");
      }
    }
    else
    {
      if (!isFirst_)
      {
        jsonWriter_.AddChunk(",\n");
      }

      std::string item;
      GenerateSingleDicomAnswer(item, dictionary_, file, dicom, false, isBulkAccessible_);
      jsonWriter_.AddChunk(item);
    }

    isFirst_ = false;
  }

  void DicomResults::Answer()
  {
    if (isXml_)
    {
      // Nothing to do in this case
    }
    else
    {
      jsonWriter_.AddChunk("]\n");

      std::string answer;
      jsonWriter_.Flatten(answer);
      OrthancPluginAnswerBuffer(context_, output_, answer.c_str(), answer.size(), "application/json");
    }
  }
}
