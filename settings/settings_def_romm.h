/* Single-source definitions: RomM group (Rommarch).
 * Grammar identical to settings_def_video_sync.h; the descriptor
 * argument span matches SDESC_<kind>_ROW; row order is menu display
 * order. */

/* Descriptor rows are #if HAVE_ROMM; the string tables always carry
 * these rows via the strings pass. The configuration.c rows stay
 * literal, beside the sealed romm_token. */
#if (defined(HAVE_ROMM) && defined(HAVE_NETWORKING) || defined(SETTINGS_DEF_STRINGS_PASS)) && !defined(SETTINGS_DEF_CONFIG_PASS)
S_ACTION(ROMM_SETTINGS,
      "romm_settings",
      "RomM",
      "Connect to the RomM server the game library comes from.")
S_STRING(romm_server_url, ROMM_SERVER_URL,
      "romm_server_url",
      "", SD_FLAG_ALLOW_INPUT, 0, NULL, NULL, setting_generic_action_start_default, NULL, NULL, NULL, ST_UI_TYPE_STRING_LINE_EDIT,
      "Server Address",
      "Where to reach RomM, such as http://192.168.1.10:8080.")
S_STRING(romm_web_url, ROMM_WEB_URL,
      "romm_web_url",
      "", SD_FLAG_ALLOW_INPUT, 0, NULL, NULL, setting_generic_action_start_default, NULL, NULL, NULL, ST_UI_TYPE_STRING_LINE_EDIT,
      "Sign-in Address",
      "Where you sign in to RomM to approve this device, when that isn't the server address: a web address in front of RomM with single sign-on, for example. Empty uses the server address.")
#endif
