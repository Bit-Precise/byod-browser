// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/grips/grips_url_handler.h"

#include <string>

#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "chrome/common/url_constants.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace grips {
namespace {

GURL ErrorURL() {
  return GURL("https://exam.cs.ac.cn/?error=invalid_exam_link");
}

bool IsValidPublicExamEntry(const GURL& url) {
  if (!url.is_valid() || url.host() != chrome::kGripsTrustedExamHost ||
      url.has_username() || url.has_password() || url.has_port() ||
      url.has_ref()) {
    return false;
  }
  return !url.path().empty() && url.path() != "/";
}

}  // namespace

bool HandleGripsExamURL(GURL* url,
                        content::BrowserContext* browser_context) {
  if (!url || !url->SchemeIs(chrome::kGripsScheme)) {
    return false;
  }

  if (url->host() == chrome::kGripsExamHost ||
      url->host() == chrome::kGripsLoginHost) {
    return false;
  }

  // Keep manual submission as an in-browser action. The HTTPS shell cannot
  // rely on the strict byod_session cookie when it is embedded in this
  // privileged page, so it performs the authenticated POST using its token.
  if (url->host() == chrome::kGripsTrustedExamHost &&
      base::StartsWith(url->path(), "/exams/", base::CompareCase::SENSITIVE) &&
      base::EndsWith(url->path(), "/complete", base::CompareCase::SENSITIVE)) {
    const std::string path(url->path());
    const std::string exam_id = path.substr(7, path.size() - 16);
    if (exam_id.empty() || exam_id.find('/') != std::string::npos) {
      *url = ErrorURL();
      return true;
    }
    *url = GURL(base::StrCat({"grips://exam.cs.ac.cn/?action=complete&exam_id=",
        base::EscapeQueryParamValue(exam_id, true)}));
    return true;
  }

  // Keep the trusted public alias as a WebUI shell. Its visible exam content
  // is loaded from https://exam.cs.ac.cn; Chromium only owns the shell and
  // native control bridge.
  if (url->host() == chrome::kGripsTrustedExamHost &&
      !url->has_username() && !url->has_password() && !url->has_port() &&
      !url->has_ref()) {
    return false;
  }

  const bool bare_entry = url->host() == chrome::kGripsTrustedExamHost &&
      (url->path().empty() || url->path() == "/");
  if (!bare_entry && !IsValidPublicExamEntry(*url)) {
    *url = ErrorURL();
    return true;
  }

  GURL::Replacements replacements;
  replacements.SetSchemeStr(url::kHttpsScheme);
  replacements.ClearRef();
  const GURL target = url->ReplaceComponents(replacements);
  if (!target.is_valid() || target.host() != chrome::kGripsTrustedExamHost) {
    *url = ErrorURL();
    return true;
  }

  // The exam shell is ordinary HTTPS content served by byod-server.  The
  // browser only owns this scheme alias and the native bridge iframe; no
  // business UI or TypeScript is compiled into Chromium.
  *url = target;
  return true;
}

}  // namespace grips
