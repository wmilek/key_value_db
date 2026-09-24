# dataset — the synthetic population, a pure function of an index.
# Depends on persondb only for the record type it fills in; touches no storage.
target_sources(app PRIVATE dataset.c)
target_include_directories(app PRIVATE .)
