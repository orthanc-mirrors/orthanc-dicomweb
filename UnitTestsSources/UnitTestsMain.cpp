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
  // Convenience class that wraps a Boost algorithm for string matching
  class StringMatcher : public boost::noncopyable
  {
  public:
    typedef std::string::const_iterator Iterator;

  private:
    typedef boost::algorithm::boyer_moore<Iterator>  Search;
    //typedef boost::algorithm::boyer_moore_horspool<std::string::const_iterator>  Search;

    // WARNING - The lifetime of "pattern_" must be larger than
    // "search_", as the latter references "pattern_"
    std::string  pattern_;
    Search       search_;
    bool         valid_;
    Iterator     matchBegin_;
    Iterator     matchEnd_;
    
  public:
    StringMatcher(const std::string& pattern) :
      pattern_(pattern),
      search_(pattern_.begin(), pattern_.end()),
      valid_(false)
    {
    }

    const std::string& GetPattern() const
    {
      return pattern_;
    }

    bool IsValid() const
    {
      return valid_;
    }

    bool Apply(Iterator start,
               Iterator end)
    {
#if BOOST_VERSION >= 106200
      matchBegin_ = search_(start, end).first;
#else
      matchBegin_ = search_(start, end);
#endif

      if (matchBegin_ == end)
      {
        valid_ = false;
      }
      else
      {
        matchEnd_ = matchBegin_ + pattern_.size();
        assert(matchEnd_ <= end);
        valid_ = true;
      }

      return valid_;
    }

    bool Apply(const std::string& corpus)
    {
      return Apply(corpus.begin(), corpus.end());
    }

    Iterator GetMatchBegin() const
    {
      if (valid_)
      {
        return matchBegin_;
      }
      else
      {
        throw OrthancException(ErrorCode_BadSequenceOfCalls);
      }
    }

    Iterator GetMatchEnd() const
    {
      if (valid_)
      {
        return matchEnd_;
      }
      else
      {
        throw OrthancException(ErrorCode_BadSequenceOfCalls);
      }
    }

    const void* GetPointerBegin() const
    {
      return &GetMatchBegin()[0];
    }

    const void* GetPointerEnd() const
    {
      return &GetMatchEnd()[0];
    }
  };

  
  class MultipartStreamParser : public boost::noncopyable
  {
  public:
    class IHandler : public boost::noncopyable
    {
    public:
      virtual ~IHandler()
      {
      }
      
      virtual void Apply(const std::map<std::string, std::string>& headers,
                          const void* part,
                          size_t size) = 0;
    };
    
    
  private:
    enum State
    {
      State_MainHeaders,
      State_UnusedArea,
      State_Content,
      State_Done
    };

    
    typedef std::map<std::string, std::string>  Dictionary;
    
    typedef boost::algorithm::boyer_moore<std::string::const_iterator>  Search;
    //typedef boost::algorithm::boyer_moore_horspool<std::string::const_iterator>  Search;
    //typedef boost::algorithm::knuth_morris_pratt<std::string::const_iterator>  Search;

    State  state_;
    Dictionary  mainHeaders_;
    IHandler*              handler_;
    StringMatcher           headersMatcher_;
    std::auto_ptr<StringMatcher>  boundaryMatcher_;
    //std::auto_ptr<Search>  boundaryMatcher_;
    ChunkedBuffer          buffer_;
    size_t                 blockSize_;


    static void ParseHeaders(Dictionary& headers,
                             StringMatcher::Iterator start,
                             StringMatcher::Iterator end)
    {
      std::string tmp(start, end);

      std::vector<std::string> lines;
      Toolbox::TokenizeString(lines, tmp, '\n');

      headers.clear();

      for (size_t i = 0; i < lines.size(); i++)
      {
        size_t separator = lines[i].find(':');
        if (separator != std::string::npos)
        {
          std::string key = Toolbox::StripSpaces(lines[i].substr(0, separator));
          std::string value = Toolbox::StripSpaces(lines[i].substr(separator + 1));

          Toolbox::ToLowerCase(key);
          headers[key] = value;
        }
      }
    }


    static bool LookupHeaderSizeValue(size_t& target,
                                      const Dictionary& headers,
                                      const std::string& key)
    {
      Dictionary::const_iterator it = headers.find(key);
      if (it == headers.end())
      {
        return false;
      }
      else
      {
        int64_t value;
        
        try
        {
          value = boost::lexical_cast<int64_t>(it->second);
        }
        catch (boost::bad_lexical_cast&)
        {
          throw OrthancException(ErrorCode_ParameterOutOfRange);
        }

        if (value < 0)
        {
          throw OrthancException(ErrorCode_ParameterOutOfRange);
        }
        else
        {
          target = static_cast<size_t>(value);
          return true;
        }
      }
    }


    static bool ParseHeaderValues(Dictionary& values,
                                  const Dictionary& headers,
                                  const std::string& key)
    {
      Dictionary::const_iterator it = headers.find(key);

      if (it == headers.end())
      {
        return false;
      }
      else
      {
        values.clear();
        
        std::vector<std::string> tokens;
        Toolbox::TokenizeString(tokens, it->second, ';');

        for (size_t i = 0; i < tokens.size(); i++)
        {
          size_t separator = tokens[i].find('=');
          if (separator != std::string::npos)
          {
            std::string key = Toolbox::StripSpaces(tokens[i].substr(0, separator));
            std::string value = Toolbox::StripSpaces(tokens[i].substr(separator + 1));

            if (!key.empty())
            {
              Toolbox::ToLowerCase(key);
              values[key] = value;
            }
          }
        }
        
        return true;
      }
    }


    void InitializeMultipart(const Dictionary& headers)
    {
      Dictionary values;
      if (!ParseHeaderValues(values, headers, "content-type"))
      {
        throw OrthancException(ErrorCode_NetworkProtocol,
                               "Multipart stream without a Content-Type");
      }

      Dictionary::const_iterator boundary = values.find("boundary");
      if (boundary == values.end())
      {
        throw OrthancException(ErrorCode_NetworkProtocol,
                               "Missing boundary in the Content-Type of a multipart stream");
      }

      LOG(INFO) << "Starting decoding of a multipart stream with boundary: " << boundary->second;
      boundaryMatcher_.reset(new StringMatcher("--" + boundary->second));        
    }
    

    void ParsePart(std::string::const_iterator start,
                   std::string::const_iterator end)
    {
      headersMatcher_.Apply(start, end);

#if 0
      if (headersMatcher_.GetIterator() != end)
      {
        std::string s(start, headersMatcher_.GetIterator());
        printf("[%s]\n", s.c_str());

        //std::map<std::string, std::string> headers;
        //std::string part(headersMatcher_.GetIterator(), end);
        //std::string part;
        //handler_->Apply(headers, part);
      }
      
      //printf("%d \n", size);
      
      // TODO - Parse headers
#endif
    }
    

    void ParseStream()
    {
      printf("."); fflush(stdout);
      if (handler_ == NULL)
      {
        return;
      }
      
      std::string corpus;
      buffer_.Flatten(corpus);

      StringMatcher::Iterator current = corpus.begin();
      StringMatcher::Iterator corpusEnd = corpus.end();

      if (state_ == State_MainHeaders)
      {
        if (headersMatcher_.Apply(corpus))
        {
          ParseHeaders(mainHeaders_, current, headersMatcher_.GetMatchBegin());
          InitializeMultipart(mainHeaders_);  // (*)
          state_ = State_UnusedArea;
          current = headersMatcher_.GetMatchEnd();
        }
        else
        {
          // The headers are not completely received yet, recycle the corpus for next iteration
          buffer_.AddChunk(corpus);
          return;
        }
      }

      assert(boundaryMatcher_.get() != NULL);  // This is initialized at (*)
      
      if (state_ == State_UnusedArea)
      {
        /**
         * "Before the first boundary is an area that is ignored by
         * MIME-compliant clients. This area is generally used to put
         * a message to users of old non-MIME clients."
         * https://en.wikipedia.org/wiki/MIME#Multipart_messages
         **/

        if (boundaryMatcher_->Apply(current, corpusEnd))
        {
          current = boundaryMatcher_->GetMatchBegin();
          state_ = State_Content;
        }
        else
        {
          // We have not seen the end of the unused area yet
          return;
        }          
      }

      StringMatcher::Iterator processed = current;

      for (;;)
      {
        size_t patternSize = boundaryMatcher_->GetPattern().size();
        size_t remainingSize = std::distance(current, corpusEnd);
        if (remainingSize < patternSize + 2)
        {
          break;  // Not enough data available
        }
        
        std::string boundary(current, current + patternSize + 2);
        if (boundary == boundaryMatcher_->GetPattern() + "--")
        {
          state_ = State_Done;
          return;
        }
        
        if (boundary != boundaryMatcher_->GetPattern() + "\r\n")
        {
          throw OrthancException(ErrorCode_NetworkProtocol,
                                 "Garbage between two items in a multipart stream");
        }

        StringMatcher::Iterator start = processed + patternSize + 2;
        
        if (!headersMatcher_.Apply(start, corpusEnd))
        {
          break;  // Not enough data available
        }

        Dictionary headers;
        ParseHeaders(headers, start, headersMatcher_.GetMatchBegin());

        size_t contentLength;
        if (LookupHeaderSizeValue(contentLength, headers, "content-length"))
        {
          if (headersMatcher_.GetMatchEnd() + contentLength <= corpusEnd)
          {
            printf("-");
            handler_->Apply(headers, headersMatcher_.GetPointerEnd(), contentLength);
            processed = headersMatcher_.GetMatchEnd() + contentLength;
          }
          else
          {
            break;  // Not enough data available
          }
        }
        else
        {
          // No "Content-Length" header: Search for the next boundary in the stream
          if (boundaryMatcher_->Apply(headersMatcher_.GetMatchEnd(), corpusEnd))
          {
            printf("+");
            handler_->Apply(headers, headersMatcher_.GetPointerEnd(),
                            std::distance(headersMatcher_.GetMatchEnd(), boundaryMatcher_->GetMatchBegin()));
            processed = boundaryMatcher_->GetMatchBegin();
          }
          else
          {
            break;  // Not enough data available
          }
        }
      }

      if (processed == corpusEnd)
      {
        // No part found, recycle the entire corpus for next iteration
        buffer_.AddChunkDestructive(corpus);
      }
      else
      {
        std::string reminder(processed, corpusEnd);
        buffer_.AddChunkDestructive(reminder);
      }
    }


  public:
    MultipartStreamParser() :
      state_(State_MainHeaders),
      handler_(NULL),
      headersMatcher_("\r\n\r\n"),
      blockSize_(10 * 1024 * 1024)
    {
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
    
    virtual void Apply(const std::map<std::string, std::string>& headers,
                       const void* part,
                       size_t size)
    {
      //printf(">> %d\n", part.size());

      std::string s((const char*) part, size);
      printf("[%s]\n", s.c_str());
      count_++;
    }

    unsigned int GetCount() const
    {
      return count_;
    }
  };
}



TEST(Multipart, DISABLED_Optimization)
{
  std::string boundary = "123456789123456789";

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
      std::string s = "--" + boundary + "\r\n\r\n\r\n";

      if (i != 0)
        s = "\r\n" + s;

      buffer.AddChunk(s);
      buffer.AddChunk(f);
    }

    buffer.AddChunk("\r\n--" + boundary + "--");
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
      OrthancPlugins::ParseMultipartBody(items, corpus.c_str(), corpus.size(), boundary);
      printf(">> %d\n", (int) items.size());
    }

    boost::posix_time::ptime end = boost::posix_time::microsec_clock::local_time();

    printf("Parsing 1: %d ms\n", (int) (end - start).total_milliseconds());
  }

  if (0)
  {
    boost::posix_time::ptime start = boost::posix_time::microsec_clock::local_time();

    {
      std::string pattern("--" + boundary + "\r\n");

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
      //parser.SetBoundary(boundary);
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
  std::string boundary = "123456789123456789";

  std::string f;
  /*f.resize(512*512*2);
  for (size_t i = 0; i < f.size(); i++)
  f[i] = i % 256;*/
  f = "hello";
  

  boost::posix_time::ptime start = boost::posix_time::microsec_clock::local_time();

  {
    Orthanc::Toto toto;

    Orthanc::MultipartStreamParser parser;

    //parser.SetBlockSize(127);
    //parser.SetBoundary(boundary);
    parser.SetHandler(toto);

    parser.AddChunk("Coucou: a\r\n");
    parser.AddChunk("Hello: b\r\n");
    parser.AddChunk("Content-Type: multipart/mixed; boundary=" + boundary + "\r\n");
    parser.AddChunk("World: c\r\n\r\n");

    for (size_t i = 0; i < 10; i++)
    {
      parser.AddChunk("--" + boundary + "\r\n");
      if (i % 2 == 0)
        parser.AddChunk("Content-Length: " + boost::lexical_cast<std::string>(f.size()) + "\r\n");
      parser.AddChunk("Content-Type: toto\r\n\r\n");
      parser.AddChunk(f);
    }

    parser.AddChunk("--" + boundary + "--");
    parser.CloseStream();
    
    printf("%d\n", toto.GetCount());
  }

  boost::posix_time::ptime end = boost::posix_time::microsec_clock::local_time();

  printf("Parsing: %d ms\n", (int) (end - start).total_milliseconds());
}


TEST(StringMatcher, Basic)
{
  Orthanc::StringMatcher matcher("---");

  ASSERT_THROW(matcher.GetMatchBegin(), Orthanc::OrthancException);

  {
    const std::string s = "abc----def";
    ASSERT_TRUE(matcher.Apply(s));
    ASSERT_EQ(3, std::distance(s.begin(), matcher.GetMatchBegin()));
    ASSERT_EQ("---", std::string(matcher.GetMatchBegin(), matcher.GetMatchEnd()));
  }

  {
    const std::string s = "abc---";
    ASSERT_TRUE(matcher.Apply(s));
    ASSERT_EQ(3, std::distance(s.begin(), matcher.GetMatchBegin()));
    ASSERT_EQ(s.end(), matcher.GetMatchEnd());
    ASSERT_EQ("---", std::string(matcher.GetMatchBegin(), matcher.GetMatchEnd()));
    ASSERT_EQ("", std::string(matcher.GetMatchEnd(), s.end()));
  }

  {
    const std::string s = "abc--def";
    ASSERT_FALSE(matcher.Apply(s));
    ASSERT_THROW(matcher.GetMatchBegin(), Orthanc::OrthancException);
    ASSERT_THROW(matcher.GetMatchEnd(), Orthanc::OrthancException);
  }
}


int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
