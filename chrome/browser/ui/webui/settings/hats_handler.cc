// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/settings/hats_handler.h"

#include "base/functional/bind.h"
#include "base/metrics/histogram_functions.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/privacy_sandbox/privacy_sandbox_settings_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/hats/hats_service.h"
#include "chrome/browser/ui/hats/hats_service_factory.h"
#include "chrome/browser/ui/hats/trust_safety_sentiment_service.h"
#include "chrome/browser/ui/hats/trust_safety_sentiment_service_factory.h"
#include "chrome/common/channel_info.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/pref_names.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/privacy_sandbox/privacy_sandbox_prefs.h"
#include "components/privacy_sandbox/privacy_sandbox_settings.h"
#include "components/safe_browsing/core/common/features.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/visibility.h"
#include "content/public/browser/web_contents.h"
#include "services/metrics/public/cpp/metrics_utils.h"


namespace {

// Generate the Product Specific bits data which accompanies privacy settings
// survey responses from |profile|.
SurveyBitsData GetPrivacySettingsProductSpecificBitsData(Profile* profile) {
  const bool third_party_cookies_blocked =
      static_cast<content_settings::CookieControlsMode>(
          profile->GetPrefs()->GetInteger(prefs::kCookieControlsMode)) ==
      content_settings::CookieControlsMode::kBlockThirdParty;

  return {{"3P cookies blocked", third_party_cookies_blocked}};
}

// Rounds down the time on page to the nearest power of 2 in seconds, with a
// max of 16 mins. This is to bucketize the time on page to be sent with the
// HaTS survey.
int64_t BucketizeTimeOnPage(double time_on_page_ms) {
  constexpr int64_t kMaxTimeOnPageMinutes = 16;
  constexpr int64_t kMaxTimeBucketSeconds = kMaxTimeOnPageMinutes * 60;

  int64_t time_on_page_s = time_on_page_ms / 1000;
  if (time_on_page_s >= kMaxTimeBucketSeconds) {
    return kMaxTimeBucketSeconds;
  }
  return ukm::GetExponentialBucketMinForUserTiming(time_on_page_s);
}

}  // namespace

namespace settings {

HatsHandler::HatsHandler() = default;

HatsHandler::~HatsHandler() = default;

void HatsHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "trustSafetyInteractionOccurred",
      base::BindRepeating(&HatsHandler::HandleTrustSafetyInteractionOccurred,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "securityPageHatsRequest",
      base::BindRepeating(&HatsHandler::HandleSecurityPageHatsRequest,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "securityPageV2HatsRequest",
      base::BindRepeating(&HatsHandler::HandleSecurityPageV2HatsRequest,
                          base::Unretained(this)));
}

/**
 * First arg in the list indicates the SecurityPageInteraction.
 * Second arg in the list indicates the SafeBrowsingState.
 */
void HatsHandler::HandleSecurityPageHatsRequest(const base::ListValue& args) {
  AllowJavascript();
}

/**
 * There are 4 arguments in the input list.
 * First arg is a set of SecurityPageV2Interactions.
 * Second arg indicates the SafeBrowsingState when the settings page was
 * opened.
 * Third arg indicates the total amount of time the user spent on the
 * security page.
 * Fourth arg indicates the SecuritySettingsBundleSetting when the settings page
 * was opened.
 */
void HatsHandler::HandleSecurityPageV2HatsRequest(const base::ListValue& args) {
  AllowJavascript();

  CHECK_EQ(4U, args.size());

}

/**
 * Generate the Product Specific string data from |profile| and |args|.
 * - First arg in the list indicates the SecurityPageInteraction.
 * - Second arg in the list indicates the SafeBrowsingState.
 * - Third arg in the list indicates the amount of time user spent on the
 * security page in focus.
 */
SurveyStringData HatsHandler::GetSecurityPageProductSpecificStringData(
    Profile* profile,
    const base::ListValue& args) {
  auto interaction = static_cast<SecurityPageInteraction>(args[0].GetInt());

  std::string security_page_interaction_type = "";
  std::string safe_browsing_setting_before = "";
  std::string safe_browsing_setting_current = "";

  switch (interaction) {
    case SecurityPageInteraction::RADIO_BUTTON_ENHANCED_CLICK: {
      security_page_interaction_type =
          "enhanced_protection_radio_button_clicked";
      break;
    }
    case SecurityPageInteraction::RADIO_BUTTON_STANDARD_CLICK: {
      security_page_interaction_type =
          "standard_protection_radio_button_clicked";
      break;
    }
    case SecurityPageInteraction::RADIO_BUTTON_DISABLE_CLICK: {
      security_page_interaction_type = "no_protection_radio_button_clicked";
      break;
    }
    case SecurityPageInteraction::EXPAND_BUTTON_ENHANCED_CLICK: {
      security_page_interaction_type =
          "enhanced_protection_expand_button_clicked";
      break;
    }
    case SecurityPageInteraction::EXPAND_BUTTON_STANDARD_CLICK: {
      security_page_interaction_type =
          "standard_protection_expand_button_clicked";
      break;
    }
    case SecurityPageInteraction::NO_INTERACTION: {
      security_page_interaction_type = "no_interaction";
      break;
    }
  }

      safe_browsing_setting_before = "no_protection";

    safe_browsing_setting_current = "no_protection";

  std::string client_channel =
      std::string(version_info::GetChannelString(chrome::GetChannel()));

  return {
      {"Security Page User Action", security_page_interaction_type},
      {"Safe Browsing Setting Before Trigger", safe_browsing_setting_before},
      {"Safe Browsing Setting After Trigger", safe_browsing_setting_current},
      {"Client Channel", client_channel},
      {"Time On Page", base::NumberToString(args[2].GetDouble())},
  };
}

/**
 * Generate the Product Specific string data from |profile| and |args|.
 * - First arg in the list is a set of SecurityPageV2Interactions.
 * - Second arg in the list indicates the SafeBrowsingState.
 * - Third arg in the list indicates the amount of time user spent on the
 * security page in focus.
 * - Fourth arg in the list indicates the SecuritySettingsBundleSetting.
 */
SurveyStringData HatsHandler::GetSecurityPageV2ProductSpecificStringData(
    Profile* profile,
    const base::ListValue& args) {
  const base::ListValue& interactions = args[0].GetList();

  std::string security_page_interactions = "";
  std::set<SecurityPageV2Interaction> interaction_set;
  // cast the int values to SecurityPageV2Interactions.
  for (const auto& interaction_value : interactions) {
    interaction_set.insert(
        static_cast<SecurityPageV2Interaction>(interaction_value.GetInt()));
  }

  // Generate the string representation of the interactions.
  std::vector<std::string> interaction_strings;
  for (const auto& interaction : interaction_set) {
    switch (interaction) {
      case SecurityPageV2Interaction::ENHANCED_BUNDLE_RADIO_BUTTON_CLICK: {
        interaction_strings.push_back("enhanced_bundle_radio_button_clicked");
        break;
      }
      case SecurityPageV2Interaction::STANDARD_BUNDLE_RADIO_BUTTON_CLICK: {
        interaction_strings.push_back("standard_bundle_radio_button_clicked");
        break;
      }
      case SecurityPageV2Interaction::SAFE_BROWSING_ROW_EXPANDED: {
        interaction_strings.push_back("safe_browsing_row_expanded");
        break;
      }
      case SecurityPageV2Interaction::
          STANDARD_SAFE_BROWSING_RADIO_BUTTON_CLICK: {
        interaction_strings.push_back(
            "standard_safe_browsing_radio_button_clicked");
        break;
      }
      case SecurityPageV2Interaction::
          ENHANCED_SAFE_BROWSING_RADIO_BUTTON_CLICK: {
        interaction_strings.push_back(
            "enhanced_safe_browsing_radio_button_clicked");
        break;
      }
      case SecurityPageV2Interaction::SAFE_BROWSING_TOGGLE_CLICK: {
        interaction_strings.push_back("safe_browsing_toggle_clicked");
        break;
      }
      case SecurityPageV2Interaction::SECURE_DNS_V2_ROW_EXPANDED: {
        interaction_strings.push_back("secure_dns_v2_row_expanded");
        break;
      }
      case SecurityPageV2Interaction::
          SECURE_DNS_V2_AUTOMATIC_RADIO_BUTTON_CLICK: {
        interaction_strings.push_back(
            "secure_dns_v2_automatic_radio_button_clicked");
        break;
      }
      case SecurityPageV2Interaction::
          SECURE_DNS_V2_FALLBACK_RADIO_BUTTON_CLICK: {
        interaction_strings.push_back(
            "secure_dns_v2_fallback_radio_button_clicked");
        break;
      }
      case SecurityPageV2Interaction::SECURE_DNS_V2_CUSTOM_RADIO_BUTTON_CLICK: {
        interaction_strings.push_back(
            "secure_dns_v2_custom_radio_button_clicked");
        break;
      }
      case SecurityPageV2Interaction::SECURE_DNS_V2_TOGGLE_CLICK: {
        interaction_strings.push_back("secure_dns_v2_toggle_clicked");
        break;
      }
      case SecurityPageV2Interaction::HTTPS_FIRST_MODE_TOGGLE_CLICK: {
        interaction_strings.push_back("https_first_mode_toggle_clicked");
        break;
      }
      case SecurityPageV2Interaction::
          BALANCED_HTTPS_FIRST_MODE_RADIO_BUTTON_CLICK: {
        interaction_strings.push_back(
            "balanced_https_first_mode_radio_button_clicked");
        break;
      }
      case SecurityPageV2Interaction::
          STRICT_HTTPS_FIRST_MODE_RADIO_BUTTON_CLICK: {
        interaction_strings.push_back(
            "strict_https_first_mode_radio_button_clicked");
        break;
      }
      case SecurityPageV2Interaction::PASSWORD_LEAK_DETECTION_TOGGLE_CLICK: {
        interaction_strings.push_back("password_leak_detection_toggle_clicked");
        break;
      }
      case SecurityPageV2Interaction::SECURE_DNS_TOGGLE_CLICK: {
        interaction_strings.push_back("secure_dns_toggle_clicked");
        break;
      }
    }
  }
  if (interaction_strings.empty()) {
    interaction_strings.push_back("no_interaction");
  }
  security_page_interactions = base::JoinString(interaction_strings, ", ");

  std::string safe_browsing_state_before = "";
      safe_browsing_state_before = "no_protection";

  std::string security_settings_bundle_setting_before = "";

  std::string safe_browsing_state_current = "";
    safe_browsing_state_current = "no_protection";

  std::string security_settings_bundle_setting_current = "";

  std::string client_channel =
      std::string(version_info::GetChannelString(chrome::GetChannel()));

  return {
      {"Security page user actions", security_page_interactions},
      {"Safe browsing setting when security page opened",
       safe_browsing_state_before},
      {"Security settings bundle setting when security page opened",
       security_settings_bundle_setting_before},
      {"Safe browsing setting when security page closed",
       safe_browsing_state_current},
      {"Security settings bundle setting when security page closed",
       security_settings_bundle_setting_current},
      {"Client channel", client_channel},
      {"Time on page (bucketed seconds)",
       base::NumberToString(BucketizeTimeOnPage(args[2].GetDouble()))},
  };
}

void HatsHandler::HandleTrustSafetyInteractionOccurred(
    const base::ListValue& args) {
  AllowJavascript();

  CHECK_EQ(1U, args.size());
  auto interaction = static_cast<TrustSafetyInteraction>(args[0].GetInt());

  // Both the HaTS service, and the T&S sentiment service (which is another
  // wrapper on the HaTS service), may decide to launch surveys based on this
  // user interaction. The HaTS service is responsible for ensuring that users
  // are not over-surveyed, and that other potential issues such as simultaneous
  // surveys are avoided.
  RequestHatsSurvey(interaction);
  InformSentimentService(interaction);
}

void HatsHandler::RequestHatsSurvey(TrustSafetyInteraction interaction) {
  Profile* profile = Profile::FromWebUI(web_ui());
  HatsService* hats_service = HatsServiceFactory::GetForProfile(
      profile, /* create_if_necessary = */ true);

  // The HaTS service may not be available for the profile, for example if it
  // is a guest profile.
  if (!hats_service) {
    return;
  }

  std::string trigger = "";
  int timeout_ms = 0;
  SurveyBitsData product_specific_bits_data = {};
  auto navigation_behavior = HatsService::NavigationBehavior::ALLOW_ANY;

  switch (interaction) {
    case TrustSafetyInteraction::RAN_SAFETY_CHECK:
      [[fallthrough]];
    case TrustSafetyInteraction::USED_PRIVACY_CARD: {
      // The control group for the Privacy guide HaTS experiment will need to
      // see either safety check or the privacy page to be eligible and have
      // never seen privacy guide.
      if (features::kHappinessTrackingSurveysForDesktopSettingsPrivacyNoGuide
              .Get() &&
          Profile::FromWebUI(web_ui())->GetPrefs()->GetBoolean(
              prefs::kPrivacyGuideViewed)) {
        return;
      }
      trigger = kHatsSurveyTriggerSettingsPrivacy;
      timeout_ms =
          features::kHappinessTrackingSurveysForDesktopSettingsPrivacyTime.Get()
              .InMilliseconds();
      product_specific_bits_data =
          GetPrivacySettingsProductSpecificBitsData(profile);
      navigation_behavior =
          HatsService::NavigationBehavior::REQUIRE_SAME_ORIGIN;
      break;
    }
    case TrustSafetyInteraction::COMPLETED_PRIVACY_GUIDE: {
      trigger = kHatsSurveyTriggerPrivacyGuide;
      timeout_ms =
          features::kHappinessTrackingSurveysForDesktopPrivacyGuideTime.Get()
              .InMilliseconds();
      navigation_behavior =
          HatsService::NavigationBehavior::REQUIRE_SAME_ORIGIN;
      break;
    }
    case TrustSafetyInteraction::OPENED_PASSWORD_MANAGER:
      [[fallthrough]];
    case TrustSafetyInteraction::RAN_PASSWORD_CHECK: {
      // Only relevant for the sentiment service
      return;
    }
  }
  // If we haven't returned, a trigger must have been set in the switch above.
  CHECK_NE(trigger, "");

  hats_service->LaunchDelayedSurveyForWebContents(
      trigger, web_ui()->GetWebContents(), timeout_ms,
      product_specific_bits_data,
      /*product_specific_string_data=*/{}, navigation_behavior);
}

void HatsHandler::InformSentimentService(TrustSafetyInteraction interaction) {
  auto* sentiment_service = TrustSafetySentimentServiceFactory::GetForProfile(
      Profile::FromWebUI(web_ui()));
  if (!sentiment_service) {
    return;
  }

  if (interaction == TrustSafetyInteraction::USED_PRIVACY_CARD) {
    sentiment_service->InteractedWithPrivacySettings(
        web_ui()->GetWebContents());
  } else if (interaction == TrustSafetyInteraction::RAN_SAFETY_CHECK) {
    sentiment_service->RanSafetyCheck();
  } else if (interaction == TrustSafetyInteraction::OPENED_PASSWORD_MANAGER) {
    sentiment_service->OpenedPasswordManager(web_ui()->GetWebContents());
  } else if (interaction == TrustSafetyInteraction::RAN_PASSWORD_CHECK) {
    sentiment_service->RanPasswordCheck();
  } else if (interaction == TrustSafetyInteraction::COMPLETED_PRIVACY_GUIDE) {
    sentiment_service->FinishedPrivacyGuide();
  }
}

}  // namespace settings
