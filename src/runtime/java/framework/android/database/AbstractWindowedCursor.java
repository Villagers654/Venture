package android.database;

/** AOSP android.database.AbstractWindowedCursor: column reads from the current row of a CursorWindow. */
public abstract class AbstractWindowedCursor extends AbstractCursor {
  protected CursorWindow mWindow;

  @Override public byte[] getBlob(int column) { checkPosition(); return mWindow.getBlob(mPos, column); }
  @Override public String getString(int column) { checkPosition(); return mWindow.getString(mPos, column); }
  @Override public short getShort(int column) { checkPosition(); return mWindow.getShort(mPos, column); }
  @Override public int getInt(int column) { checkPosition(); return mWindow.getInt(mPos, column); }
  @Override public long getLong(int column) { checkPosition(); return mWindow.getLong(mPos, column); }
  @Override public float getFloat(int column) { checkPosition(); return mWindow.getFloat(mPos, column); }
  @Override public double getDouble(int column) { checkPosition(); return mWindow.getDouble(mPos, column); }
  @Override public boolean isNull(int column) { checkPosition(); return mWindow.isNull(mPos, column); }
  @Override public int getType(int column) { checkPosition(); return mWindow.getType(mPos, column); }
  @Override protected void checkPosition() {
    super.checkPosition();
    if (mWindow == null)
      throw new StaleDataException("Attempting to access a closed CursorWindow."
          + "Most probable cause: cursor is deactivated prior to calling this method.");
  }
  @Override public CursorWindow getWindow() { return mWindow; }
  public void setWindow(CursorWindow window) {
    if (window != mWindow) {
      closeWindow();
      mWindow = window;
    }
  }
  public boolean hasWindow() { return mWindow != null; }
  protected void clearOrCreateWindow(String name) {
    if (mWindow == null) mWindow = new CursorWindow(name);
    else mWindow.clear();
  }
  protected void closeWindow() {
    if (mWindow != null) {
      mWindow.close();
      mWindow = null;
    }
  }
  @Override protected void onDeactivateOrClose() {
    super.onDeactivateOrClose();
    closeWindow();
  }
}
