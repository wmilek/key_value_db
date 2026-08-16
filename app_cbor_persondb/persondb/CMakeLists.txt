# persondb — the person management API, and the only layer that talks to the
# storage stack. person_cbor is private to it: nothing above this directory
# encodes or decodes a record (checked as DESIGN.md A7).
target_sources(app PRIVATE persondb.c person_cbor.c)
target_include_directories(app PRIVATE .)
