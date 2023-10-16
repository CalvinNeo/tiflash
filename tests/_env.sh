#!/bin/bash
# Copyright 2023 PingCAP, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


# Executable path

# Try with some common build path
TIFLASH_PATH="dbms/src/Server/tiflash"
export storage_bin="/data4/luorongzhen/tidb-deploy/tiflash-5761/bin/tiflash/tiflash"

# Server address for connecting
export storage_server="127.0.0.1"

# Server port for connecting
export storage_port=5761

# Default database for scripts
export storage_db="system"

# TiDB address
export tidb_server="127.0.0.1"

# TiDB port
export tidb_port="5711"

# TiDB status port
export tidb_status_port="5712"

# TiDB default database
export tidb_db="test"

# TiDB default table
export tidb_table="t"

# Whether run scripts with verbose output
export verbose="false"
# export verbose="true"

# Setup running env vars
#source ../../_vars.sh
#setup_dylib_path

export LANG=en_US.utf-8
export LC_ALL=en_US.utf-8