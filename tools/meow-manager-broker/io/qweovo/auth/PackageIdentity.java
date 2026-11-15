package io.qweovo.auth;

import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.IBinder;
import java.lang.reflect.Method;

final class PackageIdentity {
  private final Method lookup;
  private final Method asInterface;
  private final Method packageInfo;
  private final Method packagesForUid;

  PackageIdentity() throws ReflectiveOperationException {
    lookup = Class.forName("android.os.ServiceManager").getMethod("checkService", String.class);
    Class<?> packages = Class.forName("android.content.pm.IPackageManager");
    asInterface = Class.forName(packages.getName() + "$Stub").getMethod("asInterface", IBinder.class);
    packageInfo = packages.getMethod("getPackageInfo", String.class, long.class, int.class);
    packagesForUid = packages.getMethod("getPackagesForUid", int.class);
  }

  private Object packageService() throws ReflectiveOperationException {
    Object binder = lookup.invoke(null, "package");
    return binder == null ? null : asInterface.invoke(null, binder);
  }

  PackageInfo lookup(String name, int user) throws ReflectiveOperationException {
    return lookup(packageService(), name, user);
  }

  PackageInfo caller(int uid) throws ReflectiveOperationException {
    if (!MeowSuIdentity.appUid(uid)) return null;
    Object service = packageService();
    if (service == null) return null;
    String[] names = (String[]) packagesForUid.invoke(service, uid);
    if (names == null || names.length != 1) return null;
    PackageInfo info = lookup(service, names[0], uid / 100000);
    return info != null && info.applicationInfo != null && info.applicationInfo.uid == uid ? info : null;
  }

  private PackageInfo lookup(Object service, String name, int user) throws ReflectiveOperationException {
    return service == null ? null : (PackageInfo) packageInfo.invoke(
        service, name, (long) PackageManager.GET_SIGNING_CERTIFICATES, user);
  }
}
