============================
pKVM-IA 6.18 to 7.1 plan
============================

This document records the history-cleanup and porting plan for the pKVM-IA
implementation. It is a development document for the out-of-tree series,
not a description of a stable userspace ABI.

Goals and phases
================

The implementation has five logical parts: deprivileging the host,
paravirtualized VMCS handling, paravirtualized MMU handling, protected-VM
guest support, and paravirtualized Intel IOMMU handling.

The work is divided into three phases:

1. Fold ``SQUASHME`` commits and fixes into the commits that introduced the
   affected code.
2. Optionally reorder the surviving series as deprivilege, pvVMCS, pvMMU,
   pVM support, unavoidable fixes, and pvIOMMU.
3. Port the cleaned series to Linux 7.1.

Phase 1 history boundary
========================

The Phase 1 input is the linear range::

  61caa6bdaf42 Linux 6.18
  ..
  81bb5bb485ad iommu/vt-d: Reject PASID table setup on present CE in hypervisor

The first pKVM commit is ``fe45dcd701cf`` (``pKVM: VMX: Add initial pKVM
host support``). The range contains 767 commits. The explicit
``61caa6bdaf42`` base must be used: the local ``v6.18`` tag resolves to a
different upstream object and is not a safe rewrite boundary.

The original tip is protected by the annotated tag
``pkvm-v6.18-phase1-original``. Rewriting is done on branch
``pkvm-v6.18-phase1`` in a separate worktree.

Current logical layout
======================

The commit positions below are relative to ``61caa6bdaf42``:

============  =============================================================
Positions     Content
============  =============================================================
1--126        Deprivilege, host EPT, page ownership and sharing foundations
127--434      pvVMCS and protected VM/vCPU operations
435--499      pvMMU, guest EPT, donation, page tracking, and pvmfirmware
500--509      Build and API glue
510--570      pvIOMMU
571--580      pVM guest-side support
581--767      SQUASHME, security, correctness, and refactoring fixes
============  =============================================================

Phase 1 inventory and policy
============================

The input contains 136 commits with ``Fixes:`` trailers and 14 commits with
``SQUASHME`` in the subject. Of these, 115 commits name at least one parent
inside this pKVM series: 94 name one in-series parent and 21 name multiple
in-series parents.

The other 21 ``Fixes:`` commits target Linux commits below the pKVM series.
They include upstream KVM, SVM, TDX, XFD, PASID teardown, and VT-d fixes.
They must remain standalone; Phase 1 must not rewrite the Linux 6.18 base.

A fix is folded only when all of the following are true:

* its corrected parent remains buildable after the fix is moved;
* it does not use an API or state introduced later in the series;
* folding it does not hide a coherent cross-cutting security change; and
* the final source tree remains identical to the original tip.

Every exception is recorded in the Phase 1 manifest. A ``Fixes:`` trailer is
evidence of the defective change, but is not by itself proof that the entire
fix can be moved immediately after that change.

Unambiguous SQUASHME chains
===========================

================  ================  ========================================
Fixup             Target            Purpose
================  ================  ========================================
210faa168749      289beb3a4ede      Validate hypercall inputs
7b0fe5f80c32      6fdd05cd1022      Test protected VM before vCPU init
f1bc6936e1a3      30e9a30c5f52      Restore flush-shadow-all behavior
fac42e551ed4      6e261a4619ed      Correct Device-TLB flush enablement
c9affadaa3bd      e62dbd6d804d      Propagate cache-tag assignment errors
5db04ab1fb86      1ff5d9013996      Avoid VM-exit trace lockup
14b06d159c37      3974ce6517e2      Reject host software-interrupt injection
75505446677c      3974ce6517e2      Validate injected vectors
f63cadf46f56      addc9b4d505a      Prevent host cancellation of injection
ba7b65c4389f      b2743f17b8c9      Harden hwapic ISR update
83a14c538175      8ec836911f8d      Validate APICv refresh input
e9f1086a17b7      38ed4e8f61ec      Correct floppy Kconfig dependency
314138903e37      5ffe84fbdf82      Order spec-control host fixup
1c8f63f7bf7d      b4739a795e34      Satisfy BNDCFGS VMCS pairing
================  ================  ========================================

``59e69e05b041`` fixes ``ba7b65c4389f`` and therefore follows that chain into
``b2743f17b8c9``.

Multiple-target fixes
=====================

The following should normally be split into target-specific hunks or anchored
after the last prerequisite:

* ``c6f219bc0d4e`` -- MMU age error handling;
* ``09cc0f5b25ff`` -- port-I/O register sharing;
* ``15624f908e09`` -- MSR read/write register sharing;
* ``86e20dec1171`` -- TE/QIE disable prevention; and
* ``c1a901671fde`` -- host-vCPU and pCPU indexing.

The following are expected to remain standalone unless inspection proves a
clean and buildable split. They are coherent refactors or cross-cutting
hardening rather than corrections local to one parent:

* ``51fc55efab4c`` -- overflow validation across MMU ownership APIs;
* ``6dd2fdc5ac77`` -- BUG/WARN header refactor;
* ``f7722d51fe3e`` -- memcache helper relocation;
* ``8db253ce50ab`` -- mapping-definition relocation;
* ``02726c811475`` -- scalable-mode gates;
* ``aca4d9407385`` -- hardware-derived ATS state;
* ``a2bb737778ce`` -- common ATS/domain lookup cleanup;
* ``4aac51318280`` -- target-IOMMU domain validation; and
* ``f3d0671fec5b`` -- first-level/second-level domain rejection.

Rewrite procedure
=================

Phase 1 proceeds in reviewable checkpoints:

1. Freeze a manifest containing each candidate, its target or targets, its
   disposition, and the reason for any standalone exception.
2. Fold the unambiguous ``SQUASHME`` chains.
3. Fold single-target fixes in deprivilege, pvVMCS, pvMMU, and pvIOMMU waves.
4. Split or anchor the dependency-sensitive multiple-target fixes.
5. Keep the approved cross-cutting fixes as a small standalone section.
6. Reword a parent when folded behavior materially expands its stated scope.

After every checkpoint, compare its resulting tree with the corresponding
tree in the original series and run a cached build and quick boot. At the end
of Phase 1, the complete tree must match ``pkvm-v6.18-phase1-original``.

Security review invariants
==========================

History cleanup must not alter these invariants:

* host CPU mappings cannot access pKVM-owned or pVM-private memory;
* host-controlled DMA cannot access protected memory;
* donation and sharing failure paths restore mappings, ownership, page state,
  references, and cache tags in reverse order;
* pvmfirmware is copied after host access is revoked and before guest access;
* protected vCPU state exposed to the host is limited to intentional shared
  state;
* context, PASID, root, domain, and memcache page ownership stays balanced;
* ATS remains restricted to SATC devices; and
* CPU TLB, IOTLB, Device-TLB, and IEC invalidations remain correctly ordered.

Build and VM validation
=======================

The minimal build configuration is ``configs/config-bb`` in the original
worktree. The BusyBox VM assets and launch guidance are in ``../bbvm``. Its
launcher uses ``out-bb/arch/x86/boot/bzImage`` and boots with pKVM and strict
IOMMU translation enabled.

The full VM configuration is ``configs/config-vm-6.18``. Ubuntu VM assets
are in ``../pkvmvm`` and ``../pkvmvm/qemu_disk.sh`` documents the QEMU command,
passwordless SSH forwarding, and kernel command line.

For each rewrite checkpoint:

1. copy ``config-bb`` to a checkpoint-specific output directory as
   ``.config`` and run ``make olddefconfig``;
2. build the x86 kernel and ``bzImage``;
3. boot the BusyBox VM and require evidence that pKVM initializes and the VM
   reaches its usable shell or init completion point;
4. record the commit, tree ID, build result, boot result, and log location;
5. use the Ubuntu VM for broader lifecycle, pVM, or IOMMU validation at major
   boundaries and before declaring Phase 1 complete.

Nested npVM and pVM acceptance
==============================

Material refactors and port milestones must also pass the nested guest
matrix.  The topology is always::

  vinp2 development host
    -> L1 Ubuntu VM running the kernel under test with kvm-intel.pkvm=1
      -> L2 npVM or pVM

The L2 VMM must run inside L1, never directly on ``vinp2``.  Before starting
an L2, require every L1 CPU to enter pKVM guest mode, protected DMAR to
initialize, the pKVM hypervisor-up message to appear, and the L1 taint value
to be zero.

The required L2 cases are:

* crosvm npVM, without a protected-VM option;
* crosvm pVM, with ``--protected-vm-without-firmware``; and
* protected QEMU pVM, launched with the VM-local
  ``~/WS/pkvm_vmm/qemu-vm/runpvm.sh``.

For the protected-QEMU case, use the QEMU binary and artifacts already inside
L1.  The VM-local script uses ``-accel kvm,dirty-ring-size=0`` and
``-machine pkvm-microvm,confidential-guest-support=pkvm0``.  The similarly
named script and QEMU binary on ``vinp2`` are different and are not valid
substitutes.  Mixing the host-side artifacts with the VM-local test produced
a false failure during the Phase 1 acceptance run.

Each L2 must print its expected kernel release, mount its root filesystem,
and reach ``basic.target``, a login prompt, or a stronger usable-system
milestone.  The protected-QEMU guest must additionally report
``Hypervisor detected: PKVM``.  After every case, require no new panic, BUG,
Oops, WARNING, general-protection fault, or protected-memory-access denial in
either L1 or L2, and recheck the L1 taint value.  Preserve the exact commands,
kernel and disk hashes, and serial logs.  Use private disk copies where the
test harness permits them.

The final Phase 1 source at ``pkvm-v6.18-phase1-complete`` passed this matrix
with guest kernel ``6.18.0+`` and guest ``bzImage`` SHA-256::

  3e1c1f64e03609d8950903a12f40089846310ff79b1974e3953e8cb1d885af9f

Both crosvm modes mounted their root filesystems and reached
``basic.target``.  The VM-local protected-QEMU guest detected pKVM, mounted
its root filesystem, and reached the same userspace milestone.  The L1
finished with taint zero and no fatal kernel signature.

Phase 1 progress
================

Checkpoint 1 -- SQUASHME chains
-------------------------------

Completed on 2026-08-08. All 14 ``SQUASHME`` commits and the chained
``59e69e05b041`` max-ISR fix were absorbed. The resulting history contains
753 commits above the base, including this documentation commit. No
``SQUASHME`` subject remains.

The rewrite required dependency-aware resolutions for APICv evolution,
protected-APIC ISR validation, MMU notifier teardown, and Intel PT host-vCPU
fixup ordering. The transformed MMU commit was reworded as
``KVM: x86/mmu: pKVM: Unmap guest pages when flushing shadows``.

The rewritten source tree matched ``pkvm-v6.18-phase1-original`` exactly;
the only tree difference was this documentation. ``git range-diff`` accounted
for all absorbed changes and ``git diff --check`` was clean.

The dedicated test agent built ``out-bb-phase1`` from ``config-bb`` in 39.85
seconds and booted the resulting kernel with the BusyBox VM. The VM reached a
shell, reported successful pKVM and protected DMAR initialization, had kernel
taint value zero, and contained no BUG, WARNING, Oops, or panic signature. The
tested ``bzImage`` SHA-256 was::

  456442125cb1e73b2330777a1fd327eb4d5916038c983016dac508856db4fd6d

Checkpoint 2 -- core single-target fixes
-----------------------------------------

Completed on 2026-08-08. The following 15 fixes, each with one local and
buildable target, were absorbed into the commit that introduced the affected
code:

============  =============================================================
Fix           Corrected behavior
============  =============================================================
4c5e2f12f1b1  Avoid a duplicate VM-entry interruption-info write
ed7c9166d212  Return a value from the deprivileged ``__cond_resched`` stub
eab8f8ff6b84  Validate CR0 and CR4 before using XSAVE/XRSTOR
38a3fe450aa5  Apply exception fixup to host XSETBV handling
feac53d30fd5  Use an inclusive end in ``pkvm_find_addr_range``
987a8203c0c6  Close the ``pkvm_put_vcpu`` use-after-free race
ffc271100a1c  Order CPUID leaf zero before empty CPUID entries
50dedc858c42  Initialize the VM-exit trace-printing return value
c6321af4e11b  Release the faulted-in page on MMU failure paths
1eda2a14c1c0  Reject a second protected-VM finalization
007590a961ee  Correct the reduced host-state width assertion
d8cfc3c55b49  Clear guest GPRs after VM-Exit
86556e0d1c0f  Bounds-check the paravirtual ``cache_reg`` index
23d1ef034dcc  Check the MMU-PGD load result before running a vCPU
ea9fb75d05c3  Assert that the old vCPU fpstate exists
============  =============================================================

The dependency-aware resolutions preserved the later NMI handler alongside
the moved XSETBV fix, carried the corrected FPU initialization API through
guest-FPU setup, and retained page-release cleanup when interval-tree mapping
allocation was introduced. The resulting history contains 739 commits above
the base, including this documentation commit. All 15 fix commits disappeared
as standalone ancestors, and no temporary ``fixup!`` subject remains.

The rewritten source tree matched ``pkvm-v6.18-phase1-original`` exactly;
the only tree difference was this documentation. ``git diff --check`` was
clean.

The dedicated test agent validated source commit ``58ffd74b0d3e``. A refreshed
``config-bb`` was byte-identical after ``olddefconfig`` and the cached build
completed in 44.46 seconds. The only warning was the pre-existing modpost
section mismatch from ``patch_pkvm`` to ``text_poke_early``. The VM reached a
BusyBox shell in 0.39 seconds, reported successful pKVM and protected DMAR
initialization, had kernel taint value zero, and shut down cleanly. No BUG,
WARNING, Oops, panic, call trace, or CPU-exception signature was present. The
baseline ``fail to initialize ptp_kvm`` message remained unrelated to pKVM.
The tested ``bzImage`` SHA-256 was::

  4776a221259b8b1965433bd2740329c154e519d7e22e20b861f67c53005936dd

Checkpoint 3 -- VM and vCPU validation fixes
---------------------------------------------

Completed on 2026-08-08. Ten single-target VM and vCPU fixes were absorbed:

============  =============================================================
Fix           Corrected behavior
============  =============================================================
969a62812770  Bounds-check segment PV-interface indices
3f6135255448  Snapshot the shared LAPIC pointer during vCPU creation
4487a872c966  Remove event-injection TOCTOU and repeated shared-state reads
d389171cfc69  Correct non-protected vCPU-state switch block structure
bb87706af534  Initialize a VM handle before publishing it
c80c89410515  Bounds-check and snapshot ``cpuid_nent``
fef07346faac  Snapshot shared VM state while allocating the IPIV PID table
436af2413e6d  Clear stale PID-table entries when freeing a vCPU
739ab86c6d16  Avoid the guest-vCPU iterator variable-name collision
2face0b513d2  Prevent repeated cancellation from bypassing the guard
============  =============================================================

Two initially considered commits failed the local-and-buildable target test
and were deferred. ``f4e8da73610b`` names the VMX ``vcpu_create`` operation,
but the generic ``__vcpu_create`` code that it changes is introduced later;
moving it to the named target would import about 1,800 lines prematurely.
``590abffcb6d6`` combines an APIC bus-cycle TOCTOU fix, a CPUID ratio fix, and
a postponed-per-VM-setup refactor. It must be split or anchored in the
dependency-sensitive wave.

The replay preserved the later evolution of LAPIC allocation and protection,
NMI injection, CPUID enforcement, EPT/MSR exit sharing, and protected-APIC
reset. The resulting history contains 729 commits above the base, including
this documentation commit. All ten fix commits disappeared as standalone
ancestors. The source tree matched ``pkvm-v6.18-phase1-original`` exactly,
with only this documentation differing, and ``git diff --check`` was clean.

The dedicated test agent validated source commit ``e16f4fcb151d``. A refreshed
``config-bb`` remained byte-identical after ``olddefconfig`` and the build
completed in 45.73 seconds. The only warning was the established modpost
section mismatch from ``patch_pkvm`` to ``text_poke_early``. The VM reached a
BusyBox shell in 0.40 seconds, initialized pKVM and protected DMAR, reported
taint value zero, and powered off cleanly. No BUG, WARNING, Oops, panic, call
trace, or CPU-exception signature was present; only the baseline
``fail to initialize ptp_kvm`` message matched the broad failure scan. The
tested ``bzImage`` SHA-256 was::

  fb1ecce456f4c4ceca6e8654b349dd9b9b3c25abc14db8892db290137c8a5922

Checkpoint 4 -- pVM and MMU-local fixes
----------------------------------------

Completed on 2026-08-08. Seven pVM and MMU-local fixes were absorbed:

============  =============================================================
Fix           Corrected behavior
============  =============================================================
1d546469ae0c  Validate page-table walk ranges before recursion
6e4b44500067  Snapshot and donate memcache pages before removing their links
b068cb95b079  Clear the EPT accessed bit with an atomic read-modify-write
0dfba92e265b  Require VM finalization before donating protected-guest pages
92238882172b  Strip canonical upper bits before mapping the hyp page table
c9e21eca9a2a  Require the pvmfirmware image to fit in the guest GPA space
0b5fb84ffc89  Require the pvmfirmware image to fit below 4 GiB
============  =============================================================

The replay introduced each validation with its original API and carried it
through later lazy-TLB teardown, memcache arguments, protected-APIC changes,
guest-vCPU iteration, and pvmfirmware entry enforcement. It did not pull any
later API or page-ownership transition into an earlier commit. The resulting
history contains 722 commits above the base, including this documentation
commit. All seven fix commits disappeared as standalone ancestors. The source
tree matched ``pkvm-v6.18-phase1-original`` exactly, with only this
documentation differing, and ``git diff --check`` was clean.

A direct commit-body inventory after this checkpoint identified 89 remaining
standalone ``Fixes:`` commits, represented by 138 fix-to-target trailers
because some commits name multiple targets. These candidates remain for later
single-target, split, anchored, or approved-standalone disposition.

The low-reasoning test agent validated source commit ``b2e3ce8da25d`` using a
fresh ``config-bb`` full build. The build succeeded; the surrounding tool
observed about 29 seconds, but the command's exact timing footer was not
retained. The VM reached BusyBox in 0.40 seconds, initialized pKVM and
protected DMAR, and reported taint value zero. No BUG, WARNING, Oops, panic,
call trace, general-protection, KASAN, or UBSAN signature was present. BusyBox
accepted ``poweroff`` but QEMU did not exit, so the agent terminated only the
exact test QEMU process and verified that it was gone. The tested ``bzImage``
SHA-256 was::

  a5f2cceb972dcb1a283ef05f2174ac20f7e2bb785469ce00289ad51e364294e7

Checkpoint 5 -- local x86 and tracing fixes
--------------------------------------------

Completed on 2026-08-08. Eight single-target fixes were absorbed:

============  =============================================================
Fix           Corrected behavior
============  =============================================================
03e0fc5e5218  Gate exception-table fixup by the #GP vector
170d6bc851c3  Require PARAVIRT for protected-guest MMIO hooks
332ec5d1327f  Zero hypercall output before returning it to the host
7101e8baec54  Use fully ordered host-vCPU mode transitions
9c4bb5a01323  Serialize VM-exit counter refresh against trace dumps
79750fe07bdb  Reset all VM-exit counters when tracing is enabled
d49db8112144  Include the exception-table entry definition directly
d8eee230f4de  Close the MMIO-write scope before guest-memory hypercalls
============  =============================================================

The replay applied only the correction available at each target or earliest
buildable dependency and allowed later features to evolve normally. In
particular, the fully ordered host-vCPU mode transition is anchored at
``Create helpers to set vcpu mode`` rather than calling those helpers from the
earlier target before they are declared. The replay did not import later
hypercall inputs, guest trace state, Intel PT fixup state, secure secondary-vCPU
startup, or protected-VM code into earlier commits. The hypercall brace fix
required carrying the corrected switch scope through the later
``PKVM_GHC_START_CPU`` addition; a final source-tree comparison caught and
removed an extra historical brace before validation.

Two candidates were deliberately deferred. ``3f0363dc437f`` moves the PKRU
snapshot into ``pkvm_vcpu_enter_guest()``, but that function is introduced four
commits after the named target. It will be reanchored after ``pKVM: x86: Add
pkvm_vcpu_enter_guest()`` rather than importing that function prematurely.
``91cbf1fd5e8c`` queries protected-IOMMU paging-structure coherency and remains
with the pvIOMMU wave.

The resulting history contains 714 commits above the base, including this
documentation commit. The eight fixes no longer appear as standalone commits,
while the deferred PKRU fix remains present. There are 81 remaining standalone
``Fixes:`` commits with 130 target trailers. The source tree matches
``pkvm-v6.18-phase1-original`` exactly; only this documentation differs, and
``git diff --check`` is clean.

The low-reasoning test agent validated source commit ``967822996d3f`` using a
fresh isolated ``config-bb`` output directory. The clean build completed in
42.26 seconds. The VM reached BusyBox in 0.37 seconds, reported kernel
``6.18.0+``, initialized pKVM and protected DMAR, and had taint value zero. No
panic, Oops, BUG, WARNING, call trace, unable-to-handle, KASAN, UBSAN, or
general-protection signature was present. Guest SysRq poweroff completed and
QEMU exited with status zero. The tested ``bzImage`` SHA-256 was::

  14c097c6d77e30c00d2d03cf67880ac593f73f3b6002eb6ea75187941d2e2c40

Checkpoint 6 -- TLB synchronization and guest unshare
------------------------------------------------------

Completed on 2026-08-08. Eight request, TLB, and memory-sharing fixes were
absorbed or moved to their earliest buildable prerequisite:

============  =============================================================
Fix           Corrected behavior
============  =============================================================
ea384e5f5423  Handle ``KVM_REQ_TLB_FLUSH_GUEST`` in the hyp vCPU run loop
a46caf84e739  Recheck requests after entering guest mode
e4d5560331c5  Wait for host vCPUs after host-EPT invalidation
c5a77a405244  Wait for guest vCPUs after guest-EPT invalidation
60ed2b87f8db  Treat partial npVM unshare failure as a host error
6ade502523b0  Use the guest MMU memcache while unsharing
5ce0bf1ae4ee  Refill that memcache for guest unshare hypercalls
ebd7bc968120  Assert that prevalidated share/unshare mappings succeed
============  =============================================================

The guest-TLB request and request-race fixes were folded directly into the
commits that introduced the affected run loop and guest-EPT invalidation.
The wait fixes could not be moved to their ``Fixes:`` targets: the shared wait
helper and host-vCPU iterator appear at positions 632--633 of the original
series, while the guest-vCPU iterator appears at position 635. The host wait
was therefore anchored to the wait-helper commit, and the guest wait to the
guest-vCPU iterator commit. This preserves the window-closing order without
importing later iteration infrastructure into early host-EPT code.

The guest-unshare changes were folded as a dependency chain. Memcache use was
added to the original share/unshare implementation. The later refill commit
was expanded to cover unshare and to make the resulting mapping failures hyp
bugs. This ensures that no intermediate commit clears host page state without
having enough guest page-table memory to restore the guest-owned mapping.
Later range validation retained its memcache-size check while preserving this
guaranteed-success invariant.

The resulting history contains 706 commits above the base, including this
documentation commit. All eight fixes disappeared as standalone commits.
There are 73 remaining standalone ``Fixes:`` commits with 122 target trailers.
The source tree matches ``pkvm-v6.18-phase1-original`` exactly; only this
documentation differs, and ``git diff --check`` is clean.

The low-reasoning test agent validated source commit ``866f044fe0c6`` with a
fresh isolated ``config-bb`` output directory. The build completed in 44.14
seconds. All four CPUs entered guest mode; the VM reached BusyBox in 0.47
seconds, reported kernel ``6.18.0+``, initialized pKVM and protected DMAR, and
had taint value zero. No panic, Oops, BUG, WARNING, call trace,
unable-to-handle, KASAN, UBSAN, or general-protection signature was present.
Guest SysRq poweroff completed and QEMU exited with status zero. The tested
``bzImage`` SHA-256 was::

  0103b2820932dcbd73482d41243d591179e2d1655620e9fb82740f4b255a7ca6

Checkpoint 7 -- x86 host-state validation and placement
--------------------------------------------------------

Completed on 2026-08-08. Five host-state and code-placement fixes were
absorbed:

============  =============================================================
Fix           Corrected behavior
============  =============================================================
3626e327bf7f  Give the copied cache-flush helper its generic kernel name
f4e8da73610b  Snapshot and bounds-check the host-provided ``vcpu_id``
2065565d7a8b  Clarify that canceled pVM injections stay hidden from the host
3f0363dc437f  Snapshot host PKRU immediately before every protected vCPU run
c1ffc7804cc0  Keep the host VM-handle definition in the pKVM-specific header
============  =============================================================

Three fixes were folded directly into the commits introducing the affected
helper, cancel-injection interface, and host-handle definition. The cache
helper rename was carried through the later clear-memory and address-conversion
helpers, avoiding an interval where pKVM called the obsolete name. Moving the
host handle early required preserving its pKVM-header placement as protected
IOMMU hypercall data and guest-mapping definitions were added later.

Two fixes were reanchored at the earliest generic code they actually modify.
The named VMX vCPU-create target precedes the generic ``__vcpu_create()`` by
four commits, so ``vcpu_id`` validation was folded into ``pKVM: x86: Add
vcpu_create PV interface``. Likewise, the original PKRU target only snapshots
state during vCPU load; its correction was folded into ``pKVM: x86: Add
pkvm_vcpu_enter_guest()`` so no host code can alter PKRU between the snapshot
and guest entry.

The user-return-MSR initialization fix remains deferred because it removes a
runtime hypercall while changing early per-CPU initialization ordering. The
host-EPT #PF fix also remains standalone or reanchor-only because it depends on
later VMX exit-qualification and exception-injection plumbing.

The resulting history contains 701 commits above the base, including this
documentation commit. All five fixes disappeared as standalone commits. There
are 68 remaining standalone ``Fixes:`` commits with 117 target trailers. The
source tree matches ``pkvm-v6.18-phase1-original`` exactly; only this
documentation differs, and ``git diff --check`` is clean.

The low-reasoning test agent validated source commit ``8d5ac0c5fed5`` with a
fresh isolated ``config-bb`` output directory. The build completed in 42.95
seconds. All four CPUs entered guest mode; the VM reached BusyBox in 0.34
seconds, reported kernel ``6.18.0+``, initialized pKVM and protected DMAR, and
had taint value zero. No panic, Oops, BUG, WARNING, call trace,
unable-to-handle, KASAN, UBSAN, or general-protection signature was present.
Guest SysRq poweroff completed and QEMU exited with status zero. The tested
``bzImage`` SHA-256 was::

  a77d6d3614b0ba1d9afbd1b6bc5297e460fde5bfe4c37b63bf73306a298e2334

Checkpoint 8 -- split-target core and initialization fixes
-----------------------------------------------------------

Completed on 2026-08-08. Three fixes whose trailers named multiple layers or
whose correction changed initialization ordering were absorbed:

============  =============================================================
Fix           Corrected behavior
============  =============================================================
c913267ce3ed  Require X2APIC when ``CONFIG_PKVM_INTEL`` is introduced
4a104c296a80  Reject page-table walks whose end exceeds the address space
95cdac35ffa3  Seed user-return MSR caches during trusted pKVM initialization
============  =============================================================

The Kconfig fix was anchored in the first pKVM host-support commit, where
``CONFIG_PKVM_INTEL`` is introduced. ``X86_64`` was already present there
after an earlier fold, so this checkpoint added only the missing
``X86_X2APIC`` dependency. Later IOMMU and floppy-driver dependencies retain
their original order in the final tree.

Checkpoint 4 had already folded the first, insufficient range-validation fix
into the page-table-walk introduction. The stronger end-address check and its
caller contract therefore had one effective target and were folded into that
same commit.

The user-return-MSR fix transforms its target instead of preserving the
runtime PV interface. Per-CPU caches are initialized in ``pkvm_init()`` before
the untrusted host can influence them, and the runtime hypercall is absent.
The later host ``enable_virtualization_cpu`` operation remains present as an
explicit no-op because virtualization is already enabled during pKVM setup.
This keeps each intermediate layer buildable without importing later VM and
vCPU infrastructure into the early target.

The BSP-vCPU-ID fix remains deferred. Its validation belongs with the later
postponed per-VM setup and LAPIC-reset state, not either named target in
isolation; moving it earlier would manufacture dependencies on code that does
not yet exist.

The resulting history contains 698 commits above the base, including this
documentation commit. All three fixes disappeared as standalone commits.
There are 65 remaining standalone ``Fixes:`` commits with 112 target trailers.
The source tree matches ``pkvm-v6.18-phase1-original`` exactly; only this
documentation differs, and ``git diff --check`` is clean.

The low-reasoning test agent validated source commit ``92369b958145`` with a
fresh isolated ``config-bb`` output directory. An initial ``-j24`` build was
killed by host memory pressure before producing a kernel; a second fresh
``-j8`` build completed in 80.70 seconds. All four CPUs entered guest mode;
the VM reached BusyBox in 0.77 seconds, reported kernel ``6.18.0+``,
initialized pKVM and protected DMAR, and had taint value zero. No panic, Oops,
BUG, WARNING, call trace, unable-to-handle, KASAN, UBSAN, or
general-protection signature was present. Guest SysRq poweroff completed and
QEMU exited with status zero. The tested ``bzImage`` SHA-256 was::

  b9cec4e3cb055500907c3de189598cfe536484cdd1f3595b557b173ccc582327

Checkpoint 9 -- one-shot postponed per-VM setup
------------------------------------------------

Completed on 2026-08-08. Five fixes and refactors in the postponed per-VM
setup chain were absorbed:

============  =============================================================
Fix           Corrected behavior
============  =============================================================
1dee9bc8c79e  Snapshot generic IRQ-chip and maximum-vCPU state before VMX use
590abffcb6d6  Snapshot APIC bus-cycle state and correct CPUID ratio reporting
5f2189ad2c9e  Perform host-controlled per-VM setup exactly once
83a3d6dc91db  Validate the BSP vCPU ID against the private maximum
52a30bd43c58  Reject a zero APIC bus-cycle divisor
============  =============================================================

The generic IRQ-chip and maximum-vCPU move was split across its two effective
parents. The VMX ``vcpu_precreate`` implementation now consumes already
validated private KVM state and only reads the shared PID-table pointer. The
generic ``vcpu_create`` interface snapshots and validates IRQ-chip mode and
``max_vcpu_ids`` under the VM lock before invoking the vendor operation. This
also makes the previously folded vCPU-ID bounds check independent of IPIv.

The same generic commit introduces ``postponed_per_vm_setup()`` and its
one-shot flag. Each later feature commit adds only the shared state it owns:
BUS_LOCK, NOTIFY, non-protected ``disabled_exits``, APIC bus-cycle timing, and
finally the BSP vCPU ID. Consequently, every kernel represented by an
intermediate commit snapshots all per-VM inputs available at that point once,
before creating the first vCPU.

The TSC-frequency commit now reads ``apic_bus_cycle_ns`` once, rejects zero,
and stores the validated value privately. It also reports a zero CPUID
numerator when the calculated denominator is zero. The LAPIC-reset commit can
validate ``bsp_vcpu_id`` directly because the generic setup has already made
``max_vcpu_ids`` available regardless of IPIv.

The resulting history contains 693 commits above the base, including this
documentation commit. All five fixes disappeared as standalone commits.
There are 60 remaining standalone ``Fixes:`` commits with 100 target trailers.
The source tree matches ``pkvm-v6.18-phase1-original`` exactly; only this
documentation differs, and ``git diff --check`` is clean.

The low-reasoning test agent validated source commit ``b18b17bf5116`` with a
fresh isolated ``config-bb`` output directory. The ``-j8`` build completed in
87.57 seconds. All four CPUs entered guest mode; the VM reached BusyBox in
0.37 seconds, reported kernel ``6.18.0+``, initialized pKVM and protected
DMAR, and had taint value zero. No panic, Oops, BUG, WARNING, call trace,
unable-to-handle, KASAN, UBSAN, or general-protection signature was present.
Guest SysRq poweroff completed and QEMU exited with status zero. The tested
``bzImage`` SHA-256 was::

  dcc9fb8aca224cca1d400e093306e39837143e27907849e6577354ae3d88bee2

Checkpoint 10 -- private-header ownership
------------------------------------------

Completed on 2026-08-08. Three multi-target placement fixes were absorbed:

============  =============================================================
Fix           Corrected ownership
============  =============================================================
6dd2fdc5ac77  Keep hypervisor-only BUG, WARN, and KVM-print overrides in
              ``pkvm_redef.h``
f7722d51fe3e  Keep the shared memcache layout in ``kvm_host.h`` and its
              operations in ``kvm_pkvm.h``
8db253ce50ab  Keep the mapping-tree root in KVM state and its private node
              definitions and helpers in ``kvm_pkvm.h``
============  =============================================================

The initial hypervisor-symbol rename now creates ``pkvm_redef.h`` and includes
it before any inline pKVM code can expand the affected macros. The later debug
printing and panic commits evolve that header in place, so each intermediate
commit has the redefinitions needed by the code it introduces.

The memcache structure remains in ``kvm_host.h`` because KVM-visible VM and
vCPU state embeds it. Its push, pop, free, page, top-up, and initialization
helpers enter through ``kvm_pkvm.h`` with their respective feature commits.
Likewise, ``struct kvm_pkvm_vm`` retains the interval-tree root while the
mapping node type, iterators, and safe traversal macro enter through
``kvm_pkvm.h`` with mapping tracking and page pinning.

The resulting history contains 690 commits above the base, including the two
documentation commits. All three placement fixes disappeared as standalone
commits. There are 57 remaining standalone ``Fixes:`` commits with 89 target
trailers. The non-documentation tree matches
``pkvm-v6.18-phase1-original`` byte for byte, no temporary ``fixup!`` subject
remains, and ``git diff --check`` is clean.

The low-reasoning test agent independently verified exact tree
``1ff7046fdffd`` and validated its source commit ``7baba188c3aa`` with a fresh
isolated ``config-bb`` output directory. The ``-j8`` build completed in 79.87
seconds. All four CPUs entered guest mode; the VM reached BusyBox in 0.38
seconds, reported kernel ``6.18.0+``, initialized pKVM and protected DMAR, and
had taint value zero. No panic, Oops, BUG, WARNING, call trace,
unable-to-handle, KASAN, UBSAN, or general-protection signature was present.
Guest SysRq poweroff completed and QEMU exited with status zero. The tested
``bzImage`` SHA-256 was::

  1d3632362afa715e981dabca2abca21966c684464c2b56e1fa38cd8ed8e833f5

Checkpoint 11 -- protected register sharing
--------------------------------------------

Completed on 2026-08-08. Two late protected-vCPU register fixes were split
across the four commits that introduced the affected exit paths:

============  =============================================================
Fix           Corrected behavior
============  =============================================================
09cc0f5b25ff  Restrict port-I/O RAX exposure and host updates by direction
              and operand size
15624f908e09  Restrict RDMSR and WRMSR EAX, EDX, and ECX exchange to their
              architectural 32-bit widths
============  =============================================================

The port-I/O sharing commit now exposes RAX only for OUT and masks it to the
one-, two-, or four-byte operand width. The later host-update commit consumes
RAX only for IN, preserves the upper bits for byte and word inputs, and
zero-extends double-word inputs. Thus neither direction exposes or accepts
register state that the instruction does not architecturally consume.

The RDMSR introduction now shares only the low 32 bits of ECX and accepts only
the low 32 bits of host-supplied EAX and EDX. The WRMSR introduction similarly
shares only the low 32 bits of EAX and EDX before falling through to the
already-constrained ECX path. This keeps the untrusted host-visible structure
free of upper-register residue from the first commit implementing each exit.

The resulting history contains 688 commits above the base, including the two
documentation commits. Both late fixes disappeared as standalone commits.
There are 55 remaining standalone ``Fixes:`` commits with 85 target trailers.
The non-documentation tree matches ``pkvm-v6.18-phase1-original`` byte for
byte, no temporary ``fixup!`` subject remains, and ``git diff --check`` is
clean.

The fallback low-reasoning test agent independently verified exact tree
``3fda2a0b390c`` and validated its source commit ``e49235cb7b08`` with a fresh
isolated ``config-bb`` output directory. The configuration remained
byte-identical after ``olddefconfig``, and the ``-j8`` build completed in
71.69 seconds. All four CPUs entered guest mode; the VM reached BusyBox in
0.36 seconds, reported kernel ``6.18.0+``, initialized pKVM and protected
DMAR, and had taint value zero. No panic, Oops, BUG, WARNING, call trace,
general-protection, invalid-opcode, or segmentation-fault signature was
present. The only failure-word match was the established
``fail to initialize ptp_kvm`` baseline. Guest S5 poweroff completed cleanly
and no QEMU process remained. The tested ``bzImage`` SHA-256 was::

  4bbb326e3d730489e1f08b467d8e069845cc14f7c6c1bf5f5d1537bec7d023e8

Checkpoint 12 -- MMU and host-EPT correctness
----------------------------------------------

Completed on 2026-08-08. Two local MMU/EPT fixes were absorbed:

============  =============================================================
Fix           Corrected behavior
============  =============================================================
91cbf1fd5e8c  Flush newly allocated host-EPT page-table pages for
              non-coherent IOMMUs
c6f219bc0d4e  Preserve MMU-age errors as errors rather than treating them as
              a positive young-page result
============  =============================================================

The commit that first adds cache maintenance for host-EPT PTE writes now also
flushes each freshly zeroed page-table page before a non-coherent IOMMU can
walk it. The allocation path keeps its original ownership and failure
semantics: a failed allocation is not flushed, and a successful page remains
owned by the host-EPT pool.

The npVM aging API introduction now documents its complete integer return
contract. The later host MMU-notifier plumbing checks each hypercall result
before combining it with the boolean ``young`` state, warns and continues on
a negative error, and preserves the early-exit optimization for successful
test-only operations.

The resulting history contains 686 commits above the base, including the two
documentation commits. Both late fixes disappeared as standalone commits.
There are 53 remaining standalone ``Fixes:`` commits with 82 target trailers.
The non-documentation tree matches ``pkvm-v6.18-phase1-original`` byte for
byte, no temporary ``fixup!`` subject remains, and ``git diff --check`` is
clean.

The test agent independently verified exact tree ``f1263885564d`` and
validated its source commit ``fa18d56d6300`` with a fresh isolated
``config-bb`` output directory. The configuration remained byte-identical
after ``olddefconfig``, and the ``-j8`` build completed in 73.25 seconds. All
four CPUs entered guest mode; the VM reached BusyBox in 0.37 seconds, reported
kernel ``6.18.0+``, initialized pKVM and protected DMAR, and had taint value
zero. No panic, Oops, BUG, WARNING, call trace, general-protection,
invalid-opcode, segmentation-fault, or error signature was present. The only
failure-word match was the established ``fail to initialize ptp_kvm``
baseline. Guest SysRq S5 poweroff completed, QEMU exited with status zero, and
the temporary output was removed. The tested ``bzImage`` SHA-256 was::

  96ab9a4cfade8a45b46978062800a786c16e4154ca5b0730eabd06e5bb83e716

Checkpoint 13 -- MMU address-range validation
------------------------------------------------

Completed on 2026-08-08. The cross-cutting overflow fix
``51fc55efab4c`` was split across the eleven commits that introduced the
affected MMU ownership APIs. The first host-to-hypervisor donation commit now
introduces ``is_valid_addr_range()`` and uses it for donation in both
directions. Later host/hypervisor sharing, MMIO donation, guest donation and
sharing, guest aging, and DMA pinning commits adopt the helper as each API
appears.

The helper rejects zero-sized ranges, unsigned endpoint wraparound, and
``PAGE_ALIGN()`` wraparound. Page-aligned interfaces additionally validate
both operands. Every check remains before MMU locks, page-table walks, and
ownership transitions, so the fold adds no rollback path and cannot leave a
partially transitioned range. The pVM firmware-copy commit moves the helper
next to ``gpa_range_overlaps_pvmfw()`` when that function appears; this is a
placement-only change that preserves the final source layout.

The resulting history contains 685 commits above the base, including the two
documentation commits. The late fix disappeared as a standalone commit.
There are 52 remaining standalone ``Fixes:`` commits with 71 target trailers.
The non-documentation tree matches ``pkvm-v6.18-phase1-original`` byte for
byte, no temporary ``fixup!`` or ``SQUASHME`` subject remains, and
``git diff --check`` is clean.

The test agent independently verified checkpoint ``ebd629b3557b`` and
validated its source commit ``b4aa167bc36e`` with a fresh isolated
``config-bb`` output directory. The configuration remained byte-identical
after ``olddefconfig``, and the ``-j8`` build completed in 74.47 seconds. All
four CPUs entered guest mode; the VM reached BusyBox in 0.71 seconds, reported
kernel ``6.18.0+``, initialized pKVM and protected DMAR, and had taint value
zero. No panic, Oops, BUG, WARNING, call trace, general-protection,
invalid-opcode, segmentation-fault, or error signature was present. The only
failure-word match was the established ``fail to initialize ptp_kvm``
baseline. Guest S5 poweroff completed cleanly and QEMU exited. The tested
``bzImage`` SHA-256 was::

  8a56be4563138bfe760930e656028cdf17734d4acb13df943954d66775ee519b

Checkpoint 14 -- host protected-EPT fault injection
----------------------------------------------------

Completed on 2026-08-08. The late host-EPT exception fix
``998d354d0371`` was split across the commits that own its behavior and
dependencies. The host-EPT violation introduction now receives the host vCPU,
constructs an architectural page-fault error code from the EPT exit
qualification and host SS.DPL, and injects ``#PF`` with the valid guest linear
address. Violations without a valid linear address retain the ``#GP(0)``
fallback required for PDPTE-load and model-specific trace cases.

The existing debug-log commit owns the final failure diagnostics: protected
memory access is ratelimited, and failed MMIO mapping reports its error code.
The later exception-import commit provides the final preprocessor placement
for the queueing helpers. Successful concurrent or newly installed MMIO
mappings still return directly to retry the host instruction. Protected guest,
pvmfirmware, and IOMMU-MMIO ranges remain unmapped from the host; the change
selects the exception reported to the host and does not alter ownership, DMA
visibility, or rollback behavior.

The resulting history contains 684 commits above the base, including the two
documentation commits. The late fix disappeared as a standalone commit.
There are 51 remaining standalone ``Fixes:`` commits with 70 target trailers.
The non-documentation tree matches ``pkvm-v6.18-phase1-original`` byte for
byte, no temporary ``fixup!`` or ``SQUASHME`` subject remains, and
``git diff --check`` is clean.

The test agent independently verified checkpoint ``0d4599abdca9`` and
validated its source commit ``ae480109a440`` with a fresh isolated
``config-bb`` output directory. The configuration remained byte-identical
after ``olddefconfig``, and the ``-j8`` build completed in 84.69 seconds. All
four CPUs entered guest mode; the VM reached BusyBox in 0.49 seconds, reported
kernel ``6.18.0+``, initialized pKVM and protected DMAR, and had taint value
zero. No panic, Oops, BUG, WARNING, call trace, general-protection, or other
CPU-exception signature was present. The only failure-word match was the
established ``fail to initialize ptp_kvm`` baseline. Guest S5 poweroff
completed cleanly and QEMU exited. The tested ``bzImage`` SHA-256 was::

  d8f63c68fca23bafb4c3ecf4dc81664206087edd10ab276df3735842ef9bce28

The rewritten introducing commit was also tested in isolation. Its build
passed, and its BusyBox boot reproduced the same CPU0 deprivilege ``-EINVAL``
seen when independently building and booting untouched original commit
``16428559d0b0``. Both reached BusyBox with four CPUs and taint zero. This is
therefore a pre-existing limitation of that early historical point rather
than a bisectability regression introduced by this fold.

Checkpoint 15 -- consecutive CPU indexing
------------------------------------------

Completed on 2026-08-08. The late consecutive-indexing fix
``c1a901671fde`` was split across the commits that own the host pCPU and vCPU
state and the paths that consume it. The pCPU structure commit records the
Linux CPU number in each pCPU and builds ``pkvm_hyp->pcpus`` as a compact
array. The host-vCPU commit adds a host per-CPU vCPU pointer and commits both
compact-array entries only after the complete CPU setup succeeds.

The per-CPU setup ABI passes the physical pCPU and vCPU pointers to the
hypervisor. The hypervisor converts them, validates the pCPU number, and then
installs its per-CPU ``phys_cpu`` and ``host_vcpu`` pointers. Validation of the
embedded host-vCPU number is added when the complete ``struct kvm_vcpu`` type
becomes available to the hypervisor; placing that dereference earlier would
break the historical build. Deprivilege and VMCS host-state paths consume the
per-CPU pointers instead of indexing compact arrays with a possibly sparse
Linux CPU number.

This changes neither memory ownership nor DMA visibility. It prevents holes
created by sparse Linux CPU numbers from being mistaken for valid entries by
code that iterates the compact range ``[0, num_cpus)``.

The resulting history contains 683 commits above the base, including the two
documentation commits. The late fix disappeared as a standalone commit.
There are 50 remaining standalone ``Fixes:`` commits with 68 target trailers,
and no x86-core candidate remains. The non-documentation tree matches
``pkvm-v6.18-phase1-original`` byte for byte, no temporary ``fixup!`` or
``SQUASHME`` subject remains, and ``git diff --check`` is clean.

The test agent independently validated the byte-identical final source tree,
then at commit ``fe55e36d8069``, with a fresh isolated ``config-bb`` output
directory. The configuration remained byte-identical after ``olddefconfig``,
and the ``-j8`` build completed in 101.30 seconds. All four CPUs were online;
the VM reached BusyBox in 0.35 seconds, initialized pKVM and protected DMAR,
and had taint value zero. No panic, Oops, BUG, WARNING, call trace,
CPU-exception, deprivilege-failure, or pKVM-initialization-failure signature
was present. The only failure-word match was the established
``fail to initialize ptp_kvm`` baseline. Guest S5 poweroff completed cleanly
and QEMU exited. The tested ``bzImage`` SHA-256 was::

  d385719005ae82046756a54228bb84df39cb41d0f55df12721197b684805de64

The rewritten hypervisor-component boundary ``524377b85f9e`` was also tested
in isolation. Its fresh ``-j8`` build passed in 82.33 seconds, and its four-CPU
BusyBox boot reached the shell in 0.47 seconds with taint zero and a clean S5
shutdown. It reported the same CPU0 deprivilege ``-EINVAL`` as its untouched
original logical counterpart ``25ac952f97e0``; that counterpart also built,
reached BusyBox with four CPUs and taint zero, and shut down cleanly. The
failure is therefore inherited from this historical point rather than caused
by the fold. The rewritten boundary's ``bzImage`` SHA-256 was::

  db743de6fbaa3d773a6e5b46e9af7d359227f0e3ed3e4cc53cacdf67c17d84e3

The later boundary that introduces host-vCPU identity validation and its
untouched original counterpart are both non-buildable with ``config-bb``.
The original stops first on an incomplete exception-table type; the rewritten
history gets farther and stops on missing host-vCPU mode helper declarations.
The corresponding historical boundary was therefore not build-bisectable
before this fold.

Checkpoint 16 -- protected IOMMU domain page tables
----------------------------------------------------

Completed on 2026-08-08. Three late fixes were folded into the commits that
own protected IOMMU domain mapping, unmapping, and DMA pin release. The domain
page-table hypercall commit now treats host-supplied ranges as untrusted. Its
clear walker refuses to descend through a superpage leaf for a partial unmap,
and its map walker returns distinct error pointers for unsupported PFNs,
allocation failure, and collision with an existing superpage. In particular,
``-EEXIST`` prevents the host from responding to an unresolvable collision by
donating more memcache pages and retrying forever. The accidental comma
expression used to populate the map request is also replaced by independent
assignments.

The partial-unmap fix originally carried only the domain-page-table hypercall
as its ``Fixes:`` target, but one of its two guards belongs to the later DMA
pinning commit. That guard is therefore folded into the DMA-unpin walker that
it protects. A partial range can no longer make that walker interpret a mapped
superpage as a lower-level table and drop unrelated DMA pins. The temporary
error-pointer handling needed by an older ``switch_to_super_page()`` caller is
removed naturally when the later optimization passes the already validated
PTE into that helper.

These changes preserve the two-pass unmap ordering: mappings are made
non-present, IOTLB and Device-TLB state is invalidated, and only then are DMA
pins released. They neither create a writable host alias nor make protected
memory DMA-accessible.

Intermediate ``config-bb`` builds also exposed an older chronology error in
Checkpoint 5: the host-vCPU mode fix had been moved to its named target even
though the ordered helper declarations occur later. Both untouched pvIOMMU
counterpart commits built and booted, while their first rewritten equivalents
stopped on implicit helper declarations. The local write-barrier helper now
remains with ``Add helper to kick vcpu`` and the late correction is anchored at
``Create helpers to set vcpu mode``. This repair changes intermediate history
only; the final source tree is unchanged.

The resulting history contains 680 commits above the base, including the two
documentation commits. All three late fixes disappeared as standalone
commits. There are 47 remaining standalone ``Fixes:`` commits with 65 target
trailers. The non-documentation tree matches
``pkvm-v6.18-phase1-original`` byte for byte, no temporary ``fixup!`` or
``SQUASHME`` subject remains, and ``git diff --check`` is clean.

The corrected domain-page-table owner ``c578effba69d`` and DMA-pin owner
``b31669755d80`` were each validated in fresh isolated ``config-bb`` output
directories. Their ``-j8`` builds completed in 125.62 and 131.38 seconds,
respectively. Both four-CPU VMs reached BusyBox, initialized pKVM and protected
DMAR, reported taint zero, showed no crash or CPU-exception signature, and
completed clean S5 shutdowns. Their tested ``bzImage`` SHA-256 values were::

  c578effba69d: 67585057162c0755200083f5a2e9a97e5908217490beee1d55642068fefd2aab
  b31669755d80: 308975b32964b2a67b7f6c9188d9358839e3a0737a8f19d9e8215204241c86d0

The untouched logical counterparts ``119a4e6a8273`` and ``cb51d2d27ca8``
also built and booted successfully under the same configuration. This confirms
that the repaired boundaries preserve their original bisectability as well as
the final source.

Finally, the test agent validated the byte-identical checkpoint source with a
fresh ``config-bb`` build. Its four-CPU VM reached BusyBox in 0.44 seconds,
initialized pKVM and protected DMAR, reported taint zero, showed no BUG,
WARNING, Oops, panic, call trace, or CPU-exception signature, and shut down
cleanly. The only failure-word match was the established
``fail to initialize ptp_kvm`` baseline. The tested ``bzImage`` SHA-256 was::

  a45549047c7c654fa34bbd93df96b9ff75594f4dcb1a32ce9c528c661d5016b7

Checkpoint 17 -- protected QI and GCMD sequencing
--------------------------------------------------

Completed on 2026-08-08. Three late single-target fixes were folded into the
consecutive commits that introduce protected queued-invalidation submission,
root-table activation, and translation enablement.

The QI submission owner now converts and donates the host queue read-only
before initializing its descriptor state and enabling QI. ``iommu->qi`` is
published only after all those steps succeed, so donation failure cannot
leave a partially initialized queue visible to later paths. A host submission
or IEC-flush request made without active QI returns ``-EINVAL`` instead of
triggering ``BUG_ON``. Rejected IQT writes return through the common MMIO exit
and release ``iommu->lock`` instead of returning while it is held.

The SRTP owner now rejects root-table activation when enhanced SRTP is absent
and QI has not initialized the context- and IOTLB-flush callbacks. This turns a
host-controlled command-order NULL dereference into a normal error. The TE
owner constructs direct GCMD writes from physical GSTS rather than the virtual
shadow presented to the host. A virtual-only TE disable therefore cannot make
a later unrelated command clear physical translation and expose protected
memory to DMA. Only the affected virtual status bit is updated after each
hardware transition.

The resulting history contains 677 commits above the base, including the two
documentation commits. All three late fixes disappeared as standalone
commits. There are 44 remaining standalone ``Fixes:`` commits with 62 target
trailers. The non-documentation tree matches
``pkvm-v6.18-phase1-original`` byte for byte, no temporary ``fixup!`` or
``SQUASHME`` subject remains, and ``git diff --check`` is clean.

Each corrected owner was also validated at its exact rewritten commit with a
fresh isolated ``config-bb`` build and four-CPU BusyBox boot. The QI and SRTP
owners built in 98.59 and 100.68 seconds, respectively; the TE owner also
completed its build successfully. All three VMs initialized pKVM and protected
DMAR, reached BusyBox with four CPUs and taint zero, showed no BUG, WARNING,
Oops, panic, call trace, or CPU-exception signature, and completed clean S5
shutdowns with QEMU exit status zero. Their tested ``bzImage`` SHA-256 values
were::

  fc40490810d2: 2d92c76acd28eda1414105ca7bcc9502670337ad11e46dc08c3368e8395be181
  ea9cac60f757: fadb2819d4e499e079e3aec56957dd783e47f76f522a2c69f335ea1c94671fde
  3c18a3ce83a8: 9664fcedcb4957ef531256da9dda62ebeca34203fea8fb34d05b4793594bfe59

The only broad failure-word match was the established unrelated
``fail to initialize ptp_kvm`` baseline.

Finally, the byte-identical checkpoint source was rebuilt from a fresh
``config-bb`` output directory in 73.43 seconds. Its four-CPU VM reached
BusyBox, initialized pKVM and protected DMAR, reported taint zero, showed no
crash signature, and completed a clean S5 shutdown with QEMU exit status zero.
The tested ``bzImage`` SHA-256 was::

  ee9e33eab00d4e5226f5195c1299c5bd60e5a4adab646a49f8be7211144cf9ad

Checkpoint 18 -- immutable TE/QIE and Device-TLB invalidation
--------------------------------------------------------------

Completed on 2026-08-08. The two-target TE/QIE hardening fix and its
dependent Device-TLB assertion were folded together into the TE owner. That is
the first boundary where both initialized features can be made immutable
without asserting an invariant that earlier commits do not yet enforce. The
ITE retry rationale was anchored instead at the scalable-context owner, where
the preceding legacy path and the new scalable path have both restricted ATS
to platform SATC devices.

The TE owner now rejects host attempts to disable either translation or an
initialized queued-invalidation engine. Host IOMMU teardown and unsupported S3
suspend/resume paths cannot request those transitions while pKVM is enabled.
Direct GCMD writes derive their state from physical GSTS, and a mismatch with
the virtual shadow is a hypervisor bug. This prevents later host commands from
clearing physical TE or QIE and exposing protected memory to DMA.

Both PASID and non-PASID Device-TLB flush paths now assert that translation is
active instead of silently skipping an invalidation. A broken TE invariant can
therefore no longer leave stale device ATC entries usable after memory has been
unmapped or donated. The ITE comment records why the host device-presence test
is absent for integrated SATC devices, while explicitly retaining the residual
hard-lockup risk from device malfunction or abrupt removal initiated by the
host kernel or privileged userspace.

The resulting history contains 674 commits above the base, including the two
documentation commits. All three late fixes disappeared as standalone
commits. There are 41 remaining standalone ``Fixes:`` commits with 58 target
trailers. The non-documentation tree matches
``pkvm-v6.18-phase1-original`` byte for byte, no temporary ``fixup!`` or
``SQUASHME`` subject remains, and ``git diff --check`` is clean.

The final TE/QIE owner ``6b0e4a8f0bfa`` has tree
``cf19c4c8ad1e``; that exact tree was validated before its message-only amend
with a fresh isolated ``config-bb`` build in 85.99 seconds. The final
scalable-context owner ``d05de7ab5de1`` has tree ``a94044dbc773``; its exact
tree built in 86.86 seconds. Both four-CPU VMs initialized pKVM and protected
DMAR, reached BusyBox with taint zero, showed no BUG, WARNING, Oops, panic,
call trace, or CPU-exception signature, and completed clean S5 shutdowns with
QEMU exit status zero. Their tested ``bzImage`` SHA-256 values were::

  6b0e4a8f0bfa: 8414c3dfeaddbb63b9eb36478bb04054119189f1c0d9e24a7a5d07ca092ef3cf
  d05de7ab5de1: 26f9a794446e6d86eeaa718858be26d4c898e661483e1e84ec3c793d2b06273b

The only broad failure-word match was the established unrelated
``fail to initialize ptp_kvm`` baseline.

Finally, the byte-identical checkpoint source at tree ``932b928cfe98`` was
rebuilt from a fresh ``config-bb`` output directory in 95.34 seconds. Its
four-CPU VM reached BusyBox, initialized pKVM and protected DMAR, reported
taint zero, showed no crash signature, and completed a clean S5 shutdown with
QEMU exit status zero. The tested ``bzImage`` SHA-256 was::

  6418b75a71cfe6f1f5b8e0bbdb3745ca079f6a24370ae5735859615dd56165ad

Checkpoint 19 -- protected context-entry lifecycle
---------------------------------------------------

Completed on 2026-08-08. Four late context-management fixes were folded into
the boundaries that first own their invariants. The root-table ordering guard
was folded into the legacy-context hypercalls, while the valid-pointer and
already-present setup handling were folded into the scalable-context
hypercalls. The setup/clear race was anchored at the later upstream commit
that introduces two-phase context clearing, which is the actual point where
that race becomes possible.

Context hypercalls now reject calls made before SRTP has installed
``iommu->root_entry`` instead of deriving a pointer from NULL. Teardown only
extracts a PASID directory or legacy PGD from a present context entry,
initializes those pointers to NULL, and returns donated pages only when a valid
pointer was captured. This prevents a host-controlled non-present entry from
causing physical address zero or unrelated memory to be returned to the host.

Scalable setup now returns ``-EEXIST`` internally when the context is already
present. The hypercall takes its normal error unwind, restores the newly
donated PASID-directory pages to host ownership, and then preserves the
idempotent success result expected by the host. The two-phase clear path keeps
``iommu->lock`` held from clearing Present through cache invalidation and the
final entry clear. A racing setup therefore cannot install a new directory
between those writes and leave hardware with a present entry pointing at
host-writable physical address zero.

The resulting history contains 670 commits above the base, including the two
documentation commits. All four late fixes disappeared as standalone
commits. There are 37 remaining standalone ``Fixes:`` commits with 54 target
trailers. The non-documentation tree matches
``pkvm-v6.18-phase1-original`` byte for byte, no temporary ``fixup!`` or
``SQUASHME`` subject remains, and ``git diff --check`` is clean.

The legacy-context owner ``53eff1a57056`` was validated at its exact commit
with a fresh isolated ``config-bb`` build in 106.38 seconds. The final
scalable-context owner ``d7b3144b3928`` has tree ``cafd3c74d44b``; that exact
tree built in 106.92 seconds before its message-only amend. The final
two-phase context-clear owner ``6c3a152e3138`` has tree ``b504e06815d9``;
that exact tree also built successfully before its message-only amend. All
three four-CPU VMs initialized pKVM and protected DMAR, reached BusyBox with
taint zero, showed no BUG, WARNING, Oops, panic, call trace, or CPU-exception
signature, and completed clean S5 shutdowns with QEMU exit status zero. Their
tested ``bzImage`` SHA-256 values were::

  53eff1a57056: fb09975667b583d25f2e57378a22157d6660657246108080c00e0d6414f2176b
  d7b3144b3928: efb903a8f2c20a0ba9628165a6b80b639363001e9afdf08c9fbf58ed8ddc3277
  6c3a152e3138: 1ded52c30d6b132d5ad8b2c18814108f06a6d2f864eb83c5d3dd51d36aa80f0b

The only broad failure-word match was the established unrelated
``fail to initialize ptp_kvm`` baseline.

Finally, the byte-identical checkpoint source at tree ``b3d52a1f6fdd`` was
rebuilt from a fresh ``config-bb`` output directory in 73.06 seconds. Its
four-CPU VM reached BusyBox in 0.38 seconds, initialized pKVM and protected
DMAR, reported taint zero, showed no crash signature, and completed a clean S5
shutdown with QEMU exit status zero. The tested ``bzImage`` SHA-256 was::

  48d99a1d036fe945ec707867903286f7e1bdd4538b1a4640c561d93fac8519ec

Checkpoint 20 -- protected PASID lifecycle and domain validation
-----------------------------------------------------------------

Completed on 2026-08-08. Five late PASID fixes were folded into four
boundaries that own the required context, lock, and domain state. The
first-level hypercall rejects ``FLPT_DEFAULT_DID`` before accepting a page
table. The PASID teardown hypercall no longer carries a stack-backed table
derived from an earlier context lookup; first-level setup, second-level
setup, and teardown resolve the live PASID directory from the protected
context entry while ``iommu->lock`` is held.

The atomic teardown fix is anchored at the later upstream commit that first
clears the PASID Present bit before invalidation. That boundary supplies the
two-phase teardown helper the fix needs. In the pKVM build, the lock remains
held from the protected lookup and Present-bit clear through PASID-cache,
IOTLB, and Device-TLB invalidation and the final direct entry clear. A racing
context teardown or setup therefore cannot donate or replace the directory
page between two walks. An initial attempt to fold this invariant into the
earlier pKVM PASID-helper port failed the intermediate build because
``pasid_clear_present()`` did not exist at that point; the build gate caught
the chronology error before the checkpoint was recorded.

The domain-lifetime owner now also treats the referenced protected domain as
the authority for translation properties. First-level setup rejects a
second-level domain, derives four- or five-level walk state from the trusted
``agaw``, and validates force-snoop against IOMMU capability. Legacy-context
and second-level PASID setup reject first-level domains. Every rejection and
setup failure retains the matching domain put, while successful setup
transfers the reference to the installed entry for teardown to release.

The resulting history contains 665 commits above the protected-series base,
including the two documentation commits. All five late fixes disappeared as
standalone commits. There are 32 remaining standalone ``Fixes:`` commits with
48 target trailers. The non-documentation tree matches
``pkvm-v6.18-phase1-original`` byte for byte, no temporary ``fixup!`` or
``SQUASHME`` subject remains, and ``git diff --check`` is clean.

Each corrected owner was validated at its exact rewritten commit with a fresh
isolated ``config-bb`` build and four-CPU BusyBox boot. The first-level,
dynamic-directory, and domain-lifetime owners built in 105.44, 106.00, and
74.83 seconds, respectively; the atomic-teardown owner also completed its
build successfully. All four VMs initialized pKVM and protected DMAR, reached
BusyBox with four CPUs and taint zero, showed no BUG, WARNING, Oops, panic,
call trace, or CPU-exception signature, and completed clean S5 shutdowns with
QEMU exit status zero. Their tested ``bzImage`` SHA-256 values were::

  49a95f155941: 8864c2c6cb916d10df42962ded4e3ec5b5155904585d475ecba67b6b5a054f22
  302c444b340a: 0fbbcd00bd4174dc9e34343dc0d96377e25fee1571629b320094850df2ccdfbc
  05abc4b29a79: dc00920e15bd338b81fdecbb0183b7dff6a4a125bdda398ac279eaa82fbbe968
  9f89a300ff8f: ebce31f1d138ce908067dbc1a18296054b76b48a354de54ff54fbb05346c5470

The only broad failure-word match was the established unrelated
``fail to initialize ptp_kvm`` baseline.

Finally, the byte-identical checkpoint source at commit ``eb200cf06ae5``
and tree ``925b6674e538`` was rebuilt from a fresh ``config-bb`` output
directory. Its four-CPU VM reached BusyBox in 0.40 seconds, initialized pKVM
and protected DMAR, reported taint zero, showed no crash signature, and
completed a clean S5 shutdown with QEMU exit status zero. The tested
``bzImage`` SHA-256 was::

  76fd1f74b84c8030b9f02f334043b3e67290abe6b7c0d0f6c540e095f041716a

Checkpoint 21 -- local protected-IOMMU safety fixes
---------------------------------------------------

Completed on 2026-08-08. Four local, single-owner fixes were folded into the
commits that introduce legacy-context requests, protected cache tags, IOMMU
domain allocation, and DMA pinning.

Legacy context requests now tolerate DMA aliases that have no matching
``device_domain_info``. They pass ATS-disabled defaults instead of
dereferencing NULL. Device-TLB cache tags include the target IOMMU in their
identity as well as BDF and DID/PASID, so equal requester IDs behind different
DRHD units cannot share a tag or route an ATC invalidation to the wrong queue.

Protected-domain allocation now returns a write-protected PGD to host-owned
writable state if domain allocation fails after donation. This keeps the
ownership rollback paired with the failed operation and does not leave a host
page stranded read-only. The DMA clear walker combines recursive
``leaf_ptes_only`` results monotonically. Once any intermediate translation
changes, the invalidation hint remains clear, ensuring hardware cannot retain
an intermediate IOTLB entry whose page-table page is subsequently unpinned
and reused.

The resulting history contains 661 commits above the protected-series base,
including the two documentation commits. All four late fixes disappeared as
standalone commits. There are 28 remaining standalone ``Fixes:`` commits with
44 target trailers. The non-documentation tree matches
``pkvm-v6.18-phase1-original`` byte for byte, no temporary ``fixup!`` or
``SQUASHME`` subject remains, and ``git diff --check`` is clean.

Three corrected owners were validated at their exact rewritten commits with
fresh isolated ``config-bb`` builds and four-CPU BusyBox boots. Each VM
initialized pKVM and protected DMAR, reached BusyBox with taint zero, showed
no runtime crash signature, and completed a clean S5 shutdown with QEMU exit
status zero. Their build times and tested ``bzImage`` SHA-256 values were::

  cc15afaf2b22 (104.91 s): c21ca99c008f615075c6105fe871aa720bf6439e0f129d633fb9bf23c0d1de55
  e672b1393ba8 (104.47 s): bb4f270386c8e21c9ce3e2c1ba859b5f88ecca358026f77d70a6c5a6ab023fb4
  ec9424685eda (87.59 s):  8fa89a330e59d116ce2dfdd5936532edc9def0e6ff57bee82f0e2d7cfcf33853

The domain-allocation owner ``0c6da4067848`` also built successfully; its
``bzImage`` SHA-256 was
``56328d8041ab48f331e5ac94982709a0884a1217b5fd0e4b3ebd00a46d27eec4``.
Its VM did not reach BusyBox: CPU0 repeatedly reported host access to
protected memory and the 20-second guard terminated QEMU. The untouched
Checkpoint 20 counterpart ``500b28fc93b9`` reproduced the same failure class
on CPU1 and hit the same timeout. The differing protected addresses reflect
allocation layout, while the behavior establishes that this historical
boundary was already not boot-bisectable and was not regressed by the fold.

Finally, the byte-identical checkpoint source at commit ``a2b548bc22d8`` and
tree ``1974338c503a`` was rebuilt from a fresh ``config-bb`` output directory
in 72.54 seconds. Its four-CPU VM reached BusyBox in 0.42 seconds, initialized
pKVM and protected DMAR, reported taint zero, showed no crash signature, and
completed a clean S5 shutdown with QEMU exit status zero. The tested
``bzImage`` SHA-256 was::

  9f5a22cb9c9b6def080370de463f8ada48b816aaeb55af69520f40ba316d368a

Checkpoint 22 -- shared protected-IOMMU memcache refill
-------------------------------------------------------

Completed on 2026-08-08. The late protected-IOMMU memcache conversion was
folded into the commit that extends the common refill API with read-only
sharing donations. The preceding API-exposure commit remains a generic,
buildable boundary.

The common refill helper snapshots the host-shared memcache before following
its head and donates each page before popping metadata from it. The host
cannot retarget the shared head between validation and access, and pKVM does
not read potentially private page contents while the host still owns the
page. Protected VT-d domain refill now uses that implementation, reads the
shared page count once, and marks donated page-table pages for the host
read-only mapping required by the IOMMU ABI. The duplicate IOMMU admission
callback is removed.

The initial rewrite placed the IOMMU conversion in ``Expose memcache refill
API``. Its isolated build failed because the consumer set
``PKVM_MC_DONATE_SHARE_RO`` before that flag existed. The conversion was
moved to the immediate child ``Extend pkvm_refill_memcache for share_ro
donation``, which introduces the donation mode and is therefore the earliest
truthful buildable owner.

The resulting history contains 660 commits above the protected-series base,
including the two documentation commits. The late fix disappeared as a
standalone commit. There are 27 remaining standalone ``Fixes:`` commits with
43 target trailers. The non-documentation tree matches
``pkvm-v6.18-phase1-original`` byte for byte, no temporary ``fixup!`` or
``SQUASHME`` subject remains, and ``git diff --check`` is clean.

Both corrected boundaries were validated from fresh isolated ``config-bb``
outputs. The generic API owner ``e5d42879b840`` built successfully and its
four-CPU VM reached BusyBox with pKVM and protected DMAR initialized, taint
zero, no crash signature, and clean S5 shutdown. Its tested ``bzImage``
SHA-256 was::

  bd8fa2199f9d2121a005d4d628e98141fd3b8bffcfc8b5d31a2a97f8ccd0bbb6

The share-RO/IOMMU owner ``3d7c37f0dd45`` built in 85.97 seconds and passed
the same boot checks. Its tested ``bzImage`` SHA-256 was::

  ad218336a9b934407c7bf7ce222a558055dd1493869ee65554c5832ea17ff4a1

Finally, the byte-identical checkpoint source at commit ``1f718e46d3bf`` and
tree ``434119eb83e2`` was rebuilt from a fresh ``config-bb`` output directory
in 128.80 seconds. Its four-CPU VM reached BusyBox, initialized pKVM and
protected DMAR, showed no crash signature, and completed a clean powerdown.
The tested ``bzImage`` SHA-256 was::

  7253e2e61c54bd2f843211e823b8bcce2c6ccea0acff2fe53d770c6ca15ccb67

Checkpoint 23 -- protected-IOMMU domain lifetime
-------------------------------------------------

Completed on 2026-08-08. The free-domain use-after-free fix
``f91523f5426f`` was folded into ``Keep domains alive for active entries``.
The combined owner now serializes domain lookup, the last-reference
transition, and hash removal under ``iommu_domain_lock`` before performing
teardown. Its free helper accepts the PGD GPA directly, keeping unreferenced
domain lookup inside the locked operation.

The lifetime owner itself, originally ``9748cff51cae``, remains an approved
standalone exception. It is the first boundary with the completed context and
PASID setup/teardown consumers needed to transfer domain references to active
hardware entries and return them on teardown. Moving it back to the earlier
cache-tag owner would cross the later default-domain API, hypercall, and
translation-validation boundaries and would no longer be a truthful
bisectable commit.

The resulting history contains 659 commits above the protected-series base,
including the two documentation commits. The UAF fix disappeared as a
standalone commit. There are 26 remaining standalone ``Fixes:`` commits with
41 target trailers, including the approved lifetime exception and 21
upstream-base exceptions. The non-documentation tree remains byte-identical
to Checkpoint 22 and ``pkvm-v6.18-phase1-original``; no temporary ``fixup!``
or ``SQUASHME`` subject remains, and ``git diff --check`` is clean.

The combined owner ``5e90e73c5893`` (tree ``2cd585f62f1a``) was validated
from a fresh isolated ``config-bb`` output. It built in 73.40 seconds, and its
four-CPU VM reached BusyBox with every CPU entering guest mode, pKVM and
protected DMAR initialized, taint zero, no crash signature, and clean
powerdown. The
tested ``bzImage`` SHA-256 was::

  d5749bfced1c5bdff5c26c10812c05ee4dbfa7d2a0b1161fdf8b359f0a340f12

Checkpoint 24 -- target-IOMMU compatibility disposition
---------------------------------------------------------

Attempted on 2026-08-08 and rolled back. ``Validate domains against target
IOMMU`` depends on the explicit referenced-domain lookup introduced by the
Checkpoint 23 lifetime owner, so the fix was folded there for an isolated
bisectability test. The rewritten owner was ``45346a383429`` with tree
``6ff066911700``; the later message-only reword did not change that tree.

The fresh ``config-bb`` build completed in 71.88 seconds with ``bzImage``
SHA-256::

  de80af144c4816b0f7e48d9c77d1b32de396276ef9fced84c9e9e7d293276283

Both boots reached BusyBox with four CPUs in guest mode and initialized pKVM
and protected DMAR, but both emitted ``failed to send PV IPI: -22``, a
WARNING and call trace at ``__send_ipi_mask``, and finished with taint 512.
The matching Checkpoint 23 control ``5e90e73c5893`` was rebuilt in 72.62
seconds with SHA-256::

  06f66b8872fc6c7d5e69b71d662d587bedb9f7bb240095d4a44e7c4feb1448b6

Both control boots reached the same functional milestones with taint zero
and no PV-IPI warning or call trace. The fold therefore fails the Phase 1
bisectability gate even though its source changes are confined to protected
IOMMU domain compatibility. The rewrite was discarded and
``4aac51318280`` remains an approved standalone exception at its original
late boundary.

That retained boundary, active commit ``92f300f5b264``, was then rebuilt from
a fresh ``config-bb`` output in 73.42 seconds. Its ``bzImage`` SHA-256 was::

  9cefc4228add8fced682cbaf5f2f4e7b53b6fe0d6c84ee08159d891de6c0ba0f

Both retained-boundary boots reached BusyBox with four CPUs in guest mode,
initialized pKVM and protected DMAR, reported taint zero, showed neither the
PV-IPI warning nor another crash signature, and completed clean S5 shutdown.
This confirms that the fix is bisectable at its original late location even
though moving it into the lifetime owner is not.

Checkpoint 25 -- protected-IOMMU entry format and ATS state
-------------------------------------------------------------

Completed on 2026-08-08. The three remaining pVIOMMU candidates were split
at their actual security dependencies instead of being moved as one late
cluster.

``Enforce scalable mode gates`` (original ``02726c811475``) was folded into
``Pasid table entry teardown hypercall``, the first boundary where legacy
context, scalable context, first-level PASID, second-level PASID, and teardown
handlers all exist. The owner rejects legacy setup on scalable-mode units and
rejects scalable context or PASID operations on units without scalable mode.

``Derive ATS enablement state from hardware context entry`` (original
``aca4d9407385``) and ``Clean up ATS and domain lookup in hypercall handlers``
(original ``a2bb737778ce``) were folded into ``Port cache tag management logic
to pkvm``. That is the earliest truthful owner because it introduces the
protected Device-TLB cache tags whose creation and invalidation must follow
the live CE.DTE state. PASID setup and context teardown now derive ATS and
PFSID state from the protected context entry rather than accepting a second,
contradictory host payload.

Disposable trials validated both placements before rewriting the series. The
scalable-gate trial ``b559dc6fb74b`` had the same tree
(``c1f21fe5eb6a``) as its final owner ``69a1f52649f3``. It built in 157.56
seconds with ``bzImage`` SHA-256::

  b51d227664ac0578091f63bd5fd710f5e0ee11b80241c7429807ea1265bf3d90

The ATS/cache-tag trial ``a1c58448cd84`` (tree ``ee9f85256da4``) built in
163.53 seconds with ``bzImage`` SHA-256::

  2d41c45187a479370cbd23d3b8375deeaeb6d2b44fcd03da15fa02c89ac19eeb

Both trials reached BusyBox with four CPUs in guest mode, initialized pKVM
and protected DMAR, reported taint zero, showed no PV-IPI warning or other
crash signature, and completed clean S5 shutdown.

The actual combined cache-tag owner ``15f89cb4de03`` (tree
``4605930c26ae``) was then rebuilt from another fresh ``config-bb`` output in
117.91 seconds. Its ``bzImage`` SHA-256 was::

  98262e658960cd9146c7eea013477670d072b9149756e2a87f472ee6a2191d11

Its four-CPU VM passed the same gate: every CPU entered guest mode, pKVM and
protected DMAR initialized, BusyBox reported ``nproc=4`` and taint zero, no
PV-IPI warning or crash trace appeared, and SysRq poweroff completed S5.

The resulting history contains 656 commits above the protected-series base,
including the two documentation commits. All three late fixes disappeared as
standalone commits. There are 23 remaining ``Fixes:`` candidates with 26
target trailers: the two approved in-series standalone exceptions and the 21
upstream-base exceptions. No pending in-series candidate remains. The final
source tree remains byte-identical to Checkpoint 24 and
``pkvm-v6.18-phase1-original``; no temporary ``fixup!`` or ``SQUASHME``
subject remains, and ``git diff --check`` is clean.

Phase 1 final audit
===================

Completed on 2026-08-08. ``git range-diff --no-patch`` compared
``61caa6bdaf42..pkvm-v6.18-phase1-original`` with
``61caa6bdaf42..pkvm-v6.18-phase1``. The original range contains 767 commits
and the rewritten range contains 656. Range-diff classified 450 commits as
equal, 197 as changed, 120 as left-only, and nine as right-only. Seven
left/right entries are the same surviving subjects reidentified after their
dependencies moved; the other two right-only entries are these documentation
commits. The remaining 113 left-only entries are the recorded folds, giving
``767 - 113 + 2 = 656``. This accounts for every original patch and both
documentation additions.

The final non-documentation source is byte-identical to
``pkvm-v6.18-phase1-original``. ``git diff --check`` is clean, no
``SQUASHME``, ``fixup!``, or ``amend!`` subject remains, and the manifest has
no pending in-series candidate. The only in-series ``Fixes:`` exceptions are
the two explicitly approved protected-IOMMU lifetime/compatibility commits;
the remaining 21 exceptions target commits below the protected-series base.

The tagged tree was also built from a fresh isolated output with
``configs/config-vm-6.18``. The ``vmlinux`` build completed in 499.54 seconds
with SHA-256::

  52aefbf7a054e58c088c04817bf48f109b89cdbdb08272728513613bf38a3aa9

The Ubuntu VM booted with eight vCPUs and 16 GiB on Q35 with split irqchip and
the emulated Intel IOMMU. SSH checks reported ``6.18.0+``, ``nproc=8``, taint
zero, ``/dev/kvm`` present, and six IOMMU groups. Serial and privileged dmesg
confirmed CPU0 through CPU7 in guest mode, protected DMAR initialization, and
the pKVM hypervisor running. Scans found no panic, BUG, Oops, WARNING, call
trace, PV-IPI, KASAN, UBSAN, or general-protection signature. Guest poweroff
completed cleanly and QEMU exited normally.

Phase 1 acceptance criteria
===========================

Phase 1 is complete. The final audit verified:

* no unexplained ``SQUASHME`` commit remains;
* every in-series ``Fixes:`` commit is folded or listed as an approved
  standalone exception;
* ``git range-diff`` accounts for every original patch;
* the final tree is identical to ``pkvm-v6.18-phase1-original``;
* ``git diff --check`` is clean;
* the BusyBox configuration builds and boots; and
* the Ubuntu VM passes the selected pKVM and IOMMU smoke tests.

Folding all 115 candidates would have produced a theoretical minimum of 652
commits. The final 656 consists of that minimum plus two approved in-series
exceptions and two documentation commits; retaining truthful, bisectable
boundaries is preferable to manufacturing misleading parent commits.

Phase 2 progress
================

Phase 2 reorders the Phase 1 series without changing its final source.  The
trial replay uses the following boundaries above ``61caa6bdaf42``:

============  =============================================================
Positions     Content
============  =============================================================
1--502        Deprivilege, pvVMCS, pvMMU, and common glue
503--510      pVM guest-side support
511--562      Non-IOMMU pVM and core follow-up fixes
563--654      Initial pvIOMMU implementation and all pvIOMMU follow-ups
655--659      Phase 1, nested-test, and Phase 2 documentation
============  =============================================================

The eight guest-side pVM commits originally at Phase 1 positions 563--570
replayed immediately after the common glue without conflict.  The following
52 non-IOMMU commits include protected-APIC support, VMX state isolation,
host and guest EPT synchronization, hypervisor diagnostics, direct-map
hardening, guest-range validation, and other fixes that belong to the first
four logical parts rather than pvIOMMU.

The final 92 source commits contain both the original 60-commit pvIOMMU block
and its later IOMMU-specific follow-ups.  Generic-looking helpers stay in
this block when their first consumer is protected VT-d; for example, the
share-RO memcache extension changes protected-IOMMU domain refill and cannot
truthfully precede pvIOMMU.

Dependency-aware conflict resolutions
-------------------------------------

Four conflicts exposed real ordering dependencies:

* host-EPT synchronization was first replayed without the not-yet-present
  IOMMU flush, then the pvIOMMU commit inserted ``pkvm_iommu_pt_flush()``
  before the remote-vCPU acknowledgement;
* direct-map unmapping was first replayed without the not-yet-present IOMMU
  initializer, then pvIOMMU restored ``pkvm_host_init_iommu()`` before the
  direct mapping is made non-present;
* early guest-range validation retained its expanded share/unshare API while
  the later pvIOMMU DMA commit added the independent DMA pinning API; and
* the pvIOMMU host-EPT flush commit retained both IOMMU invalidation and the
  already-moved remote-vCPU wait, in that order.

These resolutions reproduce the final Phase 1 source exactly.  The reordered
non-documentation boundary ``13e7c0519f94`` has tree::

  8e5e8f1864ffaceef23a17082797798fd63a3f81

That is the same tree as Phase 1 non-documentation boundary
``6a1b9596467c``.  Both ranges contain 654 source commits.

Phase 2 boundary validation
---------------------------

The dedicated test agent validated each newly created dependency boundary
from a fresh ``config-bb`` output:

================  ============  ===========
Boundary          Commit        Build time
================  ============  ===========
pVM guest block   19812a98172e  70.3 s
core-fix block    4317b89369c0  69.9 s
initial pvIOMMU   f24feaa41465  70.5 s
final source      13e7c0519f94  70.6 s
================  ============  ===========

Their respective ``bzImage`` SHA-256 values were::

  4f742608ff30685c9080aaa23a5a018327ac385c1dddebd54ade5e281dd70d47
  ba0b684e69fa3c0367331d5e6e01ba45a9e53e54e4b6033ca367cbe7f25faeec
  c970c751064fc0ab2a5f2201e24ddedd9b8ae130f6b0578c358f6e2a6a718d22
  1c9b6f61fed20bb11603fcf45d17603d93af2a37f985e20539c26bc66b9b0366

The first two boundaries intentionally precede pvIOMMU.  They booted four
CPUs under pKVM, reached BusyBox, stayed untainted, showed no fatal signature,
and shut down cleanly; protected DMAR was correctly absent.  The two
post-pvIOMMU boundaries additionally initialized protected DMAR.  Their
builds and boots passed the same taint, fatal-signature, and shutdown gates.

Phase 2 nested acceptance
-------------------------

The final nested matrix passed at reordered HEAD ``64c388f79aac``.  A fresh
L1 kernel was built from ``configs/config-vm-6.18`` in 299.0 seconds.  Its
input and resulting configuration had the same SHA-256, and its ``vmlinux``
SHA-256 was::

  edb20b5108ae2849a4ade84d2e913f10588bfa187a430a40b7ff9f41a02bc014

The L1 topology was the development host running a Q35/split-irqchip VM,
which in turn ran each L2 VMM.  All eight L1 CPUs entered pKVM guest mode,
protected DMAR initialized, the pKVM hypervisor started, and the kernel
remained untainted with no fatal dmesg signature.

The guest ``bzImage`` was reused only after verifying the exact Phase 1 and
Phase 2 non-documentation source-tree equivalence.  Its SHA-256, checked on
both the development host and the L1 VM, was::

  3e1c1f64e03609d8950903a12f40089846310ff79b1974e3953e8cb1d885af9f

The three required L2 cases passed:

* ordinary crosvm npVM booted Linux 6.18, mounted its ext4 root, and reached
  ``basic.target``;
* crosvm with ``--protected-vm-without-firmware`` detected pKVM, mounted its
  ext4 root, and reached ``basic.target``; and
* protected QEMU pVM used only the artifacts and ``runpvm.sh`` inside the L1
  VM.  The VM-local command selected ``pkvm-microvm`` and ``pkvm-guest``;
  the L2 detected pKVM, mounted its ext4 root, and reached ``basic.target``.

No L2 showed a fatal kernel signature.  The protected-QEMU run produced no
protected-memory access denial or new fatal L1 dmesg message.  Every VMM was
stopped after reaching its acceptance marker, private test media were
removed, the L1 powered off cleanly, and its host port was released.

Phase 2 acceptance criteria
---------------------------

Phase 2 is complete.  The final audit verified:

* all 654 source commits are present, with no left-only or right-only source
  commit in the Phase 1-to-Phase 2 range comparison;
* the reordered non-documentation tree is byte-identical to the final Phase
  1 non-documentation tree;
* ``git diff --check`` is clean and no ``SQUASHME``, ``fixup!``, or
  ``amend!`` subject remains;
* every new logical boundary builds and boots with its phase-appropriate
  pKVM and protected-DMAR expectations; and
* the complete nested npVM and pVM acceptance matrix passes.

The series is therefore ready for the Phase 3 port to Linux 7.1.
