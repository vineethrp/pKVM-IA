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
* ``c913267ce3ed`` -- X2APIC/Kconfig dependencies;
* ``4a104c296a80`` -- page-walker validation;
* ``83a3d6dc91db`` -- BSP validation;
* ``52a30bd43c58`` -- APIC bus-cycle validation;
* ``09cc0f5b25ff`` -- port-I/O register sharing;
* ``15624f908e09`` -- MSR read/write register sharing;
* ``f91523f5426f`` -- domain lifetime and cache-tag references;
* ``86e20dec1171`` -- TE/QIE disable prevention; and
* ``c1a901671fde`` -- host-vCPU and pCPU indexing.

The following are expected to remain standalone unless inspection proves a
clean and buildable split. They are coherent refactors or cross-cutting
hardening rather than corrections local to one parent:

* ``51fc55efab4c`` -- overflow validation across MMU ownership APIs;
* ``5f2189ad2c9e`` -- consolidated postponed per-VM setup;
* ``1dee9bc8c79e`` -- generic IRQ-chip and maximum-vCPU setup;
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

Final acceptance
================

Phase 1 is complete only when:

* no unexplained ``SQUASHME`` commit remains;
* every in-series ``Fixes:`` commit is folded or listed as an approved
  standalone exception;
* ``git range-diff`` accounts for every original patch;
* the final tree is identical to ``pkvm-v6.18-phase1-original``;
* ``git diff --check`` is clean;
* the BusyBox configuration builds and boots; and
* the Ubuntu VM passes the selected pKVM and IOMMU smoke tests.

Folding all 115 candidates would produce a theoretical minimum of 652
commits. The expected result is larger because retaining coherent
cross-cutting fixes is preferable to manufacturing misleading parent commits.
