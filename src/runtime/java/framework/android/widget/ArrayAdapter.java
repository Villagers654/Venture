package android.widget;

import android.content.Context;
import android.view.*;
import java.util.*;

/** Local collection adapter; view construction uses the real layout resource. */
public class ArrayAdapter<T> extends BaseAdapter implements Filterable {
  private final Context context;
  private final int resource, textViewId;
  private int dropDownResource;
  private boolean notify = true;
  private final ArrayList<T> items;
  private ArrayList<T> unfiltered;
  private Filter filter;
  public ArrayAdapter(Context context, int resource) {
    this(context, resource, 0, new ArrayList<>());
  }
  public ArrayAdapter(Context context, int resource, int textViewId) {
    this(context, resource, textViewId, new ArrayList<>());
  }
  public ArrayAdapter(Context context, int resource, T[] items) {
    this(context, resource, 0, Arrays.asList(items));
  }
  public ArrayAdapter(Context context, int resource, int textViewId, T[] items) {
    this(context, resource, textViewId, Arrays.asList(items));
  }
  public ArrayAdapter(Context context, int resource, List<T> items) {
    this(context, resource, 0, items);
  }
  public ArrayAdapter(Context context, int resource, int textViewId, List<T> items) {
    this.context = context;
    this.resource = resource;
    this.dropDownResource = resource;
    this.textViewId = textViewId;
    this.items = new ArrayList<>(items);
  }
  public Context getContext() {
    return context;
  }
  public synchronized int getCount() {
    return items.size();
  }
  public synchronized T getItem(int position) {
    return items.get(position);
  }
  public long getItemId(int position) {
    return position;
  }
  public synchronized int getPosition(T item) {
    return items.indexOf(item);
  }
  private ArrayList<T> mutable() {
    return unfiltered == null ? items : unfiltered;
  }
  private void changed() {
    if (notify)
      notifyDataSetChanged();
  }
  public synchronized void add(T item) {
    mutable().add(item);
    changed();
  }
  public synchronized void addAll(Collection<? extends T> values) {
    mutable().addAll(values);
    changed();
  }
  @SafeVarargs
  public final void addAll(T... values) {
    addAll(Arrays.asList(values));
  }
  public synchronized void insert(T item, int index) {
    mutable().add(index, item);
    changed();
  }
  public synchronized void remove(T item) {
    mutable().remove(item);
    changed();
  }
  public synchronized void clear() {
    mutable().clear();
    changed();
  }
  public synchronized void sort(Comparator<? super T> comparator) {
    mutable().sort(comparator);
    changed();
  }
  public void setNotifyOnChange(boolean value) {
    notify = value;
  }
  public void notifyDataSetChanged() {
    super.notifyDataSetChanged();
    notify = true;
  }
  public void setDropDownViewResource(int resource) {
    dropDownResource = resource;
  }
  public View getView(int position, View convertView, ViewGroup parent) {
    return create(position, convertView, parent, resource);
  }
  public View getDropDownView(int position, View convertView, ViewGroup parent) {
    return create(position, convertView, parent, dropDownResource);
  }
  private View create(int position, View convertView, ViewGroup parent, int layout) {
    View view = convertView != null ? convertView
                                    : LayoutInflater.from(context).inflate(layout, parent, false);
    View label = textViewId == 0 ? view : view.findViewById(textViewId);
    if (!(label instanceof TextView text))
      throw new IllegalStateException("ArrayAdapter requires a TextView");
    T value = getItem(position);
    text.setText(value instanceof CharSequence chars ? chars : String.valueOf(value));
    return view;
  }
  public synchronized Filter getFilter() {
    if (filter == null)
      filter = new Filter() {
        protected FilterResults performFiltering(CharSequence constraint) {
          ArrayList<T> source;
          synchronized (ArrayAdapter.this) {
            if (unfiltered == null)
              unfiltered = new ArrayList<>(items);
            source = new ArrayList<>(unfiltered);
          }
          String prefix = constraint == null ? "" : constraint.toString().toLowerCase(Locale.ROOT);
          if (!prefix.isEmpty())
            source.removeIf(item -> {
              String value = String.valueOf(item).toLowerCase(Locale.ROOT);
              return !value.startsWith(prefix)
                  && Arrays.stream(value.split("\\s+")).noneMatch(word -> word.startsWith(prefix));
            });
          FilterResults result = new FilterResults();
          result.values = source;
          result.count = source.size();
          return result;
        }
        @SuppressWarnings("unchecked")
        protected void publishResults(CharSequence constraint, FilterResults results) {
          synchronized (ArrayAdapter.this) {
            items.clear();
            items.addAll((List<T>) results.values);
          }
          if (results.count > 0)
            notifyDataSetChanged();
          else
            notifyDataSetInvalidated();
        }
      };
    return filter;
  }
}
