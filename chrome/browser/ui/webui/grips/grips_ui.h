// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_GRIPS_GRIPS_UI_H_
#define CHROME_BROWSER_UI_WEBUI_GRIPS_GRIPS_UI_H_

#include "ui/webui/mojo_web_ui_controller.h"

class GripsExamUI : public ui::MojoWebUIController {
 public:
  explicit GripsExamUI(content::WebUI* web_ui);
  GripsExamUI(const GripsExamUI&) = delete;
  GripsExamUI& operator=(const GripsExamUI&) = delete;
  ~GripsExamUI() override;

 private:
  WEB_UI_CONTROLLER_TYPE_DECL();
};

// The BYOD account entry point. It is deliberately separate from
// Chromium's GAIA/Google identity stack: a top-level Connect navigation
// stores the Connect host-only cookie in the user's normal profile, and the
// exam later reuses that session through standard OIDC SSO.
class GripsLoginUI : public ui::MojoWebUIController {
 public:
  explicit GripsLoginUI(content::WebUI* web_ui);
  GripsLoginUI(const GripsLoginUI&) = delete;
  GripsLoginUI& operator=(const GripsLoginUI&) = delete;
  ~GripsLoginUI() override;

 private:
  WEB_UI_CONTROLLER_TYPE_DECL();
};

#endif  // CHROME_BROWSER_UI_WEBUI_GRIPS_GRIPS_UI_H_
