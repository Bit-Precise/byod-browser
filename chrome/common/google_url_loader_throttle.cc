// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/google_url_loader_throttle.h"

#include <optional>
#include <vector>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/metrics/histogram_macros.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/common/renderer_configuration.mojom.h"
#include "components/google/core/common/google_util.h"
#include "components/safe_search_api/safe_search_util.h"
#include "extensions/buildflags/buildflags.h"
#include "net/base/url_util.h"
#include "services/network/public/cpp/http_request_headers_update_params.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/mojom/x_frame_options.mojom.h"
#include "url/origin.h"

#if BUILDFLAG(ENABLE_BOUND_SESSION_CREDENTIALS)
#include "chrome/common/bound_session_request_throttled_handler.h"
#include "net/cookies/cookie_util.h"
#endif

namespace {
#if BUILDFLAG(IS_ANDROID)
const char kCCTClientDataHeader[] = "X-CCT-Client-Data";
#endif

#if BUILDFLAG(ENABLE_BOUND_SESSION_CREDENTIALS)
using RequestBoundSessionStatus =
    GoogleURLLoaderThrottle::RequestBoundSessionStatus;

RequestBoundSessionStatus GetRequestSingleBoundSessionStatus(
    const GURL& request_url,
    chrome::mojom::BoundSessionThrottlerParams* throttler_params) {
  CHECK(throttler_params);
  if (throttler_params->domain.empty()) {
    return RequestBoundSessionStatus::kNotCovered;
  }

  // Check if the request requires the short lived cookie.
  if (!request_url.DomainIs(
          net::cookie_util::CookieDomainAsHost(throttler_params->domain))) {
    return RequestBoundSessionStatus::kNotCovered;
  }

  if (!throttler_params->path.empty() &&
      !net::cookie_util::IsOnPath(throttler_params->path,
                                  request_url.GetPath())) {
    return RequestBoundSessionStatus::kNotCovered;
  }

  // Short lived cookie is fresh.
  if (throttler_params->cookie_expiry_date > base::Time::Now()) {
    return RequestBoundSessionStatus::kCoveredWithFreshCookie;
  }

  // Short lived cookie has expired.
  return RequestBoundSessionStatus::kCoveredWithMissingCookie;
}

void RecordBoundSessionStatusMetrics(bool was_deferred,
                                     bool is_main_frame_navigation,
                                     bool is_request_succeeded) {
  UMA_HISTOGRAM_BOOLEAN(
      "Signin.BoundSessionCredentials.CoveredRequestWasDeferred", was_deferred);
  if (is_request_succeeded) {
    UMA_HISTOGRAM_BOOLEAN(
        "Signin.BoundSessionCredentials.CoveredRequestWasDeferred.Success",
        was_deferred);
  } else {
    UMA_HISTOGRAM_BOOLEAN(
        "Signin.BoundSessionCredentials.CoveredRequestWasDeferred.Failure",
        was_deferred);
  }

  if (is_main_frame_navigation) {
    UMA_HISTOGRAM_BOOLEAN(
        "Signin.BoundSessionCredentials.CoveredNavigationRequestWasDeferred",
        was_deferred);
    if (is_request_succeeded) {
      UMA_HISTOGRAM_BOOLEAN(
          "Signin.BoundSessionCredentials.CoveredNavigationRequestWasDeferred."
          "Success",
          was_deferred);
    } else {
      UMA_HISTOGRAM_BOOLEAN(
          "Signin.BoundSessionCredentials.CoveredNavigationRequestWasDeferred."
          "Failure",
          was_deferred);
    }
  }
}
#endif  // BUILDFLAG(ENABLE_BOUND_SESSION_CREDENTIALS)
}  // namespace

// static
void GoogleURLLoaderThrottle::UpdateCorsExemptHeader(
    network::mojom::NetworkContextParams* params) {
  params->cors_exempt_header_list.push_back(
      safe_search_api::kGoogleAppsAllowedDomains);
  params->cors_exempt_header_list.push_back(
      safe_search_api::kYouTubeRestrictHeaderName);
#if BUILDFLAG(IS_ANDROID)
  params->cors_exempt_header_list.push_back(kCCTClientDataHeader);
#endif
}

GoogleURLLoaderThrottle::GoogleURLLoaderThrottle(
#if BUILDFLAG(IS_ANDROID)
    const std::string& client_data_header,
#endif
#if BUILDFLAG(ENABLE_BOUND_SESSION_CREDENTIALS)
    std::unique_ptr<BoundSessionRequestThrottledHandler>
        bound_session_request_throttled_handler,
#endif
    chrome::mojom::DynamicParamsPtr dynamic_params)
    :
#if BUILDFLAG(IS_ANDROID)
      client_data_header_(client_data_header),
#endif
#if BUILDFLAG(ENABLE_BOUND_SESSION_CREDENTIALS)
      bound_session_request_throttled_handler_(
          std::move(bound_session_request_throttled_handler)),
#endif
      dynamic_params_(std::move(dynamic_params)) {
}

GoogleURLLoaderThrottle::~GoogleURLLoaderThrottle() = default;

#if BUILDFLAG(ENABLE_BOUND_SESSION_CREDENTIALS)
// static
RequestBoundSessionStatus GoogleURLLoaderThrottle::GetRequestBoundSessionStatus(
    const GURL& request_url,
    const std::vector<chrome::mojom::BoundSessionThrottlerParamsPtr>&
        bound_session_throttler_params) {
  RequestBoundSessionStatus status = RequestBoundSessionStatus::kNotCovered;

  for (const auto& throttler_params : bound_session_throttler_params) {
    status = std::max(status, GetRequestSingleBoundSessionStatus(
                                  request_url, throttler_params.get()));
  }

  return status;
}
#endif  // BUILDFLAG(ENABLE_BOUND_SESSION_CREDENTIALS)

void GoogleURLLoaderThrottle::DetachFromCurrentSequence() {}

void GoogleURLLoaderThrottle::WillStartRequest(
    network::ResourceRequest* request,
    bool* defer) {
}

void GoogleURLLoaderThrottle::WillRedirectRequest(
    net::RedirectInfo* redirect_info,
    const network::mojom::URLResponseHead& response_head,
    bool* defer,
    network::HttpRequestHeadersUpdateParams* headers_update_params) {
}

void GoogleURLLoaderThrottle::WillProcessResponse(
    const GURL& response_url,
    network::mojom::URLResponseHead* response_head,
    bool* defer) {
}

#if BUILDFLAG(ENABLE_BOUND_SESSION_CREDENTIALS)
void GoogleURLLoaderThrottle::WillOnCompleteWithError(
    const network::URLLoaderCompletionStatus& status) {
  if (is_covered_by_bound_session_) {
    RecordBoundSessionStatusMetrics(is_deferred_for_bound_session_,
                                    is_main_frame_navigation_,
                                    /*is_request_succeeded=*/false);
  }
  if (deferred_request_resume_trigger_) {
    UMA_HISTOGRAM_ENUMERATION(
        "Signin.BoundSessionCredentials.DeferredRequestUnblockTrigger.Failure",
        *deferred_request_resume_trigger_);
  }
}

void GoogleURLLoaderThrottle::OnDeferRequestForBoundSessionCompleted(
    BoundSessionRequestThrottledHandler::UnblockAction unblock_action,
    chrome::mojom::ResumeBlockedRequestsTrigger resume_trigger) {
  // Use `PostTask` to avoid resuming the request before it has been deferred
  // then the request will hang. This can happen if
  // `BoundSessionRequestThrottledHandler::HandleRequestBlockedOnCookie()`
  // calls the callback synchronously.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&GoogleURLLoaderThrottle::ResumeOrCancelRequest,
                                weak_factory_.GetWeakPtr(), unblock_action,
                                resume_trigger));
}

void GoogleURLLoaderThrottle::ResumeOrCancelRequest(
    BoundSessionRequestThrottledHandler::UnblockAction unblock_action,
    chrome::mojom::ResumeBlockedRequestsTrigger resume_trigger) {
  CHECK(is_deferred_for_bound_session_);
  CHECK(bound_session_request_throttled_start_time_.has_value());
  base::TimeDelta duration =
      base::TimeTicks::Now() - *bound_session_request_throttled_start_time_;
  DEPRECATED_UMA_HISTOGRAM_MEDIUM_TIMES(
      "Signin.BoundSessionCredentials.DeferredRequestDelay", duration);
  if (is_main_frame_navigation_) {
    DEPRECATED_UMA_HISTOGRAM_MEDIUM_TIMES(
        "Signin.BoundSessionCredentials.DeferredNavigationRequestDelay",
        duration);
  }
  bound_session_request_throttled_start_time_ = std::nullopt;
  deferred_request_resume_trigger_ = resume_trigger;

  switch (unblock_action) {
    case BoundSessionRequestThrottledHandler::UnblockAction::kResume:
      delegate_->Resume();
      break;
    case BoundSessionRequestThrottledHandler::UnblockAction::kCancel:
      delegate_->CancelWithError(net::ERR_ABORTED);
      break;
  }
}
#endif
