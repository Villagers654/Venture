package android.widget;
public abstract class AdapterView<T extends Adapter> extends android.view.ViewGroup {
  private OnItemSelectedListener selected;
  private OnItemClickListener clicked;
  public AdapterView(android.content.Context c) {
    super(c);
  }
  public abstract T getAdapter();
  public abstract void setAdapter(T adapter);
  public abstract android.view.View getSelectedView();
  public abstract void setSelection(int position);
  public void setOnItemSelectedListener(OnItemSelectedListener l) {
    selected = l;
  }
  public void setOnItemClickListener(OnItemClickListener l) {
    clicked = l;
  }
  public interface OnItemSelectedListener {
    void onItemSelected(AdapterView<?> parent, android.view.View view, int position, long id);
    void onNothingSelected(AdapterView<?> parent);
  }
  public interface OnItemClickListener {
    void onItemClick(AdapterView<?> parent, android.view.View view, int position, long id);
  }
}
