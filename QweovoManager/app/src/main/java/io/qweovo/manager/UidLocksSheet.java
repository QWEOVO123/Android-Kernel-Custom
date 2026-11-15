package io.qweovo.manager;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.widget.*;
import com.google.android.material.bottomsheet.BottomSheetDialog;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.slider.Slider;
import java.util.*;
import java.util.concurrent.ExecutorService;

/** Lists kernel rules, including UIDs with no running processes. */
final class UidLocksSheet {
  private final Context context;
  private final KernelRepository repository;
  private final ExecutorService executor;
  private final Runnable dismissed;
  private final Handler main = new Handler(Looper.getMainLooper());
  private final BottomSheetDialog dialog;
  private final LinearLayout content;
  private boolean busy;

  UidLocksSheet(
      Context context, KernelRepository repository, ExecutorService executor, Runnable dismissed) {
    this.context = context;
    this.repository = repository;
    this.executor = executor;
    this.dismissed = dismissed;
    dialog = new BottomSheetDialog(context);
    content = new LinearLayout(context);
    content.setOrientation(LinearLayout.VERTICAL);
    content.setPadding(dp(24), dp(20), dp(24), dp(28));
    ScrollView scroll = new ScrollView(context);
    scroll.addView(content);
    dialog.setContentView(scroll);
    dialog.setOnDismissListener(v -> dismissed.run());
  }

  private int dp(int n) {
    return Math.round(n * context.getResources().getDisplayMetrics().density);
  }

  private void text(String label, int size) {
    TextView v = new TextView(context);
    v.setText(label);
    v.setTextSize(size);
    v.setTextIsSelectable(true);
    LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
    params.bottomMargin = dp(12);
    content.addView(v, params);
  }

  void show() {
    dialog.show();
    reload();
  }

  private void reload() {
    content.removeAllViews();
    text(AppText.get(R.string.msg_uid_priority_locks), 23);
    text(AppText.get(R.string.msg_rules_remain_after_an_app_exits_apps), 14);
    text(AppText.get(R.string.msg_reading_kernel_rules), 14);
    executor.execute(
        () -> {
          try {
            Map<Long, UidLockRule> rules = repository.locks();
            List<ProcessEntry> running = repository.processes();
            Map<Long, String> labels = new HashMap<>();
            for (long uid : rules.keySet()) labels.put(uid, repository.uidLabel(uid));
            main.post(
                () -> {
                  if (!dialog.isShowing()) return;
                  content.removeViewAt(2);
                  if (rules.isEmpty()) text(AppText.get(R.string.msg_no_locked_apps), 16);
                  for (UidLockRule rule : rules.values()) {
                    int count = 0;
                    for (ProcessEntry process : running)
                      if (process.displayTask().uid == rule.uid) count++;
                    text(labels.get(rule.uid), 18);
                    text(
                        "UID " + rule.uid + AppText.get(R.string.msg_locked_nice) + rule.nice + " · " + count + AppText.get(R.string.msg_processes_running),
                        14);
                    LinearLayout buttons = new LinearLayout(context);
                    MaterialButton edit =
                        new MaterialButton(
                            context,
                            null,
                            com.google.android.material.R.attr.materialButtonOutlinedStyle);
                    edit.setText(AppText.get(R.string.msg_edit_nice));
                    edit.setOnClickListener(v -> edit(rule));
                    MaterialButton unlock =
                        new MaterialButton(
                            context,
                            null,
                            com.google.android.material.R.attr.materialButtonOutlinedStyle);
                    unlock.setText(AppText.get(R.string.msg_unlock));
                    unlock.setOnClickListener(v -> change(rule.uid, null));
                    buttons.addView(edit, new LinearLayout.LayoutParams(0, dp(52), 1));
                    buttons.addView(unlock, new LinearLayout.LayoutParams(0, dp(52), 1));
                    LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
                    params.bottomMargin = dp(20);
                    content.addView(buttons, params);
                  }
                  MaterialButton refresh = new MaterialButton(context);
                  refresh.setText(AppText.get(R.string.msg_refresh_rules));
                  refresh.setOnClickListener(
                      v -> {
                        if (!busy) reload();
                      });
                  content.addView(refresh);
                });
          } catch (Exception e) {
            main.post(
                () -> {
                  if (!dialog.isShowing()) return;
                  content.removeViewAt(2);
                  text(KernelRepository.error(e), 14);
                });
          }
        });
  }

  private void edit(UidLockRule rule) {
    if (busy) return;
    LinearLayout box = new LinearLayout(context);
    box.setOrientation(LinearLayout.VERTICAL);
    box.setPadding(dp(24), dp(16), dp(24), 0);
    TextView value = new TextView(context);
    value.setText("nice " + rule.nice);
    value.setTextSize(24);
    box.addView(value);
    Slider slider = new Slider(context);
    slider.setValueFrom(-20);
    slider.setValueTo(19);
    slider.setStepSize(1);
    slider.setValue(rule.nice);
    slider.setTickVisible(false);
    slider.addOnChangeListener((v, n, user) -> value.setText("nice " + Math.round(n)));
    box.addView(slider);
    new MaterialAlertDialogBuilder(context)
        .setTitle("UID " + rule.uid)
        .setView(box)
        .setNegativeButton(AppText.get(R.string.msg_cancel), null)
        .setPositiveButton(AppText.get(R.string.msg_apply_and_lock), (d, which) -> change(rule.uid, Math.round(slider.getValue())))
        .show();
  }

  private void change(long uid, Integer nice) {
    if (busy) return;
    busy = true;
    executor.execute(
        () -> {
          String result;
          try {
            if (nice == null) repository.unlockUid(uid);
            else repository.lockUid(uid, nice);
            result = nice == null ? AppText.get(R.string.msg_unlocked_uid) + uid + AppText.get(R.string.msg_priority_185) : AppText.get(R.string.msg_updated_uid) + uid + AppText.get(R.string.msg_rule);
          } catch (Exception e) {
            result = KernelRepository.error(e);
          }
          String message = result;
          main.post(
              () -> {
                busy = false;
                if (!dialog.isShowing()) return;
                Toast.makeText(context, message, Toast.LENGTH_LONG).show();
                reload();
              });
        });
  }
}
