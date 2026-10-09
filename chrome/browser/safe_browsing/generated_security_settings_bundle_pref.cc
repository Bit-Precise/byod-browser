// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/safe_browsing/generated_security_settings_bundle_pref.h"

#include "chrome/browser/browser_process.h"
#include "chrome/browser/net/secure_dns_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/extensions/api/settings_private.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/safe_browsing/core/common/features.h"

namespace safe_browsing {

GeneratedSecuritySettingsBundlePref::GeneratedSecuritySettingsBundlePref(
    Profile* profile)
    : profile_(profile) {
  user_prefs_registrar_.Init(profile->GetPrefs());
}

extensions::settings_private::SetPrefResult
GeneratedSecuritySettingsBundlePref::SetPref(const base::Value* value) {
  return extensions::settings_private::SetPrefResult::SUCCESS;
}

extensions::api::settings_private::PrefObject
GeneratedSecuritySettingsBundlePref::GetPrefObject() const {

  extensions::api::settings_private::PrefObject pref_object;
  pref_object.key = kGeneratedSecuritySettingsBundlePref;
  pref_object.type = extensions::api::settings_private::PrefType::kNumber;
  pref_object.value = base::Value(0);
  return pref_object;
}

void GeneratedSecuritySettingsBundlePref::
    OnSecuritySettingsBundlePreferencesChanged() {
  NotifyObservers(kGeneratedSecuritySettingsBundlePref);
}

}  // namespace safe_browsing
