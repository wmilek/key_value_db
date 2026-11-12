#ifndef STUB_LOG_H
#define STUB_LOG_H
#include <stdio.h>
extern int hb_verbose;
#define LOG_MODULE_REGISTER(...)
#define LOG_MODULE_DECLARE(...)
#define LOG_ERR(...)  do { if (hb_verbose) { fprintf(stderr, "ERR  "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr);} } while (0)
#define LOG_WRN(...)  do { if (hb_verbose) { fprintf(stderr, "WRN  "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr);} } while (0)
#define LOG_INF(...)  do { if (hb_verbose > 1) { fprintf(stderr, "INF  "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr);} } while (0)
#define LOG_DBG(...)  do { if (hb_verbose > 2) { fprintf(stderr, "DBG  "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr);} } while (0)
#endif
