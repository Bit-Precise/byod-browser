// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/translate/core/browser/translate_url_fetcher.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"
#include "components/translate/core/browser/translate_download_manager.h"
#include "components/variations/net/variations_http_headers.h"
#include "net/base/load_flags.h"
#include "net/http/http_status_code.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"

namespace translate {

namespace {

// Retry parameter for fetching.
const int kMaxRetry = 16;

}  // namespace

TranslateURLFetcherImpl::TranslateURLFetcherImpl(int max_retry_on_5xx)
    : state_(IDLE), retry_count_(0) {}

TranslateURLFetcherImpl::TranslateURLFetcherImpl(
    const net::HttpRequestHeaders& extra_request_header)
    : TranslateURLFetcherImpl(0) {
  extra_request_header_ = extra_request_header;
}

TranslateURLFetcherImpl::~TranslateURLFetcherImpl() = default;

TranslateURLFetcherImpl::State TranslateURLFetcherImpl::state() const {
  return state_;
}

bool TranslateURLFetcherImpl::Request(const GURL& url,
                                      TranslateUrlFetcher::Callback callback,
                                      bool is_incognito) {
  // No translation script or language list is fetched. Complete asynchronously
  // so callers do not retain a pending request forever.
  if (state_ == REQUESTING || retry_count_ >= kMaxRetry)
    return false;
  ++retry_count_;
  state_ = REQUESTING;
  callback_ = std::move(callback);
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
          [](base::WeakPtr<TranslateURLFetcherImpl> fetcher) {
            if (!fetcher)
              return;
            fetcher->state_ = FAILED;
            std::move(fetcher->callback_).Run(false, std::string());
          }, weak_factory_.GetWeakPtr()));
  return true;
}

void TranslateURLFetcherImpl::OnSimpleLoaderComplete(
    std::optional<std::string> response_body) {
  if (response_body) {
    DCHECK_EQ(net::OK, simple_loader_->NetError());
    state_ = COMPLETED;
  } else {
    state_ = FAILED;
  }

  simple_loader_.reset();

  std::move(callback_).Run(state_ == COMPLETED,
                           std::move(response_body).value_or(""));
}

}  // namespace translate
