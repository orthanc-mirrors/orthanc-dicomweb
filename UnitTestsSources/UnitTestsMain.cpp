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
#include <boost/algorithm/string/predicate.hpp>

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

    const char* GetPointerBegin() const
    {
      return &GetMatchBegin()[0];
    }

    const char* GetPointerEnd() const
    {
      return &GetMatchEnd()[0];
    }
  };

  
  class MultipartStreamReader : public boost::noncopyable
  {
  public:
    typedef std::map<std::string, std::string>  HttpHeaders;
    
    class IHandler : public boost::noncopyable
    {
    public:
      virtual ~IHandler()
      {
      }
      
      virtual void Apply(const HttpHeaders& headers,
                         const void* part,
                         size_t size) = 0;
    };
    
    
  private:
    enum State
    {
      State_UnusedArea,
      State_Content,
      State_Done
    };

    
    State  state_;
    IHandler*              handler_;
    StringMatcher           headersMatcher_;
    std::auto_ptr<StringMatcher>  boundaryMatcher_;
    //std::auto_ptr<Search>  boundaryMatcher_;
    ChunkedBuffer          buffer_;
    size_t                 blockSize_;


    static void ParseHeaders(HttpHeaders& headers,
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
                                      const HttpHeaders& headers,
                                      const std::string& key)
    {
      HttpHeaders::const_iterator it = headers.find(key);
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


    static bool ParseHeaderValues(std::string& main,
                                  HttpHeaders& parameters,
                                  const HttpHeaders& headers,
                                  const std::string& key)
    {
      HttpHeaders::const_iterator it = headers.find(key);

      main.clear();
      parameters.clear();
        
      if (it == headers.end())
      {
        return false;
      }
      else
      {
        std::vector<std::string> tokens;
        Toolbox::TokenizeString(tokens, it->second, ';');

        if (tokens.empty())
        {
          return false;
        }
        
        main = tokens[0];

        for (size_t i = 1; i < tokens.size(); i++)
        {
          size_t separator = tokens[i].find('=');
          if (separator != std::string::npos)
          {
            std::string key = Toolbox::StripSpaces(tokens[i].substr(0, separator));
            std::string value = Toolbox::StripSpaces(tokens[i].substr(separator + 1));

            if (!key.empty())
            {
              Toolbox::ToLowerCase(key);
              parameters[key] = value;
            }
          }
        }
        
        return true;
      }
    }


    void ParseStream()
    {
      if (handler_ == NULL ||
          state_ == State_Done)
      {
        return;
      }
      
      assert(boundaryMatcher_.get() != NULL);
      
      std::string corpus;
      buffer_.Flatten(corpus);

      StringMatcher::Iterator current = corpus.begin();
      StringMatcher::Iterator corpusEnd = corpus.end();

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
          std::string reminder(current, corpusEnd);
          buffer_.AddChunkDestructive(reminder);
          return;
        }          
      } 
      
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

        StringMatcher::Iterator start = current + patternSize + 2;
        
        if (!headersMatcher_.Apply(start, corpusEnd))
        {
          break;  // Not enough data available
        }

        HttpHeaders headers;
        ParseHeaders(headers, start, headersMatcher_.GetMatchBegin());

        size_t contentLength;
        if (!LookupHeaderSizeValue(contentLength, headers, "content-length"))
        {
          if (boundaryMatcher_->Apply(headersMatcher_.GetMatchEnd(), corpusEnd))
          {
            size_t d = std::distance(headersMatcher_.GetMatchEnd(), boundaryMatcher_->GetMatchBegin());
            if (d <= 1)
            {
              throw OrthancException(ErrorCode_NetworkProtocol);
            }
            else
            {
              contentLength = d - 2;
            }
          }
          else
          {
            break;  // Not enough data available to have a full part
          }
        }

        if (headersMatcher_.GetMatchEnd() + contentLength + 2 > corpusEnd)
        {
          break;  // Not enough data available to have a full part
        }

        const char* p = headersMatcher_.GetPointerEnd() + contentLength;
        if (p[0] != '\r' ||
            p[1] != '\n')
        {
          throw OrthancException(ErrorCode_NetworkProtocol,
                                 "No endline at the end of a part");
        }
          
        handler_->Apply(headers, headersMatcher_.GetPointerEnd(), contentLength);
        current = headersMatcher_.GetMatchEnd() + contentLength + 2;
      }

      if (current != corpusEnd)
      {
        std::string reminder(current, corpusEnd);
        buffer_.AddChunkDestructive(reminder);
      }
    }


  public:
    MultipartStreamReader(const std::string& boundary) :
      state_(State_UnusedArea),
      handler_(NULL),
      headersMatcher_("\r\n\r\n"),
      boundaryMatcher_(new StringMatcher("--" + boundary)),
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
    }


    static bool GetMainContentType(std::string& contentType,
                                   const HttpHeaders& headers)
    {
      HttpHeaders::const_iterator it = headers.find("content-type");

      if (it == headers.end())
      {
        return false;
      }
      else
      {
        contentType = it->second;
        return true;
      }
    }


    static bool ParseMultipartHeaders(std::string& contentType,
                                      std::string& boundary,
                                      const HttpHeaders& headers)
    {
      std::string tmp;
      if (!GetMainContentType(tmp, headers))
      {
        return false;
      }

      std::vector<std::string> tokens;
      Orthanc::Toolbox::TokenizeString(tokens, tmp, ';');

      if (tokens.empty())
      {
        return false;
      }

      contentType = Orthanc::Toolbox::StripSpaces(tokens[0]);
      if (contentType.empty())
      {
        return false;
      }

      for (size_t i = 0; i < tokens.size(); i++)
      {
        std::vector<std::string> items;
        Orthanc::Toolbox::TokenizeString(items, tokens[i], '=');

        if (items.size() == 2)
        {
          if (boost::iequals("boundary", Orthanc::Toolbox::StripSpaces(items[0])))
          {
            boundary = Orthanc::Toolbox::StripSpaces(items[1]);
            return !boundary.empty();
          }
        }
      }

      return false;
    }
  };
}


class MultipartTester : public Orthanc::MultipartStreamReader::IHandler
{
private:
  struct Part
  {
    Orthanc::MultipartStreamReader::HttpHeaders   headers_;
    std::string  data_;

    Part(const Orthanc::MultipartStreamReader::HttpHeaders& headers,
         const void* part,
         size_t size) :
      headers_(headers),
      data_(reinterpret_cast<const char*>(part), size)
    {
    }
  };

  std::vector<Part> parts_;

public:
  virtual void Apply(const Orthanc::MultipartStreamReader::HttpHeaders& headers,
                     const void* part,
                     size_t size)
  {
    parts_.push_back(Part(headers, part, size));
  }

  unsigned int GetCount() const
  {
    return parts_.size();
  }

  Orthanc::MultipartStreamReader::HttpHeaders& GetHeaders(size_t i)
  {
    return parts_[i].headers_;
  }

  const std::string& GetData(size_t i) const
  {
    return parts_[i].data_;
  }
};


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


TEST(MultipartStreamReader, ParseHeaders)
{
  std::string ct, b;

  {
    Orthanc::MultipartStreamReader::HttpHeaders h;
    h["hello"] = "world";
    ASSERT_FALSE(Orthanc::MultipartStreamReader::GetMainContentType(ct, h));
    ASSERT_FALSE(Orthanc::MultipartStreamReader::ParseMultipartHeaders(ct, b, h));
  }

  {
    Orthanc::MultipartStreamReader::HttpHeaders h;
    h["content-type"] = "world";
    ASSERT_TRUE(Orthanc::MultipartStreamReader::GetMainContentType(ct, h)); 
    ASSERT_EQ(ct, "world");
    ASSERT_FALSE(Orthanc::MultipartStreamReader::ParseMultipartHeaders(ct, b, h));
  }

  {
    Orthanc::MultipartStreamReader::HttpHeaders h;
    h["content-type"] = "multipart/related; type=value; boundary=1234; hello=world";
    ASSERT_TRUE(Orthanc::MultipartStreamReader::GetMainContentType(ct, h)); 
    ASSERT_EQ(ct, h["content-type"]);
    ASSERT_TRUE(Orthanc::MultipartStreamReader::ParseMultipartHeaders(ct, b, h));
    ASSERT_EQ(ct, "multipart/related");
    ASSERT_EQ(b, "1234");
  }
}


TEST(MultipartStreamReader, BytePerByte)
{
  std::string stream = "GARBAGE";

  std::string boundary = "123456789123456789";

  {
    for (size_t i = 0; i < 10; i++)
    {
      std::string f = "hello " + boost::lexical_cast<std::string>(i);
    
      stream += "\r\n--" + boundary + "\r\n";
      if (i % 2 == 0)
        stream += "Content-Length: " + boost::lexical_cast<std::string>(f.size()) + "\r\n";
      stream += "Content-Type: toto " + boost::lexical_cast<std::string>(i) + "\r\n\r\n";
      stream += f;
    }
  
    stream += "\r\n--" + boundary + "--";
    stream += "GARBAGE";
  }

  for (unsigned int k = 0; k < 2; k++)
  {
    MultipartTester decoded;

    Orthanc::MultipartStreamReader reader(boundary);
    reader.SetBlockSize(1);
    reader.SetHandler(decoded);

    if (k == 0)
    {
      for (size_t i = 0; i < stream.size(); i++)
      {
        reader.AddChunk(&stream[i], 1);
      }
    }
    else
    {
      reader.AddChunk(stream);
    }

    reader.CloseStream();

    ASSERT_EQ(10u, decoded.GetCount());

    for (size_t i = 0; i < 10; i++)
    {
      ASSERT_EQ("hello " + boost::lexical_cast<std::string>(i), decoded.GetData(i));
      ASSERT_EQ("toto " + boost::lexical_cast<std::string>(i), decoded.GetHeaders(i)["content-type"]);

      if (i % 2 == 0)
      {
        ASSERT_EQ(2u, decoded.GetHeaders(i).size());
        ASSERT_TRUE(decoded.GetHeaders(i).find("content-length") != decoded.GetHeaders(i).end());
      }
    }
  }

}


int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
