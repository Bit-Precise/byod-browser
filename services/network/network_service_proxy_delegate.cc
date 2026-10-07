// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/network_service_proxy_delegate.h"

#include <algorithm>
#include <string>
#include <vector>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "base/types/expected.h"
#include "base/uuid.h"
#include "net/base/completion_once_callback.h"
#include "net/base/elements_upload_data_stream.h"
#include "net/base/features.h"
#include "net/base/host_port_pair.h"
#include "net/base/io_buffer.h"
#include "net/base/isolation_info.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/base/proxy_chain.h"
#include "net/base/proxy_server.h"
#include "net/base/transport_info.h"
#include "net/base/upload_bytes_element_reader.h"
#include "net/base/url_util.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/log/net_log_source_type.h"
#include "net/log/net_log_with_source.h"
#include "net/proxy_resolution/proxy_info.h"
#include "net/proxy_resolution/proxy_resolution_service.h"
#include "net/proxy_resolution/proxy_retry_info.h"
#include "net/socket/stream_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "services/network/url_loader.h"
#include "url/url_constants.h"

namespace network {

class ByodXHTTPStreamSocket;

class ByodXHTTPTransport : public base::RefCounted<ByodXHTTPTransport> {
 public:
  ByodXHTTPTransport(GURL endpoint,
                     std::string uuid,
                     net::URLRequestContext* context)
      : endpoint(std::move(endpoint)),
        uuid(std::move(uuid)),
        context(context),
        isolation(net::IsolationInfo::CreateTransient(std::nullopt)) {}
  void Revoke();
  const GURL endpoint;
  const std::string uuid;
  const raw_ptr<net::URLRequestContext> context;
  const net::IsolationInfo isolation;
  bool active = true;
  std::vector<base::WeakPtr<ByodXHTTPStreamSocket>> sockets;

 private:
  friend class base::RefCounted<ByodXHTTPTransport>;
  ~ByodXHTTPTransport() = default;
};

// VLESS v0 / encryption=none / flow="" on XHTTP packet-up. This is the
// transport below Chromium's source SSLClientSocket, not an HTTP proxy on the
// wire. No loopback listener, CONNECT, MITM certificate or renderer socket.
class ByodXHTTPStreamSocket final : public net::StreamSocket,
                                    public net::URLRequest::Delegate {
 public:
  ByodXHTTPStreamSocket(scoped_refptr<ByodXHTTPTransport> state,
                        net::HostPortPair destination)
      : state_(std::move(state)),
        destination_(std::move(destination)),
        log_(net::NetLogWithSource::Make(
            state_ ? state_->context->net_log() : nullptr,
            net::NetLogSourceType::SOCKET)),
        download_buffer_(base::MakeRefCounted<net::IOBufferWithSize>(32768)),
        response_buffer_(base::MakeRefCounted<net::IOBufferWithSize>(1024)) {}
  ~ByodXHTTPStreamSocket() override { Disconnect(); }

  int Connect(net::CompletionOnceCallback callback) override {
    if (connected_) {
      return net::OK;
    }
    if (!state_ || !state_->active || !state_->endpoint.SchemeIs("https") ||
        !base::Uuid::ParseCaseInsensitive(state_->uuid).is_valid() ||
        destination_.host().empty() || destination_.host().size() > 253 ||
        !destination_.port()) {
      return net::ERR_ACCESS_DENIED;
    }
    error_ = net::OK;
    std::erase_if(state_->sockets, [](const auto& p) { return !p; });
    state_->sockets.push_back(weak_.GetWeakPtr());
    session_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
    connect_callback_ = std::move(callback);
    deadline_.Start(FROM_HERE, base::Seconds(25),
                    base::BindOnce(&ByodXHTTPStreamSocket::Fail,
                                   weak_.GetWeakPtr(), net::ERR_TIMED_OUT));
    Post(&ByodXHTTPStreamSocket::Start);
    return net::ERR_IO_PENDING;
  }
  void Disconnect() override {
    weak_.InvalidateWeakPtrs();
    deadline_.Stop();
    upload_deadline_.Stop();
    download_.reset();
    upload_.reset();
    connect_callback_.Reset();
    read_callback_.Reset();
    write_callback_.Reset();
    read_buffer_.reset();
    incoming_.clear();
    connected_ = download_ready_ = header_sent_ = reading_ = false;
    response_header_bytes_ = 0;
    sequence_ = 0;
    received_ = 0;
    error_ = net::ERR_SOCKET_NOT_CONNECTED;
  }
  void Revoke() { Fail(net::ERR_ABORTED); }
  bool IsConnected() const override { return connected_ && state_->active; }
  bool IsConnectedAndIdle() const override {
    return IsConnected() && incoming_.empty() && !reading_ && !upload_;
  }
  int GetPeerAddress(net::IPEndPoint* out) const override {
    if (!IsConnected()) {
      return net::ERR_SOCKET_NOT_CONNECTED;
    }
    *out = peer_;
    return net::OK;
  }
  int GetLocalAddress(net::IPEndPoint*) const override {
    return net::ERR_NOT_IMPLEMENTED;
  }
  const net::NetLogWithSource& NetLog() const override { return log_; }
  bool WasEverUsed() const override { return used_; }
  net::NextProto GetNegotiatedProtocol() const override {
    return net::NextProto::kProtoUnknown;
  }
  bool GetSSLInfo(net::SSLInfo*) override { return false; }
  int64_t GetTotalReceivedBytes() const override { return received_; }
  void ApplySocketTag(const net::SocketTag&) override {}
  int SetReceiveBufferSize(int32_t) override { return net::OK; }
  int SetSendBufferSize(int32_t) override { return net::OK; }

  int Read(net::IOBuffer* buffer,
           int length,
           net::CompletionOnceCallback callback) override {
    if (!IsConnected()) {
      return error_ ? error_ : net::ERR_SOCKET_NOT_CONNECTED;
    }
    DCHECK(!read_callback_);
    used_ = true;
    if (!incoming_.empty()) {
      return CopyIncoming(buffer, length);
    }
    read_buffer_ = buffer;
    read_length_ = length;
    read_callback_ = std::move(callback);
    Post(&ByodXHTTPStreamSocket::PumpRead);
    return net::ERR_IO_PENDING;
  }
  int Write(net::IOBuffer* buffer,
            int length,
            net::CompletionOnceCallback callback,
            const net::NetworkTrafficAnnotationTag&) override {
    if (!IsConnected()) {
      return error_ ? error_ : net::ERR_SOCKET_NOT_CONNECTED;
    }
    DCHECK(!write_callback_ && !upload_);
    used_ = true;
    write_length_ = std::min(length, 65536);
    write_callback_ = std::move(callback);
    SendPacket(std::string(buffer->data(), write_length_));
    return net::ERR_IO_PENDING;
  }

 private:
  static constexpr auto kAnnotation =
      net::DefineNetworkTrafficAnnotation("byod_exam_xhttp_tunnel", R"(
        semantics {
          sender: "BYOD Exam Browser"
          description: "Transports VLESS exam streams using HTTPS XHTTP."
          trigger: "An active exam requests an explicitly listed tunnel host."
          data: "Temporary VLESS UUID, target and opaque source TLS bytes."
          destination: OTHER
          destination_other: "The trusted BYOD exam HTTPS service."
        }
        policy {
          cookies_allowed: NO
          setting: "Controlled by the active exam; revoked on exam end."
          policy_exception_justification: "BYOD exam transport."
        })");

  void Post(void (ByodXHTTPStreamSocket::*method)()) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(method, weak_.GetWeakPtr()));
  }
  std::unique_ptr<net::URLRequest> MakeRequest(const std::string& suffix) {
    // Padding is transport metadata, not authentication. This is the default
    // Xray query fallback when no Referer is present.
    GURL url(state_->endpoint.spec() + session_ + suffix +
             "?x_padding=" + std::string(128, 'X'));
    auto request = state_->context->CreateRequest(
        url, net::MAXIMUM_PRIORITY, this, kAnnotation,
        net::handles::kInvalidNetworkHandle);
    request->set_disallow_credentials();
    request->set_isolation_info(state_->isolation);
    // The outer request must never recursively enter this tunnel. Also keep
    // session traffic out of the profile HTTP cache. Long-lived downloads
    // must not consume all HTTP/1.1 slots and starve uploads. Chromium's inner
    // socket pool already limits exam connections; only the transport's own
    // HTTP requests bypass the outer pool limit (requires MAXIMUM_PRIORITY).
    request->SetLoadFlags(request->load_flags() | net::LOAD_BYPASS_PROXY |
                          net::LOAD_DISABLE_CACHE |
                          net::LOAD_IGNORE_LIMITS);
    return request;
  }
  void Start() {
    if (!state_->active) {
      Fail(net::ERR_ABORTED);
      return;
    }
    std::string compact;
    base::RemoveChars(state_->uuid, "-", &compact);
    std::vector<uint8_t> uuid;
    if (!base::HexStringToBytes(compact, &uuid) || uuid.size() != 16) {
      Fail(net::ERR_INVALID_ARGUMENT);
      return;
    }
    std::string header(1, '\0');
    header.append(reinterpret_cast<const char*>(uuid.data()), uuid.size());
    header.push_back(0);  // addons length
    header.push_back(1);  // TCP
    header.push_back(static_cast<char>(destination_.port() >> 8));
    header.push_back(static_cast<char>(destination_.port()));
    header.push_back(2);  // domain address; remote DNS
    header.push_back(static_cast<char>(destination_.host().size()));
    header.append(destination_.host());
    download_ = MakeRequest("");
    // The HTTP/1.1 download is an indefinite stream, never a reusable
    // response body. Prevent the HTTP body drainer from keeping it alive
    // after revocation. Chromium strips this hop-by-hop header on HTTP/2.
    download_->SetExtraRequestHeaderByName("Connection", "close", true);
    download_->Start();
    SendPacket(header);
  }
  void SendPacket(const std::string& bytes) {
    upload_ = MakeRequest("/" + base::NumberToString(sequence_++));
    upload_->set_method("POST");
    upload_->set_upload(net::ElementsUploadDataStream::CreateWithReader(
        net::UploadOwnedBytesElementReader::CreateWithString(bytes)));
    upload_deadline_.Start(
        FROM_HERE, base::Seconds(30),
        base::BindOnce(&ByodXHTTPStreamSocket::Fail, weak_.GetWeakPtr(),
                       net::ERR_TIMED_OUT));
    upload_->Start();
  }
  int OnConnected(net::URLRequest* request,
                  const net::TransportInfo& info,
                  net::CompletionOnceCallback) override {
    if (request == download_.get()) {
      peer_ = info.endpoint;
    }
    return net::OK;
  }
  void OnReceivedRedirect(net::URLRequest*,
                          const net::RedirectInfo&,
                          bool*) override {
    Fail(net::ERR_UNSAFE_REDIRECT);
  }
  void OnAuthRequired(net::URLRequest*,
                      const net::AuthChallengeInfo&) override {
    Fail(net::ERR_ACCESS_DENIED);
  }
  void OnSSLCertificateError(net::URLRequest*,
                             int error,
                             const net::SSLInfo&,
                             bool) override {
    Fail(error);
  }
  void OnCertificateRequested(net::URLRequest*,
                              net::SSLCertRequestInfo*) override {
    Fail(net::ERR_BAD_SSL_CLIENT_AUTH_CERT);
  }
  void OnResponseStarted(net::URLRequest* request, int error) override {
    if (error != net::OK) {
      Fail(error);
      return;
    }
    if (!request->response_headers() ||
        request->response_headers()->response_code() != 200) {
      Fail(net::ERR_TUNNEL_CONNECTION_FAILED);
      return;
    }
    if (request == download_.get()) {
      download_ready_ = true;
      Post(&ByodXHTTPStreamSocket::MaybeConnected);
    } else {
      Post(&ByodXHTTPStreamSocket::ReadUploadResponse);
    }
  }
  void ReadUploadResponse() {
    int result =
        upload_->Read(response_buffer_.get(), response_buffer_->size());
    if (result != net::ERR_IO_PENDING) {
      FinishUpload(result);
    }
  }
  void FinishUpload(int result) {
    // packet-up has an empty 200 response. Do not treat an HTML error page as
    // an accepted upload or buffer an unbounded response from a broken proxy.
    if (result != 0) {
      Fail(result < 0 ? result : net::ERR_INVALID_RESPONSE);
      return;
    }
    upload_deadline_.Stop();
    upload_.reset();
    if (!header_sent_) {
      header_sent_ = true;
      Post(&ByodXHTTPStreamSocket::MaybeConnected);
    } else if (write_callback_) {
      std::move(write_callback_).Run(write_length_);
    }
  }
  void MaybeConnected() {
    if (!download_ready_ || !header_sent_ || !connect_callback_) {
      return;
    }
    connected_ = true;
    deadline_.Stop();
    // Do not wait for the VLESS response here: Xray may wait for the source's
    // first bytes, which need Chromium to send its TLS ClientHello first.
    std::move(connect_callback_).Run(net::OK);
  }
  void PumpRead() {
    if (reading_ || !read_callback_ || !download_) {
      return;
    }
    reading_ = true;
    int result =
        download_->Read(download_buffer_.get(), download_buffer_->size());
    if (result != net::ERR_IO_PENDING) {
      FinishRead(result);
    }
  }
  void OnReadCompleted(net::URLRequest* request, int result) override {
    if (request == upload_.get()) {
      FinishUpload(result);
    } else {
      FinishRead(result);
    }
  }
  void FinishRead(int result) {
    reading_ = false;
    if (result <= 0) {
      Fail(result ? result : net::ERR_CONNECTION_CLOSED);
      return;
    }
    int offset = 0;
    const std::string chunk(download_buffer_->data(), result);
    while (response_header_bytes_ < 2 && offset < result) {
      if (chunk[offset++] != 0) {
        Fail(net::ERR_INVALID_RESPONSE);
        return;
      }
      response_header_bytes_++;
    }
    incoming_.append(chunk, offset, result - offset);
    if (incoming_.empty()) {
      Post(&ByodXHTTPStreamSocket::PumpRead);
      return;
    }
    int count = CopyIncoming(read_buffer_.get(), read_length_);
    read_buffer_.reset();
    std::move(read_callback_).Run(count);
  }
  int CopyIncoming(net::IOBuffer* buffer, int length) {
    int count = std::min(static_cast<int>(incoming_.size()), length);
    std::copy_n(incoming_.data(), count, buffer->data());
    incoming_.erase(0, count);
    received_ += count;
    return count;
  }
  void Fail(int error) {
    error_ = error;
    connected_ = false;
    deadline_.Stop();
    upload_deadline_.Stop();
    download_.reset();
    upload_.reset();
    weak_.InvalidateWeakPtrs();
    Post(&ByodXHTTPStreamSocket::NotifyFailure);
  }
  void NotifyFailure() {
    auto alive = weak_.GetWeakPtr();
    if (connect_callback_) {
      std::move(connect_callback_).Run(error_);
    }
    if (!alive) {
      return;
    }
    if (read_callback_) {
      read_buffer_.reset();
      std::move(read_callback_).Run(error_);
    }
    if (!alive) {
      return;
    }
    if (write_callback_) {
      std::move(write_callback_).Run(error_);
    }
  }

  scoped_refptr<ByodXHTTPTransport> state_;
  const net::HostPortPair destination_;
  const net::NetLogWithSource log_;
  std::string session_, incoming_;
  uint64_t sequence_ = 0;
  net::IPEndPoint peer_;
  std::unique_ptr<net::URLRequest> download_, upload_;
  scoped_refptr<net::IOBufferWithSize> download_buffer_, response_buffer_;
  scoped_refptr<net::IOBuffer> read_buffer_;
  int read_length_ = 0, write_length_ = 0, response_header_bytes_ = 0;
  int error_ = net::ERR_SOCKET_NOT_CONNECTED;
  int64_t received_ = 0;
  bool connected_ = false, download_ready_ = false, header_sent_ = false;
  bool reading_ = false, used_ = false;
  net::CompletionOnceCallback connect_callback_, read_callback_,
      write_callback_;
  base::OneShotTimer deadline_, upload_deadline_;
  base::WeakPtrFactory<ByodXHTTPStreamSocket> weak_{this};
};

void ByodXHTTPTransport::Revoke() {
  active = false;
  for (auto& socket : sockets) {
    if (socket) {
      socket->Revoke();
    }
  }
  sockets.clear();
}

namespace {

bool ApplyProxyConfigToProxyInfo(const net::ProxyConfig::ProxyRules& rules,
                                 const net::ProxyRetryInfoMap& proxy_retry_info,
                                 const GURL& url,
                                 net::ProxyInfo* proxy_info) {
  DCHECK(proxy_info);
  if (rules.empty()) {
    return false;
  }

  rules.Apply(url, proxy_info);
  proxy_info->DeprioritizeBadProxyChains(proxy_retry_info);
  return !proxy_info->is_empty() && !proxy_info->is_direct();
}

// Checks if |target_proxy| is in |proxy_list|.
bool CheckProxyList(const net::ProxyList& proxy_list,
                    const net::ProxyServer& target_proxy) {
  for (const auto& proxy_chain : proxy_list.AllChains()) {
    if (proxy_chain.is_single_proxy() &&
        proxy_chain.First().host_port_pair() == target_proxy.host_port_pair()) {
      return true;
    }
  }
  return false;
}

// Returns true if there is a possibility that |proxy_rules->Apply()| can
// choose |target_proxy|. This does not consider the bypass rules; it only
// scans the possible set of proxy server.
bool RulesContainsProxy(const net::ProxyConfig::ProxyRules& proxy_rules,
                        const net::ProxyServer& target_proxy) {
  switch (proxy_rules.type) {
    case net::ProxyConfig::ProxyRules::Type::EMPTY:
      return false;

    case net::ProxyConfig::ProxyRules::Type::PROXY_LIST:
      return CheckProxyList(proxy_rules.single_proxies, target_proxy);

    case net::ProxyConfig::ProxyRules::Type::PROXY_LIST_PER_SCHEME:
      return CheckProxyList(proxy_rules.proxies_for_http, target_proxy) ||
             CheckProxyList(proxy_rules.proxies_for_https, target_proxy);
  }

  NOTREACHED();
}

bool IsValidCustomProxyConfig(const mojom::CustomProxyConfig& config) {
  switch (config.rules.type) {
    case net::ProxyConfig::ProxyRules::Type::EMPTY:
      return true;

    case net::ProxyConfig::ProxyRules::Type::PROXY_LIST:
      return !config.rules.single_proxies.IsEmpty();

    case net::ProxyConfig::ProxyRules::Type::PROXY_LIST_PER_SCHEME:
      return !config.rules.proxies_for_http.IsEmpty() ||
             !config.rules.proxies_for_https.IsEmpty();
  }

  NOTREACHED();
}

// Merges headers from |in| to |out|. If the header already exists in |out| they
// are combined.
void MergeRequestHeaders(net::HttpRequestHeaders* out,
                         const net::HttpRequestHeaders& in) {
  for (net::HttpRequestHeaders::Iterator it(in); it.GetNext();) {
    std::optional<std::string> old_value = out->GetHeader(it.name());
    if (old_value) {
      out->SetHeader(it.name(), *old_value + ", " + it.value());
    } else {
      out->SetHeader(it.name(), it.value());
    }
  }
}

}  // namespace

NetworkServiceProxyDelegate::NetworkServiceProxyDelegate(
    mojom::CustomProxyConfigPtr initial_config,
    mojo::PendingReceiver<mojom::CustomProxyConfigClient>
        config_client_receiver,
    mojo::PendingRemote<mojom::CustomProxyConnectionObserver> observer_remote)
    : proxy_config_(std::move(initial_config)),
      receiver_(this, std::move(config_client_receiver)) {
  // Make sure there is always a valid proxy config so we don't need to null
  // check it.
  if (!proxy_config_) {
    proxy_config_ = mojom::CustomProxyConfig::New();
  }

  // |observer_remote| is an optional param for the NetworkContext.
  if (observer_remote) {
    observer_.Bind(std::move(observer_remote));
    // Unretained is safe since |observer_| is owned by |this|.
    observer_.set_disconnect_handler(
        base::BindOnce(&NetworkServiceProxyDelegate::OnObserverDisconnect,
                       base::Unretained(this)));
  }
}

NetworkServiceProxyDelegate::~NetworkServiceProxyDelegate() {
  ClearByodTunnelConfig();
}

void NetworkServiceProxyDelegate::OnResolveProxy(
    const GURL& url,
    const net::NetworkAnonymizationKey& network_anonymization_key,
    const std::string& method,
    const net::ProxyRetryInfoMap& proxy_retry_info,
    net::ProxyInfo* result) {
  if (byod_tunnel_enabled_ && url.SchemeIs(url::kHttpsScheme) &&
      std::find(byod_tunnel_hosts_.begin(), byod_tunnel_hosts_.end(), url.host()) !=
          byod_tunnel_hosts_.end()) {
    // Only explicitly signed tunnel hosts are routed through BYOD.
    result->UseNamedProxy(base::StrCat(
        {"http://", byod_proxy_server_.GetHost(), ":",
         base::NumberToString(byod_proxy_server_.GetPort())}));
    return;
  }
  if (!EligibleForProxy(*result, method)) {
    return;
  }

  net::ProxyInfo proxy_info;
  if (ApplyProxyConfigToProxyInfo(proxy_config_->rules, proxy_retry_info, url,
                                  &proxy_info)) {
    DCHECK(!proxy_info.is_empty() && !proxy_info.is_direct());
    result->OverrideProxyList(proxy_info.proxy_list());
  }
}

void NetworkServiceProxyDelegate::OnSuccessfulRequestAfterFailures(
    const net::ProxyRetryInfoMap& proxy_retry_info) {}

void NetworkServiceProxyDelegate::OnFallback(const net::ProxyChain& bad_chain,
                                             int net_error) {
  if (observer_) {
    observer_->OnFallback(bad_chain, net_error);
  }
}

base::expected<net::HttpRequestHeaders, net::Error>
NetworkServiceProxyDelegate::OnBeforeTunnelRequest(
    const net::ProxyChain& proxy_chain,
    size_t proxy_index,
    OnBeforeTunnelRequestCallback callback) {
  net::HttpRequestHeaders extra_headers;
  if (IsInProxyConfig(proxy_chain)) {
    MergeRequestHeaders(&extra_headers, proxy_config_->connect_tunnel_headers);
  }
  return extra_headers;
}

net::Error NetworkServiceProxyDelegate::OnTunnelHeadersReceived(
    const net::ProxyChain& proxy_chain,
    size_t proxy_index,
    const net::HttpResponseHeaders& response_headers,
    net::CompletionOnceCallback callback) {
  if (observer_) {
    // Copy the response headers since mojo expects a ref counted object.
    observer_->OnTunnelHeadersReceived(
        proxy_chain, proxy_index,
        base::MakeRefCounted<net::HttpResponseHeaders>(
            response_headers.raw_headers()));
  }
  return net::OK;
}

void NetworkServiceProxyDelegate::SetProxyResolutionService(
    net::ProxyResolutionService* proxy_resolution_service) {
  proxy_resolution_service_ = proxy_resolution_service;
}

void NetworkServiceProxyDelegate::SetByodTunnelConfig(
    const std::string& tunnel_hosts_serialized,
    const std::string& proxy_host,
    uint16_t proxy_port,
    const std::string& uuid,
    net::URLRequestContext* context) {
  ClearByodTunnelConfig();
  byod_tunnel_hosts_ =
      base::SplitString(tunnel_hosts_serialized, ",", base::TRIM_WHITESPACE,
                        base::SPLIT_WANT_NONEMPTY);
  byod_tunnel_enabled_ = !byod_tunnel_hosts_.empty();
  // A private routing marker, never a real HTTP proxy or DNS destination.
  byod_proxy_server_ = net::ProxyServer::FromSchemeHostAndPort(
      net::ProxyServer::SCHEME_HTTP, "byod-xhttp.invalid", uint16_t{1});
  const GURL endpoint(
      base::StrCat({"https://", proxy_host, ":",
                    base::NumberToString(proxy_port), "/v1/xhttp/"}));
  if (!context || !endpoint.is_valid() || endpoint.host() != "exam.cs.ac.cn" ||
      endpoint.has_username() || endpoint.has_password() || proxy_port == 0 ||
      !base::Uuid::ParseCaseInsensitive(uuid).is_valid()) {
    return;  // The selected route remains fail-closed.
  }
  byod_xhttp_ =
      base::MakeRefCounted<ByodXHTTPTransport>(endpoint, uuid, context);
}

std::unique_ptr<net::StreamSocket>
NetworkServiceProxyDelegate::CreateTunnelSocket(
    const net::ProxyChain& chain,
    const net::HostPortPair& target) {
  if (!chain.is_single_proxy() ||
      chain.First().host_port_pair() !=
          net::HostPortPair("byod-xhttp.invalid", 1)) {
    return nullptr;
  }
  const bool allowed =
      byod_tunnel_enabled_ &&
      std::find(byod_tunnel_hosts_.begin(), byod_tunnel_hosts_.end(),
                target.host()) != byod_tunnel_hosts_.end();
  return std::make_unique<ByodXHTTPStreamSocket>(
      allowed ? byod_xhttp_ : nullptr, target);
}

void NetworkServiceProxyDelegate::ClearByodTunnelConfig() {
  if (byod_xhttp_) {
    byod_xhttp_->Revoke();
  }
  byod_xhttp_.reset();
  byod_tunnel_enabled_ = false;
  byod_tunnel_hosts_.clear();
}

void NetworkServiceProxyDelegate::OnCustomProxyConfigUpdated(
    mojom::CustomProxyConfigPtr proxy_config,
    OnCustomProxyConfigUpdatedCallback callback) {
  DCHECK(IsValidCustomProxyConfig(*proxy_config));
  proxy_config_ = std::move(proxy_config);
  std::move(callback).Run();
}

bool NetworkServiceProxyDelegate::IsInProxyConfig(
    const net::ProxyChain& proxy_chain) const {
  if (!proxy_chain.IsValid() || proxy_chain.is_direct()) {
    return false;
  }

  // TODO(crbug.com/40284947): Support nested proxies.
  if (proxy_chain.is_single_proxy() &&
      RulesContainsProxy(proxy_config_->rules, proxy_chain.First())) {
    return true;
  }

  return false;
}

bool NetworkServiceProxyDelegate::EligibleForProxy(
    const net::ProxyInfo& proxy_info,
    const std::string& method) const {
  bool has_existing_config =
      !proxy_info.is_direct() || proxy_info.proxy_list().size() > 1u;

  if (!proxy_config_->should_override_existing_config && has_existing_config) {
    return false;
  }

  if (!proxy_config_->allow_non_idempotent_methods &&
      !net::HttpUtil::IsMethodIdempotent(method)) {
    return false;
  }

  return true;
}

void NetworkServiceProxyDelegate::OnObserverDisconnect() {
  observer_.reset();
}

}  // namespace network
