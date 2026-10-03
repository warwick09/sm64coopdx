#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_menu.h"
#include "djui_panel_join_lobbies.h"

// "Join" goes straight to the server browser (Internet / LAN tabs).
void djui_panel_join_create(struct DjuiBase* caller) {
    djui_panel_join_lobbies_create(caller, "");
}
