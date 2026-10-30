/* Minimal freestanding declarations for the unchanged xxHash header. */
#ifndef SPARC64_WASM_STRING_H_
#define SPARC64_WASM_STRING_H_
#include <stddef.h>
void* memcpy(void*, const void*, size_t);
void* memset(void*, int, size_t);
int memcmp(const void*, const void*, size_t);
#endif
