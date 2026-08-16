# scenario — everything done *with* a population. Returns structs, never
# prints, so both frontends measure the same thing.
target_sources(app PRIVATE scenario.c)
target_include_directories(app PRIVATE .)
