/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2023 Osimis S.A., Belgium
 * Copyright (C) 2024-2026 Orthanc Team SRL, Belgium
 * Copyright (C) 2021-2026 Sebastien Jodogne, ICTEAM UCLouvain, Belgium
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

#include <boost/thread/mutex.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <deque>
#include <stdint.h>


template <typename T>
class WeightedAverageMetrics : public boost::noncopyable
{
  struct Value
  {
    boost::posix_time::ptime time_;
    T                        value_;
    T                        weight_;

    Value(const T& value, const T& weight) :
    time_(boost::posix_time::microsec_clock::universal_time()),
    value_(value),
    weight_(weight)
    {
    }
  };

private:
  std::deque<Value>          values_;
  T                          totalWeightedValue_;
  T                          totalWeight_;
  int64_t                    duration_;
  boost::mutex               mutex_;

  void RemoveOldest()
  {
    // note: the mutex must be locked
    boost::posix_time::ptime now = boost::posix_time::microsec_clock::universal_time();

    if (values_.size() > 0)
    {
      Value& oldest = values_.front();
      while ((now - oldest.time_).total_seconds() > duration_)
      {
        totalWeightedValue_ -= oldest.value_ * oldest.weight_;
        totalWeight_ -= oldest.weight_;
        values_.pop_front();
        if (values_.size() > 0)
        {
          oldest = values_.front();
        }
      }
    }
  }

public:
  WeightedAverageMetrics(int64_t duration) :
    totalWeightedValue_(0),
    totalWeight_(0),
    duration_(duration)
  {
  }

  void AddValue(const T& value, const T& weight)
  {
    boost::mutex::scoped_lock lock(mutex_);

    values_.push_back(Value(value, weight));
    totalWeightedValue_ += value * weight;
    totalWeight_ += weight;
    
    RemoveOldest();
  }

  T GetAverage()
  {
    boost::mutex::scoped_lock lock(mutex_);

    RemoveOldest();

    if (totalWeight_ > 0)
    {
      return totalWeightedValue_ / totalWeight_;
    }
    else
    {
      return 0;
    }
  }
};

