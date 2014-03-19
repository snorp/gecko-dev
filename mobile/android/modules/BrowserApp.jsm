/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict"

let Cc = Components.classes;
let Ci = Components.interfaces;
let Cu = Components.utils;

Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/UITelemetry.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "sendMessageToJava",
                                  "resource://gre/modules/Messaging.jsm");

this.EXPORTED_SYMBOLS = ['setTabGetter'];

function dump(a) {
  Services.console.logStringMessage(a);
}

var BrowserApp = {

  _prefObservers: [],
  _app: null,

  tabGetter: null,

  getBrowserTab: function(tabId) {
    if (!this.tabGetter)
      return null;

    return this.tabGetter.getTabForId(tabId);
  },

  getUITelemetryObserver: function() {
    return UITelemetry;
  },

  getPreferences: function getPreferences(requestId, prefNames, count) {
    this.handlePreferencesRequest(requestId, prefNames, false);
  },

  observePreferences: function observePreferences(requestId, prefNames, count) {
    this.handlePreferencesRequest(requestId, prefNames, true);
  },

  removePreferenceObservers: function removePreferenceObservers(aRequestId) {
    let newPrefObservers = [];
    for (let prefName in this._prefObservers) {
      let requestIds = this._prefObservers[prefName];
      // Remove the requestID from the preference handlers
      let i = requestIds.indexOf(aRequestId);
      if (i >= 0) {
        requestIds.splice(i, 1);
      }

      // If there are no more request IDs, remove the observer
      if (requestIds.length == 0) {
        Services.prefs.removeObserver(prefName, this);
      } else {
        newPrefObservers[prefName] = requestIds;
      }
    }
    this._prefObservers = newPrefObservers;
  },

  notifyPrefObservers: function(aPref) {
    this._prefObservers[aPref].forEach(function(aRequestId) {
      this.getPreferences(aRequestId, [aPref], 1);
    }, this);
  },

  handlePreferencesRequest: function handlePreferencesRequest(aRequestId,
                                                              aPrefNames,
                                                              aListen) {

    let prefs = [];

    for (let prefName of aPrefNames) {
      let pref = {
        name: prefName,
        type: "",
        value: null
      };

      if (aListen) {
        if (this._prefObservers[prefName])
          this._prefObservers[prefName].push(aRequestId);
        else
          this._prefObservers[prefName] = [ aRequestId ];
        Services.prefs.addObserver(prefName, this, false);
      }

      // These pref names are not "real" pref names.
      // They are used in the setting menu,
      // and these are passed when initializing the setting menu.
      switch (prefName) {
        // The plugin pref is actually two separate prefs, so
        // we need to handle it differently
        case "plugin.enable":
          pref.type = "string";// Use a string type for java's ListPreference
          pref.value = PluginHelper.getPluginPreference();
          prefs.push(pref);
          continue;
        // Handle master password
        case "privacy.masterpassword.enabled":
          pref.type = "bool";
          pref.value = MasterPassword.enabled;
          prefs.push(pref);
          continue;
        // Handle do-not-track preference
        case "privacy.donottrackheader":
          pref.type = "string";

          let enableDNT = Services.prefs.getBoolPref("privacy.donottrackheader.enabled");
          if (!enableDNT) {
            pref.value = kDoNotTrackPrefState.NO_PREF;
          } else {
            let dntState = Services.prefs.getIntPref("privacy.donottrackheader.value");
            pref.value = (dntState === 0) ? kDoNotTrackPrefState.ALLOW_TRACKING :
                                            kDoNotTrackPrefState.DISALLOW_TRACKING;
          }

          prefs.push(pref);
          continue;
#ifdef MOZ_CRASHREPORTER
        // Crash reporter submit pref must be fetched from nsICrashReporter service.
        case "datareporting.crashreporter.submitEnabled":
          pref.type = "bool";
          pref.value = CrashReporter.submitReports;
          prefs.push(pref);
          continue;
#endif
      }

      // Pref name translation.
      switch (prefName) {
#ifdef MOZ_TELEMETRY_REPORTING
        // Telemetry pref differs based on build.
        case Telemetry.SHARED_PREF_TELEMETRY_ENABLED:
#ifdef MOZ_TELEMETRY_ON_BY_DEFAULT
          prefName = "toolkit.telemetry.enabledPreRelease";
#else
          prefName = "toolkit.telemetry.enabled";
#endif
          break;
#endif
      }

      try {
        switch (Services.prefs.getPrefType(prefName)) {
          case Ci.nsIPrefBranch.PREF_BOOL:
            pref.type = "bool";
            pref.value = Services.prefs.getBoolPref(prefName);
            break;
          case Ci.nsIPrefBranch.PREF_INT:
            pref.type = "int";
            pref.value = Services.prefs.getIntPref(prefName);
            break;
          case Ci.nsIPrefBranch.PREF_STRING:
          default:
            pref.type = "string";
            try {
              // Try in case it's a localized string (will throw an exception if not)
              pref.value = Services.prefs.getComplexValue(prefName, Ci.nsIPrefLocalizedString).data;
            } catch (e) {
              pref.value = Services.prefs.getCharPref(prefName);
            }
            break;
        }
      } catch (e) {
        dump("Error reading pref [" + prefName + "]: " + e);
        // preference does not exist; do not send it
        continue;
      }

      // Some Gecko preferences use integers or strings to reference
      // state instead of directly representing the value.
      // Since the Java UI uses the type to determine which ui elements
      // to show and how to handle them, we need to normalize these
      // preferences to the correct type.
      switch (prefName) {
        // (string) index for determining which multiple choice value to display.
        case "browser.chrome.titlebarMode":
        case "network.cookie.cookieBehavior":
        case "font.size.inflation.minTwips":
        case "home.sync.updateMode":
          pref.type = "string";
          pref.value = pref.value.toString();
          break;
      }

      prefs.push(pref);
    }

    sendMessageToJava({
      type: "Preferences:Data",
      requestId: aRequestId,    // opaque request identifier, can be any string/int/whatever
      preferences: prefs
    });
  },

  observe: function(aSubject, aTopic, aData) {
    dump("SNORP: got BrowserApp message " + aTopic);

    switch (aTopic) {
      case "nsPref:changed":
        this.notifyPrefObservers(aData);
        break;
      default:
        break;
    }
  },

  init: function() {
    if (this._inited) {
      return;
    }

    Services.obs.addObserver(this, "nsPref:changed", false);
    Services.androidBridge.browserApp = this;

    this._inited = true;
    dump("SNORP: initialized BrowserApp");
  }
};

var setTabGetter = function(tabGetter) {
  BrowserApp.tabGetter = tabGetter;
}

BrowserApp.init();