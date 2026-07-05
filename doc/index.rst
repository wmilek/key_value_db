Zephyr Key-Value DB
===================

A crash-safe, layered key-value storage stack for Zephyr, running on raw flash
partitions or on a wear-leveled UBI volume.

At the bottom sits ``blob_db``: a store of blobs addressed by a stable 64-bit
id, where every visible operation is atomic against power loss. Above it, the
root registry, containers and access interfaces build data structures and
ergonomic APIs out of those ids — every persistent structure reachable from a
single well-known integer, so nothing has to be rebuilt at mount.

These pages are the design documents. Start with :doc:`architecture` for the
stack as a whole and :doc:`principles` for the rules every layer obeys; the
per-layer documents under *Design — contracts* state what an upper layer may
rely on, while *Design — implementation* holds feasibility proofs of those
contracts that nothing above may depend on.

The API reference for the public headers is generated separately by Doxygen and
published alongside these pages under ``doxygen/``. Build and usage
instructions live in the repository ``README.md``.

.. toctree::
   :maxdepth: 2
   :caption: Design — contracts

   architecture
   principles
   layers/l0_flash
   layers/l1_blob_db
   layers/l1_model_container
   layers/l1_root_registry
   layers/l2_containers
   layers/l3_interfaces

.. toctree::
   :maxdepth: 1
   :caption: Design — implementation

   impl/l1_bucketlog
   impl/l2_logring

.. toctree::
   :maxdepth: 1
   :caption: Design — proposals

   proposals/2026-08-09-large-payloads
   proposals/2026-08-09-large-payloads-cost
   proposals/2026-08-09-kvhash-impact
   proposals/2026-08-09-rootreg-kvdb-impact
   proposals/2026-08-09-implementation-plan

.. toctree::
   :maxdepth: 1
   :caption: Design — reviews

   reviews/2026-07-26-design-doc-review

.. toctree::
   :maxdepth: 2
   :caption: Zephyr

   zephyr

Indices and tables
==================

* :ref:`genindex`
* :ref:`search`
