package android.view.accessibility;

import android.graphics.Rect;
import android.os.*;
import android.view.View;

/** In-process node values; no remote accessibility service is exposed. */
public class AccessibilityNodeInfo implements Parcelable {
  public static final int FOCUS_INPUT = 1, FOCUS_ACCESSIBILITY = 2;
  private CharSequence text, description, className, packageName;
  private final Bundle extras = new Bundle();
  private final Rect screen = new Rect(), parentBounds = new Rect();
  private View source, parent;
  private int virtualId = -1, parentId = -1, actions;
  private boolean enabled, focusable, focused, accessibilityFocused, clickable, selected, checkable,
      checked, scrollable, password, visibleToUser;
  public static AccessibilityNodeInfo obtain() {
    return new AccessibilityNodeInfo();
  }
  public static AccessibilityNodeInfo obtain(View source) {
    return obtain(source, -1);
  }
  public static AccessibilityNodeInfo obtain(View source, int virtualId) {
    var node = obtain();
    node.setSource(source, virtualId);
    return node;
  }
  public void recycle() {} // Values are owned by ordinary Java GC, with no external handle.
  public void setSource(View value) {
    setSource(value, -1);
  }
  public void setSource(View value, int id) {
    source = value;
    virtualId = id;
  }
  public void setParent(View value) {
    setParent(value, -1);
  }
  public void setParent(View value, int id) {
    parent = value;
    parentId = id;
  }
  public void setText(CharSequence value) {
    text = value;
  }
  public CharSequence getText() {
    return text;
  }
  public void setContentDescription(CharSequence value) {
    description = value;
  }
  public CharSequence getContentDescription() {
    return description;
  }
  public void setClassName(CharSequence value) {
    className = value;
  }
  public CharSequence getClassName() {
    return className;
  }
  public void setPackageName(CharSequence value) {
    packageName = value;
  }
  public CharSequence getPackageName() {
    return packageName;
  }
  public Bundle getExtras() {
    return extras;
  }
  private static void copy(Rect out, Rect value) {
    out.set(value.left, value.top, value.right, value.bottom);
  }
  public void setBoundsInScreen(Rect value) {
    copy(screen, value);
  }
  public void getBoundsInScreen(Rect out) {
    copy(out, screen);
  }
  public void setBoundsInParent(Rect value) {
    copy(parentBounds, value);
  }
  public void getBoundsInParent(Rect out) {
    copy(out, parentBounds);
  }
  public void addAction(int action) {
    actions |= action;
  }
  public int getActions() {
    return actions;
  }
  public void setEnabled(boolean value) {
    enabled = value;
  }
  public boolean isEnabled() {
    return enabled;
  }
  public void setFocusable(boolean value) {
    focusable = value;
  }
  public boolean isFocusable() {
    return focusable;
  }
  public void setFocused(boolean value) {
    focused = value;
  }
  public boolean isFocused() {
    return focused;
  }
  public void setAccessibilityFocused(boolean value) {
    accessibilityFocused = value;
  }
  public boolean isAccessibilityFocused() {
    return accessibilityFocused;
  }
  public void setClickable(boolean value) {
    clickable = value;
  }
  public boolean isClickable() {
    return clickable;
  }
  public void setSelected(boolean value) {
    selected = value;
  }
  public boolean isSelected() {
    return selected;
  }
  public void setCheckable(boolean value) {
    checkable = value;
  }
  public boolean isCheckable() {
    return checkable;
  }
  public void setChecked(boolean value) {
    checked = value;
  }
  public boolean isChecked() {
    return checked;
  }
  public void setScrollable(boolean value) {
    scrollable = value;
  }
  public boolean isScrollable() {
    return scrollable;
  }
  public void setPassword(boolean value) {
    password = value;
  }
  public boolean isPassword() {
    return password;
  }
  public void setVisibleToUser(boolean value) {
    visibleToUser = value;
  }
  public boolean isVisibleToUser() {
    return visibleToUser;
  }
  public int describeContents() {
    return 0;
  }
  public void writeToParcel(Parcel dest, int flags) {
    dest.writeParcelable(this, flags);
  }
}
