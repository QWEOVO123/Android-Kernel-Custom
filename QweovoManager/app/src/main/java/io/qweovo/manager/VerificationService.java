package io.qweovo.manager;

import android.app.Service;
import android.content.Intent;
import android.os.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import org.json.*;

/** Debug-only controlled own-process tests; no shell, su, or third-party writes. */
public final class VerificationService extends Service {
  private final JSONArray checks = new JSONArray();

  private void check(String name, boolean passed) throws Exception {
    checks.put(new JSONObject().put("test", name).put("passed", passed));
    if (!passed) throw new IllegalStateException(name);
  }

  @Override
  public void onCreate() {
    super.onCreate();
    if (!BuildConfig.DEBUG) {
      stopSelf();
      return;
    }
    new Thread(this::verify, "api-verification").start();
  }

  private void verify() {
    JSONObject report = new JSONObject();
    KernelRepository repository = new KernelRepository(getPackageManager());
    List<TaskRecord> restore = new ArrayList<>();
    boolean success = false;
    boolean ownLockCreated = false;
    try {
      report
          .put("uid", android.os.Process.myUid())
          .put(
              "context",
              KernelRepository.read("/proc/self/attr/current", 512).replace("\0", "").trim());
      check("ordinary application UID", android.os.Process.myUid() >= 10000);
      KernelRepository.Identity identity = repository.identity();
      report.put("release", identity.release);
      check("compatible identity", identity.raw.getInt("schema_version") == 1);
      List<ProcessEntry> groups = repository.processes();
      ProcessEntry own = null;
      for (ProcessEntry entry : groups) if (entry.pid == android.os.Process.myPid()) own = entry;
      check("own process grouped", own != null && own.main != null && own.threads.size() > 1);
      restore.addAll(own.threads);
      KernelRepository.NiceResult processResult = repository.nice(true, own.pid, own.startNs, 5);
      check("same-descriptor process result", processResult.success() && processResult.updated > 1);
      List<TaskRecord> changed =
          TaskParser.parse(KernelRepository.read(KernelRepository.TASKS, 16 * 1024 * 1024));
      int found = 0;
      for (TaskRecord old : restore)
        for (TaskRecord fresh : changed)
          if (old.key().equals(fresh.key())) {
            check("group nice readback " + fresh.tid, fresh.nice == 5);
            found++;
          }
      check("group members found", found > 1);
      for (TaskRecord task : restore)
        if (task.adjustable()) repository.nice(false, task.tid, task.startNs, task.nice);
      restore.clear();
      int tid = android.os.Process.myTid();
      TaskRecord self = null;
      for (TaskRecord task :
          TaskParser.parse(KernelRepository.read(KernelRepository.TASKS, 16 * 1024 * 1024)))
        if (task.tid == tid) self = task;
      check("worker thread found", self != null);
      restore.add(self);
      check("single-thread result", repository.nice(false, tid, self.startNs, 3).success());
      TaskRecord readback =
          TaskParser.verifyTarget(
              TaskParser.parse(KernelRepository.read(KernelRepository.TASKS, 16 * 1024 * 1024)),
              false,
              tid,
              self.startNs);
      check("thread nice readback", readback.nice == 3);
      repository.nice(false, tid, self.startNs, self.nice);
      restore.clear();
      boolean stale = false;
      try {
        repository.nice(false, tid, "1", 0);
      } catch (IllegalStateException expected) {
        stale = true;
      }
      check("stale task rejected before write", stale);
      if (identity.has("ttl_control")) {
        int before = repository.ttl();
        repository.ttl(before == 1);
        check("TTL unchanged write", repository.ttl() == before);
      }
      if (identity.has("cpu_control")) {
        KernelRepository.CpuState before = repository.cpus();
        int online = before.online.iterator().next();
        repository.cpu(online, true);
        check("CPU idempotent online", repository.cpus().online.contains(online));
      }
      repository.panic(identity.oops(), identity.action());
      KernelRepository.Identity after = repository.identity();
      check(
          "panic unchanged write",
          after.oops() == identity.oops() && after.action().equals(identity.action()));
      if (identity.has("priority_uid_lock")) {
        long uid = android.os.Process.myUid();
        check("own UID initially unlocked", !repository.locks().containsKey(uid));
        for (ProcessEntry process : repository.processes())
          if (process.displayTask().uid == uid) restore.addAll(process.threads);
        ownLockCreated = true;
        repository.lockUid(uid, 5);
        check("UID rule readback", repository.locks().get(uid).nice == 5);
        int count = 0;
        for (ProcessEntry process : repository.processes())
          if (process.displayTask().uid == uid) {
            check(
                "UID rule decorates process " + process.pid,
                Integer.valueOf(5).equals(process.lockedNice));
            for (TaskRecord task : process.threads)
              if (task.adjustable()) {
                check("UID sweeps thread " + task.tid, task.nice == 5);
                count++;
              }
          }
        check("UID covers multiple process threads", count > 1);
        android.os.Process.setThreadPriority(12);
        check(
            "native Android nice cannot override lock",
            android.os.Process.getThreadPriority(android.os.Process.myTid()) == 5);
        int[] inherited = {-100, -100};
        Thread child =
            new Thread(
                () -> {
                  inherited[0] = android.os.Process.getThreadPriority(android.os.Process.myTid());
                  android.os.Process.setThreadPriority(11);
                  inherited[1] = android.os.Process.getThreadPriority(android.os.Process.myTid());
                },
                "uid-lock-probe");
        child.start();
        child.join();
        check("future thread gets locked nice", inherited[0] == 5 && inherited[1] == 5);
        repository.lockUid(uid, 7);
        check(
            "updating rule changes running worker",
            android.os.Process.getThreadPriority(android.os.Process.myTid()) == 7);
        repository.unlockUid(uid);
        ownLockCreated = false;
        check("unlock removes rule", !repository.locks().containsKey(uid));
        android.os.Process.setThreadPriority(12);
        check(
            "native Android nice works after unlock",
            android.os.Process.getThreadPriority(android.os.Process.myTid()) == 12);
      }
      success = true;
    } catch (Throwable failure) {
      try {
        report.put("error", android.util.Log.getStackTraceString(failure));
      } catch (Exception ignored) {
      }
    } finally {
      int restored = 0;
      if (ownLockCreated) {
        try {
          repository.unlockUid(android.os.Process.myUid());
        } catch (Exception cleanup) {
          success = false;
        }
      }
      for (TaskRecord task : restore)
        try {
          repository.nice(false, task.tid, task.startNs, task.nice);
          restored++;
        } catch (Exception ignored) {
        }
      try {
        report
            .put("passed", success)
            .put("checks", checks)
            .put("restored_after_failure", restored)
            .put("third_party_processes_changed", false)
            .put("deliberate_fault_triggered", false);
        try (FileOutputStream output =
            new FileOutputStream(new File(getFilesDir(), "verification.json"))) {
          output.write(report.toString(2).getBytes(StandardCharsets.UTF_8));
        }
      } catch (Exception failure) {
        android.util.Log.e("QWEOVO", "Verification report", failure);
      }
      stopSelf();
    }
  }

  @Override
  public IBinder onBind(Intent intent) {
    return null;
  }
}
