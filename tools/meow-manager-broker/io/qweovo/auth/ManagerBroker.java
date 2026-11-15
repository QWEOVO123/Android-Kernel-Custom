package io.qweovo.auth;

import android.content.pm.PackageInfo;
import android.content.pm.Signature;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Process;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.FileDescriptor;
import java.io.IOException;
import java.security.MessageDigest;
import static io.qweovo.auth.ManagerWire.*;

public final class ManagerBroker {
  private static final String MANAGER = "io.qweovo.manager";
  private static final byte[] CERTIFICATE = ManagerCertificate.SHA256;
  private static final byte[] EMPTY = new byte[0];
  private final PackageIdentity identities;

  private ManagerBroker() throws ReflectiveOperationException {
    identities = new PackageIdentity();
  }

  public static void main(String[] args) throws Exception {
    if (Process.myUid() != UID) throw new SecurityException("Unexpected broker UID");
    ManagerBroker broker = new ManagerBroker();
    try {
      android.system.StructStat existing = Os.lstat(SOCKET);
      if (!OsConstants.S_ISSOCK(existing.st_mode) || existing.st_uid != UID)
        throw new IOException("Unexpected broker socket path");
      Os.remove(SOCKET);
    } catch (ErrnoException failure) {
      if (failure.errno != OsConstants.ENOENT) throw failure;
    }
    try (LocalSocket listener = new LocalSocket()) {
      listener.bind(new LocalSocketAddress(SOCKET, LocalSocketAddress.Namespace.FILESYSTEM));
      Os.chmod(SOCKET, 0666);
      try (LocalServerSocket server = new LocalServerSocket(listener.getFileDescriptor())) {
        Log.i("MeowManagerBroker", "Ready");
        while (true) {
          try (LocalSocket peer = server.accept()) {
            peer.setSoTimeout(2500);
            int uid = peer.getPeerCredentials().getUid();
            boolean allowed = broker.isManager(uid);
            DataOutputStream output = new DataOutputStream(peer.getOutputStream());
            output.writeInt(allowed ? 0 : OsConstants.EACCES);
            output.flush();
            if (allowed) broker.handle(peer, output, uid);
          } catch (IOException | ReflectiveOperationException ignored) {
            // Disconnects, invalid input and PackageManager restarts fail closed.
          }
        }
      }
    }
  }

  private boolean isManager(int uid) throws ReflectiveOperationException {
    int appId = uid % 100000;
    if (appId < 10000 || appId > 19999) return false;
    PackageInfo info = identities.lookup(MANAGER, uid / 100000);
    if (info == null || info.applicationInfo == null || info.applicationInfo.uid != uid
        || info.sharedUserId != null || info.signingInfo == null) return false;
    Signature[] signers = info.signingInfo.getApkContentsSigners();
    return signers != null && signers.length == 1 && MessageDigest.isEqual(
        CERTIFICATE, MeowSuIdentity.sha256().digest(signers[0].toByteArray()));
  }

  private void handle(LocalSocket peer, DataOutputStream output, int caller)
      throws IOException, ReflectiveOperationException {
    DataInputStream input = new DataInputStream(peer.getInputStream());
    if (input.readInt() != MAGIC) return;
    int operation = input.readUnsignedByte();
    int nodeIndex = input.readUnsignedByte();
    int size = input.readInt();
    Node[] nodes = Node.values();
    if (nodeIndex >= nodes.length || size < 0) return;
    Node node = nodes[nodeIndex];
    boolean read = operation == READ && size == 0 && node.readLimit != 0 && node != Node.CONTROL;
    boolean write = size > 0 && size <= node.writeLimit
        && ((operation == WRITE && node != Node.CONTROL)
            || (operation == UPDATE_PRIORITY && node == Node.CONTROL));
    if (!read && !write) return;
    byte[] command = new byte[size];
    input.readFully(command);
    if (write && !validCommand(command)) return;
    int errno = 0;
    byte[] result = EMPTY;
    try {
      if (write && node == Node.MEOW_SU) command = resolveGrant(command, caller);
      int mode = read ? OsConstants.O_RDONLY
          : operation == UPDATE_PRIORITY ? OsConstants.O_RDWR : OsConstants.O_WRONLY;
      FileDescriptor fd = Os.open(node.path, mode | OsConstants.O_CLOEXEC, 0);
      try {
        if (write) {
          try {
            if (Os.write(fd, command, 0, command.length) != command.length) errno = OsConstants.EIO;
          } catch (ErrnoException failure) {
            errno = failure.errno;
          }
        }
        if (operation == UPDATE_PRIORITY) Os.lseek(fd, 0, OsConstants.SEEK_SET);
        if (read || operation == UPDATE_PRIORITY) result = readNode(fd, node.readLimit);
      } finally {
        Os.close(fd);
      }
    } catch (ErrnoException failure) {
      if (errno == 0) errno = failure.errno;
    }
    if (node == Node.MEOW_SU && read && errno == 0) {
      StringBuilder own = new StringBuilder();
      for (String line : new String(result, java.nio.charset.StandardCharsets.US_ASCII).split("\n")) {
        int separator = line.indexOf(' ');
        if (separator > 0 && Integer.parseInt(line.substring(0, separator)) / 100000 == caller / 100000)
          own.append(line).append('\n');
      }
      result = own.toString().getBytes(java.nio.charset.StandardCharsets.US_ASCII);
    }
    output.writeInt(errno);
    output.writeInt(result.length);
    output.write(result);
    output.flush();
  }

  private byte[] resolveGrant(byte[] command, int caller)
      throws ErrnoException, ReflectiveOperationException {
    var ascii = java.nio.charset.StandardCharsets.US_ASCII;
    String value = new String(command, ascii);
    if (!value.matches("[01] [A-Za-z0-9_]+(?:\\.[A-Za-z0-9_]+)+\n"))
      throw new ErrnoException("grant", OsConstants.EINVAL);
    String name = value.substring(2, value.length() - 1);
    String grant = MeowSuIdentity.grant(identities.lookup(name, caller / 100000));
    if (grant == null) throw new ErrnoException("grant", OsConstants.EACCES);
    return (value.substring(0, 2) + grant + "\n").getBytes(ascii);
  }

  private static boolean validCommand(byte[] command) {
    if (command[command.length - 1] != '\n') return false;
    for (int i = 0; i < command.length - 1; i++)
      if (command[i] < 32 || command[i] > 126) return false;
    return true;
  }

  private static byte[] readNode(FileDescriptor fd, int limit) throws ErrnoException, IOException {
    ByteArrayOutputStream output = new ByteArrayOutputStream();
    byte[] buffer = new byte[16384];
    for (int count; (count = Os.read(fd, buffer, 0, buffer.length)) > 0;) {
      if (count > limit - output.size()) throw new ErrnoException("read", OsConstants.EOVERFLOW);
      output.write(buffer, 0, count);
    }
    return output.toByteArray();
  }
}
