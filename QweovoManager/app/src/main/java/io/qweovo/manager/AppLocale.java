package io.qweovo.manager;

import android.content.Context;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.os.LocaleList;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Locale;

final class AppLocale {
  private static boolean previewLoaded;
  private static Locale preview;
  private AppLocale() {}

  static String systemUiLanguage() {
    LocaleList system = Resources.getSystem().getConfiguration().getLocales();
    return LanguagePolicy.supported(system.isEmpty() ? Locale.ENGLISH : system.get(0)).getLanguage();
  }

  static Context wrap(Context context) {
    LocaleList system = Resources.getSystem().getConfiguration().getLocales();
    Locale desired = LanguagePolicy.supported(system.isEmpty() ? Locale.ENGLISH : system.get(0));
    // QA selects the debug process locale before Application/Activity resources
    // are accessed. Release builds never read this private test file.
    if (BuildConfig.DEBUG) {
      synchronized (AppLocale.class) {
        if (!previewLoaded) {
          previewLoaded = true;
          File file = new File(context.getFilesDir(), "preview-locale.txt");
          if (file.isFile() && file.length() <= 32) {
            try {
              String tag = new String(Files.readAllBytes(file.toPath()), StandardCharsets.US_ASCII).trim();
              preview = LanguagePolicy.supported(Locale.forLanguageTag(tag));
            } catch (Exception ignored) { }
          }
        }
      }
      if (preview != null) desired = preview;
    }
    return wrap(context, desired);
  }

  static Context wrap(Context context, Locale desired) {
    Configuration config = context.getResources().getConfiguration();
    if (config.getLocales().size() == 1 && desired.equals(config.getLocales().get(0)))
      return context;
    Configuration localized = new Configuration(config);
    localized.setLocales(new LocaleList(desired));
    localized.setLayoutDirection(desired);
    return context.createConfigurationContext(localized);
  }
}
