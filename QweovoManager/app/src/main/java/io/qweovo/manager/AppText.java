package io.qweovo.manager;

import android.content.Context;
import androidx.annotation.StringRes;

/** Application resources are also available to static parser/error helpers. */
final class AppText {
  private static volatile Context application;

  private AppText() {}

  static void initialize(Context context) {
    application = context;
  }

  static String get(@StringRes int id) {
    Context context = application;
    // Pure JVM parser tests intentionally have no Android Application instance.
    if (context == null) return "Kernel operation failed";
    return AppLocale.wrap(context).getString(id);
  }
}
