package io.qweovo.brokerprobe;

import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;

/** Temporary read-only endpoint for exercising caller checks on the tablet. */
public final class ReadOnlyBroker extends Service {
  static final String PACKAGE = "io.qweovo.brokerprobe";
  static final String DESCRIPTOR = "io.qweovo.brokerprobe.ReadOnlyBroker";
  static final int STATUS = IBinder.FIRST_CALL_TRANSACTION;
  private static final String MANAGER = "io.qweovo.manager";
  private static final byte[] MANAGER_CERT = HexFormat.of().parseHex(
      BuildConfig.MANAGER_CERTIFICATE_SHA256);

  private final Binder endpoint = new Binder() {
    @Override
    protected boolean onTransact(int code, Parcel request, Parcel reply, int flags)
        throws android.os.RemoteException {
      if (code != STATUS) return super.onTransact(code, request, reply, flags);
      request.enforceInterface(DESCRIPTOR);
      request.enforceNoDataAvail();
      verifyManager(Binder.getCallingUid());
      String status;
      try (FileInputStream file = new FileInputStream("/proc/qweovo_boot_status")) {
        status = new String(file.readNBytes(513), StandardCharsets.US_ASCII);
      } catch (IOException failure) {
        throw new IllegalStateException("Cannot read boot status", failure);
      }
      if (status.length() > 512) throw new IllegalStateException("Unexpected status size");
      reply.writeNoException();
      reply.writeInt(Process.myUid());
      reply.writeString(status);
      return true;
    }
  };

  @Override
  public IBinder onBind(Intent intent) {
    return endpoint;
  }

  private void verifyManager(int uid) {
    if (uid < 10000) throw new SecurityException("Manager required");
    try {
      PackageInfo info = getPackageManager().getPackageInfo(
          MANAGER, PackageManager.PackageInfoFlags.of(PackageManager.GET_SIGNING_CERTIFICATES));
      if (info.applicationInfo == null || info.applicationInfo.uid != uid || info.sharedUserId != null
          || info.signingInfo == null) throw new SecurityException("Manager required");
      Signature[] signers = info.signingInfo.getApkContentsSigners();
      if (signers != null && signers.length == 1 && MessageDigest.isEqual(
          MANAGER_CERT, MessageDigest.getInstance("SHA-256").digest(signers[0].toByteArray()))) return;
    } catch (PackageManager.NameNotFoundException | NoSuchAlgorithmException failure) {
      throw new SecurityException("Manager identity unavailable", failure);
    }
    throw new SecurityException("Manager signing certificate required");
  }
}
