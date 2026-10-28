import android.content.*;
import android.os.*;
import com.venture.runtime.*;
public final class ColdRestartTest {
  private static void check(boolean condition) { if (!condition) throw new AssertionError("cold restart behavior"); }
  public static void main(String[] args) throws Exception {
    try (RecordingHost host = new RecordingHost(args[1])) {
      RuntimeEnvironment.configure(args, host);
      Looper.prepareMainLooper();
      Intent launcher = RuntimeEnvironment.get().packageManager.getLaunchIntentForPackage(args[0]);
      check(launcher != null);
      var extraFile = java.nio.file.Files.createTempFile("venture-launch-extras-", ".properties");
      try {
        java.nio.file.Files.writeString(extraFile, "scene=diagnostic\ncaption=café\\nnext line\n");
        var initial = AppMain.initialLaunchIntent(launcher, null, extraFile.toString());
        check("diagnostic".equals(initial.getStringExtra("scene")));
        check("café\nnext line".equals(initial.getStringExtra("caption")));
        check(!launcher.hasExtra("scene") && initial.getComponent().equals(launcher.getComponent()));
        check(AppMain.initialLaunchIntent(launcher, null, null) == launcher);
        var bytes = new java.io.ByteArrayOutputStream();
        var output = new java.io.DataOutputStream(bytes);
        output.writeInt(Intent.FLAG_ACTIVITY_NEW_TASK);
        output.writeInt(1);
        output.writeUTF("scene"); output.writeBoolean(true); output.writeUTF("app-restart");
        var payload = bytes.toByteArray();
        // The file may have disappeared since the initial launch. A restart
        // still consumes only the app's intent and does not reopen that file.
        java.nio.file.Files.delete(extraFile);
        var restarted = AppMain.initialLaunchIntent(launcher, payload, extraFile.toString());
        check("app-restart".equals(restarted.getStringExtra("scene")));
        try {
          AppMain.initialLaunchIntent(launcher, null, extraFile.toString());
          throw new AssertionError("missing launch extras silently ignored");
        } catch (java.nio.file.NoSuchFileException correct) {}
        java.nio.file.Files.writeString(extraFile, "=invalid\n");
        try {
          AppMain.initialLaunchIntent(launcher, null, extraFile.toString());
          throw new AssertionError("empty extra key accepted");
        } catch (IllegalArgumentException correct) {}
      } finally { java.nio.file.Files.deleteIfExists(extraFile); }
      int cold = Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP;
      if (args.length > 8 && args[8].equals("delivered")) {
        AppMain.startActivity(new Intent(launcher).addFlags(cold));
        try { Looper.loop(); throw new AssertionError("cold delivery returned"); }
        catch (ThreadDeath stopped) {}
        check(host.restarts == 1 && !AppMain.takePendingRelaunch());
        System.out.println("PASS cold delivery wins the exit race without recreating the native activity");
        return;
      }
      if (args.length > 8 && args[8].equals("concurrent")) {
        host.restartClaimed = new java.util.concurrent.CountDownLatch(1);
        host.publishRestart = new java.util.concurrent.CountDownLatch(1);
        AppMain.startActivity(new Intent(launcher).addFlags(cold));
        var first = new Thread(() -> { try { NativeLoader.exit(0); } catch (ThreadDeath done) {} });
        first.start();
        check(host.restartClaimed.await(5, java.util.concurrent.TimeUnit.SECONDS));
        var second = new Thread(() -> { try { NativeLoader.exit(0); } catch (ThreadDeath done) {} });
        second.start();
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (second.getState() != Thread.State.WAITING && second.isAlive()
            && System.nanoTime() < deadline) Thread.yield();
        check(second.isAlive() && second.getState() == Thread.State.WAITING);
        check(host.restarts == 0 && host.events.stream().noneMatch(e -> e.startsWith("exit ")));
        host.publishRestart.countDown();
        first.join(5000); second.join(5000);
        check(!first.isAlive() && !second.isAlive() && host.restarts == 1);
        System.out.println("PASS concurrent exit waits for claimed restart publication");
        return;
      }
      AppMain.startActivity(new Intent(launcher).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
      check(!AppMain.takePendingRelaunch());
      AppMain.startActivity(new Intent(launcher).addFlags(cold));
      try { NativeLoader.exit(0); throw new AssertionError("restart returned"); }
      catch (ThreadDeath stopped) {}
      check(host.restarts == 1 && !AppMain.takePendingRelaunch());
      String extra = "-opengl quoted \"value\"\n\u0000\uD83D\uDE80";
      AppMain.startActivity(new Intent(launcher).addFlags(cold)
          .putExtra("RestartExtra", extra).putExtra("nullable", (String) null));
      try { NativeLoader.exit(0); throw new AssertionError("restart returned"); }
      catch (ThreadDeath stopped) {}
      check(host.restarts == 2 && !AppMain.takePendingRelaunch());
      var restored = AppMain.restoreRelaunchIntent(launcher, host.coldLaunchIntent());
      check(restored.getComponent().equals(launcher.getComponent()));
      check(restored.getFlags() == ((launcher.getFlags() | cold) & ~Intent.FLAG_ACTIVITY_CLEAR_TASK));
      check(extra.equals(restored.getStringExtra("RestartExtra")));
      check(restored.hasExtra("nullable") && restored.getStringExtra("nullable") == null);
      check(host.coldLaunchIntent() == null);
      // A splash activity forwarding its getIntent() starts in-process.
      AppMain.startActivity(new Intent(restored));
      check(!AppMain.takePendingRelaunch());
      try { AppMain.restoreRelaunchIntent(launcher, new byte[]{1}); throw new AssertionError("truncated intent accepted"); }
      catch (IllegalStateException correct) {}
      AppMain.startActivity(new Intent(launcher).addFlags(cold).putExtra("custom", 1));
      try { AppMain.takePendingRelaunchPayload(); throw new AssertionError("unsupported intent silently restarted"); }
      catch (UnsupportedOperationException correct) {}
      AppMain.startActivity(new Intent(launcher).addFlags(cold).putExtra("large", "x".repeat(4096)));
      try { AppMain.takePendingRelaunchPayload(); throw new AssertionError("oversized intent accepted"); }
      catch (UnsupportedOperationException correct) {}
      AppMain.startActivity(new Intent(launcher).addFlags(cold).setData(android.net.Uri.parse("app://private")));
      try { AppMain.takePendingRelaunchPayload(); throw new AssertionError("custom data lost"); }
      catch (UnsupportedOperationException correct) {}
      AppMain.startActivity(new Intent(launcher).addFlags(cold));
      try { NativeLoader.exit(7); throw new AssertionError("exit returned"); }
      catch (ThreadDeath stopped) {}
      check(host.restarts == 2 && host.events.contains("exit 7") && !AppMain.takePendingRelaunch());
      System.out.println("PASS cold launch survives exit, custom intents fail visibly, nonzero exits stay exits");
    }
  }
}
