// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {html} from '//resources/lit/v3_0/lit.rollup.js';
import type {SettingsPeoplePageElement} from './people_page.js';

export function getHtml(this: SettingsPeoplePageElement) {
  return html`<!--_html_template_start_-->
    <settings-section page-title="BYOD Browser">
      <div>
<if expr="not is_chromeos">
        <div id="byod-connect-login" class="cr-row first two-line">
          <div class="flex cr-padded-text">
            <div>$i18n{byodConnectSignIn}</div>
            <div class="secondary">$i18n{byodConnectSignInDescription}</div>
          </div>
          <cr-button class="action-button" @click="${this.onConnectLoginClick_}">
            $i18n{byodConnectSignIn}
          </cr-button>
        </div>
        <cr-link-row id="edit-profile"
            label="$i18n{profileNameAndPicture}"
            @click="${this.onProfileClick_}">
        </cr-link-row>
        <cr-link-row id="importDataDialogTrigger"
            label="$i18n{importTitle}"
            @click="${this.onImportDataClick_}">
        </cr-link-row>
</if>
      </div>
    </settings-section>
<if expr="not is_chromeos">
    ${this.showImportDataDialog_ ? html`
      <settings-import-data-dialog
          @close="${this.onImportDataDialogClose_}">
      </settings-import-data-dialog>
    ` : ''}
</if>
    <cr-toast duration="3000" id="toast">
      <span>$i18n{syncSettingsSavedToast}</span>
    </cr-toast>
<!--_html_template_end_-->`;
}
