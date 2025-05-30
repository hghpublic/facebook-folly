/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/observer/detail/Core.h>

#include <folly/ExceptionString.h>
#include <folly/observer/detail/ObserverManager.h>

namespace folly {
namespace observer_detail {

Core::VersionedData Core::getData() {
  if (!ObserverManager::DependencyRecorder::isActive()) {
    return data_.copy();
  }

  ObserverManager::DependencyRecorder::markDependency(shared_from_this());

  auto version = ObserverManager::getVersion();

  if (version_ >= version) {
    return data_.copy();
  }

  refresh(version);

  DCHECK_GE(version_, version);
  return data_.copy();
}

size_t Core::refresh(size_t version) {
  CHECK(ObserverManager::inManagerThread());

  ObserverManager::DependencyRecorder::markRefreshDependency(*this);
  SCOPE_EXIT {
    ObserverManager::DependencyRecorder::unmarkRefreshDependency(*this);
  };

  if (version_ >= version) {
    return versionLastChange_;
  }

  {
    std::lock_guard<SharedMutex> lgRefresh(refreshMutex_);

    // Recheck in case this code was already refreshed
    if (version_ >= version) {
      return versionLastChange_;
    }

    bool needRefresh = std::exchange(forceRefresh_, false) || version_ == 0;

    ObserverManager::DependencyRecorder dependencyRecorder(*this);

    // This can be run in parallel, but we expect most updates to propagate
    // bottom to top.
    dependencies_.withRLock([&](const Dependencies& dependencies) {
      for (const auto& dependency : dependencies) {
        try {
          if (dependency->refresh(version) > version_) {
            needRefresh = true;
            break;
          }
        } catch (...) {
          LOG(ERROR) << "Exception while checking dependencies for updates: "
                     << exceptionStr(current_exception());

          needRefresh = true;
          break;
        }
      }
    });

    if (!needRefresh) {
      version_ = version;
      return versionLastChange_;
    }

    try {
      VersionedData newData{
          creator_(), version, std::chrono::system_clock::now()};
      if (!newData.data) {
        throw std::logic_error("Observer creator returned nullptr.");
      }
      if (data_.copy().data != newData.data) {
        data_.swap(newData);
        versionLastChange_ = version;
      }
    } catch (...) {
      LOG(ERROR) << "Exception while refreshing Observer: "
                 << exceptionStr(current_exception());

      if (version_ == 0) {
        // Re-throw exception if this is the first time we run creator
        throw;
      }
    }

    version_ = version;

    if (versionLastChange_ != version) {
      return versionLastChange_;
    }

    auto newDependencies = dependencyRecorder.release();
    dependencies_.withWLock([&](Dependencies& dependencies) {
      for (const auto& dependency : newDependencies) {
        if (!dependencies.count(dependency)) {
          dependency->addDependent(this->shared_from_this());
        }
      }

      for (const auto& dependency : dependencies) {
        if (!newDependencies.count(dependency)) {
          dependency->maybeRemoveStaleDependents();
        }
      }

      dependencies = std::move(newDependencies);
    });
  }

  auto dstate = dependents_.copy();

  for (const auto& dependentWeak : dstate.deps) {
    if (auto dependent = dependentWeak.lock()) {
      ObserverManager::scheduleRefresh(std::move(dependent), version);
    }
  }

  return versionLastChange_;
}

void Core::setForceRefresh() {
  forceRefresh_ = true;
}

Core::Core(
    folly::Function<std::shared_ptr<const void>()> creator,
    CreatorContext creatorContext)
    : creator_(std::move(creator)),
      creatorContext_(std::move(creatorContext)) {}

Core::~Core() {
  dependencies_.withWLock([](const Dependencies& dependencies) {
    for (const auto& dependecy : dependencies) {
      dependecy->maybeRemoveStaleDependents();
    }
  });
}

Core::Ptr Core::create(
    folly::Function<std::shared_ptr<const void>()> creator,
    CreatorContext creatorContext) {
  auto core =
      Core::Ptr(new Core(std::move(creator), std::move(creatorContext)));
  return core;
}

void Core::addDependent(Core::WeakPtr dependent) {
  dependents_.withWLock([&](Dependents& dstate) {
    dstate.deps.push_back(std::move(dependent));
  });
}

void Core::maybeRemoveStaleDependents() {
  dependents_.withWLock([](Dependents& dstate) {
    auto& deps = dstate.deps;
    if (++dstate.numPotentiallyExpiredDependents < deps.size() / 4) {
      return;
    }
    auto const pred = [](auto const& d) { return d.expired(); };
    deps.erase(std::remove_if(deps.begin(), deps.end(), pred), deps.end());
    dstate.numPotentiallyExpiredDependents = 0;
  });
}
} // namespace observer_detail
} // namespace folly
