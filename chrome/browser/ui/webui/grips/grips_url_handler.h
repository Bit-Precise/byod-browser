// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_GRIPS_GRIPS_URL_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_GRIPS_GRIPS_URL_HANDLER_H_

class GURL;

namespace content {
class BrowserContext;
}

namespace grips {

// Rewrites a public exam entry such as grips://exam.cs.ac.cn/course-101 to
// the single browser-owned grips://exam/ WebUI. The HTTPS target is carried as
// data; web content is never rendered under the privileged grips scheme.
bool HandleGripsExamURL(GURL* url,
                        content::BrowserContext* browser_context);

}  // namespace grips

#endif  // CHROME_BROWSER_UI_WEBUI_GRIPS_GRIPS_URL_HANDLER_H_
