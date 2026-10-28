package horizonos.supplement.hzplatformclientcore;

import java.nio.charset.StandardCharsets;

// OS supplement declared by Epic 6419's uses-horizonos-supplement manifest.
// Recovered from AndroidClient in that APK's libil2cpp.so + metadata v107:
// Initialize 0x743d44c: ()V constructor; setup(config)V; getGlobalSessionId()J.
// MakeRequest 0x743e288 / MakeSession 0x743e7b0:
// (String module, String requestName, int apiVersion, String requestData, int cookie)J.
// StopSession 0x743ecd8: (J)V. GetMessageCount 0x743df78: (J)I.
// PopMessage 0x743f064: (JZ)Object; Get<long>(requestId), Get<int>(statusCode,
// cookie), Get<String>(statusMessage, data). Unity generates these descriptors
// from boxed arguments and generic return types, not literal JNI signatures.
public final class HorizonPlatformCore {
    public HorizonPlatformCore() {}
    public void setup(AndroidHorizonPlatformConfig config) {
        if (config == null || nativeSetup(config.appId) != 0)
            throw new IllegalStateException("Horizon Platform initialization failed");
    }
    public long getGlobalSessionId() { return 1; }
    public long makeRequest(String module, String requestName, int apiVersion, String requestData, int cookie) {
        return request(module, requestName, apiVersion, requestData, cookie, false);
    }
    public long makeSession(String module, String requestName, int apiVersion, String requestData, int cookie) {
        return request(module, requestName, apiVersion, requestData, cookie, true);
    }
    private long request(String module, String name, int version, String data, int cookie, boolean subscription) {
        if (module == null || name == null) throw new IllegalArgumentException("Missing Horizon method");
        return nativeRequest(module, name, version,
            (data == null ? "{}" : data).getBytes(StandardCharsets.UTF_8), cookie, subscription);
    }
    public void stopSession(long sessionId) { nativeStop(sessionId); }
    public int getMessageCount(long sessionId) { return nativeCount(sessionId); }
    public Object popMessage(long sessionId, boolean yield) {
        Object result = nativePop(sessionId);
        if (result == null && yield) Thread.yield();
        return result;
    }
    public static final class Message {
        public final long requestId;
        public final int statusCode, cookie;
        public final String statusMessage, data;
        public Message(long requestId, int statusCode, int cookie, String statusMessage, String data) {
            this.requestId = requestId; this.statusCode = statusCode; this.cookie = cookie;
            this.statusMessage = statusMessage; this.data = data;
        }
    }
    private static native int nativeSetup(String appId);
    private static native long nativeRequest(String module, String method, int version, byte[] data, int cookie, boolean subscription);
    private static native Object nativePop(long sessionId);
    private static native int nativeCount(long sessionId);
    private static native void nativeStop(long sessionId);
}
