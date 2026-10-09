// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/domain_reliability/service_factory.h"

#include "base/command_line.h"
#include "base/metrics/field_trial.h"
#include "chrome/common/chrome_switches.h"

namespace domain_reliability {

const char kUploadReporterString[] = "chrome";

bool ShouldCreateService(const DomainReliabilityServiceDelegate* delegate) {
  return false;
}

}  // namespace domain_reliability
