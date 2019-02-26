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
    std::string uri = GetSingleton().bulkRoot_;

    for (size_t i = 0; i < levelDepth; i++)
    {
      uri += ("/" + FormatTag(levelTagGroup[i], levelTagElement[i]) + "/" +
              boost::lexical_cast<std::string>(levelIndex[i] + 1));
    }
    
    uri += "/" + FormatTag(tagGroup, tagElement);
    
    setter(node, OrthancPluginDicomWebBinaryMode_BulkDataUri, uri.c_str());
  }


  DicomWebFormatter::Locker::Locker(const std::string& bulkRoot) :
    that_(GetSingleton()),
    lock_(that_.mutex_)
  {
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
}
