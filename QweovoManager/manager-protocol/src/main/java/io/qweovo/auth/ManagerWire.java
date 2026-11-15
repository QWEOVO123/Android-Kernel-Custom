package io.qweovo.auth;

/** Wire format for a single request on the broker's local socket. */
public final class ManagerWire {
  public static final String SOCKET = "/dev/.meow-manager/socket/control";
  public static final String MARKER = "/proc/qweovo_manager_api";
  public static final int MAGIC = 0x4d4d4731;
  public static final int READ = 1, WRITE = 2, UPDATE_PRIORITY = 3;
  public static final int UID = 5998;

  public enum Node {
    INFO("/proc/qweovo_info", 65536, 0),
    CPU("/proc/cpu_ctl", 65536, 4095),
    TTL("/proc/ttl_ctl", 128, 32),
    TASKS("/proc/process_info", 16 * 1024 * 1024, 0),
    CONTROL("/proc/process_ctl", 1024, 128),
    PANIC("/proc/panic_ctl", 0, 128),
    LOCKS("/proc/priority_locks", 65536, 128),
    SUKISU("/proc/qweovo_sukisu_next", 256, 540),
    MEOW_SU("/proc/qweovo_meow_su", 32768, 512),
    SECURITY_AUDIT("/proc/qweovo_security_audit", 16384, 6),
    BOOT_SETTINGS("/proc/qweovo_boot_settings", 256, 128),
    TEE_STATUS("/proc/qweovo_tee_status", 256, 0);

    public final String path;
    public final int readLimit;
    public final int writeLimit;

    Node(String path, int readLimit, int writeLimit) {
      this.path = path;
      this.readLimit = readLimit;
      this.writeLimit = writeLimit;
    }

    public static Node forPath(String path) {
      for (Node node : values()) if (node.path.equals(path)) return node;
      throw new IllegalArgumentException("Unknown Manager operation");
    }
  }

  private ManagerWire() {}
}
