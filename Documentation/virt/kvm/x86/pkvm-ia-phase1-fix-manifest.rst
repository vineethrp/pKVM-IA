================================
pKVM-IA Phase 1 fix manifest
================================

This is the mechanical disposition manifest for the Phase 1 rewrite.  Its
comparison point is ``pkvm-v6.18-phase1-complete``, and its
input is ``61caa6bdaf42..pkvm-v6.18-phase1-original``.  A candidate is an
original commit with a ``Fixes:`` trailer that is still a standalone change at
the checkpoint.  The SQUASHME chains and every fold recorded through
the final Phase 1 audit in ``pkvm-ia-porting-plan.rst`` are excluded.

There are 23 remaining unique candidates with 26 ``Fixes:`` target trailers.
Multiple targets are retained in the ``Targets`` field; those rows require a
split/inspection.  ``pending`` is an in-series single-target candidate.
``keep standalone`` means that every target is below the protected-series
base.

x86 core
========

No x86-core ``Fixes:`` candidates remain at this checkpoint.

pvMMU and pVM
==============

No pvMMU or pVM ``Fixes:`` candidates remain at this checkpoint.

pvIOMMU
========

No pending pVIOMMU candidate remains. The scalable-mode gate
``02726c811475`` is folded into PASID teardown, while hardware-derived ATS
state ``aca4d9407385`` and its ABI cleanup ``a2bb737778ce`` are folded into
protected cache-tag management at Checkpoint 25.

``9748cff51cae`` remains as an approved standalone exception.  Its explicit
entry-held domain-reference model requires the completed context/PASID setup
and teardown paths, so moving it to ``d2ca28a44e5d`` would cross later API and
validation boundaries.  The related free-domain race fix ``f91523f5426f`` is
folded into that lifetime owner at Checkpoint 23.

``4aac51318280`` is also an approved standalone exception. Its compatibility
check uses the explicit domain lookup introduced by the lifetime owner, but
folding it there reproducibly broke the isolated BusyBox boundary with a
PV-IPI warning and taint 512. The matching Checkpoint 23 control booted twice
with taint zero, so the failed rewrite was rolled back rather than sacrificing
bisectability.

upstream-base
=============

All 21 candidates whose targets are wholly below ``61caa6bdaf42`` remain
standalone: ``0d2c488617ee``, ``22236049f752``, ``2240c37ade9d``,
``246f859b3bf5``, ``2bc04d973235``, ``30ea24afdcdc``, ``4c01b9798331``,
``5d01357c490e``, ``68982f7ba171``, ``750384d4f6eb``, ``88fbe2f04cbe``,
``8b7e1a497a8e``, ``93c6d82b9b41``, ``a26c2a3a8967``, ``b1763bc40e94``,
``b3bb5cf51ca7``, ``d345390eb81f``, ``d42a7c74e44f``, ``d7782342e7e7``,
``e3a5c65419ff``, and ``fe57b1e182b1``.  Their original targets and touched
files are recorded in the Phase 1 inventory output and are intentionally not
rewritten into the protected series.
