package io.qweovo.auth;

import android.graphics.Bitmap;
import android.graphics.Color;
import com.google.zxing.BarcodeFormat;
import com.google.zxing.WriterException;
import com.google.zxing.common.BitMatrix;
import com.google.zxing.qrcode.QRCodeWriter;
import com.journeyapps.barcodescanner.ScanOptions;
import java.nio.charset.StandardCharsets;
import java.util.regex.Pattern;

public final class SukisuProtocol {
  public static final String REQUEST = "MEOW-SU1:";
  public static final String RESPONSE = "MEOW-SU1-SIG:";
  private static final Pattern CHALLENGE = Pattern.compile(REQUEST + "[0-9a-f]{128}");
  private static final Pattern ANSWER = Pattern.compile(RESPONSE + "[0-9a-f]{512}");
  private static final byte[] DOMAIN = "MEOWSU1\0".getBytes(StandardCharsets.US_ASCII);
  private static final char[] HEX = "0123456789abcdef".toCharArray();

  private SukisuProtocol() {}

  public static boolean isChallenge(String value) {
    return value != null && CHALLENGE.matcher(value).matches();
  }

  public static boolean isResponse(String value) {
    return value != null && ANSWER.matcher(value).matches();
  }

  public static String keyId(String challenge) {
    if (!isChallenge(challenge)) throw new IllegalArgumentException();
    return challenge.substring(REQUEST.length(), REQUEST.length() + 64);
  }

  public static byte[] message(String challenge) {
    if (!isChallenge(challenge)) throw new IllegalArgumentException();
    byte[] message = new byte[DOMAIN.length + 64];
    System.arraycopy(DOMAIN, 0, message, 0, DOMAIN.length);
    for (int i = 0; i < 64; i++) {
      int offset = REQUEST.length() + i * 2;
      message[DOMAIN.length + i] = (byte) ((Character.digit(challenge.charAt(offset), 16) << 4)
          | Character.digit(challenge.charAt(offset + 1), 16));
    }
    return message;
  }

  public static String hex(byte[] bytes) {
    char[] chars = new char[bytes.length * 2];
    for (int i = 0; i < bytes.length; i++) {
      chars[i * 2] = HEX[(bytes[i] & 0xff) >>> 4];
      chars[i * 2 + 1] = HEX[bytes[i] & 0xf];
    }
    return new String(chars);
  }

  public static ScanOptions scan(String prompt) {
    return new ScanOptions().setDesiredBarcodeFormats(ScanOptions.QR_CODE)
        .setBeepEnabled(false).setOrientationLocked(false).setPrompt(prompt);
  }

  public static Bitmap qr(String value) throws WriterException {
    BitMatrix bits = new QRCodeWriter().encode(value, BarcodeFormat.QR_CODE, 448, 448);
    int width = bits.getWidth(), height = bits.getHeight();
    int[] pixels = new int[width * height];
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        pixels[y * width + x] = bits.get(x, y) ? Color.BLACK : Color.WHITE;
      }
    }
    return Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888);
  }
}
