package io.qweovo.manager;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import io.qweovo.auth.ManagerWire;
import io.qweovo.auth.ManagerWire.Node;
import java.io.DataInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

final class ManagerClient {
  record Reply(int errno, byte[] data) {}

  private ManagerClient() {}

  static boolean availableOnKernel() throws ErrnoException {
    try {
      Os.close(Os.open(ManagerWire.MARKER, OsConstants.O_RDONLY | OsConstants.O_CLOEXEC, 0));
      return true;
    } catch (ErrnoException failure) {
      if (failure.errno == OsConstants.ENOENT) return false;
      throw failure;
    }
  }

  static Reply request(int operation, Node node, byte[] body, int limit) throws IOException {
    try (LocalSocket socket = new LocalSocket()) {
      socket.connect(new LocalSocketAddress(ManagerWire.SOCKET, LocalSocketAddress.Namespace.FILESYSTEM));
      socket.setSoTimeout(5000);
      if (socket.getPeerCredentials().getUid() != ManagerWire.UID)
        throw new IOException("Unexpected kernel service identity");
      DataInputStream input = new DataInputStream(socket.getInputStream());
      if (input.readInt() != 0) throw new IOException("Manager authorization denied");
      byte[] request = ByteBuffer.allocate(10 + body.length)
          .putInt(ManagerWire.MAGIC)
          .put((byte) operation)
          .put((byte) node.ordinal())
          .putInt(body.length)
          .put(body)
          .array();
      socket.getOutputStream().write(request);
      int errno = input.readInt();
      int count = input.readInt();
      if (count < 0 || count > limit || errno < 0 || errno > 4095)
        throw new IOException("Invalid kernel service reply");
      byte[] data = new byte[count];
      input.readFully(data);
      return new Reply(errno, data);
    }
  }

  static byte[] command(String command, int limit) throws IOException {
    byte[] bytes = (command + "\n").getBytes(StandardCharsets.US_ASCII);
    if (bytes.length > limit) throw new IOException(AppText.get(R.string.msg_the_command_is_too_long));
    return bytes;
  }
}
