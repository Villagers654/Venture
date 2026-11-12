package android.webkit;

/** AOSP android.webkit.PermissionRequest: a page's request for protected resources. */
public abstract class PermissionRequest {
  public static final String RESOURCE_VIDEO_CAPTURE = "android.webkit.resource.VIDEO_CAPTURE",
      RESOURCE_AUDIO_CAPTURE = "android.webkit.resource.AUDIO_CAPTURE",
      RESOURCE_PROTECTED_MEDIA_ID = "android.webkit.resource.PROTECTED_MEDIA_ID",
      RESOURCE_MIDI_SYSEX = "android.webkit.resource.MIDI_SYSEX";
  public PermissionRequest() {}
  public abstract android.net.Uri getOrigin();
  public abstract String[] getResources();
  public abstract void grant(String[] resources);
  public abstract void deny();
}
