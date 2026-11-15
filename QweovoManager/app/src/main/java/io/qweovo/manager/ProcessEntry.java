package io.qweovo.manager;

import java.util.*;

public final class ProcessEntry {
  public final int pid;
  public final String startNs, key;
  public final List<TaskRecord> threads;
  public final TaskRecord main;
  public final int minNice, maxNice;
  public String label, packageName = "";
  public boolean lockSupported;
  public Integer lockedNice;

  public ProcessEntry(List<TaskRecord> members) {
    ArrayList<TaskRecord> sorted = new ArrayList<>(members);
    sorted.sort(Comparator.comparingInt(t -> t.tid));
    threads = Collections.unmodifiableList(sorted);
    TaskRecord first = sorted.get(0), leader = null;
    for (TaskRecord task : sorted)
      if (task.tid == task.tgid && task.startNs.equals(task.groupStartNs)) leader = task;
    main = leader;
    pid = first.tgid;
    startNs = first.groupStartNs;
    key = pid + ":" + startNs;
    int low = 19, high = -20;
    for (TaskRecord task : sorted) {
      low = Math.min(low, task.nice);
      high = Math.max(high, task.nice);
    }
    minNice = low;
    maxNice = high;
    label = displayTask().argv0.isEmpty() ? displayTask().comm : displayTask().argv0;
  }

  public TaskRecord displayTask() {
    return main != null ? main : threads.get(0);
  }

  public String niceLabel() {
    String actual = minNice == maxNice ? "nice " + minNice : "nice " + minNice + "…" + maxNice;
    return lockedNice == null ? actual : AppText.get(R.string.msg_locked) + lockedNice + "\n" + actual;
  }

  public boolean changedPriority() {
    return minNice != 0 || maxNice != 0;
  }

  public boolean sameDisplay(ProcessEntry other) {
    return key.equals(other.key)
        && label.equals(other.label)
        && niceLabel().equals(other.niceLabel())
        && threads.size() == other.threads.size()
        && displayTask().state.equals(other.displayTask().state)
        && displayTask().argv0.equals(other.displayTask().argv0);
  }
}
