package io.qweovo.manager;

import android.app.Application;
import android.content.Context;
import android.content.res.Resources;

public final class MeowApplication extends Application {
  @Override
  public Resources getResources() {
    Context base = getBaseContext();
    return base == null ? super.getResources() : AppLocale.wrap(base).getResources();
  }

  @Override
  protected void attachBaseContext(Context base) {
    super.attachBaseContext(AppLocale.wrap(base));
  }

  @Override
  public void onCreate() {
    super.onCreate();
    AppText.initialize(this);
  }
}
