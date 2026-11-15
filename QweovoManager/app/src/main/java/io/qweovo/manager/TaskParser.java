package io.qweovo.manager;

import java.util.*;

public final class TaskParser {
  private TaskParser() {}

  public static List<TaskRecord> parse(String text) {
    String[] lines = text.split("\n");
    if (lines.length == 0) throw new IllegalArgumentException(AppText.get(R.string.msg_the_process_table_is_empty));
    String[] header = lines[0].replace("\r", "").split("\t", -1);
    Map<String, Integer> columns = new HashMap<>();
    for (int i = 0; i < header.length; i++) columns.put(header[i], i);
    for (String required :
        new String[] {
          "tid",
          "tgid",
          "ppid",
          "uid",
          "state",
          "policy",
          "nice",
          "rt_priority",
          "cpu",
          "start_ns",
          "group_start_ns",
          "kernel",
          "comm",
          "argv0"
        })
      if (!columns.containsKey(required))
        throw new IllegalArgumentException(AppText.get(R.string.msg_process_interface_version_mismatch_missing) + required);
    ArrayList<TaskRecord> result = new ArrayList<>();
    for (int i = 1; i < lines.length; i++) {
      String line = lines[i];
      if (line.endsWith("\r")) line = line.substring(0, line.length() - 1);
      if (line.isEmpty()) continue;
      String[] f = line.split("\t", -1);
      if (f.length < header.length) continue;
      try {
        String start = value(f, columns, "start_ns"), group = value(f, columns, "group_start_ns");
        if (!start.matches("[0-9]{1,20}") || !group.matches("[0-9]{1,20}")) continue;
        result.add(
            new TaskRecord(
                number(f, columns, "tid"),
                number(f, columns, "tgid"),
                number(f, columns, "ppid"),
                Long.parseLong(value(f, columns, "uid")),
                value(f, columns, "state"),
                number(f, columns, "policy"),
                number(f, columns, "nice"),
                number(f, columns, "rt_priority"),
                number(f, columns, "cpu"),
                start,
                group,
                "1".equals(value(f, columns, "kernel")),
                decode(value(f, columns, "comm")),
                decode(value(f, columns, "argv0"))));
      } catch (NumberFormatException ignored) {
        /* A changing or malformed row never becomes a target. */
      }
    }
    return result;
  }

  private static String value(String[] row, Map<String, Integer> map, String key) {
    return row[map.get(key)];
  }

  private static int number(String[] row, Map<String, Integer> map, String key) {
    return Integer.parseInt(value(row, map, key));
  }

  public static String decode(String value) {
    StringBuilder out = new StringBuilder();
    for (int i = 0; i < value.length(); i++) {
      if (value.charAt(i) == '\\'
          && i + 3 < value.length()
          && value.substring(i + 1, i + 4).matches("[0-7]{3}")) {
        out.append((char) Integer.parseInt(value.substring(i + 1, i + 4), 8));
        i += 3;
      } else out.append(value.charAt(i));
    }
    return out.toString();
  }

  public static List<ProcessEntry> group(List<TaskRecord> tasks) {
    Map<String, List<TaskRecord>> groups = new LinkedHashMap<>();
    for (TaskRecord task : tasks)
      groups.computeIfAbsent(task.tgid + ":" + task.groupStartNs, k -> new ArrayList<>()).add(task);
    ArrayList<ProcessEntry> result = new ArrayList<>();
    for (List<TaskRecord> group : groups.values()) result.add(new ProcessEntry(group));
    return result;
  }

  public static TaskRecord verifyTarget(
      List<TaskRecord> fresh, boolean wholeProcess, int id, String marker) {
    for (TaskRecord task : fresh)
      if (task.tid == id) {
        String actual = wholeProcess ? task.groupStartNs : task.startNs;
        if (!actual.equals(marker) || wholeProcess && task.tid != task.tgid)
          throw new IllegalStateException(AppText.get(R.string.msg_the_target_process_changed_refresh_and_select));
        if (!task.adjustable()) throw new IllegalArgumentException(AppText.get(R.string.msg_this_thread_s_scheduling_policy_does_not));
        return task;
      }
    throw new IllegalStateException(AppText.get(R.string.msg_the_target_exited_refresh_the_list));
  }
}
