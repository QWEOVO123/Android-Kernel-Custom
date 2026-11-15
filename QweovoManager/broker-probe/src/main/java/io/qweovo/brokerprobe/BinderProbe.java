package io.qweovo.brokerprobe;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.system.OsConstants;
import io.qweovo.auth.ManagerWire;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;

public abstract class BinderProbe extends Instrumentation {
  private boolean socketMode;
  private boolean clientMode;
  protected abstract boolean allowed();

  public static final class Manager extends BinderProbe {
    @Override protected boolean allowed() { return true; }
  }

  public static final class OtherApp extends BinderProbe {
    @Override protected boolean allowed() { return false; }
  }

  @Override
  public void onCreate(Bundle arguments) {
    super.onCreate(arguments);
    socketMode = arguments != null && "socket".equals(arguments.getString("transport"));
    clientMode = arguments != null && "client".equals(arguments.getString("transport"));
    start();
  }

  @Override
  public void onStart() {
    Bundle report = new Bundle();
    Context context = getTargetContext();
    report.putInt("caller_uid", Process.myUid());
    report.putString("caller_package", context.getPackageName());
    if (clientMode) {
      boolean passed = false;
      try {
        ClassLoader loader = context.getClassLoader();
        Class<?> client = loader.loadClass("io.qweovo.manager.ManagerClient");
        Class<?> node = loader.loadClass("io.qweovo.auth.ManagerWire$Node");
        var request = client.getDeclaredMethod("request", int.class, node, byte[].class, int.class);
        request.setAccessible(true);
        int[] indices = {0, 1, 2, 3, 6, 7};
        for (int attempt = 0; attempt < 3; attempt++) {
          for (int index : indices) {
            Object reply = request.invoke(null, 1, node.getEnumConstants()[index], new byte[0], 16 * 1024 * 1024);
            var errno = reply.getClass().getDeclaredMethod("errno");
            errno.setAccessible(true);
            if ((int) errno.invoke(reply) != 0) throw new IllegalStateException("Node " + index + " errno " + errno.invoke(reply));
          }
        }
        report.putInt("reads", 3 * indices.length);
        passed = true;
      } catch (Exception failure) {
        report.putString("error", android.util.Log.getStackTraceString(failure));
      }
      finish(passed ? Activity.RESULT_OK : Activity.RESULT_CANCELED, report);
      return;
    }
    if (socketMode) {
      boolean passed = probeSocket(report);
      finish(passed ? Activity.RESULT_OK : Activity.RESULT_CANCELED, report);
      return;
    }
    ArrayBlockingQueue<IBinder> connected = new ArrayBlockingQueue<>(1);
    ServiceConnection connection = new ServiceConnection() {
      @Override public void onServiceConnected(ComponentName name, IBinder service) {
        connected.offer(service);
      }
      @Override public void onServiceDisconnected(ComponentName name) {}
    };
    boolean bound = false;
    boolean passed = false;
    try {
      Intent intent = new Intent().setComponent(
          new ComponentName(ReadOnlyBroker.PACKAGE, ReadOnlyBroker.DESCRIPTOR));
      bound = context.bindService(intent, connection, Context.BIND_AUTO_CREATE);
      if (!bound) throw new IllegalStateException("Cannot bind read-only endpoint");
      IBinder remote = connected.poll(10, TimeUnit.SECONDS);
      if (remote == null) throw new IllegalStateException("No Binder handle");
      Parcel request = Parcel.obtain();
      Parcel reply = Parcel.obtain();
      try {
        request.writeInterfaceToken(ReadOnlyBroker.DESCRIPTOR);
        if (!remote.transact(ReadOnlyBroker.STATUS, request, reply, 0))
          throw new IllegalStateException("Unknown read-only transaction");
        try {
          reply.readException();
        } catch (SecurityException denied) {
          report.putString("result", "denied");
          finish(allowed() ? Activity.RESULT_CANCELED : Activity.RESULT_OK, report);
          return;
        }
        int brokerUid = reply.readInt();
        String status = reply.readString();
        report.putInt("broker_uid", brokerUid);
        report.putString("result", "allowed");
        report.putBoolean("read_boot_status", status != null && status.contains("schema_version=1\n"));
        passed = allowed() && brokerUid >= 10000 && brokerUid != Process.myUid()
            && report.getBoolean("read_boot_status");
      } finally {
        reply.recycle();
        request.recycle();
      }
    } catch (Exception failure) {
      report.putString("error", failure.toString());
    } finally {
      if (bound) context.unbindService(connection);
    }
    finish(passed ? Activity.RESULT_OK : Activity.RESULT_CANCELED, report);
  }

  private boolean probeSocket(Bundle report) {
    try (LocalSocket socket = new LocalSocket()) {
      socket.connect(new LocalSocketAddress(ManagerWire.SOCKET, LocalSocketAddress.Namespace.FILESYSTEM));
      socket.setSoTimeout(5000);
      int serverUid = socket.getPeerCredentials().getUid();
      report.putInt("broker_uid", serverUid);
      if (serverUid != ManagerWire.UID) return false;
      DataInputStream in = new DataInputStream(socket.getInputStream());
      int authorization = in.readInt();
      if (authorization == OsConstants.EACCES) {
        report.putString("result", "denied");
        return !allowed();
      }
      if (authorization == 0) {
        DataOutputStream out = new DataOutputStream(socket.getOutputStream());
        out.writeInt(ManagerWire.MAGIC);
        out.writeByte(ManagerWire.READ);
        out.writeByte(ManagerWire.Node.INFO.ordinal());
        out.writeInt(0);
        out.flush();
        int errno = in.readInt();
        int length = in.readInt();
        if (errno != 0 || length <= 0 || length > ManagerWire.Node.INFO.readLimit) return false;
        String data = new String(in.readNBytes(length), StandardCharsets.UTF_8);
        report.putString("result", "allowed");
        return allowed() && data.contains("qweovo.secret_kernel");
      }
      return false;
    } catch (Exception failure) {
      report.putString("error", failure.toString());
      return false;
    }
  }
}
