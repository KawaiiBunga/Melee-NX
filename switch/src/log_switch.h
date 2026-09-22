#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// Start after stdout/stderr have been redirected. Before init, writes are synchronous.
void melee_nx_log_init(void);
double melee_nx_log_now_ms(void);
void melee_nx_log_write(const char* bytes, size_t size);
void melee_nx_log_write_critical(const char* bytes, size_t size);
// Drains accepted records and commits them. Used for errors, panic and fast exit.
void melee_nx_log_flush(void);
// Joins the writer; used by host regression tests and orderly teardown.
void melee_nx_log_shutdown(void);
#ifdef __cplusplus
}
#endif
