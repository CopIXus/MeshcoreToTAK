#pragma once

#include <stdint.h>
#include <string.h>

// UTF-8 helpers. Node names and chat text (emoji included) arrive as raw bytes and are
// cut to fixed buffers; a cut in the middle of a character yields invalid UTF-8, which
// strict XML parsers (TAK Server) reject for the whole event.

// Length of the valid UTF-8 sequence at s (1-4), or 0 if it is invalid or truncated.
inline size_t utf8SeqLen(const char* s) {
  uint8_t c = (uint8_t)s[0];
  size_t n;
  uint32_t cp;
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) n = 2, cp = c & 0x1F;
  else if (c >= 0xE0 && c <= 0xEF) n = 3, cp = c & 0x0F;
  else if (c >= 0xF0 && c <= 0xF4) n = 4, cp = c & 0x07;
  else return 0;
  for (size_t i = 1; i < n; i++) {
    uint8_t d = (uint8_t)s[i];
    if ((d & 0xC0) != 0x80) return 0;  // also stops at the NUL terminator
    cp = (cp << 6) | (d & 0x3F);
  }
  if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;  // overlong / range
  if (cp >= 0xD800 && cp <= 0xDFFF) return 0;  // surrogates
  return n;
}

// Drop a partial character left at the end of s by a byte-count truncation.
inline void utf8TrimTail(char* s) {
  size_t n = strlen(s);
  size_t i = n;
  while (i > 0 && n - i < 3 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) i--;  // back over continuation bytes
  if (i == 0) {
    if (n) s[0] = 0;
    return;
  }
  if ((uint8_t)s[i - 1] >= 0x80 && utf8SeqLen(s + i - 1) != n - i + 1) s[i - 1] = 0;
}

// strncpy that always terminates and never splits a character.
inline void utf8Copy(char* dest, size_t dest_len, const char* src) {
  if (!dest || !dest_len) return;
  strncpy(dest, src ? src : "", dest_len - 1);
  dest[dest_len - 1] = 0;
  utf8TrimTail(dest);
}
