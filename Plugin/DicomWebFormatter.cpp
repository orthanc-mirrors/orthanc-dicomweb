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


#include "DicomWebFormatter.h"

#include <Plugins/Samples/Common/OrthancPluginCppWrapper.h>


namespace OrthancPlugins
{
  static std::string FormatTag(uint16_t group,
                               uint16_t element)
  {
    char buf[16];
    sprintf(buf, "%04x%04x", group, element);
    return std::string(buf);
  }


  void DicomWebFormatter::Callback(OrthancPluginDicomWebNode *node,
                                   OrthancPluginDicomWebSetBinaryNode setter,
                                   uint32_t levelDepth,
                                   const uint16_t *levelTagGroup,
                                   const uint16_t *levelTagElement,
                                   const uint32_t *levelIndex,
                                   uint16_t tagGroup,
                                   uint16_t tagElement,
                                   OrthancPluginValueRepresentation vr)
  {
    const DicomWebFormatter& that = GetSingleton();

    switch (that.mode_)
    {
      case OrthancPluginDicomWebBinaryMode_Ignore:
      case OrthancPluginDicomWebBinaryMode_InlineBinary:
        setter(node, that.mode_, NULL);
        break;

      case OrthancPluginDicomWebBinaryMode_BulkDataUri:
      {
        std::string uri = GetSingleton().bulkRoot_;

        for (size_t i = 0; i < levelDepth; i++)
        {
          uri += ("/" + FormatTag(levelTagGroup[i], levelTagElement[i]) + "/" +
                  boost::lexical_cast<std::string>(levelIndex[i] + 1));
        }
    
        uri += "/" + FormatTag(tagGroup, tagElement);
    
        setter(node, that.mode_, uri.c_str());
        break;
      }
    }
  }


  DicomWebFormatter::Locker::Locker(OrthancPluginDicomWebBinaryMode mode,
                                    const std::string& bulkRoot) :
    that_(GetSingleton()),
    lock_(that_.mutex_)
  {
    that_.mode_ = mode;
    that_.bulkRoot_ = bulkRoot;
  }


  void DicomWebFormatter::Locker::Apply(std::string& target,
                                        OrthancPluginContext* context,
                                        const void* data,
                                        size_t size,
                                        bool xml)
  {
    OrthancString s;

    if (xml)
    {
      s.Assign(OrthancPluginEncodeDicomWebXml(context, data, size, Callback));
    }
    else
    {
      s.Assign(OrthancPluginEncodeDicomWebJson(context, data, size, Callback));
    }

    s.ToString(target);
  }


  DicomWebFormatter::HttpWriter::HttpWriter(OrthancPluginRestOutput* output,
                                            bool isXml) :
    context_(OrthancPlugins::GetGlobalContext()),
    output_(output),
    isXml_(isXml),
    first_(true)
  {
    if (context_ == NULL ||
        output_ == NULL)
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NullPointer);
    }

    if (isXml_)
    {
      OrthancPluginStartMultipartAnswer(context_, output_, "related", "application/dicom+xml");
    }
    else
    {
      jsonBuffer_.AddChunk("[");
    }
  }


  void DicomWebFormatter::HttpWriter::AddInternal(const void* dicom,
                                                  size_t size,
                                                  OrthancPluginDicomWebBinaryMode mode,
                                                  const std::string& bulkRoot)
  {
    if (!first_ &&
        !isXml_)
    {
      jsonBuffer_.AddChunk(",");      
    }

    first_ = false;

    std::string item;

    {
      // TODO - Avoid a global mutex => Need to change Orthanc SDK
      OrthancPlugins::DicomWebFormatter::Locker locker(mode, bulkRoot);
      locker.Apply(item, context_, dicom, size, isXml_);
    }
   
    if (isXml_)
    {
      OrthancPluginSendMultipartItem(context_, output_, item.c_str(), item.size());
    }
    else
    {
      jsonBuffer_.AddChunk(item);
    }
  }

                  
  void DicomWebFormatter::HttpWriter::AddJson(const Json::Value& value)
  {
    MemoryBuffer dicom;
    dicom.CreateDicom(value, OrthancPluginCreateDicomFlags_None);

    AddInternal(dicom.GetData(), dicom.GetSize(), OrthancPluginDicomWebBinaryMode_Ignore, "");
  }


  void DicomWebFormatter::HttpWriter::Send()
  {
    if (!isXml_)
    {
      jsonBuffer_.AddChunk("]");
      
      std::string answer;
      jsonBuffer_.Flatten(answer);
      OrthancPluginAnswerBuffer(context_, output_, answer.c_str(), answer.size(), "application/dicom+json");
    }
  }
}
