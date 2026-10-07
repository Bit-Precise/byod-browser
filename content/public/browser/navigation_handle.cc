// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/byod_navigation_audit.h"

#include <utility>

#include "content/browser/renderer_host/navigation_request.h"
#include "content/browser/renderer_host/navigator.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "net/ssl/ssl_info.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace content {

ByodNavigationAudit GetByodNavigationAudit(NavigationHandle* handle) {
  if (handle->IsSameDocument())
    return {"none", "same_document"};
  if (handle->IsServedFromBackForwardCache())
    return {"none", "back_forward_cache"};
  // A failed final request can still retain the previous redirect response.
  // Do not mistake that hop's route for the failed destination's route.
  if (handle->GetNetErrorCode() != 0)
    return {};
  const auto* response = NavigationRequest::From(handle)->response();
  if (!response)
    return {};
  // A service worker may synthesize a response or issue a different fetch.
  // Its response proxy chain does not reliably describe this navigation.
  if (response->was_fetched_via_service_worker)
    return {"unknown", "service_worker"};
  if (!response->network_accessed) {
    if (response->was_fetched_via_cache || response->was_in_prefetch_cache)
      return {"none", "cache"};
    return {"none", "unknown"};
  }
  const auto& chain = response->proxy_chain;
  if (!chain.IsValid())
    return {"unknown", "network"};
  if (chain.is_direct())
    return {"direct", "network"};
  for (const auto& proxy : chain.proxy_servers()) {
    if (proxy.host_port_pair().host() == "byod-xhttp.invalid" &&
        proxy.host_port_pair().port() == 1)
      return {"tunnel", "network"};
  }
  return {"proxy", "network"};
}

WebContents* NavigationHandle::GetWebContents() {
  // The NavigationRequest cannot access the WebContentsImpl as it would be a
  // layering violation, hence the cast here.
  return static_cast<WebContentsImpl*>(
      NavigationRequest::From(this)->GetDelegate());
}

}  // namespace content
