// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <IO/AsynchronousWriteBuffer.h>
#include <IO/Compression/CompressedWriteBuffer.h>
#include <IO/ReadBufferFromFileDescriptor.h>
#include <IO/WriteBufferFromFileDescriptor.h>
#include <IO/copyData.h>

#include <iostream>


int main(int, char **)
try
{
    DB::ReadBufferFromFileDescriptor in1(STDIN_FILENO);
    DB::WriteBufferFromFileDescriptor out1(STDOUT_FILENO);
    DB::AsynchronousWriteBuffer out2(out1);
    DB::CompressedWriteBuffer out3(out2);

    DB::copyData(in1, out3);

    return 0;
}
catch (const DB::Exception & e)
{
    std::cerr << e.what() << ", " << e.displayText() << std::endl;
    return 1;
}
