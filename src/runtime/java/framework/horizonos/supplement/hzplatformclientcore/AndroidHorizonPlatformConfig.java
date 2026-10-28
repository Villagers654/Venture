package horizonos.supplement.hzplatformclientcore;

import android.content.Context;

// Epic 6419 AndroidClient::Initialize (libil2cpp RVA 0x743d44c) constructs
// this with UnityPlayer.currentActivity and the managed String AppID.
public final class AndroidHorizonPlatformConfig {
    final String appId;
    public AndroidHorizonPlatformConfig(Context context, String appId) {
        if (context == null || appId == null) throw new IllegalArgumentException("Missing Horizon configuration");
        this.appId = appId;
    }
}
