/** Runs the converted super_init fixture (see callers.smali) under -Xverify:all. */
public class SuperInitCheck {
    static Object call(String name, Class<?>[] types, Object... args) throws Exception {
        return Class.forName("SuperInitCallers").getMethod(name, types).invoke(null, args);
    }
    static void check(boolean ok, String what) { if (!ok) throw new AssertionError(what); }

    public static void main(String[] args) throws Exception {
        Throwable cause = new IllegalStateException("cause");
        Object wrapped = call("wrap", new Class<?>[] {Throwable.class}, cause);
        check(wrapped.getClass().getName().equals("MergedError"), "wrap class " + wrapped.getClass());
        check(((Throwable)wrapped).getCause() == cause, "wrap cause");

        Object plain = call("plain", new Class<?>[] {int.class}, 7);
        check(plain.getClass().getName().equals("PlainData"), "plain class " + plain.getClass());
        check(plain.getClass().getField("value").getInt(plain) == 7, "plain field");

        Object grand = call("grand", new Class<?>[] {String.class}, "grand");
        check(grand.getClass().getName().equals("MergedChild"), "grand class " + grand.getClass());
        check(((Exception)grand).getMessage().equals("grand"), "grand message");

        Object collide = call("collide", new Class<?>[] {String.class}, "collide");
        check(collide.getClass().getName().equals("CollidingError"), "collide class " + collide.getClass());
        check(((Throwable)collide).getMessage().equals("collide"), "collide message");
        check(!collide.getClass().getField("marked").getBoolean(collide), "the colliding constructor ran");

        Object ordinary = call("ordinary", new Class<?>[0]);
        check(ordinary.getClass().getField("marked").getBoolean(ordinary), "ordinary constructor did not run");

        check(call("handler", new Class<?>[] {String.class, boolean.class}, "12", true) == null, "handler parse");
        for (boolean flag : new boolean[] {true, false}) {
            Throwable t = (Throwable)call("handler", new Class<?>[] {String.class, boolean.class}, "x", flag);
            check(t.getClass().getName().equals("MergedError"), "handler class " + t.getClass());
            check(t.getMessage().equals(flag ? "flagged" : "plain"), "handler message " + t.getMessage());
            check(t.getCause() instanceof NumberFormatException, "handler cause");
        }
        Object busy = call("noargs", new Class<?>[] {int.class}, 5);
        check(busy.getClass().getName().equals("BusyData"), "noargs class " + busy.getClass());
        check(busy.getClass().getField("value").getInt(busy) == 5, "noargs parameter after the register shift");
        check(!busy.getClass().getField("marked").getBoolean(busy), "the colliding no-argument constructor ran");
        System.out.println("super_init fixture ok");
    }
}
