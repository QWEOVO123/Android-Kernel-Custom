package io.qweovo.manager;

public final class TaskRecord {
  public final int tid, tgid, ppid, policy, nice, rtPriority, cpu;
  public final long uid;
  public final String state, startNs, groupStartNs, comm, argv0;
  public final boolean kernel;

  public TaskRecord(
      int tid,
      int tgid,
      int ppid,
      long uid,
      String state,
      int policy,
      int nice,
      int rtPriority,
      int cpu,
      String startNs,
      String groupStartNs,
      boolean kernel,
      String comm,
      String argv0) {
    this.tid = tid;
    this.tgid = tgid;
    this.ppid = ppid;
    this.uid = uid;
    this.state = state;
    this.policy = policy;
    this.nice = nice;
    this.rtPriority = rtPriority;
    this.cpu = cpu;
    this.startNs = startNs;
    this.groupStartNs = groupStartNs;
    this.kernel = kernel;
    this.comm = comm;
    this.argv0 = argv0;
  }

  public String key() {
    return tid + ":" + startNs;
  }

  public boolean adjustable() {
    return !kernel && (policy == 0 || policy == 3) && !"0".equals(startNs);
  }

  public String policyName() {
    return switch (policy) {
      case 0 -> AppText.get(R.string.msg_normal);
      case 1 -> AppText.get(R.string.msg_fifo_real_time);
      case 2 -> AppText.get(R.string.msg_rr_real_time);
      case 3 -> AppText.get(R.string.msg_batch);
      case 5 -> AppText.get(R.string.msg_idle);
      case 6 -> "Deadline";
      default -> AppText.get(R.string.msg_scheduling_policy) + policy;
    };
  }
}
