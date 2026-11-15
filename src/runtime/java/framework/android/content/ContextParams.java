package android.content;

import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Set;

/** AOSP android.content.ContextParams (API 31): the attribution a Context.createContext context carries. */
public final class ContextParams {
  private final String attributionTag;
  private final AttributionSource next;
  private final Set<String> renouncedPermissions;
  private ContextParams(String attributionTag, AttributionSource next, Set<String> renouncedPermissions) {
    this.attributionTag = attributionTag;
    this.next = next;
    this.renouncedPermissions = renouncedPermissions;
  }
  public String getAttributionTag() { return attributionTag; }
  public AttributionSource getNextAttributionSource() { return next; }
  /** SystemApi. */
  public Set<String> getRenouncedPermissions() { return renouncedPermissions; }

  public static final class Builder {
    private String attributionTag;
    private AttributionSource next;
    private Set<String> renouncedPermissions = Collections.emptySet();
    public Builder() {}
    public Builder(ContextParams params) {
      java.util.Objects.requireNonNull(params);
      attributionTag = params.attributionTag;
      next = params.next;
      renouncedPermissions = params.renouncedPermissions;
    }
    public Builder setAttributionTag(String attributionTag) {
      this.attributionTag = attributionTag;
      return this;
    }
    public Builder setNextAttributionSource(AttributionSource next) {
      this.next = next;
      return this;
    }
    /** SystemApi. */
    public Builder setRenouncedPermissions(Set<String> renouncedPermissions) {
      this.renouncedPermissions = renouncedPermissions == null || renouncedPermissions.isEmpty()
          ? Collections.emptySet() : Collections.unmodifiableSet(new LinkedHashSet<>(renouncedPermissions));
      return this;
    }
    public ContextParams build() {
      return new ContextParams(attributionTag, next, renouncedPermissions);
    }
  }
}
