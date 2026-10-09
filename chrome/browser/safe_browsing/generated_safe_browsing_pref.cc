// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/safe_browsing/generated_safe_browsing_pref.h"

#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "chrome/common/extensions/api/settings_private.h"
#include "components/prefs/pref_service.h"

namespace settings_api = extensions::api::settings_private;

namespace safe_browsing {
const char kGeneratedSafeBrowsingPref[] = "generated.safe_browsing";

GeneratedSafeBrowsingPref::GeneratedSafeBrowsingPref(Profile* profile)
    : profile_(profile) {
  user_prefs_registrar_.Init(profile->GetPrefs());
}

extensions::settings_private::SetPrefResult GeneratedSafeBrowsingPref::SetPref(
    const base::Value* value) {
    return extensions::settings_private::SetPrefResult::PREF_NOT_MODIFIABLE;
}

extensions::api::settings_private::PrefObject
GeneratedSafeBrowsingPref::GetPrefObject() const {
  extensions::api::settings_private::PrefObject pref_object;
  pref_object.key = kGeneratedSafeBrowsingPref;
  pref_object.type = extensions::api::settings_private::PrefType::kNumber;
  pref_object.value = base::Value(0);

  ApplySafeBrowsingManagementState(*profile_, pref_object);

  return pref_object;
}

void GeneratedSafeBrowsingPref::OnSafeBrowsingPreferencesChanged() {
  NotifyObservers(kGeneratedSafeBrowsingPref);
}

void GeneratedSafeBrowsingPref::OnSettingsBundleChanged() {
}

/* static */
void GeneratedSafeBrowsingPref::ApplySafeBrowsingManagementState(
    const Profile& profile,
    settings_api::PrefObject& pref_object) {
}

}  // namespace safe_browsing
