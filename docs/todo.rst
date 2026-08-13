Deferred work
=============

Work that is knowingly incomplete, with enough of a pointer to pick it up.

This is **not** the issue tracker: anything a user has reported belongs at
https://github.com/openpmix/prrte/issues.  What is collected here is the
other kind — a path the code takes deliberately today because the better one
was left for later, a test that cannot exist yet, a measurement nobody has
been able to take.  Each entry says where the evidence is, so that the next
person can decide whether it is still true before acting on it.

Add to it when you leave something undone, and delete from it when you do
the work.

Runtime behavior
----------------

**An elastic re-extend that reuses a previously-shrunk node was reported to
hang.**  The sequence is: allocate one node, extend by three, shrink two,
then extend by one *reusing* a node the shrink released.  The first extend
completes; the re-extend does not
(`#2491 <https://github.com/openpmix/prrte/issues/2491>`_ closed the routing
half of this, not this half).  The mechanism identified at the time: a grow
completes only through ``DAEMONS_REPORTED`` → ``VM_READY``, which fires when
``daemons->num_procs == daemons->num_reported``, and both counters are
monotonic — neither is decremented when a daemon departs.  A shrink therefore
leaves them balanced, so the first extend is fine, while a re-extend raises
the fence for a daemon that must report before the grow can complete.  The
dockerswarm suite covers grow → shrink → re-grow by *hostname* and passes;
what has never been re-verified against current master is the same shape
driven through ``ras/slurm``, where the reuse code lives.  A case belongs in
``contrib/slurmswarm/run-tests.sh`` either way — it is cheap, and its
absence is why this is still an open question rather than a closed one.

**``ras/flux`` has no ``modify()``.**  It returns ``PMIX_ERR_NOT_SUPPORTED``
(``src/mca/ras/flux/ras_flux_module.c``), so the elastic extend/release
surface exists for SLURM only.  Everything above the component is
RM-agnostic; what is missing is the Flux-side conversation.

**``PMIX_RANGE_CUSTOM`` denies everyone.**  The data server's ``check_range``
has no accessor-list implementation and falls through to ``PMIX_ERROR``
(``src/runtime/data_server/``).  Denying is the safe direction for an
unimplemented rule, and it is the reason a publisher cannot express "these
specific processes may read this".

**``filem/raw`` does not survive losing a daemon mid-staging.**  Its fault
handler is deliberately minimal (``src/mca/filem/raw/filem_raw_module.c``);
the note in place observes that the real thing should be straightforward,
since the transport it stages over (xcast) is already resilient.  Until then,
a daemon lost while files are in flight fails the staging rather than
re-driving it.

**A promoted daemon's failure notice does not carry its ancestor list.**  In
``src/rml/rml_fault_handler.c``, a daemon whose parent changed reports the
failures in its subtree upward, but not the ancestor chain it now believes
in — so the new parent cannot confirm that the child agrees with it about the
shape of the repaired tree.

**Routing-tree state is not preserved across a DVM resize.**
``prte_rml_compute_routing_tree`` re-initializes the failure bitmaps on every
grow and restores only the permanent sets (``dead_dmns``, ``absent_dmns``);
whether anything else should survive the recompute is an open question marked
in ``src/rml/routed_radix.c``.

**RELM has one module and no way to choose another.**
``prte_relm_register`` registers the base implementation unconditionally
(``src/rml/relm/relm.c``); the MCA variable that would select between modules
does not exist.  This is only worth doing if a second module does.

**The post-fork child still calls into hwloc once.**  ``odls`` was converted
to async-signal-safe operations between ``fork`` and ``exec`` except for
``hwloc_set_membind``, which allocates internally; replacing it with a bare
``set_mempolicy``/``mbind`` means reproducing hwloc's NUMA nodeset handling
and was left for later (``src/mca/odls/AGENTS.md``).

Test coverage
-------------

**Init and finalize.**  Nothing covers the ``prte_init``/``prte_finalize``
sequence itself beyond a smoke test, and nothing configures with
``--disable-per-user-config-files``, so the ``#else`` arm of
``PRTE_WANT_HOME_CONFIG_FILES`` is never compiled in CI
(``src/runtime/AGENTS.md``).

**Heterogeneous DVMs.**  The byte-order helpers (``prte_hton64`` /
``prte_ntoh64``) are exercised by every inter-daemon message, but the case
they exist for — two daemons that disagree about endianness — cannot be built
from a container swarm on one host.  No test can currently fail if they are
wrong.

**The resource managers nobody has.**  ``ras``/``plm``/``ess`` components for
PBS, LSF, gridengine, Flux and PALS are compile-only in CI
(``--enable-testbuild-launchers``, and for the ones needing third-party
headers, against declaration-only stubs).  A build proves they still compile;
only a real allocation on such a system proves anything else.  SLURM is the
exception — ``contrib/slurmswarm`` runs a real one.

**macOS.**  ``contrib/dockerswarm/run-tests.sh macos`` is a single-host
subset by construction.  Everything multi-node on that platform is untested.

**``SLURM_TASKS_PER_NODE`` in its single-node spelling.**  The suite asserts
the ``2(xN)`` form that a multi-node allocation produces; the ``2(x1)`` form
goes through the same parser and is not separately covered.

Performance work not landed
---------------------------

**Scatter the launch message's per-proc residual.**  Half of "broadcast the
maps, scatter the residuals" is done and the other half is not.  Lazy
proc-data registration has landed (a daemon publishes PMIx proc-data only for
the procs it hosts, and ``derive_proc_data`` in ``pmix_server_fence.c``
answers for the rest), and so has sending placement as a node map plus a proc
map per app.  What still goes to every daemon is a per-proc record of what
the maps cannot say.

Measured on master with ``prterun --rtos donotlaunch`` and ``--prtemca
plm_base_verbose 2``, against a PMIx built *without* debug so the packing is
production-shaped:

===================  =========  ==================
nodes x ppn          procs      launch message
===================  =========  ==================
100 x 8              800        14,592 B
100 x 128            12,800     203,600 B
500 x 128            64,000     950,423 B
1000 x 128           128,000    1,878,828 B
===================  =========  ==================

About **14.5 B/proc and 18 B/node**, so at a realistic 1000 x 128 the
per-proc residual is **~99% of the message** — broadcast in full to every
daemon.  Its composition, from the same shape at three binding policies
(default 1,878,828 / ``--bind-to core`` 1,516,829 / ``--bind-to none``
1,114,829): the cpuset is roughly 6 B/proc and the rest is node rank (2),
state (4) and an attribute count (4).

Four things are settled, and the fourth is the one that stops this being
obvious:

* **The attribute count is dead weight, always.**  Exactly one proc
  attribute is set anywhere in the tree — ``PRTE_PROC_NOBARRIER``, in
  ``odls_base_default_fns.c`` — and it is ``PRTE_ATTR_LOCAL``, which
  ``prte_proc_pack``'s filter excludes.  The list is empty on the wire in
  every job, so those 4 B/proc buy nothing at any scale.
* **The state is not dead, but no daemon reads it for a proc it does not
  host.**  The odls launch loop gates on ``PRTE_PROC_STATE_INIT`` /
  ``_RESTART``, so the value distinguishes a fresh launch from a restart and
  cannot be dropped or hardcoded.  Every other read is of a local child.  It
  belongs in the scattered slice rather than in the broadcast.  Note
  ``prte_job_pack`` has a second caller — ``prte_util_encode_job_catchup``,
  which packs *already-running* jobs for a joining daemon — so "it has not
  started yet" is not a safe premise.
* **Locality need not be sent, and is not.**  It is generated from the
  cpuset by ``PMIx_server_generate_locality_string``.  But that API takes no
  topology argument: ``pmix_hwloc_generate_locality_string`` walks
  ``pmix_globals.topology.topology``, the *caller's own*.  So a daemon
  computing a remote proc's locality is assuming that node's topology matches
  its own — pre-existing, since the eager registration did it for every proc
  in the job, and an argument for the scatter rather than against it.
* **A cpuset dictionary does not work.**  Under a regular mapping the
  distinct cpusets number ``ppn`` rather than ``nprocs`` (measured: 8 distinct
  across 800 procs on 100 nodes at ``--bind-to core``, one per local rank), so
  a dictionary plus a per-proc index looks like it collapses the cost.  It
  does not generalize: plenty of placement strategies give equal local ranks
  on different nodes *different* cpusets, and then the distinct count
  approaches the proc count and dictionary-plus-index is strictly worse than
  what is there now.  A raw bitmap is also worse, not better — 16 bytes for a
  128-core node against 9 for the string ``"0-43"``.

So the residual to scatter is the cpuset and the state, and the shape is a
per-destination payload: job-level fields and maps to everyone as now, each
daemon receiving only the records for the procs it will fork.

Two things make it tractable, and two make it work:

* Lazy proc-data registration is what makes it *safe*.  Under the maps,
  ``derive_proc_data`` gets rank, global rank, app rank, appnum, local rank,
  node id and hostname from the maps alone; a scatter costs it exactly node
  rank, cpuset and the locality derived from the cpuset, and those fall
  through to a real direct modex to the daemon that forked the proc — a path
  that now exists and is exercised by ``contrib/dockerswarm``'s ``peerinfo``.
* It needs **no new broadcast movement**.  ``prte_rml_get_route`` picks the
  next hop toward its target, so N routed point-to-point sends aggregate in
  the tree: bytes leaving the HNP are the sum of the slices rather than the
  sum replicated, and each hop relays only its subtree's share.
* **Keep node rank in the broadcast** (2 B/proc) even so.  A remote node-rank
  get that finds nothing does not fail — it falls through to PMIx's
  one-job-per-node assumption and returns a *wrong* value.  That is the only
  silent-wrong-answer risk in the whole change, and 0.26 MB at 1000 x 128
  buys it away.
* The cost is a contract change, not a code change.  The launch message stops
  being one payload delivered identically and becomes common plus
  per-destination; ``prte_job_pack``/``_unpack`` are hand-written mirrors with
  no version and nothing catches a half-done edit; the receiver must handle
  two pieces with an ordering constraint; and an elastic grow needs the
  joining daemon's slice.

The written-up case is in ``docs/plans/scalable_collectives.rst``, "What a
client actually asks about a remote peer" — including the scan finding that
nothing in Open MPI asks a remote proc for ``PMIX_CPUSET`` or
``PMIX_NODE_RANK``, and that ``PMIX_LOCALITY`` never crosses the wire in
either direction.

**Small effects cannot be measured end-to-end on the container swarm.**  At
40 nodes the collect fence's wall clock has a run-to-run coefficient of
variation of 35–50%, so anything under about a third is not resolvable there
however many arms the sweep has — an attempt to settle whether compressing
the xcast payload helps (an effect of 1–7%) produced overlapping ranges whose
*sign* flipped between radices.  Measure the mechanism directly instead
(``--prtemca grpcomm_base_verbose 1`` reports the size, ratio and
microseconds of every broadcast).  See ``contrib/dockerswarm/AGENTS.md``,
§18.

