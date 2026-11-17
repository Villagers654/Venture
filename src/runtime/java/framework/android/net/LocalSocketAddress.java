package android.net;

// AOSP LocalSocketAddress: a name in one of the local socket namespaces.
public class LocalSocketAddress {
  public enum Namespace {
    ABSTRACT(0), RESERVED(1), FILESYSTEM(2);
    private final int id;
    Namespace(int id) {
      this.id = id;
    }
    int getId() {
      return id;
    }
  }
  private final String name;
  private final Namespace namespace;
  public LocalSocketAddress(String name, Namespace namespace) {
    this.name = name;
    this.namespace = namespace;
  }
  public LocalSocketAddress(String name) {
    this(name, Namespace.ABSTRACT);
  }
  public String getName() {
    return name;
  }
  public Namespace getNamespace() {
    return namespace;
  }
}
