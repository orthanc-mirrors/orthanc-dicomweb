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


#include "MultipartWriter.h"

namespace OrthancPlugins
{
  MultipartWriter::MultipartWriter(const std::string& contentType) : 
    contentType_(contentType)
  {
    // Some random string
    boundary_ = "123456789abcdefghijklmnopqrstuvwxyz@^";
  }

  void MultipartWriter::AddPart(const std::string& part)
  {
    std::string header = "--" + boundary_ + "\n";
    header += "Content-Type: " + contentType_ + "\n";
    header += "MIME-Version: 1.0\n\n";
    chunks_.AddChunk(header);
    chunks_.AddChunk(part);
    chunks_.AddChunk("\n");
  }

  void MultipartWriter::Answer(OrthancPluginContext* context,
                               OrthancPluginRestOutput* output)
  {
    // Close the body
    chunks_.AddChunk("--" + boundary_ + "--\n");

    std::string header = "multipart/related; type=" + contentType_ + "; boundary=" + boundary_;

    std::string body;
    chunks_.Flatten(body);
    OrthancPluginAnswerBuffer(context, output, body.c_str(), body.size(), header.c_str());
  }
}
