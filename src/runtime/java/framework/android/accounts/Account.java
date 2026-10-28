package android.accounts;

import android.os.*;

/** Account identifier value; constructing it does not authenticate an account. */
public final class Account implements Parcelable {
  public final String name, type;
  public Account(String name, String type) {
    if (name == null || name.isEmpty() || type == null || type.isEmpty())
      throw new IllegalArgumentException("Account name and type are required");
    this.name = name; this.type = type;
  }
  public Account(Parcel in) { this(in.readString(), in.readString()); }
  public boolean equals(Object value) {
    return value instanceof Account account && name.equals(account.name) && type.equals(account.type);
  }
  public int hashCode() { return 31 * (31 * 17 + name.hashCode()) + type.hashCode(); }
  public String toString() { return "Account {name=" + name + ", type=" + type + "}"; }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel out, int flags) { out.writeString(name); out.writeString(type); }
  public static final Parcelable.Creator<Account> CREATOR = new Parcelable.Creator<>() {
    public Account createFromParcel(Parcel in) { return new Account(in); }
    public Account[] newArray(int count) { return new Account[count]; }
  };
}
