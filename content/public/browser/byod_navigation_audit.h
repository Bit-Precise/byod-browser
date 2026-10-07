// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_BYOD_NAVIGATION_AUDIT_H_
#define CONTENT_PUBLIC_BROWSER_BYOD_NAVIGATION_AUDIT_H_

#include "content/common/content_export.h"

namespace content {
class NavigationHandle;

struct ByodNavigationAudit {
  const char* route = "unknown";
  const char* reason = "not_connected";
};

// Read response metadata without adding a method to the widely included
// NavigationHandle interface. Only call during DidRedirect/FinishNavigation.
CONTENT_EXPORT ByodNavigationAudit GetByodNavigationAudit(
    NavigationHandle* handle);
}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_BYOD_NAVIGATION_AUDIT_H_
