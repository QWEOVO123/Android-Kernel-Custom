package io.qweovo.manager;

import static org.junit.Assert.*;

import java.util.*;
import org.junit.Test;

public final class TaskParserTest {
  static final String HEADER =
      "tid\ttgid\tppid\tuid\tstate\tpolicy\tnice\trt_priority\tcpu\tstart_ns\tgroup_start_ns"
          + "\tkernel\tcomm\targv0\n";

  static String row(
      int tid, int tgid, String start, String group, int nice, String comm, String argv) {
    return tid
        + "\t"
        + tgid
        + "\t1\t10001\tS\t0\t"
        + nice
        + "\t0\t2\t"
        + start
        + "\t"
        + group
        + "\t0\t"
        + comm
        + "\t"
        + argv
        + "\n";
  }

  @Test
  public void preservesLargeMarkersAndEmptyLastColumn() {
    List<TaskRecord> tasks =
        TaskParser.parse(
            HEADER + row(10, 10, "18446744073709551615", "18446744073709551615", 0, "app", ""));
    assertEquals(1, tasks.size());
    assertEquals("18446744073709551615", tasks.get(0).startNs);
    assertEquals("", tasks.get(0).argv0);
  }

  @Test
  public void octalEscapesDecodeExactlyOnce() {
    assertEquals("a\tb\\011", TaskParser.decode("a\\011b\\134011"));
    assertEquals("你好", TaskParser.decode("你好"));
  }

  @Test
  public void groupsThreadsAndMixedPriority() {
    List<ProcessEntry> groups =
        TaskParser.group(
            TaskParser.parse(
                HEADER
                    + row(11, 10, "101", "100", 5, "worker", "app")
                    + row(10, 10, "100", "100", -5, "app", "app")));
    assertEquals(1, groups.size());
    ProcessEntry group = groups.get(0);
    assertEquals(10, group.main.tid);
    assertEquals(2, group.threads.size());
    assertEquals(-5, group.minNice);
    assertEquals(5, group.maxNice);
  }

  @Test
  public void pidReuseDoesNotMergeGroups() {
    List<ProcessEntry> groups =
        TaskParser.group(
            TaskParser.parse(
                HEADER
                    + row(10, 10, "100", "100", 0, "old", "old")
                    + row(10, 10, "200", "200", 0, "new", "new")));
    assertEquals(2, groups.size());
    assertNotEquals(groups.get(0).key, groups.get(1).key);
  }

  @Test(expected = IllegalStateException.class)
  public void stalePidClickIsNeverRebound() {
    TaskParser.verifyTarget(
        TaskParser.parse(HEADER + row(10, 10, "200", "200", 0, "new", "new")), true, 10, "100");
  }

  @Test(expected = IllegalStateException.class)
  public void groupCannotTargetWorker() {
    TaskParser.verifyTarget(
        TaskParser.parse(HEADER + row(11, 10, "101", "100", 0, "worker", "app")), true, 11, "100");
  }

  @Test(expected = IllegalArgumentException.class)
  public void missingHeaderIsNotSilentlyAccepted() {
    TaskParser.parse("tid\ttgid\n1\t1\n");
  }

  @Test
  public void malformedRowCannotBecomeControlTarget() {
    assertTrue(
        TaskParser.parse(HEADER + "bad\trow\n" + row(10, 10, "nan", "100", 0, "bad", "bad"))
            .isEmpty());
  }

  @Test
  public void missingLeaderRemainsReadOnlyInGroupMode() {
    ProcessEntry entry =
        TaskParser.group(TaskParser.parse(HEADER + row(11, 10, "101", "100", 0, "worker", "app")))
            .get(0);
    assertNull(entry.main);
    assertEquals("100", entry.startNs);
  }
}
