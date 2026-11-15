package io.qweovo.manager;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.*;
import android.widget.*;
import androidx.core.content.ContextCompat;
import com.google.android.material.bottomsheet.*;
import com.google.android.material.button.*;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.slider.Slider;
import com.google.android.material.textfield.*;
import java.io.File;
import java.io.FileOutputStream;
import java.util.*;
import java.util.concurrent.ExecutorService;

/** A detail view remains bound to one process lifetime, even after refresh. */
final class PrioritySheet {
  interface Listener {
    boolean alive();

    void snapshot(List<ProcessEntry> entries);

    void dismissed();
  }

  private final Context context;
  private final KernelRepository repository;
  private final ExecutorService executor;
  private final Listener listener;
  private final Handler main = new Handler(Looper.getMainLooper());
  private final BottomSheetDialog dialog;
  private final String processKey;
  private ProcessEntry entry;
  private TaskRecord selected;
  private boolean whole = true, working, ended;
  private TextView value, scopeInfo, result, subtitle;
  private Slider slider;
  private MaterialSwitch lockSwitch;
  private MaterialButton apply, refresh, processButton, threadButton;
  private TextInputLayout threadBox;
  private MaterialAutoCompleteTextView picker;
  private ArrayAdapter<String> threadAdapter;
  private final ArrayList<String> threadNames = new ArrayList<>();
  private final List<MaterialButton> presets = new ArrayList<>();

  PrioritySheet(
      Context context,
      KernelRepository repository,
      ExecutorService executor,
      ProcessEntry entry,
      Listener listener) {
    this.context = context;
    this.repository = repository;
    this.executor = executor;
    this.entry = entry;
    this.listener = listener;
    processKey = entry.key;
    selected = entry.main != null ? entry.main : entry.threads.get(0);
    dialog = new BottomSheetDialog(context);
  }

  private int dp(float n) {
    return (int) (n * context.getResources().getDisplayMetrics().density + .5f);
  }

  private int color(int attribute) {
    TypedValue v = new TypedValue();
    context.getTheme().resolveAttribute(attribute, v, true);
    return v.resourceId == 0 ? v.data : ContextCompat.getColor(context, v.resourceId);
  }

  private TextView text(String label, int size, boolean bold) {
    TextView v = new TextView(context);
    v.setText(label);
    v.setTextSize(size);
    v.setTextColor(color(com.google.android.material.R.attr.colorOnSurface));
    v.setLineSpacing(dp(3), 1);
    if (bold) v.setTypeface(null, Typeface.BOLD);
    return v;
  }

  private TextView muted(String label) {
    TextView v = text(label, 13, false);
    v.setTextColor(color(com.google.android.material.R.attr.colorOnSurfaceVariant));
    return v;
  }

  private LinearLayout row() {
    LinearLayout v = new LinearLayout(context);
    v.setOrientation(LinearLayout.HORIZONTAL);
    v.setGravity(Gravity.CENTER_VERTICAL);
    return v;
  }

  private void add(LinearLayout parent, View child, int margin) {
    LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
    p.bottomMargin = dp(margin);
    parent.addView(child, p);
  }

  private MaterialButton button(String label, boolean filled) {
    MaterialButton b =
        new MaterialButton(
            context,
            null,
            filled
                ? com.google.android.material.R.attr.materialButtonStyle
                : com.google.android.material.R.attr.materialButtonOutlinedStyle);
    b.setText(label);
    b.setAllCaps(false);
    b.setMinWidth(0);
    b.setCornerRadius(dp(20));
    return b;
  }

  void show() {
    ScrollView scroll = new ScrollView(context);
    LinearLayout content = new LinearLayout(context);
    content.setOrientation(LinearLayout.VERTICAL);
    content.setPadding(dp(24), dp(24), dp(24), dp(32));
    scroll.addView(content);
    dialog.setContentView(scroll);
    add(content, text(entry.label, 23, true), 6);
    subtitle = muted("");
    subtitle.setTextIsSelectable(true);
    add(content, subtitle, 18);
    lockSwitch = new MaterialSwitch(context);
    lockSwitch.setText(AppText.get(R.string.msg_lock_this_app_s_priority));
    lockSwitch.setChecked(entry.lockSupported);
    lockSwitch.setVisibility(entry.lockSupported ? View.VISIBLE : View.GONE);
    add(content, lockSwitch, 8);
    if (entry.lockSupported)
      add(content, muted(AppText.get(R.string.msg_applies_by_uid_to_existing_and_future)), 16);
    add(content, text(AppText.get(R.string.msg_scope), 14, true), 8);
    MaterialButtonToggleGroup scopes = new MaterialButtonToggleGroup(context);
    scopes.setSingleSelection(true);
    scopes.setSelectionRequired(true);
    int processId = View.generateViewId(), threadId = View.generateViewId();
    processButton = button(AppText.get(R.string.msg_whole_process), false);
    threadButton = button(AppText.get(R.string.msg_single_thread), false);
    processButton.setId(processId);
    threadButton.setId(threadId);
    scopes.addView(processButton, new LinearLayout.LayoutParams(0, dp(48), 1));
    scopes.addView(threadButton, new LinearLayout.LayoutParams(0, dp(48), 1));
    scopes.check(processId);
    add(content, scopes, 12);
    threadBox =
        new TextInputLayout(
            context, null, com.google.android.material.R.attr.textInputOutlinedStyle);
    threadBox.setHint(AppText.get(R.string.msg_select_thread));
    picker = new MaterialAutoCompleteTextView(threadBox.getContext());
    picker.setKeyListener(null);
    picker.setSingleLine(true);
    picker.setTextSize(16);
    picker.setEllipsize(android.text.TextUtils.TruncateAt.END);
    picker.setPadding(dp(16), dp(12), dp(16), dp(12));
    picker.setMinHeight(dp(60));
    picker.setOnClickListener(v -> picker.showDropDown());
    threadBox.setEndIconMode(TextInputLayout.END_ICON_DROPDOWN_MENU);
    threadAdapter = new ArrayAdapter<>(context, android.R.layout.simple_list_item_1, threadNames);
    picker.setAdapter(threadAdapter);
    threadBox.addView(picker, new LinearLayout.LayoutParams(-1, dp(60)));
    add(content, threadBox, 12);
    value = text("", 32, true);
    value.setTextColor(color(com.google.android.material.R.attr.colorPrimary));
    add(content, value, 6);
    scopeInfo = muted("");
    add(content, scopeInfo, 8);
    slider = new Slider(context);
    slider.setValueFrom(-20);
    slider.setValueTo(19);
    slider.setStepSize(1);
    slider.setValue(entry.lockedNice == null ? selected.nice : entry.lockedNice);
    slider.setTickVisible(false);
    slider.setLabelFormatter(n -> String.valueOf(Math.round(n)));
    slider.setContentDescription(AppText.get(R.string.msg_target_nice_lower_means_higher_priority));
    slider.addOnChangeListener((v, n, user) -> value.setText("nice " + Math.round(n)));
    add(content, slider, 4);
    LinearLayout scale = row();
    scale.addView(muted(AppText.get(R.string.msg_20_higher_priority)), new LinearLayout.LayoutParams(0, -2, 1));
    scale.addView(muted(AppText.get(R.string.msg_19_lower_priority)));
    add(content, scale, 16);
    LinearLayout presetRow = row();
    for (int nice : new int[] {-5, 0, 5, 10}) {
      MaterialButton b = button(nice > 0 ? "+" + nice : String.valueOf(nice), false);
      LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, dp(44), 1);
      p.rightMargin = dp(5);
      presetRow.addView(b, p);
      presets.add(b);
      b.setOnClickListener(v -> slider.setValue(nice));
    }
    add(content, presetRow, 16);
    add(content, muted(AppText.get(R.string.msg_lower_nice_means_higher_scheduling_priority_without)), 16);
    LinearLayout footer = row();
    refresh = button(AppText.get(R.string.msg_refresh_state), false);
    apply = button(AppText.get(R.string.msg_apply_priority), true);
    LinearLayout.LayoutParams left = new LinearLayout.LayoutParams(0, dp(52), 1);
    left.rightMargin = dp(8);
    footer.addView(refresh, left);
    footer.addView(apply, new LinearLayout.LayoutParams(0, dp(52), 2));
    add(content, footer, 8);
    result = muted(AppText.get(R.string.msg_the_target_is_checked_before_writing_actual));
    add(content, result, 0);
    scopes.addOnButtonCheckedListener(
        (group, id, checked) -> {
          if (checked) {
            whole = id == processId;
            TaskRecord task = target();
            if (task != null) slider.setValue(task.nice);
            update();
          }
        });
    picker.setOnItemClickListener(
        (parent, view, position, id) -> {
          selected = entry.threads.get(position);
          slider.setValue(selected.nice);
          update();
        });
    refresh.setOnClickListener(v -> refreshTarget());
    apply.setOnClickListener(v -> apply());
    lockSwitch.setOnCheckedChangeListener((v, checked) -> update());
    rebuildThreadChoices();
    update();
    dialog.setOnDismissListener(v -> listener.dismissed());
    dialog.setOnShowListener(
        v -> {
          FrameLayout sheet =
              dialog.findViewById(com.google.android.material.R.id.design_bottom_sheet);
          if (sheet != null) {
            sheet.setBackgroundTintList(
                android.content.res.ColorStateList.valueOf(
                    color(com.google.android.material.R.attr.colorSurface)));
            if (context.getResources().getConfiguration().screenWidthDp >= 600) {
              ViewGroup.LayoutParams p = sheet.getLayoutParams();
              p.width = dp(560);
              sheet.setLayoutParams(p);
            }
            BottomSheetBehavior<FrameLayout> behavior = BottomSheetBehavior.from(sheet);
            behavior.setSkipCollapsed(true);
            behavior.setState(BottomSheetBehavior.STATE_EXPANDED);
          }
        });
    dialog.show();
  }

  private TaskRecord target() {
    return whole ? entry.main : selected;
  }

  private void rebuildThreadChoices() {
    threadNames.clear();
    for (TaskRecord task : entry.threads)
      threadNames.add(task.comm + " · TID " + task.tid + " · nice " + task.nice);
    threadAdapter.notifyDataSetChanged();
    int index = selected == null ? -1 : entry.threads.indexOf(selected);
    picker.setText(index < 0 ? AppText.get(R.string.msg_the_original_thread_exited_select_another_thread) : threadNames.get(index), false);
  }

  private void update() {
    TaskRecord task = target();
    boolean uidMode = entry.lockSupported && (lockSwitch.isChecked() || entry.lockedNice != null);
    boolean allowed = !working && !ended && (uidMode || (task != null && task.adjustable()));
    boolean removing = uidMode && !lockSwitch.isChecked();
    slider.setEnabled(allowed && !removing);
    apply.setEnabled(allowed);
    apply.setText(uidMode ? (removing ? AppText.get(R.string.msg_unlock_uid) : AppText.get(R.string.msg_apply_and_lock_uid)) : AppText.get(R.string.msg_apply_priority));
    lockSwitch.setEnabled(!working && !ended && entry.lockSupported);
    refresh.setEnabled(!working);
    processButton.setEnabled(!working && !uidMode);
    threadButton.setEnabled(!working && !uidMode);
    picker.setEnabled(!working && !ended);
    for (MaterialButton b : presets) b.setEnabled(allowed && !removing);
    threadBox.setVisibility(whole || uidMode ? View.GONE : View.VISIBLE);
    value.setText("nice " + Math.round(slider.getValue()));
    subtitle.setText(
        "PID "
            + entry.pid
            + " · UID "
            + entry.displayTask().uid
            + " · "
            + entry.threads.size()
            + AppText.get(R.string.msg_threads)
            + entry.displayTask().argv0);
    scopeInfo.setText(
        ended
            ? AppText.get(R.string.msg_this_process_exited_its_old_pid_will)
            : uidMode
                ? (entry.lockedNice == null ? AppText.get(R.string.msg_this_uid_is_not_locked) : AppText.get(R.string.msg_this_uid_is_locked_at_nice) + entry.lockedNice)
                    + (removing ? AppText.get(R.string.msg_unlocking_keeps_the_current_value_and_allows) : AppText.get(R.string.msg_applies_to_this_uid_s_normal_batch))
                : task == null
                    ? AppText.get(R.string.msg_the_current_target_is_unavailable_select_another)
                    : task.policyName()
                        + AppText.get(R.string.msg_current_nice)
                        + task.nice
                        + (whole && entry.minNice != entry.maxNice ? AppText.get(R.string.msg_thread_values_differ) : ""));
  }

  private void acceptSnapshot(List<ProcessEntry> snapshot) {
    listener.snapshot(snapshot);
    ProcessEntry live = null;
    for (ProcessEntry p : snapshot)
      if (p.key.equals(processKey)) {
        live = p;
        break;
      }
    if (live == null) {
      ended = true;
      return;
    }
    String oldKey = selected == null ? "" : selected.key();
    entry = live;
    selected = null;
    for (TaskRecord task : entry.threads)
      if (task.key().equals(oldKey)) {
        selected = task;
        break;
      }
    rebuildThreadChoices();
  }

  private void refreshTarget() {
    working = true;
    update();
    executor.execute(
        () -> {
          List<ProcessEntry> snapshot = null;
          Exception error = null;
          try {
            snapshot = repository.processes();
          } catch (Exception e) {
            error = e;
          }
          List<ProcessEntry> fresh = snapshot;
          Exception failure = error;
          main.post(
              () -> {
                if (!listener.alive()) return;
                working = false;
                if (fresh != null) acceptSnapshot(fresh);
                if (!dialog.isShowing()) return;
                TaskRecord task = target();
                if (entry.lockedNice != null) slider.setValue(entry.lockedNice);
                else if (task != null) slider.setValue(task.nice);
                update();
                result.setText(
                    failure == null
                        ? (ended ? AppText.get(R.string.msg_this_process_exited_return_to_the_list) : AppText.get(R.string.msg_actual_state_refreshed))
                        : KernelRepository.error(failure));
              });
        });
  }

  private void apply() {
    TaskRecord task = target();
    if (working || ended) return;
    boolean lock = entry.lockSupported && lockSwitch.isChecked();
    boolean unlock = entry.lockSupported && !lock && entry.lockedNice != null;
    if (task == null && !lock && !unlock) return;
    boolean group = whole;
    long uid = entry.displayTask().uid;
    int id = group ? entry.pid : task == null ? 0 : task.tid;
    String marker = group ? entry.startNs : task == null ? "" : task.startNs;
    int nice = Math.round(slider.getValue());
    working = true;
    update();
    result.setText(AppText.get(R.string.msg_checking_target_and_applying));
    executor.execute(
        () -> {
          KernelRepository.NiceResult reply = null;
          Exception error = null;
          List<ProcessEntry> snapshot = null;
          try {
            if (lock) repository.lockUid(uid, nice);
            else if (unlock) repository.unlockUid(uid);
            else reply = repository.nice(group, id, marker, nice);
          } catch (Exception e) {
            error = e;
          }
          try {
            snapshot = repository.processes();
          } catch (Exception ignored) {
          }
          KernelRepository.NiceResult outcome = reply;
          Exception failure = error;
          List<ProcessEntry> fresh = snapshot;
          main.post(
              () -> {
                if (!listener.alive()) return;
                working = false;
                if (fresh != null) acceptSnapshot(fresh);
                if (!dialog.isShowing()) return;
                TaskRecord current = target();
                if (entry.lockedNice != null) slider.setValue(entry.lockedNice);
                else if (current != null) slider.setValue(current.nice);
                update();
                if (failure != null) result.setText(KernelRepository.error(failure));
                else if (lock) result.setText("UID " + uid + AppText.get(R.string.msg_locked_at_nice) + nice + AppText.get(R.string.msg_remains_effective_after_the_app_restarts));
                else if (unlock) result.setText(AppText.get(R.string.msg_uid_unlocked_current_nice_is_retained_the));
                else if (outcome != null && outcome.success())
                  result.setText(AppText.get(R.string.msg_processed) + outcome.updated + AppText.get(R.string.msg_threads_actual_state_refreshed));
                else if (outcome != null)
                  result.setText(
                      (outcome.updated > 0 ? AppText.get(R.string.msg_processed) + outcome.updated + AppText.get(R.string.msg_threads_161) : "")
                          + KernelRepository.error(outcome.error));
              });
        });
  }

  void capture(File output) throws Exception {
    View sheet = dialog.findViewById(com.google.android.material.R.id.design_bottom_sheet);
    if (sheet == null) throw new IllegalStateException("Sheet not created");
    int width = Math.min(dp(560), context.getResources().getDisplayMetrics().widthPixels);
    sheet.measure(
        View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
        View.MeasureSpec.makeMeasureSpec(dp(850), View.MeasureSpec.AT_MOST));
    sheet.layout(0, 0, width, sheet.getMeasuredHeight());
    Bitmap bitmap = Bitmap.createBitmap(width, sheet.getMeasuredHeight(), Bitmap.Config.ARGB_8888);
    sheet.draw(new Canvas(bitmap));
    try (FileOutputStream file = new FileOutputStream(output)) {
      bitmap.compress(Bitmap.CompressFormat.PNG, 100, file);
    }
    bitmap.recycle();
  }
}
