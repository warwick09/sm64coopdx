#include <stdio.h>
#include <string.h>
#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_menu.h"
#include "djui_panel_join_message.h"
#include "djui_panel_join_direct.h"
#include "djui_panel_join_private.h"
#include "djui_lobby_entry.h"
#include "djui_panel_rules.h"
#include "pc/network/network.h"
#include "pc/network/socket/socket.h"
#include "pc/network/socket/lan_discovery.h"
#include "pc/network/coopnet/coopnet.h"
#include "pc/utils/misc.h"
#include "pc/configfile.h"
#include "pc/update_checker.h"
#include "pc/debuglog.h"
#include "macros.h"

#define DJUI_DESC_PANEL_WIDTH (410.0f + (16 * 2.0f))

enum JoinTab {
    JOIN_TAB_INTERNET = 0,
    JOIN_TAB_LAN      = 1,
};

static struct DjuiPaginated* sLobbyPaginated = NULL;
static struct DjuiFlowLayout* sLobbyLayout = NULL;
static struct DjuiButton* sRefreshButton = NULL;
static struct DjuiButton* sTabInternet = NULL;
static struct DjuiButton* sTabLan = NULL;
static struct DjuiThreePanel* sDescriptionPanel = NULL;
static struct DjuiText* sTooltip = NULL;
static char* sPassword = NULL;
static enum JoinTab sTab = JOIN_TAB_INTERNET;
static struct LanServerInfo sLanResults[LAN_MAX_RESULTS];
static int sLanCount = 0;

static void djui_panel_join_lobby_description_create(void) {
    f32 bodyHeight = 600;

    struct DjuiThreePanel* panel = djui_three_panel_create(&gDjuiRoot->base, 64, bodyHeight, 0);
    struct DjuiThreePanelTheme theme = gDjuiThemes[configDjuiTheme]->threePanels;

    djui_base_set_alignment(&panel->base, DJUI_HALIGN_RIGHT, DJUI_VALIGN_CENTER);
    djui_base_set_size_type(&panel->base, DJUI_SVT_ABSOLUTE, DJUI_SVT_RELATIVE);
    djui_base_set_size(&panel->base, DJUI_DESC_PANEL_WIDTH, 1.0f);
    djui_base_set_color(&panel->base, theme.rectColor.r, theme.rectColor.g, theme.rectColor.b, theme.rectColor.a);
    djui_base_set_border_color(&panel->base, theme.borderColor.r, theme.borderColor.g, theme.borderColor.b, theme.borderColor.a);
    djui_base_set_border_width(&panel->base, 8);
    djui_base_set_padding(&panel->base, 16, 16, 16, 16);
    {
        struct DjuiFlowLayout* body = djui_flow_layout_create(&panel->base);
        djui_base_set_alignment(&body->base, DJUI_HALIGN_CENTER, DJUI_VALIGN_CENTER);
        djui_base_set_size_type(&body->base, DJUI_SVT_RELATIVE, DJUI_SVT_RELATIVE);
        djui_base_set_size(&body->base, 1.0f, 1.0f);
        djui_base_set_color(&body->base, 0, 0, 0, 0);
        djui_flow_layout_set_margin(body, 16);
        djui_flow_layout_set_flow_direction(body, DJUI_FLOW_DIR_DOWN);

        struct DjuiText* description = djui_text_create(&panel->base, "");
        djui_base_set_size_type(&description->base, DJUI_SVT_RELATIVE, DJUI_SVT_RELATIVE);
        djui_base_set_size(&description->base, 1.0f, 1.0f);
        djui_base_set_color(&description->base, 222, 222, 222, 255);
        djui_text_set_alignment(description, DJUI_HALIGN_LEFT, DJUI_VALIGN_CENTER);
        sTooltip = description;
    }
    sDescriptionPanel = panel;
}

static void djui_lobby_on_hover(struct DjuiBase* base) {
    struct DjuiLobbyEntry* entry = (struct DjuiLobbyEntry*)base;
    djui_text_set_text(sTooltip, entry->description);
}

static void djui_lobby_on_hover_end(UNUSED struct DjuiBase* base) {
    djui_text_set_text(sTooltip, "");
}

static void djui_panel_join_browser_show_message(const char* message) {
    struct DjuiText* text = djui_text_create(&sLobbyLayout->base, message);
    djui_base_set_size_type(&text->base, DJUI_SVT_RELATIVE, DJUI_SVT_RELATIVE);
    djui_base_set_size(&text->base, 1, 1);
    djui_text_set_alignment(text, DJUI_HALIGN_CENTER, DJUI_VALIGN_CENTER);
    djui_text_set_drop_shadow(text, 64, 64, 64, 100);
}

static void djui_panel_join_browser_finish(const char* emptyMessage) {
    if (!sLobbyLayout || !sLobbyPaginated || !sRefreshButton) { return; }
    djui_text_set_text(sRefreshButton->text, DLANG(LOBBIES, REFRESH));
    djui_base_set_enabled(&sRefreshButton->base, true);

    if (sLobbyLayout->base.child == NULL) {
        djui_panel_join_browser_show_message(emptyMessage);
    }
    djui_paginated_update_page_buttons(sLobbyPaginated);
}

// ---- Internet (CoopNet) ----------------------------------------------------

#ifdef COOPNET
void djui_panel_join_lobby(struct DjuiBase* caller) {
    gCoopNetDesiredLobby = (uint64_t)caller->tag;
    snprintf(gCoopNetPassword, 64, "%s", sPassword);
    network_reset_reconnect_and_rehost();
    network_set_system(NS_COOPNET);
    network_init(NT_CLIENT, false);
    djui_panel_join_message_create(caller);
}

void djui_panel_join_query(uint64_t aLobbyId, UNUSED uint64_t aOwnerId, uint16_t aConnections, uint16_t aMaxConnections, UNUSED const char* aGame, const char* aVersion, const char* aHostName, const char* aMode, const char* aDescription) {
    if (!sLobbyLayout) { return; }
    if (!sLobbyPaginated) { return; }
    if (sTab != JOIN_TAB_INTERNET) { return; } // stale result from before a tab switch
    if (aMaxConnections > MAX_PLAYERS) { return; }

    char playerText[64] = "";
    snprintf(playerText, 63, "%u/%u", aConnections, aMaxConnections);

    char mode[64] = "";
    snprintf(mode, 64, "%s", aMode);

    char version[MAX_VERSION_LENGTH] = { 0 };
    snprintf(version, MAX_VERSION_LENGTH, "%s", get_version());
    bool disabled = strcmp(version, aVersion) != 0;
    if (disabled) {
        snprintf(mode, 64, "\\#ff0000\\[%s]", aVersion);
    }

    struct DjuiBase* layoutBase = &sLobbyLayout->base;
    struct DjuiLobbyEntry* entry = djui_lobby_entry_create(layoutBase, (char*)aHostName, (char*)mode, playerText, (char*)aDescription, disabled, djui_panel_join_lobby, djui_lobby_on_hover, djui_lobby_on_hover_end);
    entry->base.tag = (s64)aLobbyId;
    djui_paginated_update_page_buttons(sLobbyPaginated);
}

void djui_panel_join_query_finish(void) {
    if (sTab != JOIN_TAB_INTERNET) { return; }
    djui_panel_join_browser_finish(DLANG(LOBBIES, NO_LOBBIES_FOUND));
}
#endif

// ---- LAN -------------------------------------------------------------------

static void djui_panel_join_lan_entry(struct DjuiBase* caller) {
    int index = (int)caller->tag;
    if (index < 0 || index >= sLanCount) { return; }
    const struct LanServerInfo* info = &sLanResults[index];

    snprintf(gGetHostName, MAX_CONFIG_STRING, "%s", info->ip);
    snprintf(configJoinIp, MAX_CONFIG_STRING, "%s", info->ip);
    configJoinPort = info->gamePort;

    network_reset_reconnect_and_rehost();
    network_set_system(NS_SOCKET);
    network_init(NT_CLIENT, false);
    djui_panel_join_message_create(caller);
}

static void djui_panel_join_lan_found(const struct LanServerInfo* info) {
    if (!sLobbyLayout || !sLobbyPaginated) { return; }
    if (sTab != JOIN_TAB_LAN) { return; }
    if (sLanCount >= LAN_MAX_RESULTS) { return; }

    // ignore duplicates (same machine answering twice)
    for (int i = 0; i < sLanCount; i++) {
        if (sLanResults[i].gamePort == info->gamePort && !strcmp(sLanResults[i].ip, info->ip)) { return; }
    }
    sLanResults[sLanCount] = *info;

    char playerText[64] = "";
    snprintf(playerText, 63, "%u/%u", info->players, info->maxPlayers);

    char mode[64] = "";
    snprintf(mode, 64, "%s", info->mode);

    bool disabled = strcmp(get_version(), info->version) != 0;
    if (disabled) {
        snprintf(mode, 64, "\\#ff0000\\[%s]", info->version);
    }

    char description[256] = "";
    snprintf(description, 256, "%s\n\n%s:%u\n\n%s\n%s\n\nSave file %u - %u stars",
             info->hostName, info->ip, info->gamePort, info->version, info->mode, info->saveSlot, info->stars);

    struct DjuiLobbyEntry* entry = djui_lobby_entry_create(&sLobbyLayout->base, (char*)info->hostName, mode, playerText, description, disabled, djui_panel_join_lan_entry, djui_lobby_on_hover, djui_lobby_on_hover_end);
    entry->base.tag = (s64)sLanCount;
    sLanCount++;
    djui_paginated_update_page_buttons(sLobbyPaginated);
}

static void djui_panel_join_lan_finish(void) {
    if (sTab != JOIN_TAB_LAN) { return; }
    djui_panel_join_browser_finish(DLANG(LOBBIES, NO_LAN_FOUND));
}

// ---- shared browser --------------------------------------------------------

static void djui_panel_join_browser_update_tabs(void) {
    if (sTabInternet) { djui_base_set_enabled(&sTabInternet->base, sTab != JOIN_TAB_INTERNET); }
    if (sTabLan)      { djui_base_set_enabled(&sTabLan->base,      sTab != JOIN_TAB_LAN); }
}

static void djui_panel_join_browser_populate(void) {
    if (!sLobbyLayout || !sRefreshButton) { return; }

    lan_discovery_scan_end();
    sLanCount = 0;
    djui_base_destroy_children(&sLobbyLayout->base);
    djui_panel_join_browser_update_tabs();

    djui_text_set_text(sRefreshButton->text, DLANG(LOBBIES, REFRESHING));
    djui_base_set_enabled(&sRefreshButton->base, false);

    if (sTab == JOIN_TAB_LAN) {
        if (!lan_discovery_scan_begin(djui_panel_join_lan_found, djui_panel_join_lan_finish)) {
            djui_panel_join_browser_show_message(DLANG(LOBBIES, LAN_SCAN_FAILED));
            djui_text_set_text(sRefreshButton->text, DLANG(LOBBIES, REFRESH));
            djui_base_set_enabled(&sRefreshButton->base, true);
        }
    } else {
#ifdef COOPNET
        if (!ns_coopnet_query(djui_panel_join_query, djui_panel_join_query_finish, sPassword)) {
            djui_panel_join_browser_show_message(DLANG(NOTIF, COOPNET_CONNECTION_FAILED));
            djui_text_set_text(sRefreshButton->text, DLANG(LOBBIES, REFRESH));
            djui_base_set_enabled(&sRefreshButton->base, true);
        }
#endif
    }
    djui_paginated_update_page_buttons(sLobbyPaginated);
}

static void djui_panel_join_lobbies_refresh(UNUSED struct DjuiBase* caller) {
    djui_panel_join_browser_populate();
}

#ifdef COOPNET
static void djui_panel_join_tab_internet(struct DjuiBase* caller) {
    if (configRulesVersion != RULES_VERSION) {
        djui_panel_rules_create(caller); // accepting the rules reopens this browser
        return;
    }
    sTab = JOIN_TAB_INTERNET;
    configJoinTab = sTab;
    djui_panel_join_browser_populate();
}
#endif

static void djui_panel_join_tab_lan(UNUSED struct DjuiBase* caller) {
    sTab = JOIN_TAB_LAN;
    configJoinTab = sTab;
    djui_panel_join_browser_populate();
}

void djui_panel_join_lobbies_on_destroy(UNUSED struct DjuiBase* caller) {
    lan_discovery_scan_end();
    if (sPassword) { free(sPassword); }
    sPassword = NULL;
    sRefreshButton = NULL;
    sTabInternet = NULL;
    sTabLan = NULL;
    sLobbyLayout = NULL;
    sLobbyPaginated = NULL;
    sLanCount = 0;

    if (sDescriptionPanel != NULL) {
        djui_base_destroy(&sDescriptionPanel->base);
        sDescriptionPanel = NULL;
    }
}

void djui_panel_join_lobbies_create(struct DjuiBase* caller, const char* password) {
    if (sPassword) { free(sPassword); sPassword = NULL; }
    sPassword = strdup(password);
    bool private = (strlen(password) > 0);

#ifdef COOPNET
    sTab = private ? JOIN_TAB_INTERNET : (configJoinTab == JOIN_TAB_LAN ? JOIN_TAB_LAN : JOIN_TAB_INTERNET);
    if (!private && sTab == JOIN_TAB_INTERNET && configRulesVersion != RULES_VERSION) {
        djui_panel_rules_create(caller);
        return;
    }
#else
    sTab = JOIN_TAB_LAN; // no CoopNet in this build: LAN + direct only
#endif

    djui_panel_join_lobby_description_create();

    struct DjuiBase* defaultBase = NULL;
    struct DjuiThreePanel* panel = djui_panel_menu_create(
        private ? DLANG(LOBBIES, PRIVATE_LOBBIES) : DLANG(JOIN, JOIN_TITLE),
        true);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
    {
        // tabs
        if (!private) {
            struct DjuiRect* tabs = djui_rect_container_create(body, 48);
#ifdef COOPNET
            sTabInternet = djui_button_left_create(&tabs->base, DLANG(JOIN, TAB_INTERNET), DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_tab_internet);
            djui_base_set_size(&sTabInternet->base, 0.485f, 48);
            sTabLan = djui_button_right_create(&tabs->base, DLANG(JOIN, TAB_LAN), DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_tab_lan);
            djui_base_set_size(&sTabLan->base, 0.485f, 48);
#else
            sTabLan = djui_button_create(&tabs->base, DLANG(JOIN, TAB_LAN), DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_tab_lan);
            djui_base_set_size(&sTabLan->base, 1.0f, 48);
#endif
        }

        // server list
        sLobbyPaginated = djui_paginated_create(body, 10);
        sLobbyLayout = sLobbyPaginated->layout;
        djui_flow_layout_set_margin(sLobbyLayout, 4);

        // Back | Refresh
        struct DjuiRect* rect2 = djui_rect_container_create(body, 64);
        {
            djui_button_left_create(&rect2->base, DLANG(MENU, BACK), DJUI_BUTTON_STYLE_BACK, djui_panel_menu_back);
            sRefreshButton = djui_button_right_create(&rect2->base, DLANG(LOBBIES, REFRESHING), DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_lobbies_refresh);
            djui_base_set_enabled(&sRefreshButton->base, false);
            defaultBase = &sRefreshButton->base;
        }

        // Direct Connection | Private Lobby
        if (!private) {
            struct DjuiRect* rect3 = djui_rect_container_create(body, 64);
            djui_button_left_create(&rect3->base, DLANG(JOIN, DIRECT), DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_direct_create);
#ifdef COOPNET
            djui_button_right_create(&rect3->base, DLANG(JOIN, PRIVATE_LOBBIES), DJUI_BUTTON_STYLE_NORMAL, djui_panel_join_private_create);
#endif
        }
    }

    if (gUpdateMessage) {
        struct DjuiText* message = djui_text_create(&panel->base, DLANG(NOTIF, UPDATE_AVAILABLE));
        djui_base_set_size_type(&message->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
        djui_base_set_size(&message->base, 1.0f, 1.0f);
        djui_base_set_color(&message->base, 255, 255, 160, 255);
        djui_text_set_alignment(message, DJUI_HALIGN_CENTER, DJUI_VALIGN_BOTTOM);
    }

    struct DjuiPanel* p = djui_panel_add(caller, panel, defaultBase);
    if (!p) { return; }
    p->on_panel_destroy = djui_panel_join_lobbies_on_destroy;

    djui_panel_join_browser_populate();
}
