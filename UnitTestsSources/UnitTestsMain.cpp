/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
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


#include <gtest/gtest.h>
#include <boost/lexical_cast.hpp>

#include "../Plugin/Configuration.h"
#include "../Plugin/Plugin.h"

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

  ParseContentType(c, a, "");
  ASSERT_TRUE(c.empty());
  ASSERT_EQ(0u, a.size());  

  ParseContentType(c, a, "multipart/related");
  ASSERT_EQ(c, "multipart/related");
  ASSERT_EQ(0u, a.size());
}


int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
