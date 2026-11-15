package io.qweovo.manager;

import android.os.Bundle;
import com.journeyapps.barcodescanner.CaptureActivity;
import com.journeyapps.barcodescanner.DecoratedBarcodeView;
import com.journeyapps.barcodescanner.camera.CameraSettings;

/** Configures the fallback camera after CaptureActivity has applied the scan intent. */
public final class SukisuScanActivity extends CaptureActivity {
  private DecoratedBarcodeView scanner;

  @Override
  protected DecoratedBarcodeView initializeContent() {
    scanner = super.initializeContent();
    return scanner;
  }

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    scanner.getCameraSettings().setFocusMode(CameraSettings.FocusMode.CONTINUOUS);
  }
}
