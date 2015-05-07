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
  DicomResults::DicomResults(const gdcm::Dict& dictionary,
                             bool isXml,
                             bool isBulkAccessible) :
    dictionary_(dictionary),
    xmlWriter_("application/dicom+xml"),
    isFirst_(true),
    isXml_(isXml),
    isBulkAccessible_(isBulkAccessible)
  {
    jsonWriter_.AddChunk("[\n");
  }

  void DicomResults::Add(const gdcm::DataSet& dicom)
  {
    if (isXml_)
    {
      std::string answer;
      GenerateSingleDicomAnswer(answer, dictionary_, dicom, true, isBulkAccessible_);
      xmlWriter_.AddPart(answer);
    }
    else
    {
      if (!isFirst_)
      {
        jsonWriter_.AddChunk(",\n");
      }

      std::string item;
      GenerateSingleDicomAnswer(item, dictionary_, dicom, false, isBulkAccessible_);
      jsonWriter_.AddChunk(item);
    }

    isFirst_ = false;
  }

  void DicomResults::Answer(OrthancPluginContext* context,
                            OrthancPluginRestOutput* output)
  {
    if (isXml_)
    {
      xmlWriter_.Answer(context, output);
    }
    else
    {
      jsonWriter_.AddChunk("]\n");

      std::string answer;
      jsonWriter_.Flatten(answer);
      OrthancPluginAnswerBuffer(context, output, answer.c_str(), answer.size(), "application/json");
    }
  }
}
