package io.qweovo.manager;

import static org.junit.Assert.*;

import io.qweovo.auth.SukisuProtocol;
import java.nio.charset.StandardCharsets;
import java.security.KeyPairGenerator;
import java.security.Signature;
import org.junit.Test;

public final class SukisuProtocolTest {
  @Test
  public void signedBytesMatchKernelRecordAndBindTheNonce() throws Exception {
    String id = "12".repeat(32);
    String first = SukisuProtocol.REQUEST + id + "fe".repeat(32);
    String next = SukisuProtocol.REQUEST + id + "ff".repeat(32);
    assertEquals(id, SukisuProtocol.keyId(first));
    assertEquals("4d454f5753553100" + id + "fe".repeat(32),
        SukisuProtocol.hex(SukisuProtocol.message(first)));
    assertEquals("MEOWSU1\0", new String(SukisuProtocol.message(first), 0, 8,
        StandardCharsets.US_ASCII));

    KeyPairGenerator generator = KeyPairGenerator.getInstance("RSA");
    generator.initialize(2048);
    var key = generator.generateKeyPair();
    Signature signature = Signature.getInstance("SHA256withRSA");
    signature.initSign(key.getPrivate());
    signature.update(SukisuProtocol.message(first));
    byte[] answer = signature.sign();
    assertTrue(SukisuProtocol.isResponse(SukisuProtocol.RESPONSE + SukisuProtocol.hex(answer)));
    signature.initVerify(key.getPublic());
    signature.update(SukisuProtocol.message(first));
    assertTrue(signature.verify(answer));
    signature.update(SukisuProtocol.message(next));
    assertFalse(signature.verify(answer));
  }

  @Test
  public void rejectsOldFlagsAndMalformedQrCodes() {
    assertFalse(SukisuProtocol.isResponse("1"));
    assertFalse(SukisuProtocol.isResponse(SukisuProtocol.RESPONSE + "00".repeat(255)));
    assertFalse(SukisuProtocol.isChallenge(SukisuProtocol.REQUEST + "00".repeat(64) + "\n"));
    assertFalse(SukisuProtocol.isChallenge(SukisuProtocol.REQUEST + "zz".repeat(64)));
  }
}
