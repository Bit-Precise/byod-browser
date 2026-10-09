// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/geolocation/network_location_request.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace device {

LocationResponseResult::LocationResponseResult(
    mojom::GeopositionResultPtr position,
    NetworkLocationRequestResult result_code,
    mojom::NetworkLocationResponsePtr raw_response)
    : position(std::move(position)),
      result_code(result_code),
      raw_response(std::move(raw_response)) {}

LocationResponseResult::LocationResponseResult(LocationResponseResult&& other) =
    default;
LocationResponseResult& LocationResponseResult::operator=(
    LocationResponseResult&& other) = default;
LocationResponseResult::~LocationResponseResult() = default;

NetworkLocationRequest::NetworkLocationRequest(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    const std::string& api_key,
    LocationResponseCallback callback)
    : location_response_callback_(std::move(callback)) {}

NetworkLocationRequest::~NetworkLocationRequest() = default;

void NetworkLocationRequest::MakeRequest(
    const WifiData& wifi_data,
    const base::Time& wifi_timestamp,
    const net::PartialNetworkTrafficAnnotationTag& partial_traffic_annotation,
    NetworkLocationRequestSource network_location_request_source) {
  // Google's Wi-Fi/IP location service is absent. Preserve the asynchronous
  // completion and replacement/cancellation contract without serializing or
  // uploading access-point identifiers. Native OS location is unaffected.
  weak_factory_.InvalidateWeakPtrs();
  request_pending_ = true;
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&NetworkLocationRequest::CompleteUnavailable,
                               weak_factory_.GetWeakPtr(), wifi_data));
}

void NetworkLocationRequest::CompleteUnavailable(WifiData wifi_data) {
  request_pending_ = false;
  auto error = mojom::GeopositionError::New();
  error->error_code = mojom::GeopositionErrorCode::kPositionUnavailable;
  error->error_message = "Google network location is unavailable in BYOD Browser.";
  location_response_callback_.Run(
      LocationResponseResult(mojom::GeopositionResult::NewError(std::move(error)),
                             NetworkLocationRequestResult::kNetworkError,
                             nullptr),
      wifi_data);
}

std::vector<mojom::AccessPointDataPtr>
NetworkLocationRequest::GetRequestDataForDiagnostics() const {
  return {};
}

}  // namespace device
