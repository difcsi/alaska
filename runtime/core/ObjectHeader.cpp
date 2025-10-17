/*
 * This file is part of the Alaska Handle-Based Memory Management System
 *
 * Copyright (c) 2025, Nick Wanninger <ncw@u.northwestern.edu>
 * Copyright (c) 2025, The Constellation Project
 * All rights reserved.
 *
 * This is free software.  You are permitted to use, redistribute,
 * and modify it as specified in the file "LICENSE".
 */

#include <alaska/ObjectHeader.hpp>
#include <alaska/alaska.hpp>
#include <cstdint>

#define C_RED 91
#define C_GREEN 92
#define C_YELLOW 93
#define C_BLUE 94
#define C_MAGENTA 95
#define C_CYAN 96

#define C_RESET 0
#define C_GRAY 90


static int current_color = 0;
static void set_color(int code) {
  if (code != current_color) {
    alaska::printf("\x1b[%dm", code);
    current_color = code;
  }
}

static void set_color_for(char c, int fallback = C_GRAY) {
  if (c >= 'A' && c <= 'z') {
    set_color(C_YELLOW);
  } else if (c >= '!' && c <= '~') {
    set_color(C_CYAN);
  } else if (c == '\n' || c == '\r') {
    set_color(C_GREEN);
  } else if (c == '\a' || c == '\b' || c == 0x1b || c == '\f' || c == '\n' || c == '\r') {
    set_color(C_RED);
  } else if ((unsigned char)c == 0xFF) {
    set_color(C_MAGENTA);
  } else {
    set_color(fallback);
  }
}

#define HEX_META

static bool use_binary = false;

static void hexdump(void *vbuf, size_t len, bool use_colors) {
#ifndef HEX_META
  use_colors = false;
#endif

  unsigned awidth = 8;
  // if (len > 0xFFFFL) awidth = 8;

  unsigned char *buf = (unsigned char *)vbuf;
  size_t w = use_binary ? 8 : 16;
  uint32_t checksum = 0;

  for (size_t i = 0; i < len; i += w) {
    unsigned char *line = buf + i;

    if (use_colors) {
      set_color(C_RESET);
      alaska::printf("|");
      set_color(C_GRAY);

      alaska::printf("%.*zx", awidth, (size_t)vbuf + i);

      set_color(C_RESET);
      alaska::printf("|");
    }
    for (size_t c = 0; c < w; c++) {
      if (c % 8 == 0) {
        if (use_colors) alaska::printf(" ");
      }
      if (i + c >= len) {
        alaska::printf("   ");
      } else {
        if (use_colors) set_color_for(line[c]);
        if (use_binary) {
          for (int j = 7; j >= 0; j--) {
            alaska::printf("%u", (line[c] >> j) & 1);
          }
          alaska::printf(" ");
        } else {
          alaska::printf("%02X ", line[c]);
        }
        checksum += line[c];
      }
    }

    if (use_colors) {
      set_color(C_RESET);
      alaska::printf("|");
      for (size_t c = 0; c < w; c++) {
        if (c != 0 && (c % 8 == 0)) {
          set_color(C_RESET);
          alaska::printf(" ");
        }

        if (i + c >= len) {
          alaska::printf(" ");
        } else {
          set_color_for(line[c]);

          alaska::printf("%c", (line[c] < 0x20) || (line[c] > 0x7e) ? '.' : line[c]);
        }
      }
      set_color(C_RESET);
      alaska::printf("|\n");

    } else {
      alaska::printf("\n");
    }
  }

  if (use_colors) {
    set_color(C_RESET);
  }
  alaska::printf("   Checksum (16-bit): 0x%04X\n", static_cast<unsigned>(checksum & 0xFFFF));
}



static void hexdump_handles(void *vbuf, size_t len) {
  const size_t handles_per_line = 4;
  const size_t bytes_per_handle = sizeof(uint64_t);
  const size_t line_bytes = handles_per_line * bytes_per_handle;

  uint64_t *handles = static_cast<uint64_t *>(vbuf);
  unsigned char *bytes = static_cast<unsigned char *>(vbuf);

  size_t total_bytes = len;
  size_t total_handles = len / bytes_per_handle;

  if (total_bytes == 0) {
    alaska::printf("\n");
    set_color(C_RESET);
    return;
  }

  uintptr_t offset = 0;
  size_t byte_offset = 0;

  while (byte_offset < total_bytes) {
    set_color(C_RESET);
    alaska::printf("   |");
    set_color(C_GRAY);
    alaska::printf("%16lx", (uintptr_t)(bytes + offset));
    set_color(C_RESET);
    alaska::printf("|");

    for (size_t h = 0; h < handles_per_line; ++h) {
      size_t index = offset + h;
      if (index < total_handles) {
        uint64_t val = handles[index];
        alaska::Mapping *m;
        void *data;



        if (alaska::check_mapping((void *)val, m, data)) {
          off_t pointer_page = ((uintptr_t)&handles[index]) >> 12;
          off_t pointee_page = ((uintptr_t)data) >> 12;
          if (pointer_page == pointee_page) {
            set_color(C_GREEN);
          } else {
            set_color(C_CYAN);
          }
        } else if (alaska::Mapping::from_handle_safe((void *)val) != nullptr) {
          set_color(C_RED);
        } else {
          set_color(C_GRAY);
        }

        alaska::printf(" %016lx", val);
        set_color(C_RESET);
      } else {
        alaska::printf("                 ");
      }
    }

    set_color(C_RESET);
    alaska::printf(" |");

    size_t bytes_this_line = line_bytes;
    if (byte_offset + bytes_this_line > total_bytes) {
      bytes_this_line = total_bytes - byte_offset;
    }

    for (size_t b = 0; b < line_bytes; ++b) {
      if (b != 0 && (b % 8 == 0)) {
        set_color(C_RESET);
        alaska::printf(" ");
      }

      if (b < bytes_this_line) {
        unsigned char ch = bytes[byte_offset + b];
        set_color_for(static_cast<char>(ch));
        alaska::printf("%c", (ch < 0x20) || (ch > 0x7e) ? '.' : ch);
      } else {
        alaska::printf(" ");
      }
    }

    set_color(C_RESET);
    alaska::printf("|\n");

    offset += handles_per_line;
    byte_offset += line_bytes;
  }

  set_color(C_RESET);
}

namespace alaska {


  void ObjectHeader::hexdump(void) {
    size_t size = this->object_size();

    // First dump the rest of the data in the object header:
    alaska::printf(
        "Object @ %p, %zu bytes, handle = %p", this, size, get_mapping()->to_handle());
    alaska::printf(" %s", this->marked ? "MARKED" : "");
    alaska::printf(" %s", this->marked ? "LOCALIZED" : "");
    alaska::printf("\n");

    // ::hexdump(this->data(), size, true);
    ::hexdump_handles(this->data(), size);
  }

}  // namespace alaska
