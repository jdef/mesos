/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ostream>
#include <string>

#include <gtest/gtest.h>

#include <process/gtest.hpp>

#include <stout/recordio.hpp>
#include <stout/result.hpp>
#include <stout/strings.hpp>

#include "common/recordio.hpp"

using process::Future;

using std::string;

using namespace mesos;
using namespace mesos::internal;


template <typename T>
bool operator==(const Result<T>& lhs, const Result<T>& rhs)
{
  if (lhs.isNone()) {
    return rhs.isNone();
  }

  if (lhs.isError()) {
    return rhs.isError() && rhs.error() == lhs.error();
  }

  return rhs.isSome() && lhs.get() == rhs.get();
}


template <typename T>
std::ostream& operator<<(std::ostream& out, const Result<T>& r)
{
  if (r.isNone()) {
    return out << "none";
  }

  if (r.isError()) {
    return out << "error(\"" << r.error() << "\")";
  }

  return out << r.get();
}


TEST(RecordIOReaderTest, EndOfFile)
{
  // Write some data to the pipe so that records
  // are available before any reads occur.
  ::recordio::Encoder<string> encoder(strings::upper);

  string data;

  data += encoder.encode("hello");
  data += encoder.encode("world!");

  process::http::Pipe pipe;
  pipe.writer().write(data);

  internal::recordio::Reader<string> reader(
      ::recordio::Decoder<string>(strings::lower),
      pipe.reader());

  AWAIT_EXPECT_EQ(Result<string>::some("hello"), reader.read());
  AWAIT_EXPECT_EQ(Result<string>::some("world!"), reader.read());

  // Have multiple outstanding reads before we close the pipe.
  Future<Result<string>> read1 = reader.read();
  Future<Result<string>> read2 = reader.read();

  EXPECT_TRUE(read1.isPending());
  EXPECT_TRUE(read2.isPending());

  pipe.writer().write(encoder.encode("goodbye"));
  pipe.writer().close();

  AWAIT_EXPECT_EQ(Result<string>::some("goodbye"), read1);
  AWAIT_EXPECT_EQ(Result<string>::none(), read2);

  // Subsequent reads should return EOF.
  AWAIT_EXPECT_EQ(Result<string>::none(), reader.read());
}


TEST(RecordIOReaderTest, DecodingFailure)
{
  ::recordio::Encoder<string> encoder(strings::upper);
  process::http::Pipe pipe;

  internal::recordio::Reader<string> reader(
      ::recordio::Decoder<string>(strings::lower),
      pipe.reader());

  // Have multiple outstanding reads before we fail the decoder.
  Future<Result<string>> read1 = reader.read();
  Future<Result<string>> read2 = reader.read();
  Future<Result<string>> read3 = reader.read();

  // Write non-encoded data to the pipe so that the decoder fails.
  pipe.writer().write(encoder.encode("encoded"));
  pipe.writer().write("not encoded!\n");

  AWAIT_EXPECT_EQ(Result<string>::some("encoded"), read1);
  AWAIT_EXPECT_FAILED(read2);
  AWAIT_EXPECT_FAILED(read3);

  // The reader is now in a failed state, subsequent
  // writes will be dropped and all reads will fail.
  pipe.writer().write(encoder.encode("encoded"));

  AWAIT_EXPECT_FAILED(reader.read());
}


TEST(RecordIOReaderTest, PipeFailure)
{
  ::recordio::Encoder<string> encoder(strings::upper);
  process::http::Pipe pipe;

  internal::recordio::Reader<string> reader(
      ::recordio::Decoder<string>(strings::lower),
      pipe.reader());

  // Have multiple outstanding reads before we fail the writer.
  Future<Result<string>> read1 = reader.read();
  Future<Result<string>> read2 = reader.read();
  Future<Result<string>> read3 = reader.read();

  // Write a record, then fail the pipe writer!
  pipe.writer().write(encoder.encode("encoded"));
  pipe.writer().fail("failure");

  AWAIT_EXPECT_EQ(Result<string>::some("encoded"), read1);
  AWAIT_EXPECT_FAILED(read2);
  AWAIT_EXPECT_FAILED(read3);

  // Subsequent reads should return a failure.
  AWAIT_EXPECT_FAILED(reader.read());
}
