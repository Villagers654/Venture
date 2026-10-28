package android.widget;
import android.database.DataSetObserver;
import android.view.*;
public abstract class BaseAdapter implements ListAdapter, SpinnerAdapter {
  private final java.util.List<DataSetObserver> observers =
      new java.util.concurrent.CopyOnWriteArrayList<>();
  public void registerDataSetObserver(DataSetObserver observer) {
    observers.add(java.util.Objects.requireNonNull(observer));
  }
  public void unregisterDataSetObserver(DataSetObserver observer) {
    observers.remove(observer);
  }
  public void notifyDataSetChanged() {
    for (var observer : observers) observer.onChanged();
  }
  public void notifyDataSetInvalidated() {
    for (var observer : observers) observer.onInvalidated();
  }
  public boolean hasStableIds() {
    return false;
  }
  public int getItemViewType(int position) {
    return 0;
  }
  public int getViewTypeCount() {
    return 1;
  }
  public boolean isEmpty() {
    return getCount() == 0;
  }
  public boolean areAllItemsEnabled() {
    return true;
  }
  public boolean isEnabled(int position) {
    return true;
  }
  public View getDropDownView(int position, View convertView, ViewGroup parent) {
    return getView(position, convertView, parent);
  }
}
