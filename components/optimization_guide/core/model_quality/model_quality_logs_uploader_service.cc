// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/optimization_guide/core/model_quality/model_quality_logs_uploader_service.h"

#include <string>

#include "base/command_line.h"
#include "base/memory/scoped_refptr.h"
#include "base/metrics/histogram_functions.h"
#include "base/strings/strcat.h"
#include "base/trace_event/trace_event.h"
#include "components/optimization_guide/core/access_token_helper.h"
#include "components/optimization_guide/core/feature_registry/mqls_feature_registry.h"
#include "components/optimization_guide/core/model_execution/feature_keys.h"
#include "components/optimization_guide/core/model_execution/model_execution_prefs.h"
#include "components/optimization_guide/core/model_quality/model_quality_log_entry.h"
#include "components/optimization_guide/core/model_quality/model_quality_util.h"
#include "components/optimization_guide/core/optimization_guide_enums.h"
#include "components/optimization_guide/core/optimization_guide_features.h"
#include "components/optimization_guide/core/optimization_guide_logger.h"
#include "components/optimization_guide/core/optimization_guide_switches.h"
#include "components/optimization_guide/core/optimization_guide_util.h"
#include "components/optimization_guide/optimization_guide_buildflags.h"
#include "components/optimization_guide/proto/model_quality_metadata.pb.h"
#include "components/optimization_guide/proto/model_quality_service.pb.h"
#include "components/prefs/pref_service.h"
#include "components/variations/net/variations_http_headers.h"
#include "net/base/url_util.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace optimization_guide {

namespace {

const char kOptimizationGuideServiceModelQualityDefaultURL[] =
    "https://chromemodelquality-pa.googleapis.com/v1:LogAiData";

// Sets user feedback for the ModelExecutionFeature corresponding to the
// `log_entry`.
void RecordUserFeedbackHistogram(proto::LogAiDataRequest* log_ai_data_request) {
  const MqlsFeatureMetadata* metadata =
      MqlsFeatureRegistry::GetInstance().GetFeature(
          log_ai_data_request->feature_case());
  CHECK(metadata);
  proto::UserFeedback user_feedback =
      metadata->get_user_feedback_callback().Run(*log_ai_data_request);
  base::UmaHistogramEnumeration(
      base::StrCat(
          {"OptimizationGuide.ModelQuality.UserFeedback.", metadata->name()}),
      static_cast<ModelQualityUserFeedback>(user_feedback));
}

}  // namespace

GURL GetModelQualityLogsUploaderServiceURL() {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kModelQualityServiceURL)) {
    return GURL(
        command_line->GetSwitchValueASCII(switches::kModelQualityServiceURL));
  }
  return GURL(kOptimizationGuideServiceModelQualityDefaultURL);
}

ModelQualityLogsUploaderService::ModelQualityLogsUploaderService(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    PrefService* pref_service)
    : model_quality_logs_uploader_service_url_(
          net::AppendOrReplaceQueryParameter(
              GetModelQualityLogsUploaderServiceURL(),
              "key",
              switches::GetModelQualityServiceAPIKey())),
      pref_service_(pref_service),
      url_loader_factory_(url_loader_factory) {
  CHECK(model_quality_logs_uploader_service_url_.SchemeIs(url::kHttpsScheme));
}

ModelQualityLogsUploaderService::~ModelQualityLogsUploaderService() = default;

bool ModelQualityLogsUploaderService::CanUploadLogs(
    const MqlsFeatureMetadata* metadata) {
  return false;
}

void ModelQualityLogsUploaderService::SetSystemMetadata(
    proto::LoggingMetadata* logging_metadata) {}

proto::PerformanceClass ModelQualityLogsUploaderService::GetPerformanceClass() {
  return proto::PERFORMANCE_CLASS_UNSPECIFIED;
}

void ModelQualityLogsUploaderService::SetUrlLoaderFactoryForTesting(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory) {
  url_loader_factory_ = url_loader_factory;
}

void ModelQualityLogsUploaderService::UploadModelQualityLogs(
    std::unique_ptr<proto::LogAiDataRequest> log_ai_data_request) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Don't do anything if the data is null or no feature is set.
  if (!log_ai_data_request) {
    return;
  }
  proto::LogAiDataRequest::FeatureCase feature =
      log_ai_data_request->feature_case();
  if (feature == proto::LogAiDataRequest::FeatureCase::FEATURE_NOT_SET) {
    return;
  }

  // Log User Feedback Histogram corresponding to the LogAiDataRequest.
  RecordUserFeedbackHistogram(log_ai_data_request.get());

  const MqlsFeatureMetadata* metadata =
      MqlsFeatureRegistry::GetInstance().GetFeature(feature);
  CHECK(metadata);
  TRACE_EVENT1("browser",
               "ModelQualityLogsUploaderService::UploadModelQualityLogs",
               "feature", metadata->name());

  // Set the client id for logging if non-zero.
  proto::LoggingMetadata* logging_metadata =
      log_ai_data_request->mutable_logging_metadata();
  int64_t client_id = GetOrCreateModelQualityClientId(feature, pref_service_);
  if (client_id != 0) {
    logging_metadata->set_client_id(client_id);
  }

  SetSystemMetadata(logging_metadata);

  proto::PerformanceClass perf_class = GetPerformanceClass();
  if (perf_class != proto::PERFORMANCE_CLASS_UNSPECIFIED) {
    logging_metadata->mutable_on_device_system_profile()->set_performance_class(
        perf_class);
  }

  UploadFinalizedLog(std::move(log_ai_data_request), feature);
}

void ModelQualityLogsUploaderService::UploadFinalizedLog(
    std::unique_ptr<proto::LogAiDataRequest> log,
    proto::LogAiDataRequest::FeatureCase feature) {
  // Google model-quality telemetry is not part of BYOD.
}

void ModelQualityLogsUploaderService::SetMqlsLogForWebUI(
    optimization_guide_internals::mojom::MqlsLogPtr log) {
  mqls_logs_for_web_ui_.push_back(std::move(log));
}

std::vector<optimization_guide_internals::mojom::MqlsLogPtr>
ModelQualityLogsUploaderService::GetMqlsLogsForWebUI() {
  return std::move(mqls_logs_for_web_ui_);
}

}  // namespace optimization_guide
