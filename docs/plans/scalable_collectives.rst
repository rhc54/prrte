Restructuring the DVM collectives around operations
===================================================

Context
-------

PRRTE's DVM-wide collectives all run on the RML radix routing tree, and until
recently they did so through an MCA framework with exactly one component.
Two problems had accumulated.

**The abstraction was on the wrong axis.** ``grpcomm`` was a framework whose
``direct`` component returned priority 5 and declared itself "always
available"; the historical ``bmg`` component had long since been deleted, and
selection was single-winner. More importantly, the choice that actually
matters — which algorithm moves the data — is a *per-operation,
per-message-size* decision, and a component cannot express that. A component
is chosen once, for the whole interface, for the life of the process.

**One implementation serves wildly different operations.** ``xcast`` carries
both the launch message and a two-byte shutdown command, on the same tag.
``fence`` serves both a zero-byte barrier and the full modex. The single tree
algorithm is near-optimal for one end of each pair and badly wrong for the
other.

Status
------

Every step was verifiable on its own, and the multi-node suite has not
regressed at any point.  Both second movements are implemented and exercised
across a real multi-node DVM, and both are **opt-in**: an unconfigured DVM
moves every broadcast and every fence exactly as it did before this work.

.. list-table::
   :header-rows: 1
   :widths: 18 62 20

   * - Commit
     - What
     - State
   * - ``404e553273``
     - ``grpcomm`` collapsed out of MCA into ``src/grpcomm/`` (net −772 lines),
       following the precedent of ``src/rml`` (itself once three frameworks)
     - done
   * - ``f46e0d4371``
     - RML lateral links: the direct send and the fault gate
     - done
   * - ``25416cf534``
     - Broadcast framing/movement split; ``tree_whole`` the only movement
     - done
   * - ``2e2a3c77ef``
     - The Bruck allgather exchange schedule
     - done
   * - ``aa41998d39``
     - The scatter's chunk partition
     - done
   * - —
     - The bulk transport itself: ``scatter_allgather``, the
       ``payload_complete`` gate, the op-order hold, and the degrade-to-tree
       fault path
     - done, **opt-in**
   * - —
     - Fence framing/movement split, ``rd_allgather``, and the movement
       interlock (Piece 3)
     - done, **opt-in**
   * - —
     - Selecting a fence's movement from ``PMIX_COLLECT_DATA`` (Piece 4)
     - done — **no PMIx change was needed**

Both halves of the bulk movement's *arithmetic* went in before any of its
transport, and are tested exhaustively without one.  That ordering was chosen
because an error in either would surface later as what looks like a transport
or corruption bug rather than an arithmetic one.

**Both second movements are now the default**, each chosen per operation:

* The **broadcast** selects by tag.  The launch message — split onto
  ``PRTE_RML_TAG_DAEMON_LAUNCH`` for exactly this purpose — scatters;
  everything else takes the tree.  The byte threshold survives only as the
  opt-in ``size`` selection.
* The **fence** selects on ``PMIX_COLLECT_DATA``.  A modex gets the exchange
  because the release fanout, not the gather, is what dominates it; a barrier
  keeps the rollup because a high-radix tree beats a dissemination exchange at
  *any* scale.

Neither default rests on a measured constant, because neither has one left to
rest on: both discriminators are categorical.  That is what made it reasonable
to turn them on rather than leave them behind a parameter — and turning them on
is the point, because a movement nobody selects is a movement nobody tests, and
the failure modes here (a broadcast that misparses, a fence that cannot
converge) are the kind that surface at scale and under fault rather than in a
unit test.

``tree`` on either parameter reverts to the previous behaviour in one flag.

The operations
--------------

.. list-table::
   :header-rows: 1
   :widths: 26 14 10 14 36

   * - Operation
     - Pattern
     - Payload
     - Ordering
     - Wanted
   * - shutdown / job-ctrl / notification / monitor commands
     - one-to-all
     - ~0
     - some
     - **unchanged**
   * - ``DAEMON_DIED`` / ``DAEMON_REVIVED``
     - one-to-all
     - ~0
     - **critical**
     - **unchanged**
   * - launch message
     - one-to-all
     - large
     - no
     - scatter + allgather
   * - ``WIREUP``
     - one-to-all
     - large
     - **critical**
     - **unchanged** — ``process_first``
   * - ``FILEM`` chunks
     - one-to-all
     - 16 KB each
     - no
     - **unchanged** — see below
   * - barrier (``PMIx_Fence``, no collect)
     - all-to-all
     - 0
     - no
     - **unchanged**
   * - modex (``PMIx_Fence``, collect)
     - all-to-all
     - large
     - no
     - direct allgather
   * - ``PMIx_Group_construct`` / ``_destruct``
     - all-to-all
     - medium
     - no
     - rides the allgather

Two of the "unchanged" rows are worth as much as the two changes:

* **The barrier needs no new algorithm.** Its cost is ``2*d*alpha``, and a
  high radix crushes ``d`` — at radix 64 a 4096-daemon DVM is depth 2. A
  dissemination barrier would be ``log2(N)*alpha = 12*alpha``, i.e.
  *worse*. The only win available is constant-factor: not dragging a zero-byte
  collective through the compression attempt, op-id sequencing and ACK rollup
  that a bulk broadcast needs.
* **Tiny broadcasts need none either**, for the same reason. A high radix is
  right when the ``r*M*beta`` term is nil.

"Gather then broadcast" is one operation, not two
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The modex today is *gather to the HNP* (``D*beta``, near-optimal) followed
by *broadcast from the HNP* (``d*r*D*beta``). The second half is the
entire cost. A direct allgather is ``D*beta`` in total. So this is **one
primitive**, and ``PRTE_RML_TAG_FENCE_RELEASE`` / ``PRTE_RML_TAG_GROUP_RELEASE``
should stop existing as broadcasts at all.

Cost model
----------

For ``N`` daemons, ``M`` bytes broadcast or ``D = N*n`` bytes
gathered, radix ``r``, depth ``d = ceil(log_r N)``, per-message latency ``alpha``
and ``beta`` seconds per byte (so ``alpha`` is latency and ``beta`` is inverse
bandwidth throughout):

.. list-table::
   :header-rows: 1

   * - Algorithm
     - Latency
     - Bandwidth
   * - broadcast, tree (today)
     - ``d*alpha``
     - ``d*r*M*beta``
   * - broadcast, scatter + RD-allgather
     - ``(d + log2 N)*alpha``
     - ``2*M*beta``
   * - allgather, gather+bcast (today)
     - ``2*d*alpha``
     - ``(1 + d*r)*D*beta``
   * - allgather, ring
     - ``(N-1)*alpha``
     - ``D*beta``
   * - allgather, recursive doubling / Bruck
     - ``log2(N)*alpha``
     - ``D*beta``

At ``N = 4096``, ``r = 64``, ``beta = 1 ns/B``,
``alpha = 50 us``, a 32 MB modex costs roughly 4.1 s on the tree and
33 ms via an RD-allgather. The launch message sees the same
``d*r -> 2`` improvement.

Two caveats belong with those numbers. ``xcast`` compresses its payload once
(``PMIx_Data_compress`` in ``xcast_nb``), so the ``d*r`` copies are of the
*compressed* bucket — a constant-factor discount on the tree's term, not a
change of shape. And ``alpha`` for the RML is a progress-thread hop, not
a raw TCP round trip, so latency terms are likely worse in practice than the
table suggests.

Why recursive doubling rather than a ring
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Recursive doubling (Bruck, for non-power-of-two ``N``) dominates the ring:
the same bandwidth term with ``log2 N`` steps instead of ``N-1``.
The ring's only genuine advantages are two lateral links instead of
``log2 N``, and contiguous data needing no final rotation — both cheap to
give up once the link machinery exists.

The decisive point is that the *large broadcast* is scatter-plus-allgather, so
**one movement strategy serves both primitives**. Keep the ring in reserve as
a fallback movement if the link count proves a problem at scale.

Lateral links cannot be avoided
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

This looks avoidable and is not, so it is worth recording. The
``r*M*beta`` fanout is per-node-per-level. Pipelining hides the *depth*
but not the fanout. Dropping to radix 2 helps only if the *routing* tree is
radix 2, since a broadcast forwards to ``prte_rml_base.children``. And
scatter-then-reassemble requires the children to exchange directly with each
other. Every bandwidth-optimal collective needs non-tree edges.

Design
------

Piece 1 — lateral links in the RML *(landed:* ``f46e0d4371`` *)*
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The enabling mechanism for everything else. ``prte_oob_base_send_nb``
(``src/rml/oob/oob_base_stubs.c``) resolves a *next hop* via
``prte_rml_get_route``, so at radix 64 a daemon reaching any non-child goes
through the HNP. But the same function already fetches a peer's
``PMIX_PROC_URI`` from the modex — distributed to every daemon by
``process_wireup()`` — and builds a peer from it. If the hop *were* the
target, the existing code connects directly.

#. **Direct send.** A ``direct`` flag on ``prte_rml_send_t`` plus
   ``prte_rml_send_buffer_direct_nb()`` / ``PRTE_RML_SEND_DIRECT``; when set,
   ``prte_oob_base_send_nb`` uses ``msg->dst`` as the hop. Fall back to a
   routed send on ``PRTE_ERR_ADDRESSEE_UNKNOWN``.

#. **A lateral-link registry** on ``prte_rml_base`` — the ranks this daemon
   holds a non-tree link to, with a registrant callback.

#. **Lateral-link loss must not trigger tree repair.** ``prte_rml_route_lost``,
   ``prte_mca_oob_tcp_component_lost_connection`` and ``..._failed_to_connect``
   must consult the registry first: report to the registrant, drop the peer,
   **no promotion, no ancestor walk, no** ``COMM_FAILED``. This is the
   highest-risk item in the whole plan — "unreachable peer read as a lost
   lifeline" has bitten this code before. It lands with its own test, before
   anything uses it.

Items 1 to 3 landed.  Three more were deliberately deferred until a collective
actually opens a lateral link, because until then there is nothing to exercise
them against:

#. **Its own connect bound.** ``prte_connect_max_time`` exists so the tree can
   heal past a dead ancestor; a lateral link has no such fallback. Give
   registered links ``prte_lateral_connect_max_time`` and report a timeout to
   the registrant rather than abandoning silently.

#. **Pre-warm.** The Bruck partner set (``rank`` XOR ``2^k``) is fixed
   and computable, as is a ring. Warm it from ``vm_ready()`` using the existing
   ``PRTE_RML_TAG_WARMUP_CONNECTION``.

#. **Idle teardown**, so a long-lived DVM running many transient subset
   collectives does not accumulate sockets. Note the descriptor budget:
   ``log2 N`` per daemon, 12 at ``N = 4096``.

Piece 2 — broadcast: framing versus movement
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**The framing is shared and stays as it is.** It is the hardest code in the
subsystem and none of it is about data movement: op-id assignment by the HNP,
the ``XCAST_ACK`` rollup with its ``is_request`` re-poll, the ``process_first``
set, late-joiner catch-up (``op_id_completed_at_promotion``), the promotion
replay hold (``replay_pending_parent``), the ``pending_completions`` FIFO, and
the fault-handler reactions to parent/children changes. One implementation,
movement-agnostic.

**Movement is pluggable**, chosen by the originator from measured payload size
and stamped on the wire. A broadcast has a single originator, so there is **no
agreement problem**:

``tree_whole``
   Today's behaviour: forward the whole payload to each routing-tree child.
   Tiny payloads.

``scatter_allgather``
   The root scatters chunks down the tree, then an RD/Bruck allgather over
   lateral links reassembles. Large payloads.

Two invariants govern the split.

*Ordering-critical traffic keeps* ``tree_whole``. The ``process_first`` set and
the forward-before-process rule exist to keep ``DAEMON_DIED`` /
``DAEMON_REVIVED`` ordered against everything else. Those are exactly the tiny
messages, so the constraint and the regime split agree rather than conflict.
Make it explicit: a payload whose delivery order is a correctness invariant may
not use lateral movement.

*Op-id order survives mixed movement.* A daemon processes ops in op-id order.
A bulk op on lateral movement can complete out of order relative to a tiny op
behind it, so ``process_msg`` must hold an out-of-order op until its
predecessors are processed — a generalisation of the existing
``replay_pending_parent`` hold. This is the main design risk on the broadcast
side.

*ACK semantics.* A daemon ACKs "my subtree has the payload". Under
``scatter_allgather`` completion is not subtree-shaped, so a daemon ACKs once
it holds the whole payload *and* its children have ACKed. The rollup itself is
unchanged.

**The tag should be the discriminator, not the size.** An earlier draft of
this document argued the opposite — that selection should read a measured
size, because otherwise "every future large payload needs a new tag to get the
benefit". That reasoning treats PRRTE as though it carried arbitrary traffic.
It does not: it carries a known, small set of things, and which of them a
message is *is* the operation. A new tag for a new bulk payload is not a cost
to be avoided, it is the declaration that makes the choice reasoned rather
than guessed.

The conflation is narrower than it looks, which is what makes this practical.
``PRTE_RML_TAG_DAEMON`` is the only overloaded tag, and within it exactly
**one** call site is large — ``plm_base_launch_support.c`` broadcasting
``jdata->launch_msg``. Everything else on that tag is a command: job-control,
halt/terminate, the allocation messages. Every other broadcast tag is already
single-purpose:

.. list-table::
   :header-rows: 1
   :widths: 34 12 54

   * - Tag
     - Size
     - Wanted
   * - ``PRTE_RML_TAG_DAEMON`` (launch message)
     - large
     - **its own tag** — then: bulk
   * - ``PRTE_RML_TAG_DAEMON`` (all other sites)
     - ~0
     - tree
   * - ``PRTE_RML_TAG_FILEM_BASE``
     - large
     - bulk; already its own tag
   * - ``PRTE_RML_TAG_WIREUP``
     - large
     - tree — ``process_first``, see below
   * - ``DAEMON_DIED`` / ``DAEMON_REVIVED``
     - ~0
     - tree — ordering-critical
   * - ``NOTIFICATION`` / ``MONITOR_REQUEST`` / ``IOF_PROXY``
     - ~0
     - tree

So splitting one tag off removes the need for a byte threshold entirely, and
the remaining selection is categorical: a small table of tag → movement, with
no number in it to be wrong about.

**Done.** ``PRTE_RML_TAG_DAEMON_LAUNCH`` carries the launch message, delivered
to the same handler on the same command stream — ``prte_daemon_recv`` ignores
the tag it arrives on, so nothing about the receiving side changed. Selection
is ``grpcomm_bcast_movement`` = ``tag`` (the default) / ``size`` / ``tree`` /
``bulk``, and ``bulk_tag_prefers_bulk()`` is the whole table.

``FILEM`` is **not** in that table, which corrects the operation list above.
Its chunks are capped at ``PRTE_FILEM_RAW_CHUNK_MAX`` = 16 KB, and at that
size the answer depends on the DVM: at a few hundred daemons the tree's
``r*M*beta`` fanout dominates and the exchange wins, at ten the exchange's
extra ``log2(N)`` latency steps cost more than the fanout saves. A knob-free
table has no business holding a guess, so scattering ``FILEM`` needs either a
larger chunk or a measurement first.

The framing/movement split landed first (``25416cf534``) with ``tree_whole`` as
the only movement.  What follows is the bulk movement itself, which is now
written; the notes below describe what was built and why, and call out the two
places where the implementation went further than the sketch.

Participants are named on the wire, never re-derived
''''''''''''''''''''''''''''''''''''''''''''''''''''

Every daemon must agree on which exchange position is which rank.  Deriving
that locally from the failed-daemon set would have daemons disagree in the
window around a fault, and the exchange would then deadlock — each waiting on
a partner the other does not believe exists.

So the **originator stamps the participant rank list** into the scatter
message, exactly as it stamps the movement id, and a daemon finds itself by
searching that list.  At four bytes a rank this is 16 KB for a 4096-daemon
DVM, carried once alongside a payload that is large by definition.  This rule
is load-bearing: do not replace it with a locally-computed set.

The two phases
''''''''''''''

**Scatter, down the existing tree.**  ``scatter_allgather``'s ``forward()``
sends each routing-tree child only the chunks destined for that child's
subtree (``radix_subtree_index`` partitions them), keeping its own.  No new
connections and no new tag: this *is* the forward, so it inherits the ACK
rollup, ``process_first`` and replay machinery unchanged.  Chunk boundaries
come from ``prte_grpcomm_chunk_bounds()``.

**Allgather, over lateral links.**  A new tag
(``PRTE_RML_TAG_XCAST_BULK``), driven by ``prte_grpcomm_bruck_step()`` over
positions, sending with ``PRTE_RML_SEND_DIRECT`` and registering each partner
through ``prte_rml_lateral_register()``.  Blocks land **rotated**, so
reassembly maps slots through ``prte_grpcomm_bruck_owner()`` — never assume
natural order.  Steps can arrive out of order, so blocks are stored by owner
position rather than appended, and the driver loops on "do I hold the blocks
this step must send" rather than advancing one step per message received.

Two things fell out of building it that the sketch did not anticipate.

*The exchange can outrun the scatter.*  The two phases travel different routes,
so a partner nearer the root can start its exchange before our scatter has
reached us — and its chunks name positions in a participant list we have not
been given yet.  Those messages are parked whole, by op-id, and drained when
the scatter arrives.  They cannot be decoded on arrival, and dropping them
would deadlock the exchange.

*Registered lateral links are never deregistered.*  Withdrawing the
registration while the socket is still open would make a later drop read as a
routing-tree fault, which is the one thing the registry exists to prevent.
Retiring the link and the registration together is the idle-teardown work,
which is still not done.

The framing change: ``payload_complete``
''''''''''''''''''''''''''''''''''''''''

Today the framing treats "op received" as "payload held".  Under
scatter+allgather it is not.  A ``payload_complete`` flag on the op is set by
the movement — immediately for ``tree_whole``, and when the last block lands
for the bulk movement — and both local delivery and the ACK to the parent gate
on it.  ``nexpected`` stays the child count, because the rollup is still
subtree-shaped; that is what keeps the reliability machinery single-copy.

Faults degrade to ``tree_whole``
''''''''''''''''''''''''''''''''

A participant lost mid-scatter or mid-allgather cannot be recovered by the
exchange itself.  The controller still holds the whole payload for its own op
and the framing already has a replay path, so on any fault touching an
in-flight bulk op the movement **flips to** ``tree_whole`` **and the op
replays whole**.  Receivers holding partial chunks have not processed yet
(``op->processed`` guards that), so they simply take the whole payload and
complete.  Correctness over speed during recovery, and no second recovery
mechanism to get wrong.

Selection
'''''''''

At the originator only.  ``grpcomm_bcast_movement`` takes four values:

``tag`` (default)
   The movement follows what the message is — ``bulk_tag_prefers_bulk()``,
   which today names only the launch message.  No constants.
``size``
   The old size rule: bulk iff the payload is at least
   ``grpcomm_bcast_bulk_min_bytes`` and the DVM at least
   ``grpcomm_bcast_bulk_min_daemons``.  Kept as an escape hatch for a
   programming model that pushes something unexpectedly large through a tag
   nobody classified — **not** the production path, because it holds a number
   nobody has measured.
``tree`` / ``bulk``
   Force one movement everywhere, for testing.

Ordering-critical tags are excluded before any of this, in every mode.

**The byte threshold was the only genuine "crossover" in the design, and the
tag split retired it rather than resolving it.**  It had existed only because
the launch message and the shutdown command shared ``PRTE_RML_TAG_DAEMON``, so
the operation could not be recovered from the call and size was the last
signal available.  To opt a new payload in, give it a tag and add it to the
table; do not reach for the threshold.

That is worth stating plainly because the two selections in this design are
*not* the same kind of thing, and describing them as though they were is how
a made-up constant acquires an air of having been measured:

.. list-table::
   :header-rows: 1
   :widths: 30 26 44

   * - Decision
     - Discriminator
     - What is unknown
   * - broadcast: tiny vs large
     - payload bytes (today)
     - a real crossover — until the launch message gets its own tag, at which
       point the question disappears rather than being answered
   * - broadcast: ordering-critical
     - tag
     - nothing; a correctness exclusion, not tuning
   * - fence: barrier
     - ``PMIX_COLLECT_DATA`` absent
     - nothing — the cost model says the high-radix tree wins at *any* scale
   * - fence: modex
     - ``PMIX_COLLECT_DATA`` present
     - not a crossover; only "does the exchange beat the rollup here", a
       yes/no with no number to tune

A size test survives, if at all, only as a backstop for a programming model
that pushes something unexpectedly large through a tag nobody classified.

``PRTE_RML_TAG_WIREUP`` is excluded even though it is large and would
otherwise qualify, because it is in the ``process_first`` set — it changes the
child set, so it must be processed before forwarding.  That exclusion is
deliberate and must stay commented in the code, or it will be "optimised"
back in.

Piece 3 — allgather: framing versus movement
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Built.** ``tree_gather_release`` and ``rd_allgather``, selected by
``grpcomm_fence_movement`` (``tree`` / ``allgather`` / ``auto``), defaulting to
``tree``.  Two things about the built shape differ from the sketch below and
are worth stating, because both were discovered by writing it:

*The seam is wider than "how it is released."*  An allgather changes what
*converged* means, not just what to do about it — the rollup is counting child
subtrees while the exchange is counting blocks — so a movement supplies three
things: ``converged``, ``contribute``, and ``answer``.  The framing keeps the
converged latch, the deadline, and everything about the tracker's lifetime.

*The exchange opts out of the recovery restart entirely.*  The restart exists
because a rollup's shape is the routing tree's and the tree just changed; an
exchange's shape is the participant list, which a repaired tree does not
touch.  Blocks already collected are still exactly what their owners
contributed, so re-offering our own would only duplicate what partners already
hold.  A participant genuinely lost is handled instead by the local fault
test, which ends the fence.

The shared framing is what ``grpcomm_fence.c`` already factors
out: the signature, the tracker, ``create_dmns()``, ``get_tracker()``,
``my_contribution``, the recovery epoch, ``abort_fence_op()``, and the
completion callback into PMIx. Movement is pluggable:

``tree_gather_release``
   Today: up-tree rollup, ``xcast`` release. Zero-payload barriers keep this;
   it is already optimal for them.

``rd_allgather`` (Bruck for non-power-of-two ``N``)
   ``log2 N`` lateral exchanges, every daemon ending with everything,
   **no release broadcast**. Large payloads.

**Ordering.** Blocks are stored by participant position and concatenated in
ascending order, so every daemon produces byte-identical output — better than
today, where the bucket order is the nondeterministic merge order at the root.

**Fault.** Simpler than the tree path. On the GLOBAL-scope pass a participant
tests its own ``coll->dmns`` against ``prte_rml_base.failed_dmns``; any
participant lost means the exchange cannot close, so it completes local
participants with ``PMIX_ERR_LOST_CONNECTION`` and deletes the tracker. Purely
local on every daemon — no controller, no epoch restart. There is no "lost a
pure relay" case at all, because relay-only daemons are not in the exchange.

**Timeout** becomes local for the same reason: there is no root, and for a
subset fence the master may not participate. A participant carrying
``PMIX_TIMEOUT`` arms its own timer and on firing broadcasts the abort so the
rest stop. This is a deliberate deviation from today's rule that the fence's
deadline is the DVM's to keep.

**Agreement.** Unlike the broadcast, every participant must independently
choose the same movement or the collective hangs. See the next section.
Regardless of how that resolves, the movement id goes on the wire and a
receiver whose tracker is running a different movement **aborts with a named**
``show_help`` **diagnostic** — turning the one catastrophic failure mode of the
design into a reportable error.

Piece 4 — getting the collect-data directive from PMIx
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**This piece turned out not to exist.  The directive was already arriving.**

The premise recorded here — that ``PMIX_COLLECT_DATA`` is consumed
client-side and the host sees only ``data``/``ndata``, so PMIx would have to
be changed to pass it up — is false, and was checked rather than reasoned
about.  The PMIx client packs the caller's ``info`` array onto the wire
verbatim; the server unpacks it into ``trk->info`` and hands *that* array
straight to ``pmix_host_server.fence_nb``.  So the directive reaches
``pmix_server_fencenb_fn()`` like any other.

Measured on a three-node DVM, printing every key the upcall received:

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * - Fence
     - ``pmix.collect`` present?
     - ``ndata``
   * - ``PMIx_Fence`` with ``PMIX_COLLECT_DATA``
     - **yes**
     - 8
   * - ``PMIx_Fence`` with no directives (a barrier)
     - no
     - **8**

The second row also disposes of the fallback this section proposed.
``ndata == 0`` is not "realistically equivalent to no-collect": a pure
barrier arrives with **eight bytes**, not none, so a size-based rule would
have sent every barrier down the exchange — the opposite of what it was for.
Do not reintroduce it as a heuristic anywhere.

What was actually needed is therefore small, and entirely inside PRRTE: read
``PMIX_COLLECT_DATA`` out of the info array in the fence entry point and let
it choose the movement.  No openpmix change, no ``PMIX_CAP_*`` flag, no
``PRTE_CHECK_PMIX_CAP`` in ``config/prte_setup_pmix.m4``, and nothing to
guard with ``#if`` — there is no older PMIx to degrade against, because
nothing about this is new.

That also makes the fence's ``auto`` **better founded than the broadcast's**,
rather than worse.  A broadcast's ``auto`` is safe only because a broadcast
has one originator whose choice is authoritative.  A fence has none — but
every participant's upcall carries the same directive, because PMIx requires
it to be uniform across a fence and enforces that within a node.  So every
daemon resolves ``auto`` identically from an input none of them had to
agree on.

The wire interlock still matters and is unchanged: it is what catches a
genuinely non-uniform request, and it turns it into a named ``show_help``
diagnostic rather than a DVM-wide hang.

Telling one fence from the next over the same participants
----------------------------------------------------------

A fence's signature is its participant list, and a job fences over the same
list repeatedly. Consecutive fences are **not** separated in time on the
daemons: a fence ends at a different instant on each of them, and — once the
exchange movement is in play — the release comes down the tree while the
exchange's blocks go straight across, so a partner that finished first legally
sends the next fence's traffic to a daemon still working on this one. Two
observed consequences, both traced in the container harness at 16 daemons:

* the next fence's contribution lands on the current fence's tracker and is
  dropped when that tracker is retired — a DVM-wide hang with no diagnostic;
* between two *consecutive allgathers* it is worse than a hang: the block goes
  to the same slot as the current fence's, ``xch_store()`` refuses it because
  the slot is already held, and the result is silently short.

Two things are needed, and the first is not sufficient on its own.

**Retire the tracker before anything can start the next fence.** Both
retirement sites — the release handler and the exchange's answer — must take
the tracker off the list *before* running the completion callback, because that
callback completes local clients and a client may fence again over the same
participants the instant it returns. This closes the case where the next fence
is started by a client of this daemon, or by a daemon downstream of it (whose
release necessarily passed through here first). It cannot close the lateral
case, where the release has not arrived at all yet.

**A generation on the signature.** A ``uint32`` carried on the wire ahead of
the participants and part of the tracker key, so fence *k* and fence *k+1* over
one participant set are different collectives. It requires **no agreement
protocol**: every participant takes part in every fence over a set, and two
identical fences may never be in flight at once, so the fences over a signature
are strictly ordered and the *k*-th is the *k*-th everywhere. Each daemon
derives the same number by counting what it has retired.

The one assumption is that the set of daemons hosting a signature's procs does
not change mid-sequence. A proc **relocated onto a daemon that never hosted one
of that job** breaks it: that daemon has counted nothing and would stamp 0
while everyone else is at *k*. The launch that places a restarted proc carries
``PRTE_JOB_FLAG_RESTART``, so such a daemon switches from deriving a generation
to **learning** one — taking it from the tracker an arriving contribution has
already built. A relocated proc is by definition the one that just came back,
so its partners are already running and already sending to it, and today a proc
is only relocated onto a new daemon after a daemon failure.

The corner left open is a newcomer that enters the fence before any
contribution reaches it. Closing it needs the newcomer to **query** the
surviving participants for the current generation of that signature, on the
relocation path only. That is deliberately not built while it is unreachable —
an unused protocol is an untested one — and the verbose message *"relocated
participants but no contribution to learn a generation from"* is the marker
that it has become reachable. A DVM-wide generational counter is the wrong
answer: agreeing one costs a collective of its own, which is precisely what
deriving it locally avoids.

What Slurm's PMI2 does, and what it tells us
--------------------------------------------

Slurm's ``src/plugins/mpi/pmi2`` is an independent implementation of the same
problem that runs at production scale, so it is worth being precise about what
it does differently.

**Its KVS fence is our tree fence, with the same asymptotics.** ``kvs.c`` merges
up the stepd tree and the root then does one
``slurm_forward_data(step_nodelist, ...)`` of the whole merged KVS to every
node - O(N·b) delivered per node, which is ``tree_gather_release``. Slurm did
not make the allgather scale.

**What scales is that MPI stops asking for one.** ``ring.c`` implements
``PMIX_Ring``, which hands each process only its left and right neighbour
values: O(1) per process at any N. It is an up-sweep/down-sweep scan - RING_IN
carries (count, left, right) up, and the root sends each child a *different*
RING_OUT giving that subtree's starting rank and its own neighbours. The
argument for it is
"PMI Extensions for Scalable MPI Startup" (Chakraborty et al., EuroMPI/ASIA
2014): with a ring plus on-demand connection establishment, the business-card
allgather is not needed at startup at all. PRRTE's equivalent lever is the
direct modex, not a faster ``rd_allgather``.

Three things transfer.

**A fence sequence number, which they also derive locally.** ``kvs_seq`` starts
at 1, is incremented in ``temp_kvs_send()`` ("expecting new kvs after now"),
travels on the wire in both directions, and is checked on arrival - which is
exactly the ``generation`` on our fence signature, arrived at independently.
Two details differ. They keep the last sequence seen *per child*
(``tree_info.children_kvs_seq[from_nodeid]``) where we keep a bool per subtree
in ``reported_slots``; theirs distinguishes a duplicate of the current round
from a straggler of an older one. And they treat any mismatch as fatal, which
they can afford because their rollup has no lateral traffic that can arrive out
of turn - ours has to hold an early arrival instead. The gap their check
exposed in ours was on the *stale* side: a contribution for a generation we had
already retired matched no tracker, so ``get_tracker()`` built one that nothing
would ever complete or delete. That is now refused on the way in.

**A scan is a shape we do not have.** Every message in ``ring.c`` is O(1) no
matter how large the job, because each child is sent only what its own subtree
needs. Our release broadcasts the whole result to everyone. For any operation
where a participant needs a slice rather than the aggregate - rank assignment,
neighbour exchange - that is the difference between O(1) and O(N) per daemon.

**Their state reset is synchronous with the send.** ``pmix_ring_out()`` sends
to its children and then clears the per-child slots and the count inside the
same handler, so no next-round message can attach to the previous round's
state. That is the same rule as retiring a tracker before delivering its
release, reached from the other direction.

Two things not to copy: every error path calls ``slurm_kill_job_step(SIGKILL)``
- there is no fault tolerance in the collective at all, which is most of why
their code is smaller than ours - and only one ring may be in flight, with
state in a fixed per-child array indexed arithmetically, because they have no
notion of a collective over a subset. Our tracker identity machinery is the
price of semantics they do not offer, not accidental complexity.

Scattering the fence release: measured, and not worth it as a tag
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``ring.c``'s per-child down messages prompt an obvious question: the release
broadcast is the one routinely large message the fence sends, and
``bulk_tag_prefers_bulk()`` names only ``PRTE_RML_TAG_DAEMON_LAUNCH``, so the
release goes out whole to every child at every level. Adding
``PRTE_RML_TAG_FENCE_RELEASE`` to that table was tried, at 8 and 16 daemons
with ``grpcomm_fence_movement tree`` so the release actually carries the modex:

===================  ==================  ==================
payload              collect (modex)     barrier
===================  ==================  ==================
8 KB/rank, N=8       0.93x               1.18x
8 KB/rank, N=16      1.17x               1.39x
512 KB/rank, N=16    0.94x               1.50x
===================  ==================  ==================

The modex gains at most 6% and is not reliably better at all below a megabyte,
while the barrier - which shares the tag - loses 18-50%, and the loss grows
with N because it is paying the exchange's fixed costs (a participant list on
the wire, log2(N) lateral connections) to move 157 bytes.

The structural finding is the useful part: **FENCE_RELEASE is the one broadcast
whose tag does not determine its size.** The table's premise - "which of them a
message is IS the operation" - holds for the launch message and fails here,
because a barrier's release and a modex's release travel the same tag six
orders of magnitude apart. That is exactly the situation splitting the launch
message onto its own tag was meant to remove. So if this is ever worth doing,
the *fence* must say so - it knows which kind of release it is emitting - by
plumbing a movement preference through ``prte_grpcomm_release_bcast``, rather
than the tag table inferring one.

Read the 6% as a lower bound rather than a verdict: the measurement is
loopback between containers on one host, which is precisely the environment
where a bandwidth optimisation shows least.

Measuring the ring's bargain without implementing a ring
--------------------------------------------------------

``PMIX_Ring`` has no PMIx API to sit behind - it was PMI2's - and no internal
PRRTE caller wants a scan either: rank assignment is central at the HNP and the
bulk broadcast derives its chunk bounds arithmetically. Writing
``prte_grpcomm_scan`` today would be a protocol with no consumer, which is the
thing this document already argues against building.

But the *question* the ring answers is testable with what PRRTE has. The ring's
bargain is "pay O(1) for the peers you actually need instead of O(N) for
everybody", and both halves exist: a collect fence is the O(N) side, and a
barrier followed by ``PMIx_Get`` of a specific peer is the O(1) side, answered
by direct modex. ``scaletest --neighbors`` times the second: after the barrier,
each rank fetches only its left and right neighbour, serially, which is the
pessimistic reading of the pattern.

Measured at radix 2, one proc per node, medians over three iterations:

=====  ========  ============  ==================  ==========
N      KB/rank   full modex    barrier + 2 gets    ring/modex
=====  ========  ============  ==================  ==========
8      8         2401          823                 0.34
8      128       12152         901                 0.07
16     8         6356          1570                0.25
16     128       31215         1697                0.05
=====  ========  ============  ==================  ==========

The gets are nearly payload-independent - 229us against 256us for sixteen times
the data per peer - because the number fetched does not depend on what anyone
else contributed. The modex grows on both axes. So the ring pattern is already
3-20x cheaper at these small sizes and pulls further ahead with N and with
payload, which is the argument for it stated in PRRTE's own numbers rather than
by analogy.

The extension worth noticing: at 16 nodes and 128 KB a rank, one get costs
~252us against a 31 ms modex, so break-even is around 124 peers - more ranks
than the job has. At these sizes fetching *every* peer on demand would still
beat collecting, which says the interesting lever is not a faster allgather but
how much of the modex an application can be persuaded not to ask for.

Two cautions on the measurement. The gets are serial, so a pipelined
implementation would do better than this, not worse; and this is direct modex
over loopback between containers, where a round trip is cheap relative to real
hardware - the ratio would move, the shape would not.

**Do not read the COLLECT column out of a ``--neighbors`` run.** The phase is
off by default for the same reason ``--verify`` is: a get perturbs the
collective beside it. That is not theoretical here - it was measured, and it
cost an afternoon.

Piece 5 — the ring share, and a fence that delivers almost nothing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The measurement above says the interesting lever is not a faster allgather but
how much of the modex never needs to move. ``neighbor_share`` is that lever,
implemented: a third fence movement, selected by
``grpcomm_fence_movement neighbor``.

Each daemon sends its own contribution to the two daemons either side of it in
the participant ring - ``coll->dmns`` in ascending rank order - on
``PRTE_RML_TAG_FENCE_NEIGHBOR``, and rolls up to the controller only the fact
that it took part. The controller broadcasts an equally empty release. The
tree still carries the *synchronization*, which is what a fence is for; it
carries none of the data. Everything a process asks for that its own daemon
does not hold comes back through direct modex.

Three properties make it work, and none of them needed anything new:

* **``PMIx_server_dmodex_request()`` takes no key.** It answers with the
  target proc's whole remote+global set and the requesting server stores all
  of it, so the first ``PMIx_Get`` of a peer pays one round trip and every
  later key from that peer is local. This is what makes a fence that delivers
  almost nothing workable rather than merely cheap.
* **``PMIX_LOCAL``-scope data was never in the picture.** A put at local scope
  is stored in the client peer's own GDS; ``pmix_server_collect_data()`` asks
  only for ``PMIX_REMOTE``, so local-scope data has never been circulated by
  any movement, and local peers read it through the ordinary server GET path.
  ``PMIX_GLOBAL`` lands in both stores and *is* circulated.
* **The ring is derived, not carried.** Same argument as the exchange: every
  daemon computes it from the same signature through the same
  ``create_dmns()``.

Holding both neighbours' blobs is part of *converging*, not of the release: a
daemon does not report participation upward until it has them, so the release
cannot be issued before every daemon holds what it is owed. That is what makes
the guarantee deterministic - after this fence you *have* your neighbours'
data - and it is free, because every send went out when its sender entered the
fence, so the wait is one hop overlapping the rollup. It cannot deadlock,
because sending never waits on receiving.

Measured on the same swarm, same knobs, one DVM per movement - COLLECT median
in microseconds, radix 2, one proc per node, five iterations:

=====  ========  ======  ===========  ==========  ==============
N      KB/rank   tree    allgather    neighbor    neighbor/tree
=====  ========  ======  ===========  ==========  ==============
8      8         3347    4065         1460        0.44
8      128       10220   11995        5620        0.55
16     8         11046   13101        3420        0.31
16     128       37413   57398        12948       0.35
=====  ========  ======  ===========  ==========  ==============

The ratio *improves* with node count (0.44 to 0.31 at 8 KB a rank), which is
the shape to expect: the rollup's cost grows with N and the ring's does not.

The barrier column is worth reading too. Forcing a movement forces it onto
barriers as well, and the ring's two extra one-hop messages cost nothing
measurable - 695us against the rollup's 775us at 8 nodes, 1660us against
1780us at 16 - while the forced exchange collapses under one (12673us at 16
nodes and 128 KB a rank, because a barrier still carries eight bytes and the
exchange still runs its full schedule for them). That is a fair warning about
what ``auto`` would have to keep doing, not an argument about the ring.

Two honest caveats on the numbers. Part of the win is that each daemon now
holds 3/N of the modex rather than all of it, and this host is small enough
for that to matter - the memory-bound effect documented in the harness guide
is real here. It is not a confound in the sense of measuring something else:
moving fewer bytes and holding fewer bytes are the same cause. But the
magnitude on a laptop may flatter it relative to a real cluster. And the
harness's ``collected_bytes`` column is computed on the assumption that every
daemon ends up holding everything, so under this movement it over-states what
is actually held by a factor of about N/3; the memory guard is therefore
conservative rather than wrong.

**Correctness was checked separately from cost**, because the whole point of
this movement is that it delivers less. ``scaletest --verify`` now reads a key
from *two* peers per iteration: a NEAR one (rank+1, which the ring already
holds) and a FAR one (half the job away, reachable only through direct modex).
Both come back for all three movements at 8 and 16 nodes. A verify that
checked only the near peer would pass with the on-demand path completely
broken, which is exactly the shape of bug this movement can introduce.

**What is deliberately not built.** Widening the share - neighbours plus one
level up and down the tree - is the obvious next dial, and the movement is
shaped so that it is a change to the ring computation and the convergence test
and nothing else. It should not be widened speculatively: the ring is the
cheapest thing that covers the neighbour-exchange pattern real applications
use, and every extra hop is bytes moved on a guess.

What a real MPI job says, and it is not what the benchmark says
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The table above is ``scaletest``, which contributes 8 KB to 512 KB a rank
because it was written to make the collective visible. Open MPI was then built
into the same swarm (``OMPI_SRC``, see the harness guide) and run under all
three movements.

**Open MPI already resolves peers on first use, which is the design this
movement is betting on.** Confirmed in the source rather than assumed:
``OMPI_ADD_PROCS_CUTOFF_DEFAULT`` is 0, so the "pre-add every proc" branch in
``ompi_proc_complete_init()`` never runs; ``ob1`` demands the whole world only
when a BTL sets ``MCA_BTL_FLAGS_SINGLE_ADD_PROCS``, and the TCP BTL sets that
only with more than one TCP module plus threads. So ``MPI_Init`` adds the
node-local peers and nothing else, and each remote proc materializes through
``ompi_proc_for_name()`` when a communication operation first names it. Open
MPI is not fetching modex data it does not need, which is exactly why direct
modex was supposed to work.

**That makes a bare MPI_Init/MPI_Finalize the wrong probe**, and the first
version of ``mpinoop`` was exactly that. It never sends a message, so it never
touches a remote peer, so it issues **zero** direct-modex requests and every
movement looks identical - which is what it reported, and the reading "the
dmodex fan-out did not bite" was an artifact of measuring nothing. What
separates the movements is **first touch**. ``mpinoop`` now has ``--ring``
(each rank exchanges with its two neighbours) and ``--all`` (explicit
point-to-point with every other rank).

``--all`` is deliberately not ``MPI_Alltoall``: a tuned alltoall on one int
runs Bruck and contacts about log2(N) partners, which understated the case
badly - it left a daemon fetching six peers where the honest answer is every
peer it does not hold.

Direct-modex requests counted across **every** daemon (``pmix_server_verbose
2`` plus ``--leave-session-attached``; without that flag only the master's
daemon reports and the count is a fraction of the truth):

========  ======  =========  ==============  ============
ranks     nodes   pattern    tree            neighbor
========  ======  =========  ==============  ============
16        8       none       0               5
16        8       ring       0               35
16        8       all        0               80
32        16      none       0               13
32        16      ring       0               106
32        16      all        0               416
========  ======  =========  ==============  ============

The ``all`` row is the design stated in arithmetic and confirmed exactly: each
daemon holds its own procs plus its two ring neighbours' - 6 ranks of 32 - and
must fetch the other 26, times 16 daemons, is 416. Under the rollup it is
always zero, because the broadcast already delivered everything.

Cost, and this is where it turns against the ring:

* **all-to-all**: first touch 43.6 ms under the rollup, 123.1 ms under the
  ring at 32 ranks - about 2.8x worse, and the *repeat* exchange was worse
  too. Paying ~26 round trips a daemon to reconstruct a modex the broadcast
  had already delivered is straightforwardly a loss.
* **neighbour exchange**: a wash. Medians over four runs at 32 ranks were
  ~21 ms under the rollup and ~25 ms under the ring. A single earlier sample
  showed the ring 3.7x *faster*; repeating it showed that was noise, and it
  is recorded here because it is exactly the kind of number that gets quoted.

So on this workload the ring share ranges from neutral to clearly worse, and
the reason is the same one in both directions: **Open MPI's whole modex here
is 1319 bytes at 16 ranks** - about 82 bytes a rank, measured off the release
with ``grpcomm_base_verbose 5``. There is nothing for a cleverer movement to
save. A direct-modex round trip costs on the order of a few hundred
microseconds regardless of payload, so the crossover is where broadcasting the
modex costs more than one round trip per peer actually needed: at ~4-6 peers
that is a millisecond or two of fetching, which the rollup does not reach
until several KB a rank. This job is three orders of magnitude below that.

Two things this does **not** say. It is a TCP-only build - no UCX, no OFI -
and those are precisely the transports that publish a large per-rank endpoint
blob; the regime where the ring wins is one this build cannot enter. And it is
16-32 ranks, where N**2 fetching is still small in absolute terms.

The conclusion to carry forward is therefore not "the ring share is bad" but
**the per-rank modex size is the whole variable**, and no movement should be
chosen without it. That is the strongest argument yet for leaving ``neighbor``
opt-in rather than wiring it into ``auto`` - and for treating
``PMIX_COLLECT_DATA`` as too coarse a discriminator, since it says whether the
caller wants the data and nothing about how much of it there is.

**And it is not reachable from ``auto``.** Unlike ``tree`` and ``allgather``,
this movement changes what a fence *delivers*, not merely how it travels - a
caller that set ``PMIX_COLLECT_DATA`` does not get the whole modex back. That
is legal (``PMIx_Get`` falls back to direct modex, which is why FAR verifies)
but it is a semantic change, so it stays opt-in until it has been measured
against a real MPI application rather than against a synthetic client.

Verification
------------

* **Unit** — ``test/unit/grpcomm/test_grpcomm.c``: partner-set derivation for
  RD/Bruck over the input matrix including non-power-of-two ``N``, the
  daemon-job NULL-array case and elastic vpid holes; movement-selection
  determinism; canonical block assembly producing identical bytes from any
  arrival order; scatter chunking round-trip.
* **Unit** — ``test/unit/rml/test_rml_routing.c``: a registered lateral link is
  classified as neither child nor lifeline, and its loss produces no tree
  repair.
* **Build** — ``--enable-debug`` (warnings as errors) clean, including the
  capability-guarded path both ways; ``make check``.
* **Multi-node** — ``contrib/dockerswarm``, run with
  ``--prtemca rml_base_radix 2`` so the tree is deep and lateral links are
  provably not tree edges. A large launch and a ``FILEM`` preload complete with
  every daemon holding identical bytes; a daemon dies mid-broadcast and the DVM
  survives; ``DAEMON_DIED`` ordering holds against a concurrent bulk broadcast;
  the movement-mismatch interlock fires cleanly when forced. A/B timing of each
  movement at several payload sizes.
* **Bisectability** — each piece must pass the full suite on its own before the
  next lands.

The multi-node baseline is **562 passed, 0 failed, 3 skipped** once this
work's own phases are counted.

**The performance question now has a tool.**  Everything above establishes
correctness; none of it says either movement is *faster*, and this document has
until now described that measurement as needing hardware nobody had.
``contrib/dockerswarm/scaletest.sh`` is that vehicle: it stands up its own
larger swarm and times a full-data ``PMIx_Fence`` against a bare barrier while
sweeping DVM size, procs per node, routing radix and payload size, writing a
CSV.  Those are exactly the two arms of the fence's selection, so
``grpcomm_fence_movement tree`` against ``allgather`` over the same sweep is a
direct A/B; ``grpcomm_bcast_movement tree`` against the default does the same
for the launch message.  What it still cannot supply is a real network — the
containers share a host, so the bandwidth term is not a cluster's — but it can
answer the shape question, which is what the defaults rest on.

Two practical notes, both of which have cost time here:

* ``check_PROGRAMS`` are built by ``make check``, **not** by ``make``.  Running
  a unit-test binary straight after ``make`` silently runs a stale one, and a
  newly added case simply does not appear in the output.
* For anything that changes the wire format, the multi-node run is
  load-bearing rather than a formality: it is what proves every daemon,
  including pure relays, agrees on the new layout.
* A phase usually starts **one** DVM and runs several cases against it, and
  ``cleanup_swarm`` ends it.  Inserting a self-contained case in the middle of
  such a phase leaves the later cases with no DVM, which presents as
  ``prun failed to initialize`` and reads like a collective failure.  New cases
  go after the last one that needs the shared DVM.

Open questions
--------------

#. **Is the lateral-link registry enough** to keep a dropped lateral link from
   ever being read as a lifeline loss? This is the one place where getting it
   wrong produces DVM-wide damage rather than a slow collective.
#. **Mixed-movement op ordering** — the hold described in Piece 2 needs the
   multi-node ordering case before it can be trusted.
#. **Descriptor budget at scale** for ``log2 N`` lateral links per
   daemon. The ring movement (two links) is the fallback.
#. **A cheaper win exists and should be measured alongside.** Much of the
   tree's cost is the ``d*r`` release fanout. A payload-aware radix for the
   release, or a chunked/pipelined ``xcast``, recovers a large fraction of the
   benefit with none of this risk.

   This remains **unmeasured**, and it is the largest open risk in the plan.
   The cost model above is standard (Thakur, van de Geijn), but PRRTE's own
   constants are not: ``alpha`` here is a progress-thread hop rather than a
   raw round trip, and ``xcast`` already compresses, which discounts the
   tree's bandwidth term by an unknown factor.  A measurement needs real
   multi-node hardware at realistic scale — ten containers on one host tell
   you nothing useful about either constant.  The seam and both halves of the
   arithmetic are already in place, so instrumenting an A/B of the two
   movements is cheap once such a machine is available.
