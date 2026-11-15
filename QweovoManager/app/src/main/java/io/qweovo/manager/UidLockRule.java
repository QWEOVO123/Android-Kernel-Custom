package io.qweovo.manager;

/** Kernel-owned rule; remains present even when no process for the UID is running. */
public final class UidLockRule {
  public final long uid;
  public final int nice;
  public final String updatedNs;

  UidLockRule(long uid, int nice, String updatedNs) {
    this.uid = uid;
    this.nice = nice;
    this.updatedNs = updatedNs;
  }
}
