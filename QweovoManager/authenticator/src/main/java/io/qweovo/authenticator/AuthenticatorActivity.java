package io.qweovo.authenticator;

import android.hardware.biometrics.BiometricManager;
import android.hardware.biometrics.BiometricPrompt;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.text.Editable;
import android.text.InputType;
import android.view.View;
import android.view.WindowManager;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.textfield.TextInputEditText;
import com.google.mlkit.common.MlKitException;
import com.google.mlkit.vision.barcode.common.Barcode;
import com.google.mlkit.vision.codescanner.GmsBarcodeScannerOptions;
import com.google.mlkit.vision.codescanner.GmsBarcodeScanning;
import com.journeyapps.barcodescanner.ScanContract;
import com.journeyapps.barcodescanner.ScanOptions;
import io.qweovo.auth.SukisuProtocol;
import java.io.InputStream;
import java.security.Signature;
import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class AuthenticatorActivity extends AppCompatActivity {
  private final ExecutorService io = Executors.newSingleThreadExecutor();
  private final ActivityResultLauncher<String[]> filePicker =
      registerForActivityResult(new ActivityResultContracts.OpenDocument(), uri -> {
        if (uri != null) askPassword(uri);
      });
  private final ActivityResultLauncher<ScanOptions> fallbackScanner =
      registerForActivityResult(new ScanContract(), result -> {
        String value = result.getContents();
        if (value != null) requestSignature(value);
      });
  private LinearLayout content;
  private MaterialButton importButton, scanButton;
  private TextView status;
  private CancellationSignal authentication;

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
    WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
    ScrollView page = new ScrollView(this);
    ViewCompat.setOnApplyWindowInsetsListener(page, (view, insets) -> {
      Insets bars = insets.getInsets(WindowInsetsCompat.Type.systemBars());
      view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
      return insets;
    });
    content = new LinearLayout(this);
    content.setOrientation(LinearLayout.VERTICAL);
    content.setPadding(dp(24), dp(28), dp(24), dp(24));
    page.addView(content);
    setContentView(page);

    TextView title = new TextView(this);
    title.setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_HeadlineMedium);
    title.setText(R.string.app_name);
    add(title, 14);
    status = new TextView(this);
    status.setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodyLarge);
    add(status, 24);
    importButton = new MaterialButton(this);
    importButton.setText(R.string.import_key);
    importButton.setOnClickListener(view -> filePicker.launch(new String[] {"*/*"}));
    add(importButton, 8);
    scanButton = new MaterialButton(this);
    scanButton.setText(R.string.scan_challenge);
    scanButton.setOnClickListener(view -> scanChallenge());
    add(scanButton, 0);
    refreshKey();
  }

  private int dp(int value) {
    return Math.round(value * getResources().getDisplayMetrics().density);
  }

  private void add(View view, int space) {
    LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
    params.bottomMargin = dp(space);
    content.addView(view, params);
  }

  private void refreshKey() {
    importButton.setEnabled(false);
    scanButton.setEnabled(false);
    io.execute(() -> {
      try {
        String id = AuthorizationKey.isPresent() ? AuthorizationKey.keyId() : null;
        whenActive(() -> {
          importButton.setVisibility(id == null ? View.VISIBLE : View.GONE);
          importButton.setEnabled(id == null);
          scanButton.setEnabled(id != null);
          status.setText(id == null ? getString(R.string.no_key) : getString(R.string.key_ready, id));
        });
      } catch (Exception failure) {
        whenActive(() -> status.setText(R.string.key_unavailable));
      }
    });
  }

  private void askPassword(Uri uri) {
    TextInputEditText input = new TextInputEditText(this);
    input.setSingleLine();
    input.setHint(R.string.import_password);
    input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
    new MaterialAlertDialogBuilder(this)
        .setTitle(R.string.import_key)
        .setView(input)
        .setNegativeButton(android.R.string.cancel, null)
        .setPositiveButton(R.string.import_confirm, (dialog, which) -> {
          Editable text = input.getText();
          char[] password = new char[text == null ? 0 : text.length()];
          if (text != null) {
            text.getChars(0, text.length(), password, 0);
            text.clear();
          }
          importFrom(uri, password);
        })
        .setOnDismissListener(dialog -> {
          if (input.getText() != null) input.getText().clear();
        }).show();
  }

  private void importFrom(Uri uri, char[] password) {
    importButton.setEnabled(false);
    io.execute(() -> {
      try (InputStream stream = getContentResolver().openInputStream(uri)) {
        if (stream == null) throw new java.io.IOException();
        AuthorizationKey.importKey(stream, password);
        whenActive(this::refreshKey);
      } catch (AuthorizationKey.HardwareKeyException failure) {
        whenActive(() -> importFailed(R.string.hardware_required));
      } catch (Exception failure) {
        whenActive(() -> importFailed(R.string.import_failed));
      } finally {
        Arrays.fill(password, '\0');
      }
    });
  }

  private void importFailed(int message) {
    importButton.setEnabled(true);
    showError(message);
  }

  private void scanChallenge() {
    scanButton.setEnabled(false);
    GmsBarcodeScannerOptions options = new GmsBarcodeScannerOptions.Builder()
        .setBarcodeFormats(Barcode.FORMAT_QR_CODE)
        .enableAutoZoom()
        .build();
    GmsBarcodeScanning.getClient(this, options).startScan().addOnCompleteListener(result ->
        whenActive(() -> {
          scanButton.setEnabled(true);
          if (result.isCanceled()) return;
          if (result.isSuccessful()) {
            requestSignature(result.getResult().getRawValue());
          } else {
            if (result.getException() instanceof MlKitException failure
                && (failure.getErrorCode() == MlKitException.CODE_SCANNER_CANCELLED
                    || failure.getErrorCode() == MlKitException.CANCELLED)) return;
            fallbackScanner.launch(SukisuProtocol.scan(getString(R.string.scan_challenge)));
          }
        }));
  }

  private void requestSignature(String code) {
    if (!SukisuProtocol.isChallenge(code)) {
      showError(R.string.invalid_challenge);
      return;
    }
    scanButton.setEnabled(false);
    io.execute(() -> {
      try {
        Signature operation = AuthorizationKey.prepareSignature(code);
        whenActive(() -> authenticate(code, operation));
      } catch (AuthorizationKey.WrongKeyException failure) {
        whenActive(() -> signFailed(R.string.wrong_key));
      } catch (Exception failure) {
        whenActive(() -> signFailed(R.string.sign_failed));
      }
    });
  }

  private void authenticate(String code, Signature operation) {
    authentication = new CancellationSignal();
    try {
      new BiometricPrompt.Builder(this)
          .setTitle(getString(R.string.approve_title))
          .setDescription(getString(R.string.approve_description,
              SukisuProtocol.keyId(code).substring(0, 12)))
          .setAllowedAuthenticators(BiometricManager.Authenticators.BIOMETRIC_STRONG
              | BiometricManager.Authenticators.DEVICE_CREDENTIAL)
          .build().authenticate(new BiometricPrompt.CryptoObject(operation),
              authentication, getMainExecutor(), new BiometricPrompt.AuthenticationCallback() {
                @Override
                public void onAuthenticationSucceeded(BiometricPrompt.AuthenticationResult result) {
                  authentication = null;
                  if (isDestroyed() || isFinishing()) return;
                  try {
                    Signature approved = result.getCryptoObject().getSignature();
                    approved.update(SukisuProtocol.message(code));
                    byte[] signature = approved.sign();
                    if (signature.length != 256) throw new java.security.SignatureException();
                    showResponse(SukisuProtocol.RESPONSE + SukisuProtocol.hex(signature));
                    scanButton.setEnabled(true);
                  } catch (Exception failure) {
                    signFailed(R.string.sign_failed);
                  }
                }

                @Override
                public void onAuthenticationError(int code, CharSequence message) {
                  authentication = null;
                  if (isDestroyed() || isFinishing()) return;
                  scanButton.setEnabled(true);
                  if (code != BiometricPrompt.BIOMETRIC_ERROR_CANCELED
                      && code != BiometricPrompt.BIOMETRIC_ERROR_USER_CANCELED)
                    new MaterialAlertDialogBuilder(AuthenticatorActivity.this)
                        .setMessage(message).setPositiveButton(android.R.string.ok, null).show();
                }
              });
    } catch (RuntimeException failure) {
      authentication = null;
      signFailed(R.string.sign_failed);
    }
  }

  private void showResponse(String response) throws Exception {
    ImageView qr = new ImageView(this);
    qr.setAdjustViewBounds(true);
    qr.setImageBitmap(SukisuProtocol.qr(response));
    qr.setContentDescription(getString(R.string.response_title));
    new MaterialAlertDialogBuilder(this).setTitle(R.string.response_title)
        .setView(qr).setPositiveButton(android.R.string.ok, null).show();
  }

  private void signFailed(int message) {
    scanButton.setEnabled(true);
    showError(message);
  }

  private void showError(int message) {
    new MaterialAlertDialogBuilder(this).setMessage(message)
        .setPositiveButton(android.R.string.ok, null).show();
  }

  private void whenActive(Runnable callback) {
    runOnUiThread(() -> {
      if (!isFinishing() && !isDestroyed()) callback.run();
    });
  }

  @Override
  protected void onDestroy() {
    if (authentication != null) authentication.cancel();
    io.shutdown();
    super.onDestroy();
  }
}
