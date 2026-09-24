#ifndef STUB_LOG_H
#define STUB_LOG_H
extern void stub_log(const char *fmt, ...);
#define LOG_MODULE_REGISTER(...)
#define LOG_ERR(...) stub_log(__VA_ARGS__)
#define LOG_WRN(...) stub_log(__VA_ARGS__)
#define LOG_INF(...) stub_log(__VA_ARGS__)
#define LOG_DBG(...) stub_log(__VA_ARGS__)
#endif
