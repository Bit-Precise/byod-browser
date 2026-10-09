// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/domain_reliability/google_configs.h"

#include <memory>

#include "base/containers/fixed_flat_map.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "net/base/url_util.h"

namespace domain_reliability {

std::unique_ptr<const DomainReliabilityConfig> MaybeGetGoogleConfig(
    const std::string& hostname) {
  return nullptr;
}

std::vector<std::unique_ptr<const DomainReliabilityConfig>>
GetAllGoogleConfigsForTesting() {
  std::vector<std::unique_ptr<const DomainReliabilityConfig>> configs_out;

  return configs_out;
}

}  // namespace domain_reliability
