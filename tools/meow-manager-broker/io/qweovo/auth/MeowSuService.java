package io.qweovo.auth;

import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Process;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.SynchronousQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

public final class MeowSuService {
  private static final String TAG = "MeowSU";
  private static final String CHECK = "/proc/qweovo_meow_su_check";
  private static final int CONCURRENT = 4;
  private final PackageIdentity identities;

  private MeowSuService() throws ReflectiveOperationException {
    identities = new PackageIdentity();
  }

  public static void main(String[] args) throws Exception {
    if (Process.myUid() != 0) throw new SecurityException("MeowSU must be started by init");
    FileDescriptor check = Os.open(CHECK, OsConstants.O_WRONLY | OsConstants.O_CLOEXEC, 0);
    Os.close(check);
    MeowSuService service = new MeowSuService();
    try {
      var existing = Os.lstat(MeowSuIdentity.SOCKET);
      if (!OsConstants.S_ISSOCK(existing.st_mode) || existing.st_uid != 0)
        throw new IOException("Unexpected MeowSU socket");
      Os.remove(MeowSuIdentity.SOCKET);
    } catch (ErrnoException failure) {
      if (failure.errno != OsConstants.ENOENT) throw failure;
    }
    ThreadPoolExecutor workers = new ThreadPoolExecutor(
        0, CONCURRENT, 30, TimeUnit.SECONDS, new SynchronousQueue<>());
    try (LocalSocket listener = new LocalSocket()) {
      listener.bind(new LocalSocketAddress(MeowSuIdentity.SOCKET, LocalSocketAddress.Namespace.FILESYSTEM));
      Os.chmod(MeowSuIdentity.SOCKET, 0666);
      try (LocalServerSocket server = new LocalServerSocket(listener.getFileDescriptor())) {
        Log.i(TAG, "Ready");
        while (true) {
          LocalSocket client = server.accept();
          try {
            workers.execute(() -> service.handle(client));
          } catch (java.util.concurrent.RejectedExecutionException busy) {
            client.close();
          }
        }
      }
    } finally {
      workers.shutdownNow();
    }
  }

  private String authorizedIdentity(int uid) throws ReflectiveOperationException {
    String grant = MeowSuIdentity.grant(identities.caller(uid));
    if (grant == null) return null;
    try {
      byte[] query = (grant + "\n").getBytes(StandardCharsets.US_ASCII);
      FileDescriptor fd = Os.open(CHECK, OsConstants.O_WRONLY | OsConstants.O_CLOEXEC, 0);
      try {
        return Os.write(fd, query, 0, query.length) == query.length ? grant : null;
      } finally {
        Os.close(fd);
      }
    } catch (ErrnoException | java.io.InterruptedIOException denied) {
      return null;
    }
  }

  private void handle(LocalSocket client) {
    java.lang.Process child = null;
    try (client) {
      client.setSoTimeout(3000);
      int uid = client.getPeerCredentials().getUid();
      DataOutputStream output = new DataOutputStream(client.getOutputStream());
      String grant = authorizedIdentity(uid);
      output.writeInt(grant == null ? OsConstants.EACCES : 0);
      if (grant == null) return;
      DataInputStream input = new DataInputStream(client.getInputStream());
      if (input.readInt() != MeowSuIdentity.MAGIC) return;
      int size = input.readInt();
      if (size < 0 || size > MeowSuIdentity.COMMAND_LIMIT) return;
      byte[] bytes = new byte[size];
      input.readFully(bytes);
      String command = new String(bytes, StandardCharsets.UTF_8);
      if (command.indexOf('\0') >= 0 || !grant.equals(authorizedIdentity(uid))) {
        output.writeInt(-1);
        output.writeInt(126);
        return;
      }
      ProcessBuilder builder = size == 0 ? new ProcessBuilder("/system/bin/sh")
          : new ProcessBuilder("/system/bin/sh", "-c", command);
      builder.directory(new java.io.File("/")).redirectErrorStream(true);
      builder.environment().clear();
      builder.environment().put("PATH", "/system/bin:/system/xbin:/vendor/bin");
      builder.environment().put("HOME", "/");
      child = builder.start();
      Log.i(TAG, "Started authorized root command for UID " + uid);
      client.setSoTimeout(0);
      OutputStream childInput = child.getOutputStream();
      Thread sender = new Thread(() -> forwardInput(input, childInput), "meow-su-stdin");
      sender.setDaemon(true);
      sender.start();
      try (var result = child.getInputStream()) {
        byte[] buffer = new byte[8192];
        for (int count; (count = result.read(buffer)) != -1;) {
          output.writeInt(count);
          output.write(buffer, 0, count);
        }
      }
      output.writeInt(-1);
      output.writeInt(child.waitFor());
      client.shutdownInput();
    } catch (IOException | ReflectiveOperationException failure) {
      // Authentication or transport failure ends this request.
    } catch (InterruptedException failure) {
      Thread.currentThread().interrupt();
    } finally {
      if (child != null && child.isAlive()) child.destroyForcibly();
    }
  }

  private static void forwardInput(InputStream source, OutputStream destination) {
    try (destination) {
      byte[] buffer = new byte[8192];
      for (int count; (count = source.read(buffer)) != -1;) {
        destination.write(buffer, 0, count);
        destination.flush();
      }
    } catch (IOException closed) {
      // The client disconnected or the root process closed stdin.
    }
  }
}
