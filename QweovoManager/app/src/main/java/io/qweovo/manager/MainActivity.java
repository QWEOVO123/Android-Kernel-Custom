package io.qweovo.manager;

import android.app.KeyguardManager;
import android.content.*;
import android.content.res.ColorStateList;
import android.graphics.*;
import android.graphics.drawable.*;
import android.hardware.biometrics.BiometricManager;
import android.hardware.biometrics.BiometricPrompt;
import android.os.*;
import android.text.*;
import android.util.*;
import android.view.*;
import android.view.inputmethod.InputMethodManager;
import android.widget.*;
import androidx.annotation.NonNull;
import androidx.annotation.RequiresApi;
import androidx.activity.result.ActivityResultLauncher;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.app.AppCompatDelegate;
import androidx.core.content.ContextCompat;
import androidx.core.graphics.Insets;
import androidx.core.view.*;
import androidx.recyclerview.widget.*;
import androidx.recyclerview.widget.ListAdapter;
import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.bottomnavigation.BottomNavigationView;
import com.google.android.material.bottomsheet.*;
import com.google.android.material.button.*;
import com.google.android.material.card.MaterialCardView;
import com.google.android.material.chip.*;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.navigationrail.NavigationRailView;
import com.google.android.material.progressindicator.LinearProgressIndicator;
import com.google.android.material.snackbar.Snackbar;
import com.google.android.material.textfield.*;
import com.google.mlkit.common.MlKitException;
import com.google.mlkit.vision.barcode.common.Barcode;
import com.google.mlkit.vision.codescanner.GmsBarcodeScannerOptions;
import com.google.mlkit.vision.codescanner.GmsBarcodeScanning;
import com.journeyapps.barcodescanner.ScanContract;
import com.journeyapps.barcodescanner.ScanOptions;
import io.qweovo.auth.SukisuProtocol;
import java.io.File;
import java.io.FileOutputStream;
import java.text.SimpleDateFormat;
import java.util.*;
import java.util.concurrent.*;

public final class MainActivity extends AppCompatActivity {
  private final Handler main = new Handler(Looper.getMainLooper());
  private final ExecutorService io = Executors.newSingleThreadExecutor();
  private final ExecutorService icons = Executors.newFixedThreadPool(2);
  private final LruCache<String, Bitmap> iconCache = new LruCache<>(64);
  private KernelRepository repository;
  private KernelRepository.Identity identity;
  private KernelRepository.CpuState cpuState;
  private Integer ttl;
  private Boolean sukisuNextBoot;
  private KernelRepository.SukisuChallenge sukisuChallenge;
  private boolean scanningSukisuResponse;
  private final ActivityResultLauncher<ScanOptions> sukisuResponseScanner =
      registerForActivityResult(new ScanContract(), result -> {
        scanningSukisuResponse = false;
        submitSukisuScan(result.getContents());
      });
  private CancellationSignal credentialRequest;
  private Runnable credentialAction;
  private List<ProcessEntry> processes = new ArrayList<>();
  private LinearLayout root;
  private FrameLayout page;
  private MaterialToolbar toolbar;
  private LinearProgressIndicator progress;
  private NavigationRailView rail;
  private BottomNavigationView bottom;
  private ProcessAdapter adapter;
  private SuAdapter suAdapter;
  private TextView processSummary, emptyMessage;
  private TextView suSummary, suEmpty;
  private List<KernelRepository.MeowSuApp> suApps = new ArrayList<>();
  private String suQuery = "";
  private RecyclerView recycler;
  private int destination = R.id.nav_home, filter = 0, sort = 0;
  private String query = "", compatibilityError = AppText.get(R.string.msg_checking_kernel);
  private boolean destroyed, foreground, refreshing, autoRefresh = true, detailOpen, busy;
  private long lastRead;
  private String systemLanguage;
  private final Runnable ticker =
      new Runnable() {
        public void run() {
          if (foreground && destination == R.id.nav_process && autoRefresh && !detailOpen)
            refreshProcesses();
          if (foreground) main.postDelayed(this, 3000);
        }
      };

  private int primary() {
    return color(com.google.android.material.R.attr.colorPrimary);
  }

  private int onSurface() {
    return color(com.google.android.material.R.attr.colorOnSurface);
  }

  private int muted() {
    return color(com.google.android.material.R.attr.colorOnSurfaceVariant);
  }

  private int color(int attr) {
    TypedValue value = new TypedValue();
    getTheme().resolveAttribute(attr, value, true);
    return value.resourceId != 0 ? ContextCompat.getColor(this, value.resourceId) : value.data;
  }

  private int dp(float value) {
    return (int) (value * getResources().getDisplayMetrics().density + .5f);
  }

  private LinearLayout vertical() {
    LinearLayout view = new LinearLayout(this);
    view.setOrientation(LinearLayout.VERTICAL);
    return view;
  }

  private LinearLayout horizontal() {
    LinearLayout view = new LinearLayout(this);
    view.setOrientation(LinearLayout.HORIZONTAL);
    view.setGravity(Gravity.CENTER_VERTICAL);
    return view;
  }

  private TextView text(String value, int size, boolean bold) {
    TextView view = new TextView(this);
    view.setText(value);
    view.setTextSize(size);
    view.setTextColor(onSurface());
    view.setLineSpacing(dp(3), 1);
    if (bold) view.setTypeface(null, Typeface.BOLD);
    return view;
  }

  private TextView secondary(String value) {
    TextView view = text(value, 13, false);
    view.setTextColor(muted());
    return view;
  }

  private void gap(LinearLayout parent, int height) {
    View space = new View(this);
    parent.addView(space, new LinearLayout.LayoutParams(1, dp(height)));
  }

  private void add(LinearLayout parent, View child, int bottomMargin) {
    LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
    params.bottomMargin = dp(bottomMargin);
    parent.addView(child, params);
  }

  private MaterialButton button(String title, boolean filled) {
    MaterialButton view =
        new MaterialButton(
            this,
            null,
            filled
                ? com.google.android.material.R.attr.materialButtonStyle
                : com.google.android.material.R.attr.materialButtonOutlinedStyle);
    view.setText(title);
    view.setAllCaps(false);
    view.setMinHeight(dp(48));
    view.setCornerRadius(dp(20));
    return view;
  }

  private MaterialCardView card(LinearLayout parent, LinearLayout content, boolean tonal) {
    MaterialCardView view = new MaterialCardView(this);
    view.setRadius(dp(24));
    view.setCardElevation(0);
    view.setStrokeWidth(0);
    view.setCardBackgroundColor(
        color(
            tonal
                ? com.google.android.material.R.attr.colorPrimaryContainer
                : com.google.android.material.R.attr.colorSurface));
    content.setPadding(dp(20), dp(20), dp(20), dp(20));
    view.addView(content);
    add(parent, view, 16);
    return view;
  }

  private LinearLayout scrollPage() {
    ScrollView scroll = new ScrollView(this);
    scroll.setFillViewport(true);
    scroll.setClipToPadding(false);
    LinearLayout content = vertical();
    content.setPadding(dp(24), dp(20), dp(24), dp(24));
    scroll.addView(content);
    page.addView(scroll, new FrameLayout.LayoutParams(-1, -1));
    return content;
  }

  private Chip badge(String label) {
    Chip chip = new Chip(this);
    chip.setText(label);
    chip.setCheckable(false);
    chip.setClickable(false);
    chip.setEnsureMinTouchTargetSize(false);
    chip.setTextSize(12);
    chip.setChipStrokeWidth(0);
    chip.setChipCornerRadius(dp(12));
    chip.setChipMinHeight(dp(28));
    chip.setChipBackgroundColor(
        ColorStateList.valueOf(color(com.google.android.material.R.attr.colorSecondaryContainer)));
    chip.setTextColor(color(com.google.android.material.R.attr.colorOnSecondaryContainer));
    return chip;
  }

  private void note(LinearLayout parent, String title, String description) {
    add(parent, text(title, 18, true), 8);
    add(parent, secondary(description), 0);
  }

  @Override
  protected void attachBaseContext(Context base) {
    super.attachBaseContext(AppLocale.wrap(base));
  }

  @Override
  public void onCreate(Bundle saved) {
    int mode =
        getSharedPreferences("ui", 0).getInt("theme", AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM);
    AppCompatDelegate.setDefaultNightMode(mode);
    super.onCreate(saved);
    systemLanguage = AppLocale.systemUiLanguage();
    compatibilityError = AppText.get(R.string.msg_checking_kernel);
    if (saved != null) {
      destination = saved.getInt("destination", R.id.nav_home);
      query = saved.getString("query", "");
      suQuery = saved.getString("suQuery", "");
      filter = saved.getInt("filter");
      sort = saved.getInt("sort");
      autoRefresh = saved.getBoolean("auto", true);
    }
    repository = new KernelRepository(getPackageManager());
    buildShell();
    showPage();
    loadState(true);
    if (BuildConfig.DEBUG && getIntent().getBooleanExtra("verify", false))
      startService(new Intent(this, VerificationService.class));
    if (BuildConfig.DEBUG) renderDebugRequest(getIntent(), 20);
  }

  private void buildShell() {
    WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
    root = vertical();
    root.setBackgroundColor(color(android.R.attr.colorBackground));
    setContentView(root);
    ViewCompat.setOnApplyWindowInsetsListener(
        root,
        (view, insets) -> {
          Insets safe =
              insets.getInsets(
                  WindowInsetsCompat.Type.systemBars() | WindowInsetsCompat.Type.displayCutout());
          view.setPadding(safe.left, safe.top, safe.right, safe.bottom);
          return insets;
        });
    toolbar = new MaterialToolbar(this);
    toolbar.setTitle(getString(R.string.app_name));
    toolbar.setTitleTextColor(onSurface());
    toolbar.setBackgroundColor(color(android.R.attr.colorBackground));
    Drawable logo = ContextCompat.getDrawable(this, R.drawable.ic_logo).mutate();
    logo.setTint(primary());
    toolbar.setLogo(logo);
    MenuItem refresh = toolbar.getMenu().add(0, 1, 0, AppText.get(R.string.msg_refresh));
    refresh.setIcon(R.drawable.ic_refresh);
    refresh.setShowAsAction(MenuItem.SHOW_AS_ACTION_IF_ROOM);
    refresh.getIcon().setTint(onSurface());
    toolbar.getMenu().add(0, 2, 1, AppText.get(R.string.msg_appearance));
    toolbar.getMenu().add(0, 3, 2, AppText.get(R.string.msg_about_meow_kernel_manager));
    toolbar.setOnMenuItemClickListener(
        item -> {
          if (item.getItemId() == 1) {
            loadState(false);
            if (destination == R.id.nav_process) refreshProcesses();
            if (destination == R.id.nav_su) refreshSu();
          } else if (item.getItemId() == 2) chooseTheme();
          else about();
          return true;
        });
    root.addView(toolbar, new LinearLayout.LayoutParams(-1, dp(64)));
    progress = new LinearProgressIndicator(this);
    progress.setIndeterminate(true);
    progress.setVisibility(View.INVISIBLE);
    root.addView(progress, new LinearLayout.LayoutParams(-1, dp(3)));
    LinearLayout body = horizontal();
    root.addView(body, new LinearLayout.LayoutParams(-1, 0, 1));
    page = new FrameLayout(this);
    if (getResources().getConfiguration().screenWidthDp >= 600) {
      rail = new NavigationRailView(this);
      rail.inflateMenu(R.menu.navigation);
      rail.setBackgroundColor(color(android.R.attr.colorBackground));
      rail.setOnItemSelectedListener(
          item -> {
            navigate(item.getItemId());
            return true;
          });
      body.addView(rail, new LinearLayout.LayoutParams(dp(88), -1));
      rail.setSelectedItemId(destination);
    }
    body.addView(page, new LinearLayout.LayoutParams(0, -1, 1));
    if (rail == null) {
      bottom = new BottomNavigationView(this);
      bottom.inflateMenu(R.menu.navigation);
      bottom.setLabelVisibilityMode(
          com.google.android.material.navigation.NavigationBarView.LABEL_VISIBILITY_LABELED);
      bottom.setElevation(0);
      bottom.setOnItemSelectedListener(
          item -> {
            navigate(item.getItemId());
            return true;
          });
      root.addView(bottom, new LinearLayout.LayoutParams(-1, -2));
      bottom.setSelectedItemId(destination);
    }
    syncSuNav();
  }

  private void navigate(int id) {
    destination = id;
    hideKeyboard();
    showPage();
    if (id == R.id.nav_process && identity != null) refreshProcesses();
  }

  private void hideKeyboard() {
    ((InputMethodManager) getSystemService(INPUT_METHOD_SERVICE))
        .hideSoftInputFromWindow(root.getWindowToken(), 0);
  }

  private void showPage() {
    if (page == null) return;
    page.removeAllViews();
    adapter = null;
    recycler = null;
    processSummary = null;
    suAdapter = null;
    suSummary = null;
    suEmpty = null;
    toolbar.setTitle(
        destination == R.id.nav_boot
            ? getString(R.string.nav_boot)
            : destination == R.id.nav_tee
                ? getString(R.string.nav_tee)
                : destination == R.id.nav_process
            ? AppText.get(R.string.msg_processes)
            : destination == R.id.nav_controls
                ? AppText.get(R.string.msg_controls)
                : destination == R.id.nav_debug
                    ? AppText.get(R.string.msg_debug)
                    : destination == R.id.nav_su
                        ? AppText.get(R.string.nav_su) : getString(R.string.app_name));
    if (identity == null) {
      renderGate();
      return;
    }
    if (destination == R.id.nav_boot) renderBootSettings();
    else if (destination == R.id.nav_tee) renderTee();
    else if (destination == R.id.nav_process) renderProcesses();
    else if (destination == R.id.nav_controls) renderControls();
    else if (destination == R.id.nav_debug) renderDebug();
    else if (destination == R.id.nav_su) renderSu();
    else renderHome();
  }

  private void renderGate() {
    LinearLayout content = scrollPage(), body = vertical();
    add(body, text(AppText.get(R.string.msg_check_compatibility_first), 28, true), 12);
    add(body, secondary(compatibilityError), 20);
    MaterialButton retry = button(AppText.get(R.string.msg_check_again), true);
    retry.setOnClickListener(v -> loadState(true));
    add(body, retry, 0);
    card(content, body, true);
    LinearLayout explanation = vertical();
    note(explanation, AppText.get(R.string.msg_built_for_your_kernel), AppText.get(R.string.msg_checks_the_kernel_identity_api_version_and));
    card(content, explanation, false);
    addSecurityAudit(content);
  }

  private void renderBootSettings() {
    LinearLayout content = scrollPage(), next = vertical();
    add(next, text("下次启动", 20, true), 10);
    RadioGroup group = new RadioGroup(this);
    RadioButton normal = bootTarget("正常启动", 101);
    RadioButton menu = bootTarget("进入 BootMenu", 102);
    RadioButton bootloader = bootTarget("进入 Bootloader", 103);
    group.addView(normal);
    group.addView(menu);
    group.addView(bootloader);
    add(next, group, 4);
    add(next, secondary("单次生效；不选择时下次正常启动。"), 0);
    card(content, next, true);

    LinearLayout timer = vertical();
    add(timer, text("Menu 超时", 20, true), 8);
    TextView timeoutValue = text("60 秒", 16, false);
    add(timer, timeoutValue, 4);
    SeekBar bar = new SeekBar(this);
    bar.setMax(115);
    add(timer, bar, 4);
    add(timer, secondary("5–120 秒；有触屏或按键后取消倒计时。"), 0);
    card(content, timer, false);

    io.execute(() -> {
      KernelRepository.BootSettings settings;
      try {
        settings = KernelRepository.bootSettings();
      } catch (Exception failure) {
        main.post(() -> snack(KernelRepository.error(failure)));
        return;
      }
      main.post(() -> {
        if (destroyed) return;
        group.check(settings.target == 1 ? 102 : settings.target == 2 ? 103 : 101);
        int seconds = Math.max(5, Math.min(120, settings.timeout));
        bar.setProgress(seconds - 5);
        timeoutValue.setText(seconds + " 秒");
        group.setOnCheckedChangeListener((ignored, checked) -> {
          String target = checked == 102 ? "menu" : checked == 103 ? "bootloader" : "normal";
          io.execute(() -> {
            try {
              KernelRepository.setBootTarget(target);
              main.post(() -> snack("已保存，下次启动生效"));
            } catch (Exception failure) {
              main.post(() -> snack("保存失败：" + KernelRepository.error(failure)));
            }
          });
        });
        bar.setOnSeekBarChangeListener(
            new SeekBar.OnSeekBarChangeListener() {
              @Override
              public void onProgressChanged(SeekBar seek, int progress, boolean fromUser) {
                timeoutValue.setText((progress + 5) + " 秒");
              }

              @Override
              public void onStartTrackingTouch(SeekBar seek) {}

              @Override
              public void onStopTrackingTouch(SeekBar seek) {
                int value = seek.getProgress() + 5;
                io.execute(() -> {
                  try {
                    KernelRepository.setMenuTimeout(value);
                    main.post(() -> snack("已保存：超时 " + value + " 秒"));
                  } catch (Exception failure) {
                    main.post(() -> snack("保存失败：" + KernelRepository.error(failure)));
                  }
                });
              }
            });
      });
    });
  }

  private RadioButton bootTarget(String label, int id) {
    RadioButton button = new RadioButton(this);
    button.setId(id);
    button.setText(label);
    button.setTextSize(16);
    button.setPadding(0, dp(6), 0, dp(6));
    return button;
  }

  private void renderTee() {
    LinearLayout content = scrollPage(), panel = vertical();
    add(panel, text("TEE 欺骗", 26, true), 8);
    add(panel, secondary("是否启用由 BootMenu 的 TEE Simulator 决定。"), 12);
    TextView state = text("状态：读取中…", 16, false);
    TextView keybox = text("Keybox：读取中…", 16, false);
    TextView service = text("服务：读取中…", 16, false);
    add(panel, state, 4);
    add(panel, keybox, 4);
    add(panel, service, 4);
    MaterialButton refresh = button("刷新", false);
    refresh.setOnClickListener(view -> loadTee(state, keybox, service, refresh));
    add(panel, refresh, 10);
    card(content, panel, true);
    loadTee(state, keybox, service, refresh);
  }

  private void loadTee(TextView state, TextView keybox, TextView service, MaterialButton refresh) {
    refresh.setEnabled(false);
    io.execute(() -> {
      String tee = null;
      try {
        tee = KernelRepository.teeStatus();
      } catch (Exception unavailable) {
        // The node only exists on kernels with the boot-settings interface.
      }
      String running = property("init.svc.meow_tee");
      String result = tee;
      main.post(
          () -> {
            if (destroyed) return;
            refresh.setEnabled(true);
            if (result == null) {
              state.setText("状态：接口不可用");
              keybox.setText("Keybox：—");
              service.setText("服务：—");
              return;
            }
            Map<String, String> fields = KernelRepository.keyValues(result);
            state.setText("状态：" + ("1".equals(fields.get("enabled")) ? "本次已启用" : "本次未启用"));
            keybox.setText("Keybox：" + teeKeybox(fields.get("keybox")));
            service.setText(
                "服务："
                    + ("running".equals(running)
                        ? "运行中"
                        : running.isEmpty() || "stopped".equals(running) ? "未启用" : running));
          });
    });
  }

  private static String teeKeybox(String value) {
    if (value == null) return "未知";
    switch (value) {
      case "ok":
        return "正常";
      case "missing":
        return "未找到 keybox.xml";
      case "symlink":
        return "不能是符号链接";
      case "owner":
        return "属主必须是 root:root";
      case "mode":
        return "权限必须是 600";
      case "not_regular":
        return "不是普通文件";
      default:
        return "无法读取";
    }
  }

  private static String property(String name) {
    try {
      java.lang.Process process =
          new ProcessBuilder("getprop", name).redirectErrorStream(true).start();
      java.io.ByteArrayOutputStream output = new java.io.ByteArrayOutputStream();
      byte[] buffer = new byte[256];
      for (int count; (count = process.getInputStream().read(buffer)) > 0; )
        output.write(buffer, 0, count);
      process.waitFor();
      return output.toString("UTF-8").trim();
    } catch (Exception unavailable) {
      return "";
    }
  }

  private void renderHome() {
    LinearLayout content = scrollPage(), hero = vertical();
    TextView title = text(AppText.get(R.string.msg_kernel_ready), 30, true);
    title.setTextColor(color(com.google.android.material.R.attr.colorOnPrimaryContainer));
    add(hero, title, 10);
    add(hero, text(identity.raw.optString("display_name", "MadeByQWEOVO"), 18, true), 8);
    TextView release = secondary(identity.release);
    release.setTextIsSelectable(true);
    add(hero, release, 14);
    add(hero, secondary(Build.MODEL + "  /  Android " + Build.VERSION.RELEASE), 0);
    card(content, hero, true);
    if (identity.has("sukisu_signed_boot")) renderSukisuChallenge(content);
    else if (identity.has("sukisu_next_boot")) renderSukisuNextBoot(content);
    if (identity.has("meow_su")) renderMeowSu(content);
    addSecurityAudit(content);
    LinearLayout metrics = horizontal();
    metric(
        metrics,
        AppText.get(R.string.msg_online_cores),
        cpuState == null ? "—" : cpuState.online.size() + " / " + cpuState.present.size());
    metric(metrics, AppText.get(R.string.msg_active_cores), cpuState == null ? "—" : String.valueOf(cpuState.active.size()));
    metric(metrics, AppText.get(R.string.msg_visible_processes), processes.isEmpty() ? "—" : String.valueOf(processes.size()));
    add(content, metrics, 22);
    add(content, text(AppText.get(R.string.msg_available_features), 20, true), 14);
    LinearLayout features = vertical();
    capability(features, AppText.get(R.string.msg_cpu_control), AppText.get(R.string.msg_view_online_and_active_state_and_control), identity.has("cpu_control"));
    capability(features, AppText.get(R.string.msg_process_priority), AppText.get(R.string.msg_adjust_nice_by_process_or_thread), identity.has("process_control"));
    capability(features, AppText.get(R.string.msg_forwarding_ttl), AppText.get(R.string.msg_control_software_ipv4_forwarding_behavior), identity.has("ttl_control"));
    capability(features, AppText.get(R.string.msg_fault_policy), AppText.get(R.string.msg_recoverable_oops_behavior_and_actions_after_panic), identity.has("panic_control"));
    card(content, features, false);
  }

  private void addSecurityAudit(LinearLayout content) {
    LinearLayout panel = vertical();
    note(panel, getString(R.string.security_audit_title), getString(R.string.security_audit_description));
    MaterialButton open = button(getString(R.string.security_audit_open), true);
    open.setOnClickListener(view -> loadSecurityAudit(open));
    add(panel, open, 0);
    io.execute(() -> {
      try {
        if (!KernelRepository.securityStatus().containsKey("audit_storage")) return;
        main.post(() -> {
          if (!destroyed && content.isAttachedToWindow()) card(content, panel, false);
        });
      } catch (Exception unavailable) {
        // Older kernels do not expose Security Audit.
      }
    });
  }

  private void loadSecurityAudit(MaterialButton button) {
    button.setEnabled(false);
    io.execute(() -> {
      try {
        Map<String, String> state = KernelRepository.securityStatus();
        boolean storage = "1".equals(state.get("audit_storage"));
        String records = storage ? repository.securityAudit() : "";
        main.post(() -> {
          if (destroyed) return;
          button.setEnabled(true);
          TextView text = secondary(records.isBlank() ? getString(storage
              ? R.string.security_audit_empty : R.string.security_audit_unavailable) : records);
          text.setTextIsSelectable(true);
          text.setPadding(dp(24), dp(8), dp(24), dp(8));
          ScrollView scroll = new ScrollView(this);
          scroll.addView(text);
          MaterialAlertDialogBuilder dialog = new MaterialAlertDialogBuilder(this)
              .setTitle(R.string.security_audit_title).setView(scroll)
              .setPositiveButton(android.R.string.ok, null);
          if (storage) dialog.setNeutralButton(R.string.security_audit_clear, (ignored, which) ->
              new MaterialAlertDialogBuilder(this)
                  .setMessage(R.string.security_audit_confirm)
                  .setNegativeButton(android.R.string.cancel, null)
                  .setPositiveButton(R.string.security_audit_clear, (confirmed, action) -> {
                    io.execute(() -> {
                      try {
                        repository.clearSecurityAudit();
                        main.post(() -> { if (!destroyed) snack(getString(R.string.security_audit_cleared)); });
                      } catch (Exception failure) {
                        main.post(() -> { if (!destroyed) snack(KernelRepository.error(failure)); });
                      }
                    });
                  }).show());
          dialog.show();
        });
      } catch (Exception failure) {
        main.post(() -> {
          if (destroyed) return;
          button.setEnabled(true);
          snack(KernelRepository.error(failure));
        });
      }
    });
  }

  private void renderSukisuNextBoot(LinearLayout content) {
    LinearLayout row = horizontal();
    LinearLayout label = vertical();
    String title = AppText.get(R.string.sukisu_next_title);
    add(label, text(title, 18, true), 6);
    add(label, secondary(AppText.get(sukisuNextBoot == null
        ? R.string.msg_state_temporarily_unavailable : R.string.sukisu_next_description)), 0);
    row.addView(label, new LinearLayout.LayoutParams(0, -2, 1));
    MaterialSwitch toggle = new MaterialSwitch(this);
    toggle.setContentDescription(title);
    toggle.setChecked(Boolean.TRUE.equals(sukisuNextBoot));
    toggle.setEnabled(sukisuNextBoot != null && !busy && credentialRequest == null);
    toggle.setOnCheckedChangeListener((view, checked) -> {
      if (Boolean.valueOf(checked).equals(sukisuNextBoot)) return;
      if (checked) {
        toggle.setChecked(false);
        confirmSukisuNextBoot();
      } else {
        toggle.setEnabled(false);
        action(() -> repository.setSukisuNextBoot(false), getString(R.string.sukisu_next_disarmed));
      }
    });
    row.addView(toggle);
    card(content, row, false);
  }

  private void renderSukisuChallenge(LinearLayout content) {
    LinearLayout panel = vertical();
    add(panel, text(getString(R.string.sukisu_next_title), 18, true), 6);
    if (sukisuChallenge == null) {
      add(panel, secondary(getString(R.string.msg_state_temporarily_unavailable)), 0);
    } else if (sukisuChallenge.armed) {
      add(panel, secondary(getString(R.string.sukisu_next_armed)), 0);
    } else {
      add(panel, secondary(getString(R.string.sukisu_scan_instructions)), 10);
      MaterialButton show = button(getString(R.string.sukisu_show_challenge), true);
      show.setEnabled(!busy);
      show.setOnClickListener(view -> showSukisuChallenge());
      add(panel, show, 6);
      MaterialButton scan = button(getString(R.string.sukisu_scan_response), false);
      scan.setEnabled(!busy);
      scan.setOnClickListener(view -> scanSukisuResponse());
      add(panel, scan, 0);
    }
    card(content, panel, false);
  }

  private void showSukisuChallenge() {
    if (sukisuChallenge == null) return;
    try {
      ImageView qr = new ImageView(this);
      qr.setAdjustViewBounds(true);
      qr.setImageBitmap(SukisuProtocol.qr(sukisuChallenge.code));
      qr.setContentDescription(getString(R.string.sukisu_show_challenge));
      new MaterialAlertDialogBuilder(this)
          .setTitle(R.string.sukisu_show_challenge)
          .setView(qr)
          .setNegativeButton(android.R.string.cancel, null)
          .setPositiveButton(R.string.sukisu_scan_response, (dialog, which) -> scanSukisuResponse())
          .show();
    } catch (com.google.zxing.WriterException failure) {
      snack(KernelRepository.error(failure));
    }
  }

  private void scanSukisuResponse() {
    if (busy || scanningSukisuResponse) return;
    scanningSukisuResponse = true;
    GmsBarcodeScannerOptions options = new GmsBarcodeScannerOptions.Builder()
        .setBarcodeFormats(Barcode.FORMAT_QR_CODE)
        .enableAutoZoom()
        .build();
    GmsBarcodeScanning.getClient(this, options).startScan().addOnCompleteListener(result -> {
      scanningSukisuResponse = false;
      if (destroyed || isFinishing() || result.isCanceled()) return;
      if (result.isSuccessful()) {
        submitSukisuScan(result.getResult().getRawValue());
        return;
      }
      if (result.getException() instanceof MlKitException failure
          && (failure.getErrorCode() == MlKitException.CODE_SCANNER_CANCELLED
              || failure.getErrorCode() == MlKitException.CANCELLED)) return;
      scanningSukisuResponse = true;
      sukisuResponseScanner.launch(SukisuProtocol.scan(getString(R.string.sukisu_scan_response))
          .setCaptureActivity(SukisuScanActivity.class));
    });
  }

  private void renderMeowSu(LinearLayout content) {
    LinearLayout panel = vertical();
    note(panel, getString(R.string.meow_su_title), getString(R.string.meow_su_description));
    gap(panel, 10);
    MaterialButton open = button(getString(R.string.meow_su_apps), true);
    open.setOnClickListener(view -> selectDestination(R.id.nav_su));
    add(panel, open, 0);
    card(content, panel, false);
  }

  private void selectDestination(int id) {
    if (rail != null) rail.setSelectedItemId(id);
    else if (bottom != null) bottom.setSelectedItemId(id);
    else navigate(id);
  }

  private void syncSuNav() {
    boolean enabled = identity != null && identity.has("meow_su");
    Menu menu = rail != null ? rail.getMenu() : bottom != null ? bottom.getMenu() : null;
    if (menu != null) {
      MenuItem item = menu.findItem(R.id.nav_su);
      if (item != null) item.setVisible(enabled);
    }
    if (!enabled && identity != null && destination == R.id.nav_su) {
      destination = R.id.nav_home;
      if (rail != null) rail.setSelectedItemId(R.id.nav_home);
      else if (bottom != null) bottom.setSelectedItemId(R.id.nav_home);
    }
  }

  private void renderSu() {
    if (!identity.has("meow_su")) {
      featureUnavailable(AppText.get(R.string.su_unavailable));
      return;
    }
    LinearLayout content = vertical();
    content.setPadding(dp(24), dp(16), dp(24), 0);
    page.addView(content, new FrameLayout.LayoutParams(-1, -1));
    TextInputLayout searchBox =
        new TextInputLayout(this, null, com.google.android.material.R.attr.textInputOutlinedStyle);
    searchBox.setHint(AppText.get(R.string.su_search_hint));
    searchBox.setStartIconDrawable(R.drawable.ic_search);
    searchBox.setBoxCornerRadii(dp(24), dp(24), dp(24), dp(24));
    TextInputEditText search = new TextInputEditText(searchBox.getContext());
    search.setSingleLine(true);
    search.setText(suQuery);
    searchBox.addView(search);
    add(content, searchBox, 10);
    Runnable filterLater = this::filterSu;
    search.addTextChangedListener(
        new TextWatcher() {
          public void beforeTextChanged(CharSequence s, int start, int count, int after) {}

          public void onTextChanged(CharSequence s, int start, int before, int count) {
            suQuery = s.toString();
            main.removeCallbacks(filterLater);
            main.postDelayed(filterLater, 180);
          }

          public void afterTextChanged(Editable s) {}
        });
    LinearLayout heading = horizontal();
    suSummary = secondary(AppText.get(R.string.su_loading));
    heading.addView(suSummary, new LinearLayout.LayoutParams(0, -2, 1));
    MaterialButton refresh = button(AppText.get(R.string.msg_refresh), false);
    refresh.setMinWidth(dp(72));
    refresh.setOnClickListener(view -> refreshSu());
    heading.addView(refresh);
    add(content, heading, 8);
    FrameLayout listHost = new FrameLayout(this);
    content.addView(listHost, new LinearLayout.LayoutParams(-1, 0, 1));
    RecyclerView list = new RecyclerView(this);
    list.setLayoutManager(new LinearLayoutManager(this));
    list.setItemAnimator(null);
    list.setClipToPadding(false);
    list.setPadding(0, 0, 0, dp(16));
    suAdapter = new SuAdapter();
    list.setAdapter(suAdapter);
    listHost.addView(list, new FrameLayout.LayoutParams(-1, -1));
    suEmpty = secondary(AppText.get(R.string.su_loading));
    suEmpty.setGravity(Gravity.CENTER);
    listHost.addView(suEmpty, new FrameLayout.LayoutParams(-1, -1));
    filterSu();
    refreshSu();
  }

  private void refreshSu() {
    if (suAdapter == null) return;
    io.execute(
        () -> {
          try {
            List<KernelRepository.MeowSuApp> apps = repository.meowSuApps();
            main.post(
                () -> {
                  if (destroyed || suAdapter == null) return;
                  suApps = apps;
                  filterSu();
                });
          } catch (Exception failure) {
            main.post(
                () -> {
                  if (destroyed) return;
                  snack(KernelRepository.error(failure));
                });
          }
        });
  }

  private void filterSu() {
    if (suAdapter == null) return;
    String needle = suQuery.trim().toLowerCase(Locale.ROOT);
    ArrayList<KernelRepository.MeowSuApp> visible = new ArrayList<>();
    int granted = 0;
    for (KernelRepository.MeowSuApp app : suApps) {
      if (app.allowed()) granted++;
      if (!needle.isEmpty()
          && !(app.label() + " " + app.name()).toLowerCase(Locale.ROOT).contains(needle)) continue;
      visible.add(app);
    }
    suAdapter.submitList(visible);
    if (suSummary != null)
      suSummary.setText(getString(R.string.su_granted_count, granted, suApps.size()));
    if (suEmpty != null) {
      suEmpty.setVisibility(visible.isEmpty() ? View.VISIBLE : View.GONE);
      suEmpty.setText(AppText.get(R.string.su_empty_search));
    }
  }

  private void updateSuApp(String name, boolean allowed) {
    ArrayList<KernelRepository.MeowSuApp> next = new ArrayList<>();
    for (KernelRepository.MeowSuApp app : suApps)
      next.add(
          app.name().equals(name)
              ? new KernelRepository.MeowSuApp(app.name(), app.label(), allowed)
              : app);
    suApps = next;
    filterSu();
  }

  private void bindSuIcon(ImageView image, KernelRepository.MeowSuApp app) {
    image.setTag(app.name());
    image.setImageResource(R.drawable.ic_su);
    image.setImageTintList(ColorStateList.valueOf(primary()));
    Bitmap cached = iconCache.get(app.name());
    if (cached != null) {
      image.setImageTintList(null);
      image.setImageBitmap(cached);
      return;
    }
    icons.execute(
        () -> {
          try {
            Bitmap bitmap = iconCache.get(app.name());
            if (bitmap == null) {
              Drawable drawable = getPackageManager().getApplicationIcon(app.name());
              bitmap = Bitmap.createBitmap(112, 112, Bitmap.Config.ARGB_8888);
              Canvas canvas = new Canvas(bitmap);
              drawable.setBounds(0, 0, 112, 112);
              drawable.draw(canvas);
              iconCache.put(app.name(), bitmap);
            }
            Bitmap result = bitmap;
            main.post(
                () -> {
                  if (!destroyed && app.name().equals(image.getTag())) {
                    image.setImageTintList(null);
                    image.setImageBitmap(result);
                  }
                });
          } catch (Exception ignored) {
          }
        });
  }

  private void submitSukisuScan(String code) {
    if (code != null)
      action(
          () -> {
            repository.submitSukisuSignature(code);
            /* A signed grant is only useful when the next boot shows the menu. */
            try {
              KernelRepository.setBootTarget("menu");
            } catch (Exception ignored) {
              // Older kernels have no boot-settings interface; keep the grant.
            }
          },
          getString(R.string.sukisu_next_armed));
  }

  private void confirmSukisuNextBoot() {
    if (busy || identity == null || credentialRequest != null) return;
    showPage();
    requireDeviceCredential(
        getString(R.string.sukisu_auth_title),
        getString(R.string.sukisu_auth_description),
        () -> action(() -> repository.setSukisuNextBoot(true), getString(R.string.sukisu_next_armed)));
  }

  private void requireDeviceCredential(String title, String description, Runnable afterAuth) {
    if (credentialRequest != null) return;
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) {
      snack(getString(R.string.sukisu_auth_unsupported));
      return;
    }
    KeyguardManager keyguard = getSystemService(KeyguardManager.class);
    if (keyguard == null || !keyguard.isDeviceSecure()) {
      snack(getString(R.string.sukisu_auth_setup_required));
      return;
    }
    requestDeviceCredential(title, description, afterAuth);
  }

  @RequiresApi(Build.VERSION_CODES.R)
  private void requestDeviceCredential(String title, String description, Runnable afterAuth) {
    CancellationSignal request = new CancellationSignal();
    credentialRequest = request;
    credentialAction = afterAuth;
    try {
      new BiometricPrompt.Builder(this)
          .setTitle(title)
          .setDescription(description)
          .setAllowedAuthenticators(BiometricManager.Authenticators.DEVICE_CREDENTIAL)
          .build()
          .authenticate(request, getMainExecutor(), new BiometricPrompt.AuthenticationCallback() {
            @Override
            public void onAuthenticationSucceeded(BiometricPrompt.AuthenticationResult result) {
              finishDeviceCredential(request, true);
            }

            @Override
            public void onAuthenticationError(int code, CharSequence message) {
              finishDeviceCredential(request, false);
              if (code != BiometricPrompt.BIOMETRIC_ERROR_CANCELED
                  && code != BiometricPrompt.BIOMETRIC_ERROR_USER_CANCELED) {
                snack(getString(R.string.sukisu_auth_failed));
              }
            }
          });
    } catch (IllegalArgumentException | IllegalStateException | SecurityException failure) {
      finishDeviceCredential(request, false);
      snack(getString(R.string.sukisu_auth_failed));
    }
  }

  private void finishDeviceCredential(CancellationSignal request, boolean run) {
    if (destroyed || credentialRequest != request) return;
    credentialRequest = null;
    Runnable action = credentialAction;
    credentialAction = null;
    if (run && action != null) action.run();
  }

  private void metric(LinearLayout row, String label, String value) {
    LinearLayout box = vertical();
    box.setPadding(dp(12), dp(12), dp(8), dp(12));
    add(box, text(value, 24, true), 4);
    add(box, secondary(label), 0);
    row.addView(box, new LinearLayout.LayoutParams(0, -2, 1));
  }

  private void capability(LinearLayout parent, String title, String description, boolean enabled) {
    LinearLayout row = horizontal(), left = vertical();
    add(left, text(title, 16, true), 2);
    add(left, secondary(description), 0);
    row.addView(left, new LinearLayout.LayoutParams(0, -2, 1));
    row.addView(badge(enabled ? AppText.get(R.string.msg_available) : AppText.get(R.string.msg_disabled)));
    add(parent, row, 18);
  }

  private void renderProcesses() {
    if (!identity.has("process_info")) {
      featureUnavailable(AppText.get(R.string.msg_the_process_interface_is_disabled));
      return;
    }
    LinearLayout content = vertical();
    content.setPadding(dp(24), dp(16), dp(24), 0);
    page.addView(content, new FrameLayout.LayoutParams(-1, -1));
    TextInputLayout searchBox =
        new TextInputLayout(this, null, com.google.android.material.R.attr.textInputOutlinedStyle);
    searchBox.setHint(AppText.get(R.string.msg_search_apps_process_names_pids_or_uids));
    searchBox.setStartIconDrawable(R.drawable.ic_search);
    searchBox.setBoxCornerRadii(dp(24), dp(24), dp(24), dp(24));
    TextInputEditText search = new TextInputEditText(searchBox.getContext());
    search.setSingleLine(true);
    search.setText(query);
    searchBox.addView(search);
    add(content, searchBox, 12);
    Runnable filterLater = this::filterProcesses;
    search.addTextChangedListener(
        new TextWatcher() {
          public void beforeTextChanged(CharSequence s, int start, int count, int after) {}

          public void onTextChanged(CharSequence s, int start, int before, int count) {
            query = s.toString();
            main.removeCallbacks(filterLater);
            main.postDelayed(filterLater, 180);
          }

          public void afterTextChanged(Editable s) {}
        });
    LinearLayout options = horizontal();
    HorizontalScrollView scroll = new HorizontalScrollView(this);
    scroll.setHorizontalScrollBarEnabled(false);
    ChipGroup filters = new ChipGroup(this);
    filters.setSingleSelection(true);
    filters.setSelectionRequired(true);
    filters.setSingleLine(true);
    String[] labels = {AppText.get(R.string.msg_apps), AppText.get(R.string.msg_all), AppText.get(R.string.msg_system), AppText.get(R.string.msg_adjusted)};
    for (int i = 0; i < labels.length; i++) {
      final int index = i;
      Chip chip = new Chip(this);
      chip.setId(View.generateViewId());
      chip.setText(labels[i]);
      chip.setCheckable(true);
      filters.addView(chip);
      chip.setChecked(filter == i);
      chip.setOnClickListener(
          v -> {
            filter = index;
            filterProcesses();
          });
    }
    scroll.addView(filters);
    options.addView(scroll, new LinearLayout.LayoutParams(0, -2, 1));
    MaterialButton order = button(AppText.get(R.string.msg_sort), false);
    order.setMinWidth(dp(64));
    order.setOnClickListener(
        v ->
            new MaterialAlertDialogBuilder(this)
                .setTitle(AppText.get(R.string.msg_sort_processes))
                .setSingleChoiceItems(
                    new String[] {AppText.get(R.string.msg_app_name), AppText.get(R.string.msg_process_id), AppText.get(R.string.msg_priority), AppText.get(R.string.msg_thread_count)},
                    sort,
                    (dialog, which) -> {
                      sort = which;
                      filterProcesses();
                      if (recycler != null) recycler.scrollToPosition(0);
                      dialog.dismiss();
                    })
                .show());
    options.addView(order);
    add(content, options, 8);
    if (identity.has("priority_uid_lock")) {
      MaterialButton locks = button(AppText.get(R.string.msg_view_uid_locks), false);
      locks.setOnClickListener(
          v -> {
            detailOpen = true;
            new UidLocksSheet(
                    this,
                    repository,
                    io,
                    () -> {
                      detailOpen = false;
                      if (!destroyed) refreshProcesses();
                    })
                .show();
          });
      add(content, locks, 8);
    }
    LinearLayout refreshRow = horizontal();
    processSummary = secondary(AppText.get(R.string.msg_reading_processes));
    refreshRow.addView(processSummary, new LinearLayout.LayoutParams(0, -2, 1));
    MaterialSwitch automatic = new MaterialSwitch(this);
    automatic.setText(AppText.get(R.string.msg_auto));
    automatic.setTextSize(12);
    automatic.setContentDescription(AppText.get(R.string.msg_refresh_every_three_seconds_pauses_when_you));
    automatic.setChecked(autoRefresh);
    automatic.setOnCheckedChangeListener((v, on) -> autoRefresh = on);
    refreshRow.addView(automatic);
    add(content, refreshRow, 8);
    FrameLayout listHost = new FrameLayout(this);
    content.addView(listHost, new LinearLayout.LayoutParams(-1, 0, 1));
    recycler = new RecyclerView(this);
    recycler.setLayoutManager(new LinearLayoutManager(this));
    recycler.setItemAnimator(null);
    recycler.setClipToPadding(false);
    recycler.setPadding(0, 0, 0, dp(16));
    adapter = new ProcessAdapter();
    recycler.setAdapter(adapter);
    listHost.addView(recycler, new FrameLayout.LayoutParams(-1, -1));
    emptyMessage = secondary(AppText.get(R.string.msg_no_matching_processes));
    emptyMessage.setGravity(Gravity.CENTER);
    listHost.addView(emptyMessage, new FrameLayout.LayoutParams(-1, -1));
    filterProcesses();
  }

  private void filterProcesses() {
    if (adapter == null) return;
    String needle = query.trim().toLowerCase(Locale.ROOT);
    ArrayList<ProcessEntry> visible = new ArrayList<>();
    for (ProcessEntry entry : processes) {
      TaskRecord task = entry.displayTask();
      boolean application = task.uid >= 10000 && !task.kernel;
      if (filter == 0 && !application
          || filter == 2 && application
          || filter == 3 && !entry.changedPriority()) continue;
      String haystack =
          (entry.label
                  + " "
                  + entry.packageName
                  + " "
                  + task.comm
                  + " "
                  + task.argv0
                  + " "
                  + entry.pid
                  + " "
                  + task.uid)
              .toLowerCase(Locale.ROOT);
      if (!needle.isEmpty() && !haystack.contains(needle)) {
        boolean threadMatch = false;
        for (TaskRecord thread : entry.threads)
          if ((thread.comm + " " + thread.tid).toLowerCase(Locale.ROOT).contains(needle)) {
            threadMatch = true;
            break;
          }
        if (!threadMatch) continue;
      }
      visible.add(entry);
    }
    Comparator<ProcessEntry> comparator =
        switch (sort) {
          case 1 -> Comparator.comparingInt(e -> e.pid);
          case 2 -> Comparator.comparingInt(e -> e.minNice);
          case 3 -> Comparator.<ProcessEntry>comparingInt(e -> e.threads.size()).reversed();
          default -> Comparator.comparing(e -> e.label.toLowerCase(Locale.ROOT));
        };
    visible.sort(comparator.thenComparingInt(e -> e.pid));
    adapter.submitList(visible);
    emptyMessage.setVisibility(visible.isEmpty() ? View.VISIBLE : View.GONE);
    emptyMessage.setText(processes.isEmpty() && lastRead == 0 ? AppText.get(R.string.msg_reading_processes_83) : AppText.get(R.string.msg_no_matching_processes));
    if (processSummary != null)
      processSummary.setText(
          visible.size()
              + AppText.get(R.string.msg_processes_84)
              + (lastRead == 0
                  ? AppText.get(R.string.msg_waiting_for_refresh)
                  : new SimpleDateFormat("HH:mm:ss", Locale.getDefault())
                      .format(new Date(lastRead))));
  }

  private void renderControls() {
    LinearLayout content = scrollPage();
    add(content, text(AppText.get(R.string.msg_cpu_and_network), 26, true), 8);
    add(content, secondary(AppText.get(R.string.msg_uses_public_interfaces_and_reads_back_the)), 20);
    if (identity.has("cpu_control")) {
      LinearLayout cpu = vertical();
      note(
          cpu,
          AppText.get(R.string.msg_cpu_cores),
          cpuState == null
              ? AppText.get(R.string.msg_state_temporarily_unavailable)
              : AppText.get(R.string.msg_online)
                  + cpuState.online.size()
                  + AppText.get(R.string.msg_active)
                  + cpuState.active.size()
                  + AppText.get(R.string.msg_detected)
                  + cpuState.present.size()
                  + AppText.get(R.string.msg_cores));
      gap(cpu, 16);
      if (cpuState != null) {
        LinearLayout row = null;
        int column = 0;
        int columns = getResources().getConfiguration().screenWidthDp >= 900 ? 4 : 2;
        for (int number : cpuState.present) {
          if (column % columns == 0) {
            row = horizontal();
            add(cpu, row, 8);
          }
          LinearLayout tile = vertical();
          tile.setPadding(dp(8), dp(4), dp(8), dp(4));
          LinearLayout title = horizontal();
          title.addView(text("CPU " + number, 16, true), new LinearLayout.LayoutParams(0, -2, 1));
          MaterialSwitch toggle = new MaterialSwitch(this);
          toggle.setContentDescription("CPU " + number + AppText.get(R.string.msg_online_state));
          toggle.setChecked(cpuState.online.contains(number));
          toggle.setEnabled(!busy && cpuState.hotpluggable.contains(number));
          toggle.setOnCheckedChangeListener(
              (view, on) -> {
                if (on == cpuState.online.contains(number)) return;
                if (!on && cpuState.active.size() == 1 && cpuState.active.contains(number)) {
                  view.setChecked(true);
                  snack(AppText.get(R.string.msg_at_least_one_active_core_must_remain));
                  return;
                }
                action(() -> repository.cpu(number, on), AppText.get(R.string.msg_cpu_state_updated));
              });
          title.addView(toggle);
          tile.addView(title);
          add(
              tile,
              secondary(
                  cpuState.active.contains(number)
                      ? AppText.get(R.string.msg_active_97)
                      : cpuState.online.contains(number) ? AppText.get(R.string.msg_online_not_active) : AppText.get(R.string.msg_offline)),
              0);
          row.addView(tile, new LinearLayout.LayoutParams(0, -2, 1));
          column++;
        }
        if (column % columns != 0) {
          for (int i = column % columns; i < columns; i++) {
            row.addView(new View(this), new LinearLayout.LayoutParams(0, 1, 1));
          }
        }
      }
      gap(cpu, 8);
      add(cpu, secondary(AppText.get(R.string.msg_cpu_changes_affect_the_whole_device_vendor)), 0);
      card(content, cpu, false);
    }
    if (identity.has("ttl_control")) {
      LinearLayout network = vertical();
      LinearLayout row = horizontal();
      LinearLayout title = vertical();
      add(title, text(AppText.get(R.string.msg_preserve_forwarding_ttl), 18, true), 6);
      add(title, secondary(AppText.get(R.string.msg_applies_only_to_software_ipv4_forwarding)), 0);
      row.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
      MaterialSwitch toggle = new MaterialSwitch(this);
      toggle.setContentDescription(AppText.get(R.string.msg_preserve_forwarding_ttl));
      toggle.setChecked(ttl != null && ttl == 1);
      toggle.setEnabled(ttl != null && !busy);
      toggle.setOnCheckedChangeListener(
          (view, on) -> action(() -> repository.ttl(on), AppText.get(R.string.msg_ttl_policy_updated)));
      row.addView(toggle);
      add(network, row, 12);
      add(network, secondary(AppText.get(R.string.msg_when_disabled_ttl_decreases_normally_does_not)), 0);
      card(content, network, false);
    }
  }

  private void renderDebug() {
    if (!identity.has("panic_control")) {
      featureUnavailable(AppText.get(R.string.msg_the_fault_policy_interface_is_disabled_105));
      return;
    }
    LinearLayout content = scrollPage();
    add(content, text(AppText.get(R.string.msg_fault_handling), 26, true), 8);
    add(content, secondary(AppText.get(R.string.msg_configure_behavior_for_this_boot_session_does)), 20);
    LinearLayout policy = vertical();
    LinearLayout row = horizontal(), title = vertical();
    add(title, text(AppText.get(R.string.msg_continue_after_recoverable_oops), 17, true), 4);
    add(title, secondary(AppText.get(R.string.msg_severe_faults_still_enter_panic)), 0);
    row.addView(title, new LinearLayout.LayoutParams(0, -2, 1));
    MaterialSwitch attempt = new MaterialSwitch(this);
    attempt.setChecked(identity.oops());
    attempt.setContentDescription(AppText.get(R.string.msg_continue_after_recoverable_oops));
    row.addView(attempt);
    add(policy, row, 20);
    add(policy, text(AppText.get(R.string.msg_action_after_panic), 16, true), 10);
    MaterialButtonToggleGroup actions = new MaterialButtonToggleGroup(this);
    actions.setSingleSelection(true);
    actions.setSelectionRequired(true);
    int reboot = View.generateViewId(), dump = View.generateViewId();
    MaterialButton rebootButton = button(AppText.get(R.string.msg_normal_reboot), false), dumpButton = button("Ramdump", false);
    rebootButton.setId(reboot);
    dumpButton.setId(dump);
    actions.addView(rebootButton, new LinearLayout.LayoutParams(0, dp(52), 1));
    actions.addView(dumpButton, new LinearLayout.LayoutParams(0, dp(52), 1));
    dumpButton.setEnabled(identity.dumpReady());
    actions.check("ramdump".equals(identity.action()) ? dump : reboot);
    add(policy, actions, 16);
    add(
        policy,
        secondary(
            identity.dumpReady()
                ? AppText.get(R.string.msg_ramdump_is_ready_a_usb_dump_is)
                : AppText.get(R.string.msg_ramdump_is_not_ready_only_normal_reboot)),
        18);
    MaterialButton apply = button(AppText.get(R.string.msg_apply_settings), true);
    apply.setEnabled(false);
    Runnable dirty =
        () ->
            apply.setEnabled(
                !busy
                    && (attempt.isChecked() != identity.oops()
                        || !((actions.getCheckedButtonId() == dump ? "ramdump" : "reboot")
                            .equals(identity.action()))));
    attempt.setOnCheckedChangeListener((view, on) -> dirty.run());
    actions.addOnButtonCheckedListener((group, id, on) -> dirty.run());
    apply.setOnClickListener(
        v -> {
          boolean value = attempt.isChecked();
          String action = actions.getCheckedButtonId() == dump ? "ramdump" : "reboot";
          requireDeviceCredential(
              getString(R.string.panic_auth_title),
              getString(R.string.panic_auth_description),
              () ->
                  action(
                      () -> repository.panic(value, action),
                      AppText.get(R.string.msg_fault_policy_applied)));
        });
    add(policy, apply, 0);
    card(content, policy, false);
    LinearLayout defaults = vertical();
    note(defaults, AppText.get(R.string.msg_defaults_restored_after_reboot), AppText.get(R.string.msg_by_default_oops_does_not_continue_and));
    card(content, defaults, true);
  }

  private void featureUnavailable(String message) {
    LinearLayout content = scrollPage();
    add(content, text(message, 22, true), 12);
    add(content, secondary(AppText.get(R.string.msg_this_feature_is_not_enabled_in_the)), 0);
  }

  private interface Operation {
    void run() throws Exception;
  }

  private static final class State {
    KernelRepository.Identity identity;
    KernelRepository.CpuState cpu;
    Integer ttl;
    Boolean sukisuNextBoot;
    KernelRepository.SukisuChallenge sukisuChallenge;
  }

  private State readState() throws Exception {
    State state = new State();
    if (BuildConfig.DEBUG && getIntent().getBooleanExtra("simulate_unsupported", false))
      throw new Exception(AppText.get(R.string.msg_this_kernel_does_not_support_api_1));
    state.identity = repository.identity();
    if (state.identity.has("cpu_control"))
      try {
        state.cpu = repository.cpus();
      } catch (Exception ignored) {
      }
    if (state.identity.has("ttl_control"))
      try {
        state.ttl = repository.ttl();
      } catch (Exception ignored) {
      }
    if (state.identity.has("sukisu_signed_boot")) {
      try {
        state.sukisuChallenge = repository.sukisuChallenge();
      } catch (Exception ignored) {
      }
    } else if (state.identity.has("sukisu_next_boot")) {
      try {
        state.sukisuNextBoot = repository.sukisuNextBoot();
      } catch (Exception ignored) {
      }
    }
    return state;
  }

  private void applyState(State state) {
    identity = state.identity;
    cpuState = state.cpu;
    ttl = state.ttl;
    sukisuNextBoot = state.sukisuNextBoot;
    sukisuChallenge = state.sukisuChallenge;
    syncSuNav();
  }

  private void loadState(boolean first) {
    progress.setVisibility(View.VISIBLE);
    io.execute(
        () -> {
          try {
            State state = readState();
            main.post(
                () -> {
                  if (destroyed) return;
                  applyState(state);
                  compatibilityError = "";
                  progress.setVisibility(View.INVISIBLE);
                  if (destination != R.id.nav_process || adapter == null) showPage();
                  if (first) refreshProcesses();
                });
          } catch (Exception failure) {
            main.post(
                () -> {
                  if (destroyed) return;
                  identity = null;
                  syncSuNav();
                  compatibilityError = AppText.get(R.string.msg_cannot_connect_to_a_compatible_kernel) + KernelRepository.error(failure);
                  progress.setVisibility(View.INVISIBLE);
                  showPage();
                });
          }
        });
  }

  private void action(Operation operation, String success) {
    if (busy || identity == null) return;
    busy = true;
    progress.setVisibility(View.VISIBLE);
    io.execute(
        () -> {
          Exception error = null;
          try {
            operation.run();
          } catch (Exception failure) {
            error = failure;
          }
          State state = null;
          try {
            state = readState();
          } catch (Exception ignored) {
          }
          State result = state;
          Exception failure = error;
          main.post(
              () -> {
                if (destroyed) return;
                busy = false;
                progress.setVisibility(View.INVISIBLE);
                if (result != null) applyState(result);
                showPage();
                snack(failure == null ? success : KernelRepository.error(failure));
              });
        });
  }

  private void refreshProcesses() {
    if (refreshing || identity == null || !identity.has("process_info")) return;
    refreshing = true;
    io.execute(
        () -> {
          try {
            List<ProcessEntry> result = repository.processes();
            main.post(
                () -> {
                  if (destroyed) return;
                  refreshing = false;
                  processes = result;
                  lastRead = System.currentTimeMillis();
                  if (destination == R.id.nav_process) filterProcesses();
                  else if (destination == R.id.nav_home && !busy) showPage();
                });
          } catch (Exception failure) {
            main.post(
                () -> {
                  if (destroyed) return;
                  refreshing = false;
                  if (processSummary != null) processSummary.setText(AppText.get(R.string.msg_read_failed_tap_refresh_at_the_top));
                  snack(KernelRepository.error(failure));
                });
          }
        });
  }

  private void snack(String message) {
    if (!destroyed) Snackbar.make(root, message, Snackbar.LENGTH_LONG).setAnchorView(bottom).show();
  }

  private void chooseTheme() {
    String[] choices = {AppText.get(R.string.msg_follow_system), AppText.get(R.string.msg_light), AppText.get(R.string.msg_dark)};
    int[] modes = {
      AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM,
      AppCompatDelegate.MODE_NIGHT_NO,
      AppCompatDelegate.MODE_NIGHT_YES
    };
    int current = getSharedPreferences("ui", 0).getInt("theme", modes[0]), selected = 0;
    for (int i = 0; i < 3; i++) if (modes[i] == current) selected = i;
    new MaterialAlertDialogBuilder(this)
        .setTitle(AppText.get(R.string.msg_appearance))
        .setSingleChoiceItems(
            choices,
            selected,
            (dialog, which) -> {
              getSharedPreferences("ui", 0).edit().putInt("theme", modes[which]).apply();
              dialog.dismiss();
              AppCompatDelegate.setDefaultNightMode(modes[which]);
            })
        .setNegativeButton(AppText.get(R.string.msg_cancel), null)
        .show();
  }

  private void about() {
    new MaterialAlertDialogBuilder(this)
        .setTitle(getString(R.string.app_name) + " " + BuildConfig.VERSION_NAME)
        .setMessage(
            AppText.get(R.string.msg_a_no_root_console_for_the_madebyqweovo))
        .setPositiveButton(AppText.get(R.string.msg_got_it), null)
        .show();
  }

  @Override
  protected void onResume() {
    super.onResume();
    if (systemLanguage != null && !systemLanguage.equals(AppLocale.systemUiLanguage())) {
      recreate();
      return;
    }
    if (identity != null && !busy) loadState(false);
    foreground = true;
    main.removeCallbacks(ticker);
    main.postDelayed(ticker, 3000);
  }

  @Override
  protected void onPause() {
    foreground = false;
    main.removeCallbacks(ticker);
    super.onPause();
  }

  @Override
  public void onSaveInstanceState(@NonNull Bundle out) {
    out.putInt("destination", destination);
    out.putString("query", query);
    out.putInt("filter", filter);
    out.putInt("sort", sort);
    out.putBoolean("auto", autoRefresh);
    out.putString("suQuery", suQuery);
    super.onSaveInstanceState(out);
  }

  @Override
  protected void onDestroy() {
    destroyed = true;
    if (credentialRequest != null) credentialRequest.cancel();
    main.removeCallbacksAndMessages(null);
    io.shutdown();
    icons.shutdownNow();
    super.onDestroy();
  }

  private final class SuAdapter
      extends ListAdapter<KernelRepository.MeowSuApp, SuHolder> {
    SuAdapter() {
      super(
          new DiffUtil.ItemCallback<KernelRepository.MeowSuApp>() {
            public boolean areItemsTheSame(
                @NonNull KernelRepository.MeowSuApp a, @NonNull KernelRepository.MeowSuApp b) {
              return a.name().equals(b.name());
            }

            public boolean areContentsTheSame(
                @NonNull KernelRepository.MeowSuApp a, @NonNull KernelRepository.MeowSuApp b) {
              return a.label().equals(b.label()) && a.allowed() == b.allowed();
            }
          });
    }

    @NonNull
    public SuHolder onCreateViewHolder(@NonNull ViewGroup parent, int type) {
      MaterialCardView card = new MaterialCardView(MainActivity.this);
      card.setRadius(dp(20));
      card.setStrokeWidth(0);
      card.setCardElevation(0);
      card.setCardBackgroundColor(color(com.google.android.material.R.attr.colorSurface));
      RecyclerView.LayoutParams params = new RecyclerView.LayoutParams(-1, -2);
      params.bottomMargin = dp(8);
      card.setLayoutParams(params);
      LinearLayout row = horizontal();
      row.setPadding(dp(16), dp(14), dp(16), dp(14));
      ImageView icon = new ImageView(MainActivity.this);
      LinearLayout.LayoutParams iconParams = new LinearLayout.LayoutParams(dp(42), dp(42));
      iconParams.rightMargin = dp(14);
      row.addView(icon, iconParams);
      LinearLayout labels = vertical();
      TextView name = text("", 16, true), meta = secondary("");
      name.setSingleLine(true);
      name.setEllipsize(TextUtils.TruncateAt.END);
      meta.setSingleLine(true);
      meta.setEllipsize(TextUtils.TruncateAt.MIDDLE);
      add(labels, name, 2);
      add(labels, meta, 0);
      row.addView(labels, new LinearLayout.LayoutParams(0, -2, 1));
      MaterialSwitch toggle = new MaterialSwitch(MainActivity.this);
      row.addView(toggle);
      card.addView(row);
      return new SuHolder(card, icon, name, meta, toggle);
    }

    public void onBindViewHolder(@NonNull SuHolder holder, int position) {
      KernelRepository.MeowSuApp app = getItem(position);
      holder.name.setText(app.label());
      holder.meta.setText(app.name());
      holder.toggle.setOnCheckedChangeListener(null);
      holder.toggle.setChecked(app.allowed());
      holder.toggle.setEnabled(true);
      holder.toggle.setContentDescription(
          getString(R.string.su_toggle_description, app.label()));
      holder.itemView.setOnClickListener(
          view -> {
            if (holder.toggle.isEnabled()) holder.toggle.toggle();
          });
      holder.toggle.setOnCheckedChangeListener(
          (view, checked) -> {
            if (checked == app.allowed()) return;
            view.setEnabled(false);
            io.execute(
                () -> {
                  try {
                    boolean actual = repository.setMeowSuAllowed(app.name(), checked);
                    main.post(
                        () -> {
                          if (destroyed) return;
                          updateSuApp(app.name(), actual);
                        });
                  } catch (Exception failure) {
                    main.post(
                        () -> {
                          if (destroyed) return;
                          view.setChecked(app.allowed());
                          view.setEnabled(true);
                          snack(KernelRepository.error(failure));
                        });
                  }
                });
          });
      bindSuIcon(holder.icon, app);
    }
  }

  private static final class SuHolder extends RecyclerView.ViewHolder {
    final ImageView icon;
    final TextView name, meta;
    final MaterialSwitch toggle;

    SuHolder(View view, ImageView icon, TextView name, TextView meta, MaterialSwitch toggle) {
      super(view);
      this.icon = icon;
      this.name = name;
      this.meta = meta;
      this.toggle = toggle;
    }
  }

  private final class ProcessAdapter extends ListAdapter<ProcessEntry, ProcessHolder> {
    ProcessAdapter() {
      super(
          new DiffUtil.ItemCallback<ProcessEntry>() {
            public boolean areItemsTheSame(@NonNull ProcessEntry a, @NonNull ProcessEntry b) {
              return a.key.equals(b.key);
            }

            public boolean areContentsTheSame(@NonNull ProcessEntry a, @NonNull ProcessEntry b) {
              return a.sameDisplay(b);
            }
          });
    }

    @NonNull
    public ProcessHolder onCreateViewHolder(@NonNull ViewGroup parent, int type) {
      MaterialCardView card = new MaterialCardView(MainActivity.this);
      card.setRadius(dp(20));
      card.setStrokeWidth(0);
      card.setCardElevation(0);
      card.setCardBackgroundColor(color(com.google.android.material.R.attr.colorSurface));
      RecyclerView.LayoutParams params = new RecyclerView.LayoutParams(-1, -2);
      params.bottomMargin = dp(8);
      card.setLayoutParams(params);
      LinearLayout row = horizontal();
      row.setPadding(dp(16), dp(16), dp(16), dp(16));
      ImageView icon = new ImageView(MainActivity.this);
      LinearLayout.LayoutParams iconParams = new LinearLayout.LayoutParams(dp(40), dp(40));
      iconParams.rightMargin = dp(14);
      row.addView(icon, iconParams);
      LinearLayout labels = vertical();
      TextView name = text("", 16, true), description = secondary(""), meta = secondary("");
      name.setSingleLine(true);
      name.setEllipsize(TextUtils.TruncateAt.END);
      description.setSingleLine(true);
      description.setEllipsize(TextUtils.TruncateAt.MIDDLE);
      add(labels, name, 3);
      add(labels, description, 5);
      add(labels, meta, 0);
      row.addView(labels, new LinearLayout.LayoutParams(0, -2, 1));
      TextView priority = text("", 13, true);
      priority.setTextColor(primary());
      priority.setPadding(dp(10), 0, 0, 0);
      row.addView(priority);
      card.addView(row);
      return new ProcessHolder(card, icon, name, description, meta, priority);
    }

    public void onBindViewHolder(@NonNull ProcessHolder holder, int position) {
      ProcessEntry entry = getItem(position);
      holder.name.setText(entry.label);
      TaskRecord task = entry.displayTask();
      holder.description.setText(task.argv0.isEmpty() ? task.comm : task.argv0);
      holder.meta.setText(
          "PID " + entry.pid + "   ·   " + entry.threads.size() + AppText.get(R.string.msg_threads_uid) + task.uid);
      holder.priority.setText(entry.niceLabel());
      holder.itemView.setContentDescription(
          entry.label + "，PID " + entry.pid + "，" + entry.niceLabel());
      holder.itemView.setOnClickListener(v -> showPriority(entry));
      bindIcon(holder.icon, entry);
    }
  }

  private static final class ProcessHolder extends RecyclerView.ViewHolder {
    final ImageView icon;
    final TextView name, description, meta, priority;

    ProcessHolder(
        View view,
        ImageView icon,
        TextView name,
        TextView description,
        TextView meta,
        TextView priority) {
      super(view);
      this.icon = icon;
      this.name = name;
      this.description = description;
      this.meta = meta;
      this.priority = priority;
    }
  }

  private void bindIcon(ImageView image, ProcessEntry entry) {
    image.setTag(entry.key);
    image.setImageResource(R.drawable.ic_process);
    image.setImageTintList(ColorStateList.valueOf(primary()));
    if (entry.packageName.isEmpty()) return;
    Bitmap cached = iconCache.get(entry.packageName);
    if (cached != null) {
      image.setImageTintList(null);
      image.setImageBitmap(cached);
      return;
    }
    icons.execute(
        () -> {
          try {
            Bitmap bitmap = iconCache.get(entry.packageName);
            if (bitmap == null) {
              Drawable drawable = getPackageManager().getApplicationIcon(entry.packageName);
              bitmap = Bitmap.createBitmap(112, 112, Bitmap.Config.ARGB_8888);
              Canvas canvas = new Canvas(bitmap);
              drawable.setBounds(0, 0, 112, 112);
              drawable.draw(canvas);
              iconCache.put(entry.packageName, bitmap);
            }
            Bitmap result = bitmap;
            main.post(
                () -> {
                  if (!destroyed && entry.key.equals(image.getTag())) {
                    image.setImageTintList(null);
                    image.setImageBitmap(result);
                  }
                });
          } catch (Exception ignored) {
          }
        });
  }

  private void showPriority(ProcessEntry entry) {
    detailOpen = true;
    new PrioritySheet(
            this,
            repository,
            io,
            entry,
            new PrioritySheet.Listener() {
              public boolean alive() {
                return !destroyed;
              }

              public void snapshot(List<ProcessEntry> entries) {
                processes = entries;
                lastRead = System.currentTimeMillis();
                filterProcesses();
              }

              public void dismissed() {
                detailOpen = false;
                refreshProcesses();
              }
            })
        .show();
  }

  @Override
  protected void onNewIntent(Intent intent) {
    super.onNewIntent(intent);
    setIntent(intent);
    if (BuildConfig.DEBUG) renderDebugRequest(intent, 20);
  }

  /** Draws this App's own native Views for QA when the device display is asleep. */
  private void renderDebugRequest(Intent intent, int attempts) {
    String screen = intent.getStringExtra("render_page");
    if (screen == null) return;
    if (identity == null
        && attempts > 0
        && !intent.getBooleanExtra("simulate_unsupported", false)) {
      main.postDelayed(() -> renderDebugRequest(intent, attempts - 1), 250);
      return;
    }
    destination =
        switch (screen) {
          case "processes" -> R.id.nav_process;
          case "controls" -> R.id.nav_controls;
          case "debug" -> R.id.nav_debug;
          default -> R.id.nav_home;
        };
    showPage();
    if (identity != null
        && destination == R.id.nav_process
        && processes.isEmpty()
        && attempts > 0) {
      refreshProcesses();
      main.postDelayed(() -> renderDebugRequest(intent, attempts - 1), 250);
      return;
    }
    main.postDelayed(
        () -> {
          if (destroyed) return;
          try {
            int width =
                intent.getIntExtra("preview_width_dp", 0) > 0
                    ? dp(intent.getIntExtra("preview_width_dp", 411))
                    : getResources().getDisplayMetrics().widthPixels;
            int height =
                intent.getIntExtra("preview_height_dp", 0) > 0
                    ? dp(intent.getIntExtra("preview_height_dp", 850))
                    : getResources().getDisplayMetrics().heightPixels;
            root.measure(
                View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(height, View.MeasureSpec.EXACTLY));
            root.layout(0, 0, width, height);
            String name = intent.getStringExtra("render_name");
            if (name == null || !name.matches("[A-Za-z0-9_-]+")) name = "preview";
            Bitmap bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            root.draw(new Canvas(bitmap));
            try (FileOutputStream output =
                new FileOutputStream(new File(getFilesDir(), name + ".png"))) {
              bitmap.compress(Bitmap.CompressFormat.PNG, 100, output);
            }
            bitmap.recycle();
            if (intent.getBooleanExtra("render_priority", false)) {
              for (ProcessEntry entry : processes)
                if (entry.pid == android.os.Process.myPid()) {
                  PrioritySheet sheet =
                      new PrioritySheet(
                          this,
                          repository,
                          io,
                          entry,
                          new PrioritySheet.Listener() {
                            public boolean alive() {
                              return !destroyed;
                            }

                            public void snapshot(List<ProcessEntry> entries) {
                              processes = entries;
                              filterProcesses();
                            }

                            public void dismissed() {
                              detailOpen = false;
                            }
                          });
                  detailOpen = true;
                  sheet.show();
                  main.postDelayed(
                      () -> {
                        try {
                          sheet.capture(new File(getFilesDir(), "priority-preview.png"));
                        } catch (Exception error) {
                          android.util.Log.e(getString(R.string.app_name), "Preview", error);
                        }
                      },
                      350);
                  break;
                }
            }
          } catch (Exception error) {
            android.util.Log.e(getString(R.string.app_name), "Preview", error);
          }
        },
        700);
  }
}
