/*
 * Removed. The stackscan collector no longer uses a hazard bit or a
 * compiler-inserted stack write barrier: reclamation runs inside Alaska's
 * stop-the-world barrier, where a conservative per-thread stack scan provides a
 * perfect "present" snapshot (see contrib/alaska/runtime/rt/{init,refcount,
 * barrier,gc_bitmaps}.cpp). This file is intentionally empty and is no longer
 * part of the build.
 */
