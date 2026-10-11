Tool support
============

A *tool* is any process that connects to a PRRTE daemon's PMIx server
with ``PMIx_tool_init()`` rather than being a client of it: ``prun``,
``pterm``, a debugger, a monitoring agent, a scheduler. This page sets
out how PRRTE gives each tool connection an identity, how it records the
processes behind those connections, and how it notices them leaving. It
then lists the changes that bring the code into line with that model,
what PRRTE relies on from PMIx, and how it is all tested.

Line references are to master ``a783dab594``.


Context
-------

The uncommitted changes on this branch added
``prte_pmix_server_tool_join()`` to decide whether a tool may join a
namespace the DVM already holds. Review showed that the decision rested
on the wrong distinction:

* It branched on ``PRTE_JOB_FLAG_TOOL``, but PRRTE also sets that flag on
  jobs it launches (``PMIX_SPAWN_TOOL``, debugger daemons).
* It tested a launched process against the DVM's euid, rather than the
  job's owner.

Mapping the whole path showed more. What a tool does after it connects -
clones, extra ranks, leaving, coming back - is handled incorrectly or not
at all, and the join rule alone cannot fix that. This page therefore sets
out one model for all of it first, then the changes that make the code
follow it. The branch's other changes stay; only ``tool_join``'s rule is
replaced.


The model
---------

Two kinds of job a tool connection can name
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Every tool connection names a namespace and rank, either supplied by the
tool or assigned by PRRTE. The namespace belongs to one of two kinds of
job, and the kind decides everything else.

.. list-table::
   :header-rows: 1
   :widths: 22 39 39

   * -
     - Launched job
     - Connected job
   * - Created by
     - A spawn: ``prun``, ``PMIx_Spawn``, including a spawn with
       ``PMIX_SPAWN_TOOL`` and debugger daemons.
     - The first connection of a self-started tool:
       ``prte_pmix_server_register_tool()`` on the master, or the master's
       ``PRTE_PLM_TOOL_ATTACHED_CMD`` handler.
   * - Its processes exist because
     - PRRTE forked them (odls).
     - A connection said so.
   * - Its lifecycle is driven by
     - The normal state machine: waitpid and I/O completion.
     - Its connections. A process ends when its last connection goes.
   * - Marked by
     - Nothing in particular. It may carry ``PRTE_JOB_FLAG_TOOL``.
     - A new local job attribute, ``PRTE_JOB_CONNECTED``. All sixteen job
       flag bits are in use.

``PRTE_JOB_FLAG_TOOL`` keeps its current meaning: the job's processes do
not count against slots or allocations, and are mapped and bound
accordingly. It no longer decides anything about connections.
``PRTE_JOB_CONNECTED`` does.

Who decides
~~~~~~~~~~~

**The DVM master admits every tool connection.** A tool may connect to
any daemon. A daemon other than the master relays every tool connection
(``PRTE_PLM_TOOL_ATTACHED_CMD``), as it does today, and the master's answer
is final. The master also names a tool that did not name itself, using
the routine that names launched jobs (``prte_plm_base_next_nspace()``), so
the two can never collide.

**The master counts connections.** For each process of a connected job,
the master keeps the number of live connections, summed over every daemon
they reach. Each accepted connection adds one and each departure removes
one. A process ends when its count reaches zero, not on the first
departure.

**Identity is checked against the job's owner (``jdata->uid``), for every
job.** That is the tool's user for a connected job, and the requester's
for a launched one. PRRTE does not run privileged, so every process it
launches runs as the DVM's user, and in any supported configuration that
is the owner. A privileged DVM would set each child's user to its job's
owner at launch, and the same check holds. PMIx already checks a launched
process that connects as a tool against the user registered for its
rank, and PRRTE registers each job under ``jdata->uid``, so the two layers
agree.

Admitting a connection
~~~~~~~~~~~~~~~~~~~~~~

One function, ``tool_join``, makes this decision on both of the master's
paths. A connection names a namespace and rank, and carries the
connecting process's user and pid. The master decides as follows.

#. **The namespace is unknown.** The master builds a connected job owned
   by this user, with this rank as its first process and a connection
   count of one. The rank must pass ``prte_pmix_server_tool_rank_ok()``:
   below the sentinel ranks and no larger than ``prte_pmix_max_tool_rank``
   (default 65535). The rank indexes the job's process array, so it sets
   that array's size. A tool that gave no namespace is named with
   ``prte_plm_base_next_nspace()``.

#. **The namespace is known and the user is not ``jdata->uid``.** The
   connection is refused (``PERM``).

#. **The namespace is a launched job.** The connection is accepted only if
   the rank exists. Nothing is added and nothing is counted. This is a
   process PRRTE launched connecting as a tool under the identity it was
   given (``PMIx_tool_init()`` adopts ``PMIX_NAMESPACE`` and ``PMIX_RANK``
   from its environment). odls owns its lifecycle, so the connection's
   departure is ignored.

#. **The namespace is a connected job.**

   * If the job is terminating (``PRTE_JOB_FLAG_TERMINATING`` is set), the
     connection is refused with ``PMIX_ERR_RESOURCE_BUSY``. The tool may
     retry.
   * If the rank is unknown, it must pass the rank check. The process is
     added with its pid and node and marked RUNNING, ``num_procs`` and
     ``app->num_procs`` grow by one, and its count is one.
   * If the rank is known and live, this is a clone, and its count grows
     by one.
   * If the rank is known but already terminated, while other ranks keep
     the job alive, the process is revived:

     - ``RECORDED`` is cleared and it is marked ALIVE and RUNNING;
     - ``num_terminated`` and ``app->num_terminated`` each drop by one;
     - its pid and node are updated;
     - its count is one.

Losing a connection
~~~~~~~~~~~~~~~~~~~

One function makes this decision for departures seen on the master and
for those relayed to it. Each connection departs exactly once, in one of
two ways:

* **Finalize.** The tool calls ``PMIx_tool_finalize()``, and PMIx calls the
  host's ``client_finalized`` upcall.
* **Drop.** The socket closes without a finalize, and PMIx raises
  ``PMIX_ERR_LOST_CONNECTION``. PMIx does not raise it for a connection
  that has already finalized.

The master then decides:

* **The scheduler's process:** the scheduler role is cleared, as now.
* **A process of a connected job:** its count drops by one. At zero the
  process is marked TERMINATED. The job then completes through the normal
  path once every rank has ended.
* **Anything else** - a launched job, or an unknown job: ignored.


Each case in turn
-----------------

Tools PRRTE starts
~~~~~~~~~~~~~~~~~~

A job spawned with ``PMIX_SPAWN_TOOL`` or as debugger daemons is a launched
job. It is named, mapped and forked like any other, and its processes are
registered with PMIx as clients. If such a process, or any process PRRTE
launched, calls ``PMIx_tool_init()``, PMIx sees a ``PMIX_TOOL_CLIENT``
under the process's own name, and PRRTE handles it by rule 3: the rank
exists and the user is the owner, so the connection is accepted and
nothing changes.

* **Finalize.** PMIx calls ``client_finalized`` with the process's
  ``server_object``, and PRRTE sets ``HAS_DEREG``, as now. PMIx gives the
  host no way to tell this apart from the client itself finalizing. That
  limitation is recorded here and left unchanged.
* **Drop.** ``_lost_conn`` finds a launched job and does nothing.

  Today it calls ``tool_departed``, because the job carries the TOOL
  flag. So a debugger daemon that closes its tool connection is marked
  terminated on the master while the process still runs.
* **Process exit.** waitpid reports it, as for any launched process.

A self-started tool connected to the master
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``_toolconn`` → ``prte_pmix_server_register_tool()`` → ``tool_join`` (rules
1-4) → ``toolconn_reg_complete``, which returns the name to the tool. The
namespace is registered with the master's PMIx server only when the job
is created (rule 1).

A self-started tool connected to another daemon
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

#. The daemon relays ``PRTE_PLM_TOOL_ATTACHED_CMD``: the command line, pid,
   user and group, and the name or the rank.
#. The master applies rules 1-4, updates its count, and replies with a
   status and the namespace.
#. The daemon's ``pmix_server_tconn_return()`` builds a local copy of the
   job, or reuses the one it has, and registers the namespace with its own
   PMIx server.
#. **The daemon no longer judges admission itself.**

   * The branch currently re-runs ``tool_join`` against the daemon's local
     copy and ignores the result.
   * Instead, if the daemon's local registration fails, it tells the tool
     so and sends ``PRTE_PLM_TOOL_DEPARTED_CMD``, so the master's count is
     undone. Today the daemon reports the master's status whatever
     happened locally.

Clones
~~~~~~

A clone is a second ``tool_connected`` for the same namespace and rank.
PMIx calls ``tool_connected`` once per connection, clones included, and
reports each one's departure separately. Rule 4 counts the clone and the
departure rule subtracts, so the process survives until its last
connection goes. Because the count lives on the master, this holds
whether the clones are on one daemon or several.

Today the first clone to leave ends the process, and with it the job:
the namespace is deregistered, a job-end event is sent and reservations
are disposed of.

Tools with several ranks
~~~~~~~~~~~~~~~~~~~~~~~~

Each new rank is added by rule 4, with its pid and node, and
``num_procs`` and ``app->num_procs`` grow by one. That includes ranks that
connect directly to the master. The job completes when every rank's
count has reached zero.

Today a second rank that connects directly to the master is silently not
recorded, so the first departure ends the job.

Finalize and drop
~~~~~~~~~~~~~~~~~

* **Finalize.** ``client_finalized`` with ``server_object == NULL`` →
  ``tool_departed``, unchanged. The departure rule is then applied on the
  master, or after relaying with ``PRTE_PLM_TOOL_DEPARTED_CMD``.
* **Drop.** ``_lost_conn`` must handle **every** process the event names.

  PMIx merges ``PMIX_ERR_LOST_CONNECTION`` events that occur within a
  one-second window. ``source`` names the first peer, and each later peer
  is added as a ``PMIX_PROCID`` info entry, duplicates included
  (``pmix_event.h:390-457`` in PMIx).

  Today only ``source`` is handled. A second tool that drops within the
  same second therefore leaks its job and everything it holds.
* **Both** apply the same rule, so a clone that finalizes and a clone that
  drops each count once.

Coming back
~~~~~~~~~~~

* **After the job is gone.** A connection under the same name builds a new
  connected job (rule 1). Two things have to be cleaned up first:

  - The namespace must be taken out of ``departed_jobs``. Otherwise direct
    modex requests for it are answered "not found" until it ages out of
    that list.
  - Every daemon's copy must be gone. Today a daemon's copy is released
    only when the DVM is persistent, because ``dvm_notify`` sends
    ``PRTE_DAEMON_DVM_CLEANUP_JOB_CMD`` only then. Otherwise the daemon's
    PMIx registration remains, and PMIx refuses a reconnect under the same
    name through that daemon.
* **One rank comes back while others hold the job.** It is revived by rule
  4.
* **During teardown** - after TERMINATED has been activated, before
  ``cleanup_job`` has run. The connection is refused with
  ``PMIX_ERR_RESOURCE_BUSY``.

  Today it attaches to a process that has already been recorded as
  ended, and the namespace is deregistered underneath it.
* **A child job outlives its tool, which then reconnects under the same
  name.**

  ``cleanup_job`` (``state_dvm.c:1152``) looks the parent up by name.
  It finds the *new* parent, removes the child from a list it is not on,
  and releases it, which corrupts memory in non-debug builds.

  The fix: remove the child only if it is actually on that parent's
  ``children`` list.


Code changes
------------

These are relative to this branch.

``src/util/attr.h``, ``src/util/attr.c``
   Add the job key ``PRTE_JOB_CONNECTED`` (bool) and the proc key
   ``PRTE_PROC_TOOL_CONNECTIONS`` (uint32). Both are local, are never
   packed, and get print names. Attributes avoid growing every
   ``prte_proc_t`` for something only tool processes use.

``src/prted/pmix/pmix_server_register_fns.c``
   * Rewrite ``prte_pmix_server_tool_join()`` to rules 2-4:

     - the owner check, for every job;
     - a launched job recognized by the marker;
     - a terminating job refused as busy;
     - adding, cloning and reviving, with counting.
   * In ``prte_pmix_server_register_tool()``:

     - a new job gets the marker and a count of one;
     - in the existing-job branch, call ``tool_join`` only on the master.
       On another daemon, accept, because the master has already decided.
   * Remove the namespace from ``departed_jobs`` when a job is created
     under it, using a new helper in ``pmix_server.c`` beside
     ``prte_pmix_server_job_departed()``.

``src/mca/plm/base/plm_base_receive.c``
   * ``PRTE_PLM_TOOL_ATTACHED_CMD``: the new-job branch sets the marker
     and a count of one. The existing-job branch already calls
     ``tool_join``.
   * ``PRTE_PLM_TOOL_DEPARTED_CMD`` (``:1269-1293``): call the shared
     departure function instead of activating TERMINATED.

``src/prted/pmix/pmix_server_gen.c``
   * ``prte_pmix_server_tool_departed()``: on the master, apply the
     departure rule (the scheduler, a connected job's count, or ignore).
     On another daemon, relay as now.
   * ``pmix_server_tconn_return()``: if the local registration fails,
     tell the tool that status and send ``PRTE_PLM_TOOL_DEPARTED_CMD``.
   * Fix the stale comments at ``:213-217`` and ``:813-816``: a daemon
     does keep a copy.

``src/prted/pmix/pmix_server.c``
   * ``_lost_conn``: depart ``source`` and every ``PMIX_PROCID`` in the
     event's info. Use the marker to choose: a connection to an unknown
     job or a connected job departs; one to a launched job is ignored.
   * Add the departed-jobs removal helper.

``src/mca/state/dvm/state_dvm.c``
   * ``dvm_notify``: send ``PRTE_DAEMON_DVM_CLEANUP_JOB_CMD`` for a
     connected job even when the DVM is not persistent.
   * ``cleanup_job``: remove a child from its parent's list only if it is
     a member.
   * Fix the comment at ``:915-917``: connected jobs do reach
     ``check_complete``.

Docs
   * ``src/prted/pmix/AGENTS.md``: replace the branch's join text with
     the model above: the two kinds of job, the master decides and
     counts, and the admission and departure rules.
   * ``src/mca/plm/AGENTS.md``: the ``PRTE_PLM_TOOL_ATTACHED_CMD`` and
     ``PRTE_PLM_TOOL_DEPARTED_CMD`` bullets.


What PRRTE relies on from PMIx
------------------------------

These assumptions need rechecking whenever PMIx's tracking of
namespaces and ranks changes.

#. One ``tool_connected`` (or ``tool_connected2``) per connection, clones
   included, and no upcall for a connection that PMIx itself refuses.
#. Exactly one departure per accepted connection: ``client_finalized`` for
   a finalize, ``PMIX_ERR_LOST_CONNECTION`` for a drop, and every dropped
   peer named in the event.
#. After the host deregisters a namespace, closing all of its connections
   including clones, a new connection under that name starts fresh.

PMIx-side findings
~~~~~~~~~~~~~~~~~~

These turned up while mapping the paths above. They are inputs to the
PMIx work, not PRRTE changes.

* **First-entry gate.** When a self-started tool joins a namespace PMIx
  already lists, PMIx gates it on the namespace's *first* rank entry
  (``ptl_base_connection_hdlr.c:1552-1600``). Once rank 0 has finalized
  and no clone of it remains, every other rank's connection, and rank 0's
  own reconnect, is refused with ``PMIX_ERR_NOT_SUPPORTED``, with no status
  word sent back.
* **Scheduler finalize.** ``client_finalized`` is not called for a
  scheduler, and its finalize raises no event. A scheduler that finalizes
  cleanly is therefore never reported to the host.
* **Accepted then failed.** If ``process_cbfunc`` fails after the host has
  accepted the connection, check whether a departure is still reported.
  If none is, that connection's count never returns to zero.


Verification
------------

Unit tests (``test/unit/prted/test_prted.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``tool_join``:

  - the owner check on both kinds of job;
  - a launched job: rank present or absent, and never counted;
  - a connected job: adding a rank, a clone (count 2), reviving after
    termination, refusal while terminating, and the rank limit.
* The departure function: a count of 2 → 1 leaves the process alive; 1 → 0
  marks it TERMINATED; a launched job is ignored.
* ``_lost_conn`` with an info array holding several ``PMIX_PROCID``
  entries.
* The branch's current "not counted twice" assertion changes accordingly.

Multi-node tests (``contrib/dockerswarm/run-tests.sh``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The cases go in ``test_requesters`` and a new ``test_tool_lifecycle``.
``roletool`` gains a ``hold <secs> finalize|drop`` operation: connect,
wait, then either finalize or ``_exit`` without finalizing.

* **Clone pairs**, both on one daemon, and split between the master and
  ``node2``. When the first clone leaves, the job is still there, as seen
  by a ``PMIX_QUERY_NAMESPACES`` query from a third tool. When the second
  leaves, the job is gone.
* **Multiple ranks**: rank 0 connected to the master and rank 1 through
  ``node2``.
* **Drop versus finalize.**
* **Two tools dropped within one second**: both jobs go.
* **Reconnect after teardown** under the same name, at the master and
  through ``node2``.
* **A departed rank rejoining** while another rank holds the job.
* **A job spawned with ``PMIX_SPAWN_TOOL``** whose process connects back
  as a tool and then drops that connection: the job stays running until
  the process exits.

Full runs
~~~~~~~~~

* ``make check`` and the help cross-check on macOS, configured with
  ``--enable-debug``.
* The full dockerswarm suite, with the same ``PRTE_SWARM`` given to
  ``build.sh`` and to ``run-tests.sh``.
* The slurmswarm suite, with the same ``PRTE_SLURM_SWARM`` given to both.
* All of these against PMIx built from master once the pending PMIx change
  has landed, stating the commits built.

Commits
~~~~~~~

The branch's first commit is reworked into two:

#. "Check a tool against its job's owner before it joins a job the DVM
   holds": the marker and the admission rules.
#. "Track each of a tool's connections and end it when the last one
   leaves":

   - counting and the departure rule;
   - ``_lost_conn``;
   - the reconnect fixes;
   - the ``cleanup_job`` membership check.

The branch's other commits are unchanged.
