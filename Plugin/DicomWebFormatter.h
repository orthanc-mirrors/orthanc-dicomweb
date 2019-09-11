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

#include <Core/ChunkedBuffer.h>
#include <Core/DicomFormat/DicomMap.h>

#include <orthanc/OrthancCPlugin.h>

#include <json/value.h>

#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>


namespace OrthancPlugins
{
  class DicomWebFormatter : public boost::noncopyable
  {
  private:
    boost::mutex                     mutex_;
    OrthancPluginDicomWebBinaryMode  mode_;
    std::string                      bulkRoot_;

    static DicomWebFormatter& GetSingleton()
    {
      static DicomWebFormatter formatter;
      return formatter;
    }

    static void Callback(OrthancPluginDicomWebNode *node,
                         OrthancPluginDicomWebSetBinaryNode setter,
                         uint32_t levelDepth,
                         const uint16_t *levelTagGroup,
                         const uint16_t *levelTagElement,
                         const uint32_t *levelIndex,
                         uint16_t tagGroup,
                         uint16_t tagElement,
                         OrthancPluginValueRepresentation vr);

  public:
    class Locker : public boost::noncopyable
    {
    private:
      DicomWebFormatter&         that_;
      boost::mutex::scoped_lock  lock_;

    public:
      Locker(OrthancPluginDicomWebBinaryMode mode,
             const std::string& bulkRoot);

      void Apply(std::string& target,
                 OrthancPluginContext* context,
                 const void* data,
                 size_t size,
                 bool xml);

      void Apply(std::string& target,
                 OrthancPluginContext* context,
                 const Json::Value& value,
                 bool xml);
    };

    class HttpWriter : public boost::noncopyable
    {
    private:
      OrthancPluginContext*     context_;
      OrthancPluginRestOutput*  output_;
      bool                      isXml_;
      bool                      first_;
      Orthanc::ChunkedBuffer    jsonBuffer_;

      void AddInternal(const void* dicom,
                       size_t size,
                       OrthancPluginDicomWebBinaryMode mode,
                       const std::string& bulkRoot);

    public:
      HttpWriter(OrthancPluginRestOutput* output,
                 bool isXml);

      bool IsXml() const
      {
        return isXml_;
      }

      void AddDicom(const void* dicom,
                    size_t size,
                    const std::string& bulkRoot)
      {
        AddInternal(dicom, size, OrthancPluginDicomWebBinaryMode_BulkDataUri, bulkRoot);
      }

      void AddOrthancMap(const Orthanc::DicomMap& value);

      void AddOrthancJson(const Json::Value& value);

      void AddDicomWebSerializedJson(const void* data,
                                     size_t size);

      void Send();
    };
  };
}
