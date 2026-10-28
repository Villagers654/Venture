package android.widget;
public interface Adapter {
  int getCount();
  Object getItem(int position);
  long getItemId(int position);
  android.view.View getView(
      int position, android.view.View convertView, android.view.ViewGroup parent);
  void registerDataSetObserver(android.database.DataSetObserver observer);
  void unregisterDataSetObserver(android.database.DataSetObserver observer);
  boolean hasStableIds();
  int getItemViewType(int position);
  int getViewTypeCount();
  boolean isEmpty();
}
