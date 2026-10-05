// IDF6 兼容垫片：pgmspace 家族宏/函数置空（PROGMEM 表按普通 const 内存访问，
// Xtensa 平坦地址空间语义正确——与 Arduino ESP32 的 pgm_read 宏行为一致）
#pragma once

#ifndef LAAP_PGMSPACE_COMPAT_H
#define LAAP_PGMSPACE_COMPAT_H

#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PGM_P
#define PGM_P const char *
#endif
#ifndef PSTR
#define PSTR(s) (s)
#endif
#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const unsigned char *)(addr))
#endif
#ifndef pgm_read_word
#define pgm_read_word(addr) (*(const unsigned short *)(addr))
#endif
#ifndef pgm_read_dword
#define pgm_read_dword(addr) (*(const unsigned int *)(addr))
#endif

#endif
