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
S_ACTION_EX(ROMM_PAIR,
      "romm_pair", SD_FLAG_NONE, NULL, NULL, CMD_EVENT_ROMM_PAIR,
      "Pair This Device",
      "Open the pairing page, which gives a code to approve in RomM. RomM then gives this device its own access, which you can revoke there.")
S_ACTION_EX(ROMM_CHECK_CONNECTION,
      "romm_check_connection", SD_FLAG_NONE, NULL, NULL, CMD_EVENT_ROMM_CHECK_CONNECTION,
      "Check Connection",
      "See whether RomM answers at the server address and, once this device is paired, which RomM user it acts for.")
S_ACTION_EX(ROMM_REFRESH_LIBRARY,
      "romm_refresh_library", SD_FLAG_NONE, NULL, NULL, CMD_EVENT_ROMM_REFRESH_LIBRARY,
      "Refresh Library",
      "Update the game lists from RomM, adding and removing platforms and games to match it.")
S_ACTION(ROMM_SYSTEMS,
      "romm_systems",
      "Systems",
      "Which of RomM's systems this device syncs. Open one to sync it, refresh, refetch or rebuild it, or stop syncing it.")
S_ACTION(ROMM_CONNECT,
      "romm_connect",
      "Connect to RomM",
      "Set where your RomM server is and pair this device with it, to play your RomM games here.")
S_ACTION_EX(ROMM_REFRESH_PLAYLIST,
      "romm_refresh_playlist", SD_FLAG_NONE, NULL, NULL, CMD_EVENT_ROMM_REFRESH_PLAYLIST,
      "Refresh Playlist",
      "Fetch this playlist's games and covers from RomM again, all of them, and write it anew as RomM has it.")
S_UINT_EX(romm_sync_on_start, ROMM_SYNC_ON_START,
      "romm_sync_on_start",
      DEFAULT_ROMM_SYNC_ON_START, SD_FLAG_NONE, SDESC_RANGE_MINMAX, 0, 0, ROMM_SYNC_START_LAST-1, 1, 0, setting_action_ok_uint, setting_get_string_representation_uint_romm_sync_on_start, NULL, NULL, NULL, NULL, ST_UI_TYPE_UINT_COMBOBOX,
      "Sync Library at Start",
      "What Rommarch brings up to date from RomM each time it starts. Partial: each system, except one with more games or covers to fetch than the Partial Sync Limit, which waits for Refresh Library. Full: everything. None: nothing until Refresh Library.")
S_UINT_EX(romm_sync_start_limit, ROMM_SYNC_START_LIMIT,
      "romm_sync_start_limit",
      DEFAULT_ROMM_SYNC_START_LIMIT, SD_FLAG_NONE, SDESC_RANGE_MINMAX, 0, 10, 5000, 10, 0, setting_action_ok_uint, NULL, NULL, NULL, NULL, NULL, 0,
      "Partial Sync Limit",
      "With Sync Library at Start set to Partial, a system with more new or changed games than this, or more covers to fetch, isn't synced at start.")
#endif
