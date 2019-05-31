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


#include <gtest/gtest.h>
#include <boost/lexical_cast.hpp>

#include "../Plugin/Configuration.h"

using namespace OrthancPlugins;

OrthancPluginContext* context_ = NULL;


TEST(ContentType, Parse)
{
  std::string c;
  std::map<std::string, std::string> a;

  ParseContentType(c, a, "Multipart/Related; TYPE=Application/Dicom; Boundary=heLLO");
  ASSERT_EQ(c, "multipart/related");
  ASSERT_EQ(2u, a.size());
  ASSERT_EQ(a["type"], "Application/Dicom");
  ASSERT_EQ(a["boundary"], "heLLO");

  // The WADO-RS client must support the case where the WADO-RS server
  // escapes the "type" subfield in the Content-Type header
  // cf. https://tools.ietf.org/html/rfc7231#section-3.1.1.1
  ParseContentType(c, a, "Multipart/Related; TYPE=\"Application/Dicom\"  ;  Boundary=heLLO");
  ASSERT_EQ(c, "multipart/related");
  ASSERT_EQ(2u, a.size());
  ASSERT_EQ(a["type"], "Application/Dicom");
  ASSERT_EQ(a["boundary"], "heLLO");
  
  ParseContentType(c, a, "");
  ASSERT_TRUE(c.empty());
  ASSERT_EQ(0u, a.size());  

  ParseContentType(c, a, "multipart/related");
  ASSERT_EQ(c, "multipart/related");
  ASSERT_EQ(0u, a.size());
}



#include <Core/ChunkedBuffer.h>
#include <Core/Toolbox.h>
#include <Core/SystemToolbox.h>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/algorithm/searching/boyer_moore.hpp>
#include <boost/version.hpp>

TEST(Multipart, Optimization)
{
  std::string separator = Orthanc::Toolbox::GenerateUuid();

  std::string corpus;

  {
    std::string f;
    f.resize(512*512*2);
    for (size_t i = 0; i < f.size(); i++)
      f[i] = i % 256;
  
    Orthanc::ChunkedBuffer buffer;

    for (size_t i = 0; i < 100; i++)
    {
      std::string s = "--" + separator + "\r\n\r\n\r\n";

      if (i != 0)
        s = "\r\n" + s;

      buffer.AddChunk(s);
      buffer.AddChunk(f);
    }

    buffer.AddChunk("\r\n--" + separator + "--");
    buffer.Flatten(corpus);
  }
  
  boost::posix_time::ptime start = boost::posix_time::microsec_clock::local_time();

  {
    std::vector<OrthancPlugins::MultipartItem> items;
    OrthancPlugins::ParseMultipartBody(items, corpus.c_str(), corpus.size(), separator);
    printf(">> %d\n", (int) items.size());
  }

  boost::posix_time::ptime end = boost::posix_time::microsec_clock::local_time();

  printf("Parsing 1: %d ms\n", (int) (end - start).total_milliseconds());

  start = boost::posix_time::microsec_clock::local_time();

  {
    std::string pattern("--" + separator + "\r\n");

    boost::algorithm::boyer_moore<std::string::const_iterator>
      search(pattern.begin(), pattern.end());

#if BOOST_VERSION >= 106200
    std::string::iterator it = search(corpus.begin(), corpus.end()).first;
#else
    std::string::iterator it = search(corpus.begin(), corpus.end());
#endif

    unsigned int c = 0;
    while (it != corpus.end())
    {
      std::string t(it, it + pattern.size());
      printf("[%s]\n", t.c_str());
      
      c++;
      
#if BOOST_VERSION >= 106200
      it = search(std::next(it, pattern.size()), corpus.end()).first;
#else
      it = search(std::next(it, pattern.size()), corpus.end());
#endif
    }

    printf("count: %d\n", c);
  }

  end = boost::posix_time::microsec_clock::local_time();

  printf("Parsing 2: %d ms\n", (int) (end - start).total_milliseconds());
}


int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
