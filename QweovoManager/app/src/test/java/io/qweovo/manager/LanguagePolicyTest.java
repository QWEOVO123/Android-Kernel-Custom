package io.qweovo.manager;

import static org.junit.Assert.assertEquals;
import java.util.Locale;
import org.junit.Test;

public final class LanguagePolicyTest {
  @Test public void chineseVariantsUseChinese() {
    for (String tag : new String[] {"zh-CN", "zh-TW", "zh-HK", "zh-Hant"})
      assertEquals(Locale.SIMPLIFIED_CHINESE, LanguagePolicy.supported(Locale.forLanguageTag(tag)));
  }

  @Test public void englishAndOtherLanguagesUseEnglish() {
    for (String tag : new String[] {"en-US", "en-GB", "fr-FR", "ja-JP", "ar", "de-DE"})
      assertEquals(Locale.ENGLISH, LanguagePolicy.supported(Locale.forLanguageTag(tag)));
    assertEquals(Locale.ENGLISH, LanguagePolicy.supported(null));
  }
}
