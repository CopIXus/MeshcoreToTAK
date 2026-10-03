#include "TakCot.h"
#include "TakText.h"
#include "TakVersion.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// Output is always valid UTF-8 XML text: invalid or truncated sequences and XML-illegal
// control characters are dropped, and a character that does not fit whole is not started.
void TakCot::xmlEscape(const char* in, char* out, size_t out_len) {
  size_t o = 0;
  if (!in) {
    out[0] = 0;
    return;
  }
  for (size_t i = 0; in[i];) {
    char c = in[i];
    size_t n = utf8SeqLen(in + i);
    if (!n) {
      i++;
      continue;
    }
    const char* rep = nullptr;
    if (c == '&') rep = "&amp;";
    else if (c == '<') rep = "&lt;";
    else if (c == '>') rep = "&gt;";
    else if (c == '"') rep = "&quot;";
    else if (c == '\'') rep = "&apos;";
    else if ((uint8_t)c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
      i++;
      continue;
    }
    const char* src = rep ? rep : in + i;
    size_t len = rep ? strlen(rep) : n;
    if (o + len >= out_len) break;
    memcpy(out + o, src, len);
    o += len;
    i += n;
  }
  out[o] = 0;
}

void TakCot::formatTime(time_t t, char* buf, size_t len) {
  struct tm tm_buf;
  gmtime_r(&t, &tm_buf);
  snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
           tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
           tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
}

// "uid:Group/name" (CloudTAK web form) -> "uid/Group/name.png". ATAK and TAK Portal
// resolve usericons by exact file path, so the extension is required.
void TakCot::iconToPath(const char* icon, char* out, size_t out_len) {
  if (!icon) {
    out[0] = 0;
    return;
  }
  size_t n = 0;
  const char* last_seg = out;
  for (size_t i = 0; icon[i] && n + 1 < out_len; i++) {
    char c = (icon[i] == ':' || icon[i] == '\\') ? '/' : icon[i];
    out[n++] = c;
    if (c == '/') last_seg = out + n;
  }
  out[n] = 0;
  if (n && !strchr(last_seg, '.') && n + 4 < out_len) {
    memcpy(out + n, ".png", 5);
  }
}

int32_t TakCot::colorToArgb(const char* hex_color, float opacity) {
  // #RRGGBB + opacity -> signed 0xAARRGGBB, as ATAK expects
  if (!hex_color || hex_color[0] != '#' || strlen(hex_color) < 7) return -1;
  unsigned r = 0, g = 0, b = 0;
  sscanf(hex_color + 1, "%02x%02x%02x", &r, &g, &b);
  if (!(opacity > 0.0f) || opacity > 1.0f) opacity = 1.0f;
  uint32_t a = (uint32_t)(opacity * 255.0f + 0.5f);
  return (int32_t)((a << 24) | (r << 16) | (g << 8) | b);
}

size_t TakCot::buildPoint(char* dest, size_t dest_len, const TakNodeRecord& node,
                          const TakPrefs& prefs, time_t now_utc) {
  if (!dest || dest_len < 256) return 0;
  const TakCotStyle& style = TakNodes::styleFor(TakNodes::matchFilter(node.name, prefs), prefs);
  char t0[32], t1[32], call_esc[80], rem_esc[96], icon_path[TAK_ICON_LEN + 8], usericon[200];
  formatTime(now_utc, t0, sizeof(t0));
  formatTime(now_utc + (time_t)prefs.stale_sec, t1, sizeof(t1));

  char callsign[sizeof(node.name)];
  TakNodes::callsignFor(node.name, prefs, callsign, sizeof(callsign));
  xmlEscape(callsign, call_esc, sizeof(call_esc));
  xmlEscape(style.remarks, rem_esc, sizeof(rem_esc));

  int32_t color = colorToArgb(style.marker_color, style.marker_opacity);

  // Empty icon = let the CoT type pick the symbol (2525 / marker) on the client.
  usericon[0] = 0;
  if (style.icon[0]) {
    iconToPath(style.icon, icon_path, sizeof(icon_path));
    char icon_esc[TAK_ICON_LEN + 24];
    xmlEscape(icon_path, icon_esc, sizeof(icon_esc));
    snprintf(usericon, sizeof(usericon), "<usericon iconsetpath='%s'/>", icon_esc);
  } else if (strcmp(style.type, "b-m-p-s-m") == 0) {
    // ATAK colors spot markers from this path, not from <color>.
    snprintf(usericon, sizeof(usericon), "<usericon iconsetpath='COT_MAPPING_SPOTMAP/b-m-p-s-m/%ld'/>",
             (long)color);
  }

  int n = snprintf(
      dest, dest_len,
      "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>"
      "<event version='2.0' uid='%s' type='%s' how='%s' time='%s' start='%s' stale='%s'>"
      "<point lat='%.6f' lon='%.6f' hae='9999999.0' ce='9999999.0' le='9999999.0'/>"
      "<detail>"
      "<contact callsign='%s'/>"
      "<remarks>%s</remarks>"
      "%s"
      "<color argb='%ld' value='%ld'/>"
      "%s"
      "<takv device='MeshCore GPS Tracker' platform='MeshCore TAK Gateway' version='" TAK_GW_VERSION "'/>"
      "</detail>"
      "</event>",
      node.uid, style.type, style.how, t0, t0, t1, node.lat, node.lon, call_esc, rem_esc, usericon,
      (long)color, (long)color, style.archived ? "<archive/>" : "");

  if (n < 0 || (size_t)n >= dest_len) return 0;
  return (size_t)n;
}

size_t TakCot::buildDelete(char* dest, size_t dest_len, const char* uid, time_t now_utc) {
  char t0[32], t1[32];
  formatTime(now_utc, t0, sizeof(t0));
  formatTime(now_utc + 5, t1, sizeof(t1));
  int n = snprintf(
      dest, dest_len,
      "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>"
      "<event version='2.0' uid='%s' type='t-x-d-d' how='h-g-i-g-o' time='%s' start='%s' stale='%s'>"
      "<point lat='0' lon='0' hae='9999999.0' ce='9999999.0' le='9999999.0'/>"
      "<detail><link uid='%s' relation='p-p'/></detail>"
      "</event>",
      uid, t0, t0, t1, uid);
  if (n < 0 || (size_t)n >= dest_len) return 0;
  return (size_t)n;
}

static void fmtPoint(float lat, float lon, char* out, size_t len) {
  if (lat == 0.0f && lon == 0.0f) snprintf(out, len, "lat='0' lon='0' hae='9999999.0' ce='9999999.0'");
  else snprintf(out, len, "lat='%.6f' lon='%.6f' hae='9999999.0' ce='50.0'", lat, lon);
}

size_t TakCot::buildChat(char* dest, size_t dest_len, const char* gw_uid, const char* room,
                         const char* sender, const char* text, float lat, float lon, time_t now_utc) {
  char t0[32], t1[32], pt[96], room_esc[TAK_ROOM_LEN * 6], snd_esc[TAK_CALLSIGN_LEN * 6], txt_esc[1024];
  formatTime(now_utc, t0, sizeof(t0));
  formatTime(now_utc + 3600, t1, sizeof(t1));
  fmtPoint(lat, lon, pt, sizeof(pt));
  xmlEscape(room, room_esc, sizeof(room_esc));
  xmlEscape(sender, snd_esc, sizeof(snd_esc));
  xmlEscape(text, txt_esc, sizeof(txt_esc));
  char msg_id[24];
  snprintf(msg_id, sizeof(msg_id), "%08lx%08lx", (unsigned long)now_utc, (unsigned long)esp_random());

  int n = snprintf(
      dest, dest_len,
      "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>"
      "<event version='2.0' uid='GeoChat.%s.%s.%s' type='b-t-f' how='h-g-i-g-o' time='%s' start='%s' stale='%s'>"
      "<point %s le='9999999.0'/>"
      "<detail>"
      "<__chat parent='RootContactGroup' groupOwner='false' messageId='%s' chatroom='%s' id='%s' senderCallsign='%s'>"
      "<chatgrp uid0='%s' uid1='%s' id='%s'/></__chat>"
      "<link uid='%s' type='a-f-G-U-C' relation='p-p'/>"
      "<remarks source='BAO.F.ATAK.%s' to='%s' time='%s'>%s</remarks>"
      "</detail></event>",
      gw_uid, room_esc, msg_id, t0, t0, t1, pt, msg_id, room_esc, room_esc, snd_esc, gw_uid, room_esc,
      room_esc, gw_uid, gw_uid, room_esc, t0, txt_esc);
  if (n < 0 || (size_t)n >= dest_len) return 0;
  return (size_t)n;
}

size_t TakCot::buildPresence(char* dest, size_t dest_len, const char* gw_uid, const TakPrefs& prefs,
                             time_t now_utc) {
  char t0[32], t1[32], pt[96], call_esc[TAK_CALLSIGN_LEN * 6], rooms[160] = "", rooms_esc[400];
  formatTime(now_utc, t0, sizeof(t0));
  formatTime(now_utc + 600, t1, sizeof(t1));
  fmtPoint(prefs.chat_lat, prefs.chat_lon, pt, sizeof(pt));
  xmlEscape(prefs.chat_callsign[0] ? prefs.chat_callsign : "MeshCore GW", call_esc, sizeof(call_esc));
  for (int i = 0; i < TAK_MAX_CHAT; i++) {
    const TakChatChannel& c = prefs.chat[i];
    if (!c.enabled || !c.secret_len) continue;
    size_t l = strlen(rooms);
    snprintf(rooms + l, sizeof(rooms) - l, "%s%s", l ? ", " : "", c.room);
  }
  if (prefs.public_on) {
    size_t l = strlen(rooms);
    snprintf(rooms + l, sizeof(rooms) - l, "%s%s (Public)", l ? ", " : "", prefs.public_room);
  }
  xmlEscape(rooms, rooms_esc, sizeof(rooms_esc));

  // ATAK only sends into a chat room when this contact is an online member of it.
  // The uid must match the __chat id used by buildChat, which is the room name.
  char group_xml[400] = "";
  const char* added[TAK_MAX_CHAT];
  int n_added = 0;
  for (int i = 0; i < TAK_MAX_CHAT; i++) {
    const TakChatChannel& c = prefs.chat[i];
    if (!c.enabled || !c.secret_len || !c.room[0]) continue;
    bool dup = false;
    for (int j = 0; j < n_added; j++) dup = dup || strcasecmp(added[j], c.room) == 0;
    if (dup) continue;
    added[n_added++] = c.room;
    char esc[TAK_ROOM_LEN * 6];
    xmlEscape(c.room, esc, sizeof(esc));
    size_t l = strlen(group_xml);
    snprintf(group_xml + l, sizeof(group_xml) - l, "<group name='%s' uid='%s'/>", esc, esc);
  }
  char groups_el[420] = "";
  if (group_xml[0]) snprintf(groups_el, sizeof(groups_el), "<groups>%s</groups>", group_xml);

  int n = snprintf(
      dest, dest_len,
      "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>"
      "<event version='2.0' uid='%s' type='a-f-G-U-C' how='h-e' time='%s' start='%s' stale='%s'>"
      "<point %s le='9999999.0'/>"
      "<detail>"
      "<contact callsign='%s' endpoint='*:-1:stcp'/>"
      "<uid Droid='%s'/>"
      "<__group name='Cyan' role='Team Member'/>"
      "%s"
      "<remarks>MeshCore chat bridge: %s</remarks>"
      "<takv device='Heltec V3' platform='MeshCore TAK Gateway' os='ESP32' version='" TAK_GW_VERSION "'/>"
      "</detail></event>",
      gw_uid, t0, t0, t1, pt, call_esc, call_esc, groups_el, rooms_esc);
  if (n < 0 || (size_t)n >= dest_len) return 0;
  return (size_t)n;
}
