// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "chrome/browser/ui/webui/grips/grips_ui.h"

#include "base/functional/callback_helpers.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/uuid.h"
#include "base/strings/escape.h"
#include "base/supports_user_data.h"
#include "base/values.h"
#include "base/timer/timer.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/fullscreen/browser_window_fullscreen_controller.h"
#include "chrome/common/url_constants.h"
#include "chrome/grit/grips_resources.h"
#include "chrome/grit/grips_resources_map.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/byod_navigation_audit.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "net/base/url_util.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/base/base_window.h"
#include "ui/webui/webui_util.h"
#include "url/origin.h"

namespace {

void ConfigureExamObserver(content::WebContents* web_contents,
                           const std::string& source_host,
                           std::vector<url::Origin> allowed_origins,
                           std::optional<double> exam_ends_at_ms,
                           const std::string& audit_session_id,
                           const std::string& audit_token);
void DeactivateExamObserver(content::WebContents* web_contents);

std::string AuditURL(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS())
    return std::string();
  GURL::Replacements replacements;
  replacements.ClearUsername();
  replacements.ClearPassword();
  replacements.ClearQuery();
  replacements.ClearRef();
  GURL clean = url.ReplaceComponents(replacements);
  if (clean.spec().size() > 4096)
    return clean.DeprecatedGetOriginAsURL().spec();
  return clean.spec();
}

// Independent of the tab lifetime so a tab-close event can finish sending.
// Only in-memory, bounded retries; browser shutdown/OS kill is best effort.
class GripsAuditReporter : public base::RefCounted<GripsAuditReporter> {
 public:
  GripsAuditReporter(scoped_refptr<network::SharedURLLoaderFactory> factory,
                    std::string session_id, std::string token)
      : factory_(std::move(factory)), session_id_(std::move(session_id)),
        token_(std::move(token)),
        endpoint_("https://exam.cs.ac.cn/v1/sessions/" + session_id_ + "/events") {}

  const std::string& session_id() const { return session_id_; }

  void Report(base::DictValue event) {
    event.Set("client_event_id", base::Uuid::GenerateRandomV4().AsLowercaseString());
    event.Set("observed_at_ms", static_cast<double>(
        base::Time::Now().InMillisecondsSinceUnixEpoch()));
    if (pending_.size() >= 256) {
      LOG(WARNING) << "BYOD audit queue full; observation dropped";
      return;
    }
    pending_.push_back(std::move(event));
    if (!busy_)
      Send();
  }

 private:
  friend class base::RefCounted<GripsAuditReporter>;
  ~GripsAuditReporter() = default;

  void Send() {
    if (pending_.empty()) { busy_ = false; return; }
    busy_ = true;
    // A single event stays below the server's 64 KiB limit even for long
    // URLs. IDs remain stable across retries, including lost acknowledgments.
    base::ListValue events;
    events.Append(pending_.front().Clone());
    base::DictValue payload;
    payload.Set("events", std::move(events));
    // JSON's numeric type includes integers; omit the forced ".0" suffix on
    // our exactly representable Unix milliseconds for the int64 API field.
    auto body = base::WriteJsonWithOptions(
        payload, base::JSONWriter::OPTIONS_OMIT_DOUBLE_TYPE_PRESERVATION);
    CHECK(body);
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = endpoint_;
    request->method = "POST";
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
    request->redirect_mode = network::mojom::RedirectMode::kError;
    request->load_flags = net::LOAD_DISABLE_CACHE | net::LOAD_BYPASS_PROXY;
    request->headers.SetHeader("Authorization", "Bearer " + token_);
    static constexpr auto annotation = net::DefineNetworkTrafficAnnotation(
        "byod_exam_page_audit", R"(
          semantics {
            sender: "BYOD Exam Browser"
            description: "Reports page-only observations during an active exam."
            trigger: "Exam page navigation or leaving the exam."
            data: "Session bearer token, sanitized page URL, actual network route,
                   client timestamp and lifecycle reason. No query or fragment."
            destination: OTHER
            destination_other: "The trusted BYOD exam control service."
          }
          policy {
            cookies_allowed: NO
            setting: "Only active in the examination tab after exam start."
            policy_exception_justification: "Required exam audit."
          })");
    loader_ = network::SimpleURLLoader::Create(std::move(request), annotation);
    loader_->AttachStringForUpload(*body, "application/json");
    loader_->SetTimeoutDuration(base::Seconds(10));
    loader_->DownloadToString(factory_.get(),
        base::BindOnce(&GripsAuditReporter::OnComplete, base::WrapRefCounted(this)),
        1024);
  }

  void OnComplete(std::optional<std::string> body) {
    int status = 0;
    if (loader_->ResponseInfo() && loader_->ResponseInfo()->headers)
      status = loader_->ResponseInfo()->headers->response_code();
    const bool ok = loader_->NetError() == net::OK && status == 200;
    loader_.reset();
    if (!ok && (status == 0 || status == 429 || status >= 500) && ++retries_ <= 4) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&GripsAuditReporter::Send, base::WrapRefCounted(this)),
          base::Seconds(status == 429 ? 60 : 1 << retries_));
      return;
    }
    if (!ok)
      LOG(WARNING) << "BYOD audit delivery failed; HTTP status=" << status;
    pending_.pop_front();
    retries_ = 0;
    Send();
  }

  scoped_refptr<network::SharedURLLoaderFactory> factory_;
  const std::string session_id_;
  const std::string token_;
  const GURL endpoint_;
  std::deque<base::DictValue> pending_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  bool busy_ = false;
  int retries_ = 0;
};

// Tracks whether BYOD changed the browser window's fullscreen state so the
// previous state can be restored when the exam ends. This is profile-scoped,
// allowing the WebUI handler to be recreated when the tab navigates away from
// grips://exam.
class GripsFullscreenState : public base::SupportsUserData::Data {
 public:
  explicit GripsFullscreenState(bool restore_on_clear)
      : restore_on_clear(restore_on_clear) {}
  const bool restore_on_clear;
};

int kGripsFullscreenStateKey = 0;

void SetByodBrowserFullscreen(Profile* profile,
                              content::WebContents* web_contents,
                              bool required,
                              bool lock_fullscreen) {
  if (!profile || !web_contents)
    return;
  BrowserWindowInterface* browser =
      GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(web_contents);
  if (!browser || !browser->GetWindow())
    return;

  if (required) {
    BrowserWindowFullscreenController* fullscreen_controller =
        BrowserWindowFullscreenController::From(browser);
    if (!fullscreen_controller)
      return;

    // Heartbeats may refresh the native policy while the same exam remains
    // active. Keep the original state, but update the force-exit guard.
    if (profile->GetUserData(&kGripsFullscreenStateKey)) {
      fullscreen_controller->SetForceFullscreen(lock_fullscreen);
      if (lock_fullscreen && !browser->GetWindow()->IsFullscreen())
        chrome::ToggleFullscreenMode(browser, /*user_initiated=*/false);
      return;
    }

    const bool was_fullscreen = browser->GetWindow()->IsFullscreen();
    profile->SetUserData(
        &kGripsFullscreenStateKey,
        std::make_unique<GripsFullscreenState>(!was_fullscreen));
    fullscreen_controller->SetForceFullscreen(lock_fullscreen);
    if (!was_fullscreen)
      chrome::ToggleFullscreenMode(browser, /*user_initiated=*/false);
    return;
  }

  auto* state = static_cast<GripsFullscreenState*>(
      profile->GetUserData(&kGripsFullscreenStateKey));
  if (!state)
    return;
  const bool restore = state->restore_on_clear;
  if (auto* fullscreen_controller =
          BrowserWindowFullscreenController::From(browser)) {
    // Clear the guard before toggling out so the normal window implementation
    // is allowed to leave fullscreen when the exam ends.
    fullscreen_controller->SetForceFullscreen(false);
  }
  profile->SetUserData(&kGripsFullscreenStateKey, nullptr);
  if (restore && browser->GetWindow()->IsFullscreen())
    chrome::ToggleFullscreenMode(browser, /*user_initiated=*/false);
}

class GripsExamNetworkMessageHandler : public content::WebUIMessageHandler {
 public:
  GripsExamNetworkMessageHandler() = default;
  GripsExamNetworkMessageHandler(const GripsExamNetworkMessageHandler&) = delete;
  GripsExamNetworkMessageHandler& operator=(const GripsExamNetworkMessageHandler&) = delete;

  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        "setByodTunnelConfig",
        base::BindRepeating(&GripsExamNetworkMessageHandler::HandleSet,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "clearByodTunnelConfig",
        base::BindRepeating(&GripsExamNetworkMessageHandler::HandleClear,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        "setByodFullscreen",
        base::BindRepeating(
            &GripsExamNetworkMessageHandler::HandleFullscreen,
            base::Unretained(this)));
  }

 private:
  static bool ReadBytes(const base::DictValue& dict,
                        const char* key,
                        std::vector<uint8_t>* output) {
    const base::ListValue* list = dict.FindList(key);
    if (!list || list->empty() || list->size() > 64) {
      return false;
    }
    output->clear();
    output->reserve(list->size());
    for (const base::Value& value : *list) {
      std::optional<int> item = value.GetIfInt();
      if (!item || *item < 0 || *item > 255) {
        return false;
      }
      output->push_back(static_cast<uint8_t>(*item));
    }
    return true;
  }

  static bool ReadAllowedOrigins(const base::DictValue& dict,
                                 std::vector<url::Origin>* output) {
    output->clear();
    const base::ListValue* list = dict.FindList("allowedOrigins");
    // Older browser resources may omit the field. The observer still keeps
    // exam.cs.ac.cn and the configured source host as mandatory defaults.
    if (!list)
      return true;
    if (list->size() > 64)
      return false;
    output->reserve(list->size());
    for (const base::Value& value : *list) {
      const std::string* serialized = value.GetIfString();
      if (!serialized || serialized->size() > 2048)
        return false;
      GURL origin_url(*serialized);
      if (!origin_url.is_valid() || !origin_url.SchemeIsHTTPOrHTTPS() ||
          origin_url.has_username() || origin_url.has_password() ||
          !origin_url.query().empty() || !origin_url.ref().empty() ||
          (origin_url.path() != "" && origin_url.path() != "/")) {
        return false;
      }
      url::Origin origin = url::Origin::Create(origin_url);
      if (origin.opaque())
        return false;
      output->push_back(std::move(origin));
    }
    return true;
  }

  static bool ReadTunnelHosts(const base::DictValue& dict,
                              std::vector<std::string>* output) {
    output->clear();
    const base::ListValue* list = dict.FindList("tunnelHosts");
    if (!list || list->size() > 64)
      return false;
    output->reserve(list->size());
    for (const base::Value& value : *list) {
      const std::string* serialized = value.GetIfString();
      if (!serialized || serialized->empty() || serialized->size() > 253)
        return false;
      std::string host = base::ToLowerASCII(*serialized);
      GURL url("https://" + host);
      if (!url.is_valid() || url.host() != host || url.has_port() ||
          url.path() != "/" || !url.query().empty() || !url.ref().empty() ||
          url.has_username() || url.has_password())
        return false;
      output->push_back(std::move(host));
    }
    return true;
  }

  void HandleSet(const base::ListValue& args) {
    if (args.empty()) {
      return;
    }
    const base::DictValue* dict = args[0].GetIfDict();
    if (!dict) {
      return;
    }
    const std::string* source_host = dict->FindString("sourceHost");
    const std::string* proxy_host = dict->FindString("proxyHost");
    const std::string* endpoint_id = dict->FindString("endpointId");
    const std::string* ticket = dict->FindString("ticket");
    const std::string* audit_session_id = dict->FindString("auditSessionId");
    const std::string* audit_token = dict->FindString("auditToken");
    std::optional<double> exam_ends_at_ms = dict->FindDouble("examEndsAtMs");
    std::optional<int> proxy_port = dict->FindInt("proxyPort");
    std::vector<url::Origin> allowed_origins;
    std::vector<std::string> tunnel_hosts;
    if (!source_host || !proxy_host || !endpoint_id || !ticket ||
        !proxy_port || *proxy_port < 1 || *proxy_port > 65535 ||
        !ReadAllowedOrigins(*dict, &allowed_origins) ||
        !ReadTunnelHosts(*dict, &tunnel_hosts)) {
      return;
    }
    // The trusted HTTPS shell is the only bridge sender. Never allow it to
    // choose a reporting origin or inject headers/path segments.
    if (!audit_session_id || audit_session_id->empty() ||
        audit_session_id->size() > 128 ||
        audit_session_id->find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
        !audit_token || audit_token->empty() || audit_token->size() > 512 ||
        audit_token->find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
      return;
    Profile* profile = Profile::FromWebUI(web_ui());
    if (!profile) {
      return;
    }
    auto config = network::mojom::ByodTunnelConfig::New();
    config->source_host = base::JoinString(tunnel_hosts, ",");
    config->proxy_host = *proxy_host;
    config->proxy_port = static_cast<uint16_t>(*proxy_port);
    config->endpoint_id = *endpoint_id;
    config->ticket = *ticket;
    profile->GetDefaultStoragePartition()->GetNetworkContext()->
        SetByodTunnelConfig(std::move(config));
    ConfigureExamObserver(web_ui()->GetWebContents(), *source_host,
                           std::move(allowed_origins), exam_ends_at_ms,
                           *audit_session_id, *audit_token);
  }

  void HandleClear(const base::ListValue&) {
    Profile* profile = Profile::FromWebUI(web_ui());
    if (profile) {
      profile->GetDefaultStoragePartition()->GetNetworkContext()->
          SetByodTunnelConfig(nullptr);
    }
    DeactivateExamObserver(web_ui()->GetWebContents());
  }

  void HandleFullscreen(const base::ListValue& args) {
    if (args.empty())
      return;
    std::optional<bool> required = args[0].GetIfBool();
    if (!required)
      return;
    bool lock_fullscreen = false;
    if (args.size() > 1) {
      std::optional<bool> lock = args[1].GetIfBool();
      if (!lock)
        return;
      lock_fullscreen = *lock;
    }
    SetByodBrowserFullscreen(Profile::FromWebUI(web_ui()),
                              web_ui()->GetWebContents(), *required,
                              *required && lock_fullscreen);
  }
};

// The HTTPS exam shell owns the UI and OIDC flow. This observer only keeps the
// native tunnel/fullscreen controls alive after the shell navigates to the
// source site and returns violations/expiry to the HTTPS shell.
class GripsExamCallbackObserver
    : public content::WebContentsObserver,
      public content::WebContentsUserData<GripsExamCallbackObserver> {
 public:
  static void Arm(content::WebContents* web_contents) {
    CreateForWebContents(web_contents);
  }

  GripsExamCallbackObserver(const GripsExamCallbackObserver&) = delete;
  GripsExamCallbackObserver& operator=(const GripsExamCallbackObserver&) = delete;

 private:
  friend void ConfigureExamObserver(content::WebContents*,
                                    const std::string&,
                                    std::vector<url::Origin>,
                                    std::optional<double>,
                                    const std::string&, const std::string&);
  friend void DeactivateExamObserver(content::WebContents*);
  friend class content::WebContentsUserData<GripsExamCallbackObserver>;
  explicit GripsExamCallbackObserver(content::WebContents* web_contents)
      : content::WebContentsObserver(web_contents),
        content::WebContentsUserData<GripsExamCallbackObserver>(*web_contents) {}

  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (!handle->IsInPrimaryMainFrame())
      return;
    if (auditing_)
      ReportNavigation(handle, false);
    if (!handle->HasCommitted())
      return;
    const GURL callback = handle->GetURL();
    std::string ended;
    if (callback.scheme() == "grips" &&
        (callback.host() == "exam" || callback.host() == "exam.cs.ac.cn") &&
        net::GetValueForKeyInQuery(callback, "ended", &ended) && ended == "1") {
      Reset();
      return;
    }

    // The exam control document owns manual submission and violation
    // handling.  Once the source page is active, allow the trusted
    // grips://exam document to commit in this tab so its action query can
    // call /v1/exams/{id}/complete and clear the native policy.
    if (callback.scheme() == "grips" &&
        (callback.host() == "exam" || callback.host() == "exam.cs.ac.cn")) {
      LeaveExam("control_page");
      return;
    }

    // The OIDC callback returns to an ordinary HTTPS marker first. Chromium
    // can block an HTTP redirect directly from HTTPS to a custom scheme as an
    // external-protocol launch. This observer belongs to the original Grips
    // WebContents, so complete the same-tab return natively after the HTTPS
    // marker has committed.
    std::string auth_complete;
    if (callback.scheme() == "https" && callback.host() == "exam.cs.ac.cn" &&
        callback.path() == "/" &&
        net::GetValueForKeyInQuery(callback, "auth", &auth_complete) &&
        auth_complete == "1") {
      content::OpenURLParams params(
          GURL("grips://exam.cs.ac.cn/?auth=1"), content::Referrer(),
          WindowOpenDisposition::CURRENT_TAB,
          ui::PAGE_TRANSITION_AUTO_TOPLEVEL, false);
      web_contents()->OpenURL(params, base::NullCallback());
      return;
    }

    // Handle the trusted control-plane callback before the active-exam guard.
    // Otherwise a previous source navigation could bounce /byod/complete back
    // to itself instead of returning to the grips document in this tab.
    if (callback.scheme() == "https" && callback.host() == "exam.cs.ac.cn" &&
        callback.path() == "/byod/complete") {
      std::string target_spec;
      std::string session_id;
      if (!net::GetValueForKeyInQuery(callback, "target", &target_spec) ||
          !net::GetValueForKeyInQuery(callback, "session_id", &session_id) ||
          session_id.empty()) {
        return;
      }
      const GURL target(target_spec);
      if (!target.is_valid() || target.scheme() != "https" ||
          target.host() != "exam.cs.ac.cn" || target.path() == "/") {
        return;
      }
      control_url_ = target;
      LeaveExam("control_page");
      active_exam_ = false;
      target_url_ = GURL();
      const std::string exam_url =
          "grips://exam.cs.ac.cn/?target=" +
          base::EscapeQueryParamValue(target.spec(), true) +
          "&session_id=" + base::EscapeQueryParamValue(session_id, true);
      content::OpenURLParams params(
          GURL(exam_url), content::Referrer(),
          WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
          false);
      web_contents()->OpenURL(params, base::NullCallback());
      return;
    }

    // OIDC redirects and control-plane pages are allowed while establishing or
    // ending a session. The signed policy and native tunnel are enforced only
    // for the configured source host.
    if (IsAllowedNavigation(callback)) {
      // The source origin updates the restore target. Additional origins from
      // navigation.allowed_origins (for example IAaa) are transient auth or
      // other explicitly trusted pages and must not replace the exam target.
      if (!source_host_.empty() && callback.scheme() == "https" &&
          callback.host() == source_host_) {
        active_exam_ = true;
        target_url_ = callback;
      }
      return;
    }

    // Once the exam is active, a top-level navigation may not leave the
    // signed exam origin. This catches view-source:, file:, devtools:, and
    // ordinary external URLs after commit and restores the exam document.
    if (active_exam_ && !callback.is_empty()) {
      if (target_url_.is_valid()) {
        content::OpenURLParams params(
            target_url_, content::Referrer(),
            WindowOpenDisposition::CURRENT_TAB,
            ui::PAGE_TRANSITION_AUTO_TOPLEVEL, false);
        web_contents()->OpenURL(params, base::NullCallback());
      }
      return;
    }
  }

  void DidOpenRequestedURL(
      content::WebContents* new_contents,
      content::RenderFrameHost* source_render_frame_host,
      const GURL& url,
      const content::Referrer& referrer,
      WindowOpenDisposition disposition,
      ui::PageTransition transition,
      bool started_from_context_menu,
      bool renderer_initiated) override {
    if (active_exam_ && new_contents) {
      // Keep explicitly allowed authentication popups in this same tab so
      // they remain covered by this observer and cannot become an unmanaged
      // exam window. The source page's opener-based flow still gets the URL.
      if (IsAllowedNavigation(url)) {
        content::OpenURLParams params(
            url, referrer, WindowOpenDisposition::CURRENT_TAB,
            ui::PAGE_TRANSITION_AUTO_TOPLEVEL, started_from_context_menu);
        web_contents()->OpenURL(params, base::NullCallback());
      }
      new_contents->Close();
    }
  }

  void OnVisibilityChanged(content::Visibility visibility) override {
    if (!active_exam_ || visibility != content::Visibility::HIDDEN ||
        target_url_.is_empty()) {
      return;
    }
    // Closing a tab first makes it hidden. Defer the background path so a
    // synchronous tab destruction records tab_closed instead and never tries
    // to navigate a tab that is being torn down.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&GripsExamCallbackObserver::HandleBackground,
                       weak_factory_.GetWeakPtr()));
  }

  void HandleBackground() {
    if (!active_exam_ || web_contents()->IsBeingDestroyed() ||
        web_contents()->GetVisibility() != content::Visibility::HIDDEN)
      return;
    LeaveExam("background");
    const std::string exam_url =
        "grips://exam.cs.ac.cn/?target=" +
        base::EscapeQueryParamValue(
            control_url_.is_valid() ? control_url_.spec() : target_url_.spec(),
            true) +
        "&violation=background";
    active_exam_ = false;
    content::OpenURLParams params(
        GURL(exam_url), content::Referrer(),
        WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
        false);
    web_contents()->OpenURL(params, base::NullCallback());
  }

  void OnExamExpired() {
    LeaveExam("expired");
    Profile* profile = Profile::FromBrowserContext(
        web_contents()->GetBrowserContext());
    if (profile) {
      profile->GetDefaultStoragePartition()->GetNetworkContext()->
          SetByodTunnelConfig(nullptr);
      SetByodBrowserFullscreen(profile, web_contents(), false, false);
    }
    Reset();
    content::OpenURLParams params(
        GURL("grips://exam.cs.ac.cn/?ended=1"), content::Referrer(),
        WindowOpenDisposition::CURRENT_TAB, ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
        false);
    web_contents()->OpenURL(params, base::NullCallback());
  }

  bool active_exam_ = false;
  bool auditing_ = false;
  scoped_refptr<GripsAuditReporter> audit_;
  GURL target_url_;
  GURL control_url_;
  std::string source_host_;
  std::vector<url::Origin> allowed_origins_;
  base::OneShotTimer deadline_timer_;

  void DidRedirectNavigation(content::NavigationHandle* handle) override {
    if (auditing_ && handle->IsInPrimaryMainFrame())
      ReportNavigation(handle, true);
  }

  void WebContentsDestroyed() override { LeaveExam("tab_closed"); }

  void PrimaryMainFrameRenderProcessGone(base::TerminationStatus) override {
    LeaveExam("renderer_gone");
  }

  void ReportNavigation(content::NavigationHandle* handle, bool redirect) {
    GURL url = handle->GetURL();
    const auto& chain = handle->GetRedirectChain();
    if (redirect && chain.size() >= 2)
      url = chain[chain.size() - 2];
    const auto route = content::GetByodNavigationAudit(handle);
    base::DictValue event;
    event.Set("type", redirect ? "page_redirect" : "page_navigation");
    event.Set("url", AuditURL(url));
    if (redirect)
      event.Set("redirect_url", AuditURL(handle->GetURL()));
    event.Set("navigation_id", base::NumberToString(handle->GetNavigationId()));
    event.Set("route", url.SchemeIsHTTPOrHTTPS() ? route.route : "none");
    event.Set("route_reason", url.SchemeIsHTTPOrHTTPS() ? route.reason : "non_http");
    event.Set("outcome", redirect ? "redirect" :
        handle->GetNetErrorCode() == net::ERR_ABORTED ? "cancelled" :
        handle->GetNetErrorCode() != net::OK ? "failed" :
        handle->HasCommitted() ? "committed" : "cancelled");
    event.Set("net_error", handle->GetNetErrorCode());
    if (handle->GetResponseHeaders())
      event.Set("http_status", handle->GetResponseHeaders()->response_code());
    audit_->Report(std::move(event));
  }

  void LeaveExam(const char* reason) {
    if (!auditing_ || !audit_)
      return;
    base::DictValue event;
    event.Set("type", "exam_left");
    event.Set("reason", reason);
    event.Set("url", AuditURL(web_contents()->GetLastCommittedURL()));
    audit_->Report(std::move(event));
    auditing_ = false;
  }

  bool IsAllowedNavigation(const GURL& url) const {
    if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS())
      return false;
    if (url.scheme() == "https" && url.host() == "exam.cs.ac.cn")
      return true;
    if (!source_host_.empty() && url.scheme() == "https" &&
        url.host() == source_host_)
      return true;
    const url::Origin origin = url::Origin::Create(url);
    if (origin.opaque())
      return false;
    for (const url::Origin& allowed : allowed_origins_) {
      if (origin.IsSameOriginWith(allowed))
        return true;
    }
    return false;
  }

 public:
  void SetSourceHost(const std::string& source_host,
                     std::vector<url::Origin> allowed_origins,
                     std::optional<double> exam_ends_at_ms,
                     const std::string& audit_session_id,
                     const std::string& audit_token) {
    if (!audit_ || audit_->session_id() != audit_session_id) {
      LeaveExam("session_replaced");
      audit_ = base::MakeRefCounted<GripsAuditReporter>(
          web_contents()->GetBrowserContext()->GetDefaultStoragePartition()->
              GetURLLoaderFactoryForBrowserProcess(), audit_session_id, audit_token);
    }
    if (!auditing_) {
      auditing_ = true;
      base::DictValue event;
      event.Set("type", "exam_entered");
      event.Set("reason", "started");
      audit_->Report(std::move(event));
    }
    deadline_timer_.Stop();
    source_host_ = source_host;
    allowed_origins_ = std::move(allowed_origins);
    // The native policy is installed while the HTTPS shell is embedded in
    // this WebUI. Treat the shell itself as the safe restore target until the
    // source navigation commits and replaces it.
    active_exam_ = true;
    target_url_ = web_contents()->GetLastCommittedURL();
    control_url_ = target_url_;
    if (exam_ends_at_ms && *exam_ends_at_ms > 0) {
      const base::Time deadline =
          base::Time::FromMillisecondsSinceUnixEpoch(*exam_ends_at_ms);
      const base::TimeDelta delay = deadline - base::Time::Now();
      if (delay <= base::TimeDelta()) {
        OnExamExpired();
      } else {
        deadline_timer_.Start(
            FROM_HERE, delay,
            base::BindOnce(&GripsExamCallbackObserver::OnExamExpired,
                           base::Unretained(this)));
      }
    }
  }

  void Reset() {
    LeaveExam("policy_cleared");
    audit_.reset();
    deadline_timer_.Stop();
    active_exam_ = false;
    target_url_ = GURL();
    control_url_ = GURL();
    source_host_.clear();
    allowed_origins_.clear();
  }

  WEB_CONTENTS_USER_DATA_KEY_DECL();
  base::WeakPtrFactory<GripsExamCallbackObserver> weak_factory_{this};
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(GripsExamCallbackObserver);

void ConfigureExamObserver(content::WebContents* web_contents,
                           const std::string& source_host,
                           std::vector<url::Origin> allowed_origins,
                           std::optional<double> exam_ends_at_ms,
                           const std::string& audit_session_id,
                           const std::string& audit_token) {
  if (!web_contents)
    return;
  GripsExamCallbackObserver::CreateForWebContents(web_contents);
  if (auto* observer =
          GripsExamCallbackObserver::FromWebContents(web_contents)) {
    observer->SetSourceHost(source_host, std::move(allowed_origins),
                            exam_ends_at_ms, audit_session_id, audit_token);
  }
}

void DeactivateExamObserver(content::WebContents* web_contents) {
  if (!web_contents)
    return;
  if (auto* observer =
          GripsExamCallbackObserver::FromWebContents(web_contents)) {
    observer->Reset();
  }
}

}  // namespace

GripsExamUI::GripsExamUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui, /*enable_chrome_send=*/true) {
  web_ui->AddMessageHandler(std::make_unique<GripsExamNetworkMessageHandler>());
  const std::string host(
      web_ui->GetWebContents()->GetURL().host());
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      Profile::FromWebUI(web_ui), host == chrome::kGripsTrustedExamHost
          ? chrome::kGripsTrustedExamHost : chrome::kGripsExamHost);
  source->SetSupportedScheme(chrome::kGripsScheme);
  webui::SetupWebUIDataSource(source, kGripsResources,
                              IDR_GRIPS_BRIDGE_HTML);

  // Configuration and session requests are limited to the trusted exam
  // origin; the server returns only a short-lived browser session token.
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ConnectSrc,
      "connect-src https://exam.cs.ac.cn http://127.0.0.1:8787;");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ScriptSrc,
      "script-src grips://exam grips://exam.cs.ac.cn 'self';");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::FrameSrc,
      "frame-src https://exam.cs.ac.cn;");

  GripsExamCallbackObserver::Arm(web_ui->GetWebContents());
}

GripsExamUI::~GripsExamUI() = default;

WEB_UI_CONTROLLER_TYPE_IMPL(GripsExamUI)

GripsLoginUI::GripsLoginUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui, /*enable_chrome_send=*/false) {
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      Profile::FromWebUI(web_ui), chrome::kGripsLoginHost);
  source->SetSupportedScheme(chrome::kGripsScheme);
  webui::SetupWebUIDataSource(source, kGripsResources,
                              IDR_GRIPS_LOGIN_HTML);

  // login.ts performs a top-level navigation to Connect. Keep the page's
  // script and stylesheet scoped to the trusted login origin; Connect itself
  // is a normal HTTPS page after the navigation commits.
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ScriptSrc,
      "script-src grips://login 'self';");
}

GripsLoginUI::~GripsLoginUI() = default;

WEB_UI_CONTROLLER_TYPE_IMPL(GripsLoginUI)
