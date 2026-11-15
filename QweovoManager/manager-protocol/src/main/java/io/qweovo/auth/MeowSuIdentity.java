package io.qweovo.auth;

import android.content.pm.PackageInfo;
import android.content.pm.Signature;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

/** Canonical boot grant identity obtained from the trusted PackageManager. */
public final class MeowSuIdentity {
  public static final String SOCKET = "/dev/.meow-manager/su";
  public static final int MAGIC = 0x4d535531;
  public static final int COMMAND_LIMIT = 16384;
  private static final char[] HEX = "0123456789abcdef".toCharArray();

  private MeowSuIdentity() {}

  public static boolean appUid(int uid) {
    int id = uid % 100000;
    return id >= 10000 && id <= 19999;
  }

  public static String grant(PackageInfo info) {
    if (info == null || info.applicationInfo == null || !appUid(info.applicationInfo.uid)
        || info.sharedUserId != null || info.signingInfo == null) return null;
    Signature[] signers = info.signingInfo.getApkContentsSigners();
    if (signers == null || signers.length != 1) return null;
    MessageDigest digest = sha256();
    digest.update((info.applicationInfo.uid + "\n" + info.packageName + "\n"
        + info.firstInstallTime + "\n").getBytes(StandardCharsets.UTF_8));
    byte[] bytes = digest.digest(signers[0].toByteArray());
    char[] hex = new char[bytes.length * 2];
    for (int i = 0; i < bytes.length; i++) {
      hex[i * 2] = HEX[(bytes[i] & 0xff) >>> 4];
      hex[i * 2 + 1] = HEX[bytes[i] & 15];
    }
    return info.applicationInfo.uid + " " + new String(hex);
  }

  static MessageDigest sha256() {
    try {
      return MessageDigest.getInstance("SHA-256");
    } catch (java.security.NoSuchAlgorithmException impossible) {
      throw new AssertionError(impossible);
    }
  }
}
