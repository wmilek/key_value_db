# ui — the frontends. Exactly one is compiled, chosen by Kconfig; both are
# argument parsing and formatting over scenario.h.
target_sources(app PRIVATE main.c)
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_FRONTEND_BENCH app PRIVATE ui_bench.c)
target_sources_ifdef(CONFIG_APP_CBOR_PERSONDB_FRONTEND_SHELL app PRIVATE ui_shell.c)
