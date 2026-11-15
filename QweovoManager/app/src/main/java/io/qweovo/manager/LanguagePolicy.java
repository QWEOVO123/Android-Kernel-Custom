package io.qweovo.manager;

import java.util.Locale;

/** The primary system language controls the UI; unsupported languages use English. */
final class LanguagePolicy {
  private LanguagePolicy() {}

  static Locale supported(Locale primary) {
    return primary != null && "zh".equals(primary.getLanguage())
        ? Locale.SIMPLIFIED_CHINESE : Locale.ENGLISH;
  }
}
