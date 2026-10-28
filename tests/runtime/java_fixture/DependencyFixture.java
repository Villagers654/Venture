public final class DependencyFixture {
    public static class Parent {}
    public static class Optional extends Parent {
        public static native int value(int input);
    }
    public static class Ready {
        public static native int value(int input);
    }
    public static class Invalid {
        public static native int value(int input);
    }
    public static final class Loader extends java.net.URLClassLoader {
        private boolean available;
        Loader() {
            super(new java.net.URL[]{DependencyFixture.class.getProtectionDomain().getCodeSource().getLocation()},
                  DependencyFixture.class.getClassLoader());
        }
        @Override protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
            synchronized (getClassLoadingLock(name)) {
                if (name.equals("DependencyFixture$Invalid")) throw new VerifyError("fixture verification failure");
                if (name.equals("DependencyFixture$Parent") && !available) throw new ClassNotFoundException(name);
                if (!name.equals("DependencyFixture$Parent") && !name.equals("DependencyFixture$Optional")
                        && !name.equals("DependencyFixture$Ready")) return super.loadClass(name, resolve);
                Class<?> cls = findLoadedClass(name);
                if (cls == null) cls = findClass(name);
                if (resolve) resolveClass(cls);
                return cls;
            }
        }
    }
    public static ClassLoader loader() { return new Loader(); }
    public static void enable(ClassLoader loader) { ((Loader)loader).available = true; }
}
