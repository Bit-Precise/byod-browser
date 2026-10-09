// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {SettingsPeoplePageIndexElement} from './people_page_index.js';

export function getHtml(this: SettingsPeoplePageIndexElement) {
  return html`<!--_html_template_start_-->
<cr-view-manager id="viewManager" class="cr-centered-card-container"
    ?show-all="${this.shouldShowAll}">
  <settings-people-page slot="view" id="parent"
      route-path="${this.routes_.PEOPLE.path}">
  </settings-people-page>

<if expr="not is_chromeos">
   <settings-manage-profile slot="view" id="manageProfile"
       data-parent-view-id="parent"
       route-path="${this.routes_.MANAGE_PROFILE.path}">
   </settings-manage-profile>
</if>

</cr-view-manager>
<!--_html_template_end_-->`;
}
