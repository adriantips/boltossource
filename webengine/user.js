// ===========================================================================
//  BoltOS Web Engine  -  webengine/user.js
//
//  Firefox's preferences for the engine's profile. The aim is that a page
//  can't tell this Firefox from one on Windows 11 and looks the same in it:
//  Windows' user agent and platform, Windows' default fonts, Windows 11's
//  scrollbars, the light theme, and Windows' mouse habits. The rest keeps it
//  quiet: no first-run pages, no telemetry, no prompts for things BoltOS
//  doesn't have (notifications, location, a camera).
//
//  boltweb.py copies this into the profile at every start, with @VERSION@ set
//  to the Firefox that is installed.
// ===========================================================================

// ---- Windows, as far as a page can tell -------------------------------------
user_pref("general.useragent.override", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:@VERSION@) Gecko/20100101 Firefox/@VERSION@");
user_pref("general.platform.override", "Win32");
user_pref("general.oscpu.override", "Windows NT 10.0; Win64; x64");
user_pref("general.appversion.override", "5.0 (Windows)");
user_pref("dom.webdriver.enabled", false);          // navigator.webdriver as in anyone's Firefox
user_pref("remote.prefs.recommended", false);       // and none of the automation defaults
user_pref("intl.accept_languages", "en-US, en");
user_pref("intl.locale.requested", "en-US");
user_pref("browser.search.region", "US");

// ---- Windows' fonts: Firefox for Windows' defaults ----------------------------
// (fonts.conf maps Consolas and Segoe UI onto their stand-ins)
user_pref("font.default.x-western", "serif");
user_pref("font.name.serif.x-western", "Times New Roman");
user_pref("font.name.sans-serif.x-western", "Arial");
user_pref("font.name.monospace.x-western", "Consolas");
user_pref("font.name.cursive.x-western", "Comic Sans MS");
user_pref("font.name.serif.x-unicode", "Times New Roman");
user_pref("font.name.sans-serif.x-unicode", "Arial");
user_pref("font.name.monospace.x-unicode", "Consolas");
user_pref("font.size.variable.x-western", 16);
user_pref("font.size.monospace.x-western", 13);
user_pref("gfx.font_rendering.fontconfig.max_generic_substitutions", 3);

// ---- Windows 11's look --------------------------------------------------------
user_pref("widget.non-native-theme.enabled", true);
user_pref("widget.non-native-theme.scrollbar.style", 5);     // Windows 11's thin scrollbars
user_pref("widget.gtk.overlay-scrollbars.enabled", true);   // shown while scrolling, as Windows 11 does
user_pref("ui.useOverlayScrollbars", 1);
user_pref("ui.systemUsesDarkTheme", 0);
user_pref("layout.css.prefers-color-scheme.content-override", 1);
user_pref("browser.theme.content-theme", 1);
user_pref("browser.theme.toolbar-theme", 1);
user_pref("widget.content.allow-gtk-dark-theme", false);
user_pref("ui.accentcolor", "#0078d4");                     // Windows' default blue
user_pref("ui.accentcolortext", "#ffffff");
user_pref("layout.css.devPixelsPerPx", "1.0");              // 100%, 96 dpi
user_pref("layout.css.dpi", 96);

// ---- Windows' mouse ---------------------------------------------------------------
user_pref("general.autoScroll", true);                  // middle-click scrolls
user_pref("middlemouse.paste", false);                  // and doesn't paste, as on Linux
user_pref("middlemouse.contentLoadURL", false);
user_pref("layout.word_select.eat_space_to_next_word", true);
user_pref("ui.key.menuAccessKeyFocuses", false);        // Alt alone opens no menu bar

// ---- one window -------------------------------------------------------------------------
user_pref("browser.tabs.inTitlebar", 1);
user_pref("browser.tabs.closeWindowWithLastTab", false);
user_pref("dom.allow_scripts_to_close_windows", false);
user_pref("dom.disable_window_move_resize", true);
user_pref("full-screen-api.warning.timeout", 0);
user_pref("full-screen-api.transition-duration.enter", "0 0");
user_pref("full-screen-api.transition-duration.leave", "0 0");
user_pref("permissions.default.desktop-notification", 2);
user_pref("permissions.default.geo", 2);
user_pref("permissions.default.camera", 2);
user_pref("permissions.default.microphone", 2);
user_pref("permissions.default.xr", 2);
user_pref("browser.translations.automaticallyPopup", false);
user_pref("browser.download.useDownloadDir", true);
user_pref("browser.download.folderList", 2);
user_pref("browser.download.dir", "/home/boltweb/Downloads");
user_pref("browser.download.alwaysOpenPanel", false);
user_pref("pdfjs.disabled", false);
user_pref("media.eme.enabled", true);                   // DRM video, as Firefox on Windows plays it
user_pref("media.gmp-widevinecdm.visible", true);
user_pref("media.gmp-widevinecdm.enabled", true);
user_pref("media.hardware-video-decoding.enabled", false);

// ---- quiet ----------------------------------------------------------------------------
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("browser.startup.homepage", "about:blank");
user_pref("browser.startup.page", 0);
user_pref("startup.homepage_welcome_url", "");
user_pref("startup.homepage_welcome_url.additional", "");
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.newtabpage.enabled", false);
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("browser.tabs.warnOnClose", false);
user_pref("browser.uitour.enabled", false);
user_pref("browser.messaging-system.whatsNewPanel.enabled", false);
user_pref("browser.vpn_promo.enabled", false);
user_pref("extensions.pocket.enabled", false);
user_pref("app.update.auto", false);
user_pref("app.update.enabled", false);
user_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);
user_pref("datareporting.policy.firstRunURL", "");
user_pref("datareporting.healthreport.uploadEnabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("privacy.webrtc.hideGlobalIndicator", true);
