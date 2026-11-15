package io.qweovo.authenticator;

import android.security.keystore.KeyInfo;
import android.security.keystore.KeyProperties;
import android.security.keystore.KeyProtection;
import io.qweovo.auth.SukisuProtocol;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.math.BigInteger;
import java.security.GeneralSecurityException;
import java.security.KeyFactory;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.PrivateKey;
import java.security.PublicKey;
import java.security.Signature;
import java.security.interfaces.RSAPublicKey;
import java.util.Arrays;
import java.util.Enumeration;

final class AuthorizationKey {
  private static final String PROVIDER = "AndroidKeyStore";
  private static final String ALIAS = "meow-sukisu-authorization";
  private static final int MAX_IMPORT_SIZE = 32 * 1024;
  static final int AUTH_TYPES = KeyProperties.AUTH_BIOMETRIC_STRONG
      | KeyProperties.AUTH_DEVICE_CREDENTIAL;

  private AuthorizationKey() {}

  private static KeyStore androidStore() throws Exception {
    KeyStore store = KeyStore.getInstance(PROVIDER);
    store.load(null);
    return store;
  }

  static boolean isPresent() throws Exception {
    return androidStore().containsAlias(ALIAS);
  }

  static String keyId() throws Exception {
    return fingerprint(androidStore().getCertificate(ALIAS).getPublicKey());
  }

  static void importKey(InputStream input, char[] password) throws Exception {
    KeyStore destination = androidStore();
    if (destination.containsAlias(ALIAS)) throw new GeneralSecurityException();
    byte[] encrypted = new byte[MAX_IMPORT_SIZE + 1];
    try {
      int length = 0;
      int read;
      while (length < encrypted.length &&
          (read = input.read(encrypted, length, encrypted.length - length)) > 0)
        length += read;
      if (length == 0 || length > MAX_IMPORT_SIZE) throw new GeneralSecurityException();
      KeyStore source = KeyStore.getInstance("PKCS12");
      source.load(new ByteArrayInputStream(encrypted, 0, length), password);
      Enumeration<String> aliases = source.aliases();
      String alias = aliases.hasMoreElements() ? aliases.nextElement() : null;
      if (alias == null || aliases.hasMoreElements() || !source.isKeyEntry(alias))
        throw new GeneralSecurityException();
      KeyStore.PasswordProtection protection = new KeyStore.PasswordProtection(password);
      KeyStore.PrivateKeyEntry entry;
      try {
        entry = (KeyStore.PrivateKeyEntry) source.getEntry(alias, protection);
      } finally {
        protection.destroy();
      }
      fingerprint(entry.getCertificate().getPublicKey());
      KeyProtection policy = new KeyProtection.Builder(KeyProperties.PURPOSE_SIGN)
          .setDigests(KeyProperties.DIGEST_SHA256)
          .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1)
          .setUserAuthenticationRequired(true)
          .setUserAuthenticationParameters(0, AUTH_TYPES)
          .build();
      destination.setEntry(ALIAS, entry, policy);
      boolean accepted = false;
      try {
        PrivateKey key = (PrivateKey) destination.getKey(ALIAS, null);
        KeyInfo info = KeyFactory.getInstance("RSA", PROVIDER).getKeySpec(key, KeyInfo.class);
        int level = info.getSecurityLevel();
        if ((level != KeyProperties.SECURITY_LEVEL_TRUSTED_ENVIRONMENT
            && level != KeyProperties.SECURITY_LEVEL_STRONGBOX)
            || !info.isUserAuthenticationRequirementEnforcedBySecureHardware())
          throw new HardwareKeyException();
        accepted = true;
      } finally {
        if (!accepted) destination.deleteEntry(ALIAS);
      }
    } finally {
      Arrays.fill(encrypted, (byte) 0);
    }
  }

  static Signature prepareSignature(String challenge) throws Exception {
    KeyStore store = androidStore();
    if (!SukisuProtocol.keyId(challenge).equals(
        fingerprint(store.getCertificate(ALIAS).getPublicKey())))
      throw new WrongKeyException();
    Signature operation = Signature.getInstance("SHA256withRSA");
    operation.initSign((PrivateKey) store.getKey(ALIAS, null));
    return operation;
  }

  private static String fingerprint(PublicKey publicKey) throws Exception {
    if (!(publicKey instanceof RSAPublicKey rsa) || rsa.getModulus().bitLength() != 2048
        || !rsa.getPublicExponent().equals(BigInteger.valueOf(65537)))
      throw new GeneralSecurityException();
    ByteArrayOutputStream integers = new ByteArrayOutputStream();
    appendDer(integers, 2, rsa.getModulus().toByteArray());
    appendDer(integers, 2, rsa.getPublicExponent().toByteArray());
    ByteArrayOutputStream sequence = new ByteArrayOutputStream();
    appendDer(sequence, 0x30, integers.toByteArray());
    return SukisuProtocol.hex(MessageDigest.getInstance("SHA-256").digest(sequence.toByteArray()));
  }

  private static void appendDer(ByteArrayOutputStream out, int tag, byte[] value) {
    out.write(tag);
    int length = value.length;
    if (length < 128) {
      out.write(length);
    } else {
      int bytes = length <= 255 ? 1 : 2;
      out.write(0x80 | bytes);
      if (bytes == 2) out.write(length >>> 8);
      out.write(length & 0xff);
    }
    out.write(value, 0, length);
  }

  static final class HardwareKeyException extends GeneralSecurityException {}
  static final class WrongKeyException extends GeneralSecurityException {}
}
