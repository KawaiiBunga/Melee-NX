// The one zlib function switch/deko/dksh_file.cpp uses, implemented in host_stubs.cpp (the standard
// reflected CRC-32, polynomial 0xEDB88320, as zlib's), so the tool needs no zlib.
#pragma once
typedef unsigned long uLong;
typedef unsigned int uInt;
typedef unsigned char Bytef;
extern "C" uLong crc32(uLong crc, const Bytef* buf, uInt len);
