// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/grips/grips_ui.h"

#include <string>

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/url_constants.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/test/browser_test.h"
#include "net/base/url_util.h"
#include "url/gurl.h"

namespace {

class GripsUIBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* web_contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(GripsUIBrowserTest, ExamHostUsesExamController) {
  ASSERT_TRUE(
      ui_test_utils::NavigateToURL(browser(), GURL(chrome::kGripsExamUrl)));
  ASSERT_TRUE(web_contents()->GetWebUI());
  EXPECT_NE(nullptr, web_contents()
                         ->GetWebUI()
                         ->GetController()
                         ->GetAs<GripsExamUI>());
  EXPECT_EQ(u"BYOD Exam", web_contents()->GetTitle());
}

IN_PROC_BROWSER_TEST_F(GripsUIBrowserTest, PublicExamLinkIsRewritten) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(),
      GURL("grips://exam.cs.ac.cn/course-101?attempt=first")));

  ASSERT_TRUE(web_contents()->GetWebUI());
  const GURL committed = web_contents()->GetLastCommittedURL();
  EXPECT_TRUE(committed.SchemeIs(chrome::kGripsScheme));
  EXPECT_EQ(chrome::kGripsExamHost, committed.host());

  std::string target;
  ASSERT_TRUE(net::GetValueForKeyInQuery(committed, "target", &target));
  EXPECT_EQ("https://exam.cs.ac.cn/course-101?attempt=first", target);
}

IN_PROC_BROWSER_TEST_F(GripsUIBrowserTest, LoginHostUsesConnectController) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("grips://login/?manual=1")));

  ASSERT_TRUE(web_contents()->GetWebUI());
  const GURL committed = web_contents()->GetLastCommittedURL();
  EXPECT_TRUE(committed.SchemeIs(chrome::kGripsScheme));
  EXPECT_EQ(chrome::kGripsLoginHost, committed.host());
  EXPECT_NE(nullptr, web_contents()
                         ->GetWebUI()
                         ->GetController()
                         ->GetAs<GripsLoginUI>());
  EXPECT_EQ(u"Connect sign-in · BYOD Browser", web_contents()->GetTitle());
}

IN_PROC_BROWSER_TEST_F(GripsUIBrowserTest, UntrustedExamHostIsRejected) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("grips://exam.example.test/course-101")));
  const GURL committed = web_contents()->GetLastCommittedURL();
  EXPECT_EQ(chrome::kGripsExamHost, committed.host());

  std::string error;
  ASSERT_TRUE(net::GetValueForKeyInQuery(committed, "error", &error));
  EXPECT_EQ("invalid_exam_link", error);
}

}  // namespace
