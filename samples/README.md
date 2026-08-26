# Samples

Small, single-purpose programs that show **how to use one API**, and nothing
else. Each sample narrates every call it makes and the value it returned, so
the console output reads like the API documentation it demonstrates.

What a sample deliberately is *not*: it does not time anything, does not sweep
parameters, and does not chase a headline number. Those belong to the
benchmark applications one level up:

| If you want | Look at |
|---|---|
| how an API is called, in the smallest complete program | `samples/` (here) |
| how fast it is on real hardware | [`app_perf/`](../app_perf), [`app_perf_mc/`](../app_perf_mc), [`app_perf_kvdb/`](../app_perf_kvdb) |
| how to build a real product on the stack | [`app_cbor_persondb/`](../app_cbor_persondb) |

| Sample | Demonstrates |
|---|---|
| [`kvhash/`](kvhash) | the L2 Map shape (`kvhash_map_ops`): create / get / set / del over a persistent hash map, plus where a container's root id comes from |

## Building and running

Every sample builds standalone and runs on `native_sim`, so no hardware is
needed to follow along:

```shell
west build -p always -b native_sim samples/kvhash
./build/zephyr/zephyr.exe
```

They are ordinary Zephyr applications, so any supported board works too — the
`boards/` directory in each sample carries the storage geometry for
`native_sim` and the nRF5340-DK:

```shell
west build -p always -b nrf5340dk/nrf5340/cpuapp samples/kvhash
west flash
```

Twister builds and runs them alongside the test suites:

```shell
west twister -T key_value_db/samples -p native_sim -v --inline-logs
```
