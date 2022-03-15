#!/bin/bash
# Copyright 2022 PingCAP, Ltd.
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


set -ueox pipefail

scriptpath="$(
  cd "$(dirname "$0")"
  pwd -P
)"
SRCPATH=${1:-$(
  cd $scriptpath/../..
  pwd -P
)}

BUILD_BRANCH=${BUILD_BRANCH:-master}
NPROC=${NPROC:-$(nproc || grep -c ^processor /proc/cpuinfo)}
UPDATE_CCACHE=${UPDATE_CCACHE:-false}

source ${SRCPATH}/release-centos7-llvm/scripts/env.sh

if [[ ${CI_CCACHE_USED_SRCPATH} != ${SRCPATH} ]]; then
  rm -rf "${CI_CCACHE_USED_SRCPATH}"
  mkdir -p /build && cd /build
  cp -r ${SRCPATH} ${CI_CCACHE_USED_SRCPATH}
fi

UPDATE_CCACHE=${UPDATE_CCACHE} BUILD_BRANCH=${BUILD_BRANCH} sh ${CI_CCACHE_USED_SRCPATH}/release-centos7-llvm/scripts/tiflash-ut-coverage-prepare.sh
NPROC=${NPROC} sh ${CI_CCACHE_USED_SRCPATH}/release-centos7-llvm/scripts/tiflash-ut-coverage-build.sh
UPDATE_CCACHE=${UPDATE_CCACHE} BUILD_BRANCH=${BUILD_BRANCH} sh ${CI_CCACHE_USED_SRCPATH}/release-centos7-llvm/scripts/tiflash-ut-coverage-finish.sh
