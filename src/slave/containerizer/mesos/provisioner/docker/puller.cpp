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

#include "slave/containerizer/mesos/provisioner/docker/puller.hpp"

#include <vector>

#include <stout/os.hpp>

#include <process/subprocess.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/local_puller.hpp"
#include "slave/containerizer/mesos/provisioner/docker/registry_puller.hpp"

using std::pair;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Subprocess;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

Try<Owned<Puller>> Puller::create(const Flags& flags)
{
  const string puller = flags.docker_puller;

  if (puller == "local") {
    return Owned<Puller>(new LocalPuller(flags));
  }

  if (puller == "registry") {
    Try<Owned<Puller>> puller = RegistryPuller::create(flags);
    if (puller.isError()) {
      return Error("Failed to create registry puller: " + puller.error());
    }

    return puller.get();
  }

  return Error("Unknown or unsupported docker puller: " + puller);
}


Future<Nothing> untar(const string& file, const string& directory)
{
  const vector<string> argv = {
    "tar",
    "-C",
    directory,
    "-x",
    "-f",
    file
  };

  Try<Subprocess> s = subprocess(
      "tar",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"));

  if (s.isError()) {
    return Failure(
        "Failed to create untar subprocess for file '" +
        file + "': " + s.error());
  }

  return s.get().status()
    .then([file](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure(
            "Failed to reap untar subprocess for file '" + file + "'");
      }

      if (!WIFEXITED(status.get()) ||
          WEXITSTATUS(status.get()) != 0) {
        return Failure(
            "Untar process for file '" + file + "' failed with exit code: " +
            WSTRINGIFY(status.get()));
      }

      return Nothing();
    });
}


Future<pair<string, string>> untarLayer(
    const string& layerPath,
    const string& directory,
    const string& layerId)
{
  // We untar the layer from source into a directory, then move the
  // layer into store. We do this instead of untarring directly to
  // store to make sure we don't end up with partially untarred layer
  // rootfs.

  const string localRootfsPath =
    paths::getImageArchiveLayerRootfsPath(directory, layerId);

  // Image layer has been untarred but is not present in the store directory.
  if (os::exists(localRootfsPath)) {
    LOG(WARNING) << "Image layer '" << layerId << "' rootfs present in staging "
                 << "directory but not in store directory '"
                 << localRootfsPath << "'. Removing staged rootfs and untarring"
                 << "layer again.";

    Try<Nothing> rmdir = os::rmdir(localRootfsPath);
    if (rmdir.isError()) {
      return Failure("Failed to remove incomplete staged rootfs for layer '" +
                     layerId + "': " + rmdir.error());
    }
  }

  Try<Nothing> mkdir = os::mkdir(localRootfsPath);
  if (mkdir.isError()) {
    return Failure("Failed to create rootfs path '" + localRootfsPath +
                   "': " + mkdir.error());
  }

  // The tar file will be removed when the staging directory is
  // removed.
  return untar(
      layerPath,
      localRootfsPath)
    .then([directory, layerId]() -> Future<pair<string, string>> {
      const string rootfsPath =
        paths::getImageArchiveLayerRootfsPath(directory, layerId);

      if (!os::exists(rootfsPath)) {
        return Failure("Failed to find the rootfs path after extracting layer"
                       " '" + layerId + "'");
      }

      return pair<string, string>(layerId, rootfsPath);
    });
}


} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
