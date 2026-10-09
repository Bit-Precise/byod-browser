// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/page_load_metrics/google/browser/google_url_util.h"

#include <algorithm>
#include <optional>
#include <string_view>

#include "base/strings/string_util.h"
#include "components/google/core/common/google_util.h"
#include "components/page_load_metrics/browser/page_load_metrics_util.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"

namespace page_load_metrics {

std::optional<std::string> GetGoogleHostnamePrefix(const GURL& url) {
    return std::nullopt;
}

bool IsGoogleHostname(const GURL& url) {
  return GetGoogleHostnamePrefix(url).has_value();
}

bool IsGoogleSearchHostname(const GURL& url) {
  return false;
}

bool IsProbablyGoogleSearchUrl(const GURL& url) {
  if (!page_load_metrics::IsGoogleSearchHostname(url)) {
    return false;
  }

  const std::string_view path = url.path();
  if (path == "/maps" || path.find("/maps/") != std::string_view::npos) {
    return false;
  }

  return true;
}

// Determine if the given url has query associated with it.
bool HasGoogleSearchQuery(const GURL& url) {
  // NOTE: we do not require 'q=' in the query, as AJAXy search may instead
  // store the query in the URL fragment.
  return QueryContainsComponentPrefix(url.query(), "q=") ||
         QueryContainsComponentPrefix(url.ref(), "q=");
}

bool IsGoogleSearchResultUrl(const GURL& url) {
  if (!IsGoogleSearchHostname(url)) {
    return false;
  }

  if (!HasGoogleSearchQuery(url)) {
    return false;
  }

  const std::string_view path = url.path();
  return path == "/search" || path == "/webhp" || path == "/custom" ||
         path == "/";
}

bool IsGoogleSearchHomepageUrl(const GURL& url) {
  if (!IsGoogleSearchHostname(url)) {
    return false;
  }

  const std::string_view path = url.path();
  if (path == "/webhp" || path == "/") {
    return true;
  }

  return (path == "/custom" || path == "/search") && !HasGoogleSearchQuery(url);
}

bool IsGoogleSearchRedirectorUrl(const GURL& url) {
  if (!IsGoogleSearchHostname(url)) {
    return false;
  }

  // The primary search redirector.  Google search result redirects are
  // differentiated from other general google redirects by 'source=web' in the
  // query string.
  if (url.path() == "/url" && url.has_query() &&
      QueryContainsComponent(url.query(), "source=web")) {
    return true;
  }

  // Intent-based navigations from search are redirected through a second
  // redirector, which receives its redirect URL in the fragment/hash/ref
  // portion of the URL (the portion after '#'). We don't check for the presence
  // of certain params in the ref since this redirector is only used for
  // redirects from search.
  return url.path() == "/searchurl/r.html" && url.has_ref();
}

// TODO(crbug.com/375343470): This is a trampoline to
// google_util::IsGoogleSearchPrewarmUrl. Remove this once callers are migrated
// to use google_util directly.
bool IsGoogleSearchPrewarmUrl(const GURL& url) {
  return google_util::IsGoogleSearchPrewarmUrl(url);
}

}  // namespace page_load_metrics
