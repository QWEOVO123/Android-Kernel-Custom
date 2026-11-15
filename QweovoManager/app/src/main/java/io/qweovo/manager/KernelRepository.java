package io.qweovo.manager;

import android.content.pm.PackageManager;
import android.content.pm.PackageInfo;
import android.system.*;
import io.qweovo.auth.ManagerWire;
import io.qweovo.auth.MeowSuIdentity;
import io.qweovo.auth.ManagerWire.Node;
import io.qweovo.auth.SukisuProtocol;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.regex.*;
import org.json.*;

public final class KernelRepository {
  static final String INFO = "/proc/qweovo_info",
      CPU = "/proc/cpu_ctl",
      TTL = "/proc/ttl_ctl",
      TASKS = "/proc/process_info",
      CONTROL = "/proc/process_ctl",
      PANIC = "/proc/panic_ctl",
      LOCKS = "/proc/priority_locks",
      SUKISU_NEXT = "/proc/qweovo_sukisu_next";
  private final PackageManager packages;
  private final Map<String, String[]> labels = new ConcurrentHashMap<>();

  public KernelRepository(PackageManager packages) {
    this.packages = packages;
  }

  public static final class Identity {
    public final JSONObject raw, capabilities, configuration;
    public final String release;

    Identity(JSONObject json) throws JSONException {
      raw = json;
      capabilities = json.getJSONObject("capabilities");
      configuration = json.getJSONObject("panic_configuration");
      release = json.getString("kernel_release");
    }

    public boolean has(String key) {
      return capabilities.optBoolean(key, false);
    }

    public boolean oops() {
      return configuration.optJSONObject("oops_continue").optBoolean("value", false);
    }

    public String action() {
      return configuration.optJSONObject("panic_action").optString("value", "reboot");
    }

    public boolean dumpReady() {
      return configuration.optBoolean("ramdump_ready", false);
    }
  }

  public Identity identity() throws Exception {
    JSONObject json = new JSONObject(readNode(INFO, 65536));
    if (json.optInt("schema_version") != 1
        || !"qweovo.secret_kernel".equals(json.optString("kernel_id")))
      throw new IOException(AppText.get(R.string.msg_this_kernel_does_not_support_api_1));
    if (json.optBoolean("requires_root", true)) throw new IOException(AppText.get(R.string.msg_this_interface_requires_root_and_is_not));
    JSONObject interfaces = json.getJSONObject("interfaces");
    for (String[] path :
        new String[][] {
          {"kernel_info", INFO},
          {"cpu_control", CPU},
          {"ttl_control", TTL},
          {"process_info", TASKS},
          {"process_control", CONTROL},
          {"panic_control", PANIC}
        })
      if (!path[1].equals(interfaces.optString(path[0])))
        throw new IOException(AppText.get(R.string.msg_kernel_interface_paths_do_not_match_api));
    if (json.getJSONObject("capabilities").optBoolean("priority_uid_lock", false)) {
      JSONObject config = json.optJSONObject("priority_lock_configuration");
      if (!LOCKS.equals(interfaces.optString("priority_locks"))
          || config == null
          || config.optInt("api_version") != 1
          || !"uid".equals(config.optString("scope"))) throw new IOException(AppText.get(R.string.msg_the_uid_lock_interface_is_incompatible));
    }
    if ((json.getJSONObject("capabilities").optBoolean("sukisu_next_boot", false)
        || json.getJSONObject("capabilities").optBoolean("sukisu_signed_boot", false))
        && !SUKISU_NEXT.equals(interfaces.optString("sukisu_next_boot")))
      throw new IOException(AppText.get(R.string.msg_kernel_interface_paths_do_not_match_api));
    return new Identity(json);
  }

  private void require(String capability) throws Exception {
    if (!identity().has(capability)) throw new IOException(AppText.get(R.string.msg_this_feature_is_disabled_in_the_current));
  }

  public List<ProcessEntry> processes() throws Exception {
    require("process_info");
    List<ProcessEntry> entries = TaskParser.group(TaskParser.parse(readNode(TASKS, 16 * 1024 * 1024)));
    boolean lockSupported = identity().has("priority_uid_lock");
    Map<Long, UidLockRule> rules = lockSupported ? locks() : Collections.emptyMap();
    for (ProcessEntry entry : entries) {
      TaskRecord main = entry.displayTask();
      entry.lockSupported = lockSupported && applicationUid(main.uid);
      UidLockRule rule = rules.get(main.uid);
      entry.lockedNice = rule == null ? null : rule.nice;
      String[] metadata =
          labels.computeIfAbsent(
              main.uid + "|" + main.argv0,
              key -> {
                try {
                  String base = main.argv0.split(":", 2)[0];
                  boolean packageLike = base.matches("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z0-9_]+)+");
                  if (main.kernel || (main.uid < 10000 && !packageLike))
                    return new String[] {"", ""};
                  String[] names = packages.getPackagesForUid((int) main.uid);
                  if (names != null && names.length > 0) {
                    String name = null;
                    for (String candidate : names)
                      if (candidate.equals(main.argv0) || main.argv0.startsWith(candidate + ":")) {
                        name = candidate;
                        break;
                      }
                    if (name == null) {
                      if (names.length == 1 && main.uid >= 10000) name = names[0];
                      else return new String[] {"", ""};
                    }
                    return new String[] {
                      packages.getApplicationLabel(packages.getApplicationInfo(name, 0)).toString(),
                      name
                    };
                  }
                } catch (Exception ignored) {
                }
                return new String[] {"", ""};
              });
      entry.packageName = metadata[1];
      if (!metadata[0].isEmpty()) entry.label = metadata[0];
      else if (main.kernel) entry.label = main.comm;
      else {
        String value = main.argv0.isEmpty() ? main.comm : main.argv0;
        entry.label = value.contains("/") ? value.substring(value.lastIndexOf('/') + 1) : value;
      }
    }
    return entries;
  }

  public static boolean applicationUid(long uid) {
    return uid >= 0 && uid <= 0xffffffffL && uid % 100000 >= 10000 && uid % 100000 <= 19999;
  }

  public Map<Long, UidLockRule> locks() throws Exception {
    require("priority_uid_lock");
    Map<Long, UidLockRule> rules = new TreeMap<>();
    String[] lines = readNode(LOCKS, 65536).split("\n");
    if (lines.length == 0 || !lines[0].equals("uid\tnice\tupdated_ns"))
      throw new IOException(AppText.get(R.string.msg_the_lock_list_format_is_incompatible));
    for (int i = 1; i < lines.length; i++) {
      String[] fields = lines[i].split("\t", -1);
      if (fields.length != 3) throw new IOException(AppText.get(R.string.msg_invalid_lock_rule_format));
      long uid = Long.parseLong(fields[0]);
      int nice = Integer.parseInt(fields[1]);
      if (!applicationUid(uid) || nice < -20 || nice > 19 || !fields[2].matches("[0-9]+"))
        throw new IOException(AppText.get(R.string.msg_invalid_lock_rule_parameters));
      rules.put(uid, new UidLockRule(uid, nice, fields[2]));
    }
    return rules;
  }

  public void lockUid(long uid, int nice) throws Exception {
    require("priority_uid_lock");
    if (!applicationUid(uid) || nice < -20 || nice > 19)
      throw new IllegalArgumentException(AppText.get(R.string.msg_invalid_uid_or_nice_value));
    write(LOCKS, "lock " + uid + " " + nice, 128);
    UidLockRule actual = locks().get(uid);
    if (actual == null || actual.nice != nice) throw new IOException(AppText.get(R.string.msg_the_lock_rule_changed_refresh_the_state));
  }

  public void unlockUid(long uid) throws Exception {
    require("priority_uid_lock");
    if (!applicationUid(uid)) throw new IllegalArgumentException(AppText.get(R.string.msg_invalid_application_uid));
    write(LOCKS, "unlock " + uid, 128);
    if (locks().containsKey(uid)) throw new IOException(AppText.get(R.string.msg_the_rule_was_set_again_refresh_the));
  }

  public String uidLabel(long uid) {
    ArrayList<String> names = new ArrayList<>();
    String[] packageNames = packages.getPackagesForUid((int) uid);
    if (packageNames != null)
      for (String name : packageNames) {
        try {
          names.add(packages.getApplicationLabel(packages.getApplicationInfo(name, 0)).toString());
        } catch (Exception ignored) {
          names.add(name);
        }
      }
    return names.isEmpty() ? AppText.get(R.string.msg_app_uid) + uid : android.text.TextUtils.join(" / ", names);
  }

  public static final class CpuState {
    public final Set<Integer> present, online, active, hotpluggable;

    CpuState(Map<String, String> values) {
      present = cpuList(values.get("present"));
      online = cpuList(values.get("online"));
      active = cpuList(values.get("active"));
      hotpluggable = cpuList(values.get("hotpluggable"));
    }
  }

  public CpuState cpus() throws Exception {
    require("cpu_control");
    Map<String, String> fields = new HashMap<>();
    for (String line : readNode(CPU, 16384).split("\n")) {
      int delimiter = line.indexOf(':');
      if (delimiter > 0)
        fields.put(line.substring(0, delimiter), line.substring(delimiter + 1).trim());
    }
    if (!fields.keySet().containsAll(Arrays.asList("present", "online", "active", "hotpluggable")))
      throw new IOException(AppText.get(R.string.msg_cpu_state_format_mismatch));
    return new CpuState(fields);
  }

  static Set<Integer> cpuList(String value) {
    Set<Integer> result = new TreeSet<>();
    if (value == null || value.isEmpty()) return result;
    for (String part : value.split(",")) {
      String[] range = part.split("-");
      int first = Integer.parseInt(range[0]),
          last = range.length == 2 ? Integer.parseInt(range[1]) : first;
      if (first < 0 || last > 4095 || last < first)
        throw new IllegalArgumentException(AppText.get(R.string.msg_invalid_cpu_number_range));
      for (int i = first; i <= last; i++) result.add(i);
    }
    return result;
  }

  public int ttl() throws Exception {
    require("ttl_control");
    int value = Integer.parseInt(readNode(TTL, 128).trim());
    if (value != 0 && value != 1) throw new IOException(AppText.get(R.string.msg_invalid_ttl_state));
    return value;
  }

  public void cpu(int cpu, boolean online) throws Exception {
    require("cpu_control");
    if (cpu < 0) throw new IllegalArgumentException();
    write(CPU, (online ? "online " : "offline ") + cpu, 4095);
  }

  public void ttl(boolean preserve) throws Exception {
    require("ttl_control");
    write(TTL, preserve ? "1" : "0", 32);
  }

  public boolean sukisuNextBoot() throws Exception {
    require("sukisu_next_boot");
    String value = readNode(SUKISU_NEXT, 4).trim();
    if ("1".equals(value)) return true;
    if ("0".equals(value)) return false;
    throw new IOException(AppText.get(R.string.sukisu_next_invalid_state));
  }

  public void setSukisuNextBoot(boolean allowed) throws Exception {
    require("sukisu_next_boot");
    write(SUKISU_NEXT, allowed ? "1" : "0", 2);
    if (sukisuNextBoot() != allowed)
      throw new IOException(AppText.get(R.string.sukisu_next_changed));
  }

  public static final class SukisuChallenge {
    public final String code;
    public final boolean armed;

    SukisuChallenge(String code, boolean armed) {
      this.code = code;
      this.armed = armed;
    }
  }

  public SukisuChallenge sukisuChallenge() throws Exception {
    require("sukisu_signed_boot");
    String[] lines = readNode(SUKISU_NEXT, 256).trim().split("\n");
    if (lines.length == 2 && lines[0].startsWith("challenge=")) {
      String code = lines[0].substring("challenge=".length());
      if (SukisuProtocol.isChallenge(code) && ("armed=0".equals(lines[1]) || "armed=1".equals(lines[1])))
        return new SukisuChallenge(code, "armed=1".equals(lines[1]));
    }
    throw new IOException(AppText.get(R.string.sukisu_next_invalid_state));
  }

  public void submitSukisuSignature(String response) throws Exception {
    require("sukisu_signed_boot");
    if (!SukisuProtocol.isResponse(response))
      throw new IOException(AppText.get(R.string.sukisu_invalid_signature));
    try {
      write(SUKISU_NEXT, response, 540);
    } catch (ErrnoException failure) {
      // EKEYREJECTED is 129 in the Linux UAPI used by this kernel.
      if (failure.errno == 129)
        throw new IOException(AppText.get(R.string.sukisu_invalid_signature), failure);
      throw failure;
    }
    if (!sukisuChallenge().armed)
      throw new IOException(AppText.get(R.string.sukisu_next_changed));
  }

  record MeowSuApp(String name, String label, boolean allowed) {}

  List<MeowSuApp> meowSuApps() throws Exception {
    require("meow_su");
    Set<String> grants = new HashSet<>(Arrays.asList(
        readNode(Node.MEOW_SU.path, Node.MEOW_SU.readLimit).split("\n")));
    List<MeowSuApp> apps = new ArrayList<>();
    for (PackageInfo info : packages.getInstalledPackages(PackageManager.GET_SIGNING_CERTIFICATES)) {
      String grant = MeowSuIdentity.grant(info);
      if (grant == null) continue;
      String label = packages.getApplicationLabel(info.applicationInfo).toString();
      apps.add(new MeowSuApp(info.packageName, label, grants.contains(grant)));
    }
    apps.sort(Comparator.comparing(MeowSuApp::label, String.CASE_INSENSITIVE_ORDER)
        .thenComparing(MeowSuApp::name));
    return apps;
  }

  boolean setMeowSuAllowed(String name, boolean allowed) throws Exception {
    require("meow_su");
    write(Node.MEOW_SU.path, (allowed ? "1 " : "0 ") + name, Node.MEOW_SU.writeLimit);
    String grant = MeowSuIdentity.grant(
        packages.getPackageInfo(name, PackageManager.GET_SIGNING_CERTIFICATES));
    return grant != null && Arrays.asList(
        readNode(Node.MEOW_SU.path, Node.MEOW_SU.readLimit).split("\n")).contains(grant);
  }

  public Identity panic(boolean attempt, String action) throws Exception {
    Identity identity = identity();
    if (!identity.has("panic_control")) throw new IOException(AppText.get(R.string.msg_the_fault_policy_interface_is_disabled));
    if (!"reboot".equals(action) && !"ramdump".equals(action))
      throw new IllegalArgumentException(AppText.get(R.string.msg_unsupported_fault_action));
    if ("ramdump".equals(action) && !identity.dumpReady()) throw new IOException(AppText.get(R.string.msg_ramdump_is_not_ready));
    write(PANIC, "oops_continue=" + (attempt ? 1 : 0) + " panic_action=" + action, 128);
    return identity();
  }

  public static final class NiceResult {
    public final int updated, error;
    public final String status;

    NiceResult(int updated, int error, String status) {
      this.updated = updated;
      this.error = error;
      this.status = status;
    }

    public boolean success() {
      return error == 0 && updated > 0;
    }
  }

  public NiceResult nice(boolean process, int id, String marker, int nice) throws Exception {
    require("process_control");
    if (nice < -20 || nice > 19 || id <= 0 || !marker.matches("[1-9][0-9]{0,19}"))
      throw new IllegalArgumentException(AppText.get(R.string.msg_invalid_priority_parameters));
    // A stale click is never rebound to a newer task with the same ID.
    TaskParser.verifyTarget(TaskParser.parse(readNode(TASKS, 16 * 1024 * 1024)), process, id, marker);
    String command = (process ? "process " : "thread ") + id + " " + marker + " " + nice;
    if (ManagerClient.availableOnKernel()) {
      ManagerClient.Reply reply = ManagerClient.request(ManagerWire.UPDATE_PRIORITY, Node.CONTROL,
          ManagerClient.command(command, Node.CONTROL.writeLimit), Node.CONTROL.readLimit);
      return priorityResult(new String(reply.data(), StandardCharsets.US_ASCII).trim(), reply.errno());
    }
    FileDescriptor fd = openNode(CONTROL, OsConstants.O_RDWR);
    int writeError = 0;
    String status = "";
    try {
      try {
        writeOnce(fd, command, 128);
      } catch (ErrnoException failure) {
        writeError = failure.errno;
      }
      int readError = 0;
      try {
        Os.lseek(fd, 0, OsConstants.SEEK_SET);
        status = new String(readAll(fd, 1024), StandardCharsets.US_ASCII).trim();
      } catch (ErrnoException failure) {
        readError = failure.errno;
      }
      return priorityResult(status, writeError != 0 ? writeError : readError);
    } finally {
      Os.close(fd);
    }
  }

  private static NiceResult priorityResult(String status, int errno) {
    Matcher result = Pattern.compile("updated=(\\d+) error=(-?\\d+)").matcher(status);
    if (!result.matches()) return new NiceResult(0, errno != 0 ? errno : OsConstants.EIO, status);
    int updated = Integer.parseInt(result.group(1));
    int kernelError = -Integer.parseInt(result.group(2));
    return new NiceResult(updated, errno != 0 ? errno : kernelError, status);
  }

  private static String readNode(String path, int limit) throws Exception {
    if (!ManagerClient.availableOnKernel()) return read(path, limit);
    ManagerClient.Reply reply = ManagerClient.request(ManagerWire.READ, Node.forPath(path), new byte[0], limit);
    if (reply.errno() != 0) throw new ErrnoException("Manager read", reply.errno());
    return new String(reply.data(), StandardCharsets.UTF_8);
  }

  static Map<String, String> securityStatus() throws Exception {
    Map<String, String> fields = new HashMap<>();
    for (String line : read("/proc/qweovo_security_status", 1024).split("\n")) {
      int separator = line.indexOf('=');
      if (separator > 0) fields.put(line.substring(0, separator), line.substring(separator + 1));
    }
    return fields;
  }

  String securityAudit() throws Exception {
    return readNode(Node.SECURITY_AUDIT.path, Node.SECURITY_AUDIT.readLimit);
  }

  void clearSecurityAudit() throws Exception {
    write(Node.SECURITY_AUDIT.path, "clear", Node.SECURITY_AUDIT.writeLimit);
  }

  static Map<String, String> keyValues(String text) {
    Map<String, String> fields = new HashMap<>();
    for (String line : text.split("\n")) {
      int separator = line.indexOf('=');
      if (separator > 0) fields.put(line.substring(0, separator), line.substring(separator + 1));
    }
    return fields;
  }

  static BootSettings bootSettings() throws Exception {
    Map<String, String> fields =
        keyValues(readNode(Node.BOOT_SETTINGS.path, Node.BOOT_SETTINGS.readLimit));
    return new BootSettings(
        parseInt(fields.get("target"), 0), parseInt(fields.get("timeout"), 60));
  }

  static void setBootTarget(String target) throws Exception {
    write(Node.BOOT_SETTINGS.path, "target=" + target, Node.BOOT_SETTINGS.writeLimit);
  }

  static void setMenuTimeout(int seconds) throws Exception {
    write(Node.BOOT_SETTINGS.path, "timeout=" + seconds, Node.BOOT_SETTINGS.writeLimit);
  }

  static String teeStatus() throws Exception {
    return readNode(Node.TEE_STATUS.path, Node.TEE_STATUS.readLimit);
  }

  private static int parseInt(String value, int fallback) {
    try {
      return value == null ? fallback : Integer.parseInt(value.trim());
    } catch (NumberFormatException failure) {
      return fallback;
    }
  }

  static final class BootSettings {
    final int target;
    final int timeout;

    BootSettings(int target, int timeout) {
      this.target = target;
      this.timeout = timeout;
    }
  }

  static String read(String path, int limit) throws Exception {
    FileDescriptor fd = openNode(path, OsConstants.O_RDONLY);
    try {
      return new String(readAll(fd, limit), StandardCharsets.UTF_8);
    } finally {
      Os.close(fd);
    }
  }

  private static byte[] readAll(FileDescriptor fd, int limit) throws Exception {
    ByteArrayOutputStream output = new ByteArrayOutputStream();
    byte[] buffer = new byte[16384];
    int count;
    while ((count = Os.read(fd, buffer, 0, buffer.length)) > 0) {
      if (Thread.currentThread().isInterrupted()) throw new InterruptedIOException();
      if (output.size() + count > limit) throw new IOException(AppText.get(R.string.msg_the_interface_response_is_too_large));
      output.write(buffer, 0, count);
    }
    return output.toByteArray();
  }

  private static void write(String path, String command, int limit) throws Exception {
    if (ManagerClient.availableOnKernel()) {
      ManagerClient.Reply reply = ManagerClient.request(ManagerWire.WRITE, Node.forPath(path),
          ManagerClient.command(command, limit), 0);
      if (reply.errno() != 0) {
        android.util.Log.w("MeowWrite", path + " errno=" + reply.errno());
        throw new ErrnoException("Manager write", reply.errno());
      }
      return;
    }
    FileDescriptor fd = openNode(path, OsConstants.O_WRONLY | OsConstants.O_TRUNC);
    try {
      writeOnce(fd, command, limit);
    } finally {
      Os.close(fd);
    }
  }

  private static void writeOnce(FileDescriptor fd, String command, int limit) throws Exception {
    byte[] data = (command + "\n").getBytes(StandardCharsets.US_ASCII);
    if (data.length > limit) throw new IOException(AppText.get(R.string.msg_the_command_is_too_long));
    if (Os.write(fd, data, 0, data.length) != data.length) throw new IOException(AppText.get(R.string.msg_the_kernel_did_not_receive_the_complete));
  }

  private static FileDescriptor openNode(String path, int flags) throws Exception {
    return Os.open(path, flags | OsConstants.O_CLOEXEC, 0);
  }

  public static String error(Throwable error) {
    if (error instanceof ErrnoException nativeError) return error(nativeError.errno);
    return error.getMessage() == null ? AppText.get(R.string.msg_operation_failed_refresh_the_state_and_try) : error.getMessage();
  }

  public static String error(int errno) {
    if (errno == OsConstants.ESTALE) return AppText.get(R.string.msg_the_target_changed_refresh_and_select_it);
    if (errno == OsConstants.ESRCH) return AppText.get(R.string.msg_the_process_or_thread_has_exited);
    if (errno == OsConstants.EOPNOTSUPP) return AppText.get(R.string.msg_the_target_or_scheduling_policy_does_not);
    if (errno == OsConstants.EBUSY) return AppText.get(R.string.msg_the_cpu_is_busy_or_at_least);
    if (errno == OsConstants.EPERM || errno == OsConstants.EACCES) return AppText.get(R.string.msg_the_system_denied_this_operation_actual_state);
    if (errno == OsConstants.EAGAIN) return AppText.get(R.string.msg_another_policy_changed_the_priority_check_the);
    if (errno == OsConstants.E2BIG) return AppText.get(R.string.msg_the_thread_count_or_command_length_exceeds);
    return AppText.get(R.string.msg_operation_failed_errno) + errno + AppText.get(R.string.msg_check_the_actual_state);
  }
}
