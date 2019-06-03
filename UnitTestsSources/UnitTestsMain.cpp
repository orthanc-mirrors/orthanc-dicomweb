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
#include <Core/OrthancException.h>
#include <Core/SystemToolbox.h>
#include <Core/Toolbox.h>
#include <boost/algorithm/searching/boyer_moore.hpp>
#include <boost/algorithm/searching/boyer_moore_horspool.hpp>
#include <boost/algorithm/searching/knuth_morris_pratt.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/version.hpp>


namespace Orthanc
{
  class MultipartStreamParser : public boost::noncopyable
  {
  public:
    class IHandler : public boost::noncopyable
    {
    public:
      virtual ~IHandler()
      {
      }
      
      virtual void Handle(const std::map<std::string, std::string>& headers,
                          const std::string& part) = 0;
    };
    
    
  private:
    typedef boost::algorithm::boyer_moore<std::string::const_iterator>  Search;
    //typedef boost::algorithm::boyer_moore_horspool<std::string::const_iterator>  Search;
    //typedef boost::algorithm::knuth_morris_pratt<std::string::const_iterator>  Search;

    IHandler*              handler_;
    std::auto_ptr<Search>  searchHeadersEnd_;
    std::auto_ptr<Search>  searchPattern_;
    std::string            pattern_;
    ChunkedBuffer          buffer_;
    size_t                 blockSize_;


    void ParsePart(std::string::const_iterator start,
                   std::string::const_iterator end)
    {
#if BOOST_VERSION >= 106200
      std::string::const_iterator pos = (*searchHeadersEnd_) (start, end).first;
#else
      std::string::const_iterator pos = (*searchHeadersEnd_) (start, end);
#endif

      std::string s(start, pos);
      printf("[%s]\n", s.c_str());
      
      //printf("%d \n", size);
      
      // TODO - Parse headers
      //handler_->Handle(part);
    }
    

    void ParseStream()
    {
      printf("."); fflush(stdout);
      if (searchPattern_.get() == NULL ||
          handler_ == NULL)
      {
        return;
      }
      
      std::string corpus;
      buffer_.Flatten(corpus);

      std::string::const_iterator previous = corpus.end();

#if BOOST_VERSION >= 106200
      std::string::const_iterator current = (*searchPattern_) (corpus.begin(), corpus.end()).first;
#else
      std::string::const_iterator current = (*searchPattern_) (corpus.begin(), corpus.end());
#endif

      while (current != corpus.end())
      {
        if (previous == corpus.end() &&
            std::distance(current, reinterpret_cast<const std::string&>(corpus).begin()) != 0)
        {
          // TODO - There is heading garbage! => Decide what to do!
          throw OrthancException(ErrorCode_NetworkProtocol);
        }
        
        if (previous != corpus.end())
        {
          std::string::const_iterator start = previous + pattern_.size();
          size_t size = std::distance(start, current);

          if (size > 0)
          {
            ParsePart(start, current);
          }
        }

        previous = current;
        current += pattern_.size();
        
#if BOOST_VERSION >= 106200
        current = (*searchPattern_) (current, reinterpret_cast<const std::string&>(corpus).end()).first;
#else
        current = (*searchPattern_) (current, reinterpret_cast<const std::string&>(corpus).end());
#endif
      }

      if (previous == corpus.end())
      {
        // No part found, recycle the entire corpus for next iteration
        buffer_.AddChunkDestructive(corpus);
      }
      else
      {
        std::string reminder(previous, reinterpret_cast<const std::string&>(corpus).end());
        buffer_.AddChunkDestructive(reminder);
      }
    }


  public:
    MultipartStreamParser() :
      handler_(NULL),
      blockSize_(10 * 1024 * 1024)
    {
      const std::string s = "\r\n\r\n";
      searchHeadersEnd_.reset(new Search(s.begin(), s.end()));
    }

    void SetBlockSize(size_t size)
    {
      if (size == 0)
      {
        throw OrthancException(ErrorCode_ParameterOutOfRange);
      }
      else
      {
        blockSize_ = size;
      }        
    }

    size_t GetBlockSize() const
    {
      return blockSize_;
    }

    void SetHandler(IHandler& handler)
    {
      handler_ = &handler;
    }
    
    void SetSeparator(const std::string& separator)
    {
      pattern_ = "--" + separator;
      searchPattern_.reset(new Search(pattern_.begin(), pattern_.end()));
    }
    
    void AddChunk(const void* chunk,
                  size_t size)
    {
      if (size != 0)
      {
        size_t oldSize = buffer_.GetNumBytes();
      
        buffer_.AddChunk(chunk, size);

        if (oldSize / blockSize_ != buffer_.GetNumBytes() / blockSize_)
        {
          ParseStream();
        }
      }
    }

    void AddChunk(const std::string& chunk)
    {
      if (!chunk.empty())
      {
        AddChunk(chunk.c_str(), chunk.size());
      }
    }

    void CloseStream()
    {
      if (buffer_.GetNumBytes() != 0)
      {
        ParseStream();
      }

      std::string tmp;
      buffer_.Flatten(tmp);
      printf("Reminder: [%s]\n", tmp.c_str());
    }
  };


  class Toto : public MultipartStreamParser::IHandler
  {
  private:
    unsigned int count_;
    
  public:
    Toto() : count_(0)
    {
    }
    
    virtual void Handle(const std::map<std::string, std::string>& headers,
                        const std::string& part)
    {
      //printf(">> %d\n", part.size());
      count_++;
    }

    unsigned int GetCount() const
    {
      return count_;
    }
  };
}



TEST(Multipart, Optimization)
{
  std::string separator = "123456789123456789";

  std::string corpus;

  if (1)
  {
    std::string f;
    f.resize(512*512*2);
    for (size_t i = 0; i < f.size(); i++)
      f[i] = i % 256;
  
    Orthanc::ChunkedBuffer buffer;

    for (size_t i = 0; i < 10; i++)
    {
      std::string s = "--" + separator + "\r\n\r\n\r\n";

      if (i != 0)
        s = "\r\n" + s;

      buffer.AddChunk(s);
      buffer.AddChunk(f);
    }

    buffer.AddChunk("\r\n--" + separator + "--");
    buffer.Flatten(corpus);

    Orthanc::SystemToolbox::WriteFile(corpus, "tutu");
  }
  else
  {
    Orthanc::SystemToolbox::ReadFile(corpus, "tutu");
  }

  if (1)
  {
    boost::posix_time::ptime start = boost::posix_time::microsec_clock::local_time();

    {
      std::vector<OrthancPlugins::MultipartItem> items;
      OrthancPlugins::ParseMultipartBody(items, corpus.c_str(), corpus.size(), separator);
      printf(">> %d\n", (int) items.size());
    }

    boost::posix_time::ptime end = boost::posix_time::microsec_clock::local_time();

    printf("Parsing 1: %d ms\n", (int) (end - start).total_milliseconds());
  }

  if (0)
  {
    boost::posix_time::ptime start = boost::posix_time::microsec_clock::local_time();

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
        //printf("[%s]\n", t.c_str());
      
        c++;
      
#if BOOST_VERSION >= 106200
        it = search(it + pattern.size(), corpus.end()).first;
#else
        it = search(it + pattern.size(), corpus.end());
#endif
      }

      printf("count: %d\n", c);
    }

    boost::posix_time::ptime end = boost::posix_time::microsec_clock::local_time();

    printf("Parsing 2: %d ms\n", (int) (end - start).total_milliseconds());
  }

  if (1)
  {
    boost::posix_time::ptime start = boost::posix_time::microsec_clock::local_time();

    {
      Orthanc::Toto toto;

      Orthanc::MultipartStreamParser parser;

      //parser.SetBlockSize(127);
      parser.SetSeparator(separator);
      parser.SetHandler(toto);

#if 1
      size_t bs = corpus.size() / 101;

      const char* pos = corpus.c_str();
      for (size_t i = 0; i < corpus.size() / bs; i++, pos += bs)
      {
        parser.AddChunk(pos, bs);
      }

      parser.AddChunk(pos, corpus.size() % bs);
#else
      parser.AddChunk(corpus);
#endif

      parser.CloseStream();

      printf("%d\n", toto.GetCount());
    }

    boost::posix_time::ptime end = boost::posix_time::microsec_clock::local_time();

    printf("Parsing 3: %d ms\n", (int) (end - start).total_milliseconds());
  }
}



TEST(Multipart, Optimization2)
{
  std::string separator = "123456789123456789";

  std::string f;
  f.resize(512*512*2);
  for (size_t i = 0; i < f.size(); i++)
    f[i] = i % 256;
  

  boost::posix_time::ptime start = boost::posix_time::microsec_clock::local_time();

  {
    Orthanc::Toto toto;

    Orthanc::MultipartStreamParser parser;

    //parser.SetBlockSize(127);
    parser.SetSeparator(separator);
    parser.SetHandler(toto);

    for (size_t i = 0; i < 100; i++)
    {
      parser.AddChunk("--" + separator + "\r\n");
      parser.AddChunk("Content-Type: toto\r\n");
      parser.AddChunk("Content-Length: " + boost::lexical_cast<std::string>(f.size()) + "\r\n\r\n");
      parser.AddChunk(f);
    }

    parser.AddChunk("--" + separator + "--");
    parser.CloseStream();
    
    printf("%d\n", toto.GetCount());
  }

  boost::posix_time::ptime end = boost::posix_time::microsec_clock::local_time();

  printf("Parsing: %d ms\n", (int) (end - start).total_milliseconds());
}


int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
